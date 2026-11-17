#ifndef KYTY_KERNEL_READ_WATCH_LOCKS_H_
#define KYTY_KERNEL_READ_WATCH_LOCKS_H_

// gta6-prep: ram-lock
// Keeps the guest memory of the PageManager's read watches in RAM (Windows,
// --lock-read-watched-mb). A read watch makes guest pages PAGE_NOACCESS so that the CPU's reads of
// what the GPU wrote fault. Guest memory is one pagefile-backed section, mapped at the guest
// addresses and once more as a whole (the backing view, always read-write). Another fork found that
// Windows mishandles a no-access page of a mapped view once it has paged the page out: a later
// protection change on it can bug check (MEMORY_MANAGEMENT 0x1A/0x41202 in nt!MiGetPageProtection;
// chenxiao07/KytyPS5#5, reproduced there, not here). A page locked in RAM never leaves it. So
// before a run of guest pages loses its access, their pages of the backing view are locked
// (VirtualLock cannot lock a no-access page), and once the access is back they are unlocked.
//
// Pure: the OS calls go through `Os` (read_watch_lock_tests counts them). Not thread-safe: the
// guest address space calls it under a mutex of its own, with its state lock held shared, so the
// mapping and the translation from guest page to backing page cannot change during a call.

#include "kernel/memory.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::LibKernel::Memory {

// The pages of the bookkeeping: the PageManager's (TRACKER_PAGE_SIZE) and the host's.
inline constexpr uint64_t ReadWatchPageSize = 0x1000;
// What the raise of the working set before the first lock adds to the budget: VirtualLock locks no
// more than the minimum working set holds, less a few pages.
inline constexpr uint64_t ReadWatchLockMargin = uint64_t {16} << 20u;

// `Os` works on the backing view, where `offset` and `size` are page-aligned byte offsets:
//   bool Prepare(uint64_t budget): once, before the first Lock(), makes room for `budget` bytes of
//     locks. False: the locks stay off.
//   bool Lock(uint64_t offset, uint64_t size): VirtualLock.
//   void Unlock(uint64_t offset, uint64_t size): VirtualUnlock, only ever of pages Lock() locked.
//     VirtualUnlock of a page that is not locked takes it out of the working set instead.
//   void QueryLocked(uint64_t offset, uint64_t pages, std::vector<uint8_t>& locked): after a failed
//     Lock() of those pages, which of them are locked all the same (`locked` has `pages` zeros).
template <typename Os>
class ReadWatchLocks {
public:
	static constexpr uint64_t PageSize = ReadWatchPageSize;

	explicit ReadWatchLocks(Os os = {}): m_os(std::move(os)) {}

	// The most bytes locked at once; 0: no new locks. Pages locked already stay locked until their
	// watch ends.
	void SetBudget(uint64_t bytes) noexcept {
		m_budget_pages.store(bytes / PageSize, std::memory_order_relaxed);
	}
	[[nodiscard]] bool Enabled() const noexcept {
		return m_budget_pages.load(std::memory_order_relaxed) != 0;
	}
	// Whether some guest page holds a lock (Unlock() has something to do).
	[[nodiscard]] bool HasLocks() const noexcept {
		return m_held_count.load(std::memory_order_relaxed) != 0;
	}

	// A read watch starts on the guest pages [start, start + size), which the backing view holds
	// from `offset` on (one run of contiguous backing). Locks their backing pages while the budget
	// allows; pages past it stay unlocked and are counted.
	void Lock(uint64_t offset, uint64_t start, uint64_t size) {
		const auto budget = m_budget_pages.load(std::memory_order_relaxed);
		if (budget == 0 || size == 0) {
			return;
		}
		// Whole pages: the start and its backing share their offset within a page.
		const auto skew = start & (PageSize - 1);
		start -= skew;
		offset -= skew;
		const auto pages = (size + skew + PageSize - 1) / PageSize;

		// The pages to lock with one call: contiguous in the guest and in the backing view.
		uint64_t pending_start  = 0;
		uint64_t pending_offset = 0;
		uint64_t pending_pages  = 0;

		const auto flush = [&] {
			if (pending_pages != 0) {
				LockRun(pending_start, pending_offset, pending_pages);
				pending_pages = 0;
			}
		};
		auto held = m_held.lower_bound(start);
		for (uint64_t index = 0; index < pages; index++) {
			const auto guest   = start + index * PageSize;
			const auto backing = offset + index * PageSize;
			if (held != m_held.end() && held->first == guest) {
				if (held->second == backing) {
					// Locked for this page already: a run protected again through it.
					flush();
					++held;
					continue;
				}
				// Mapped over other memory since its lock: that lock goes.
				held = Release(held, true);
			}
			if (const auto holders = m_holders.find(backing); holders != m_holders.end()) {
				// Another guest mapping of the same memory holds the lock.
				flush();
				holders->second++;
				held = std::next(m_held.emplace_hint(held, guest, backing));
				continue;
			}
			if (m_locked_pages + pending_pages >= budget) {
				flush();
				m_over_budget++;
				continue;
			}
			if (!m_prepared) {
				m_prepared = true;
				if (!m_os.Prepare(budget * PageSize)) {
					m_budget_pages.store(0, std::memory_order_relaxed);
					Publish();
					return;
				}
			}
			if (pending_pages == 0) {
				pending_start  = guest;
				pending_offset = backing;
			}
			pending_pages++;
		}
		flush();
		Publish();
	}

	// The read watch ends on the guest pages of [vaddr, vaddr + size), which have their access
	// back: unlocks the backing pages Lock() locked for them that no other guest page holds.
	void Unlock(uint64_t vaddr, uint64_t size) {
		if (m_held.empty() || size == 0) {
			return;
		}
		const auto end = vaddr + size;
		for (auto held = m_held.lower_bound(vaddr & ~(PageSize - 1));
		     held != m_held.end() && held->first < end;) {
			held = Release(held, false);
		}
		FlushUnlocks();
		Publish();
	}

	// Any thread.
	[[nodiscard]] ReadWatchLockStats Stats() const noexcept {
		ReadWatchLockStats stats;
		stats.budget_bytes      = m_budget_pages.load(std::memory_order_relaxed) * PageSize;
		stats.locked_bytes      = m_stats_locked.load(std::memory_order_relaxed) * PageSize;
		stats.over_budget_pages = m_stats_over_budget.load(std::memory_order_relaxed);
		stats.failed_pages      = m_stats_failed.load(std::memory_order_relaxed);
		return stats;
	}

	// The guest pages that hold a lock.
	[[nodiscard]] uint64_t HeldPages() const noexcept { return m_held.size(); }

	[[nodiscard]] Os&       GetOs() noexcept { return m_os; }
	[[nodiscard]] const Os& GetOs() const noexcept { return m_os; }

private:
	using Held = std::map<uint64_t, uint64_t>;

	// Locks the backing pages [offset, offset + pages) for the guest pages from `start` on, none of
	// which any guest page holds yet.
	void LockRun(uint64_t start, uint64_t offset, uint64_t pages) {
		if (m_os.Lock(offset, pages * PageSize)) {
			for (uint64_t index = 0; index < pages; index++) {
				Hold(start + index * PageSize, offset + index * PageSize);
			}
			m_locked_pages += pages;
			return;
		}
		// VirtualLock may have locked some of them before it failed: those are held like the
		// others, so that they are unlocked once their watch ends.
		m_query.assign(pages, 0);
		m_os.QueryLocked(offset, pages, m_query);
		for (uint64_t index = 0; index < pages; index++) {
			if (m_query[index] != 0) {
				Hold(start + index * PageSize, offset + index * PageSize);
				m_locked_pages++;
			} else {
				m_failed++;
			}
		}
	}

	void Hold(uint64_t guest, uint64_t backing) {
		m_held.emplace(guest, backing);
		m_holders.emplace(backing, 1);
	}

	// Drops the lock `held` holds; unlocks its backing page when no other guest page holds it, at
	// once or batched with the next ones (FlushUnlocks()).
	typename Held::iterator Release(typename Held::iterator held, bool now) {
		const auto backing = held->second;
		if (const auto holders = m_holders.find(backing);
		    holders != m_holders.end() && --holders->second == 0) {
			m_holders.erase(holders);
			m_locked_pages--;
			if (now) {
				m_os.Unlock(backing, PageSize);
			} else if (m_unlock_pages != 0 &&
			           backing == m_unlock_offset + m_unlock_pages * PageSize) {
				m_unlock_pages++;
			} else {
				FlushUnlocks();
				m_unlock_offset = backing;
				m_unlock_pages  = 1;
			}
		}
		return m_held.erase(held);
	}

	void FlushUnlocks() {
		if (m_unlock_pages != 0) {
			m_os.Unlock(m_unlock_offset, m_unlock_pages * PageSize);
			m_unlock_pages = 0;
		}
	}

	void Publish() noexcept {
		m_held_count.store(m_held.size(), std::memory_order_relaxed);
		m_stats_locked.store(m_locked_pages, std::memory_order_relaxed);
		m_stats_over_budget.store(m_over_budget, std::memory_order_relaxed);
		m_stats_failed.store(m_failed, std::memory_order_relaxed);
	}

	Os m_os;
	// The guest pages that hold a lock, and their backing page.
	Held m_held;
	// The backing pages locked, and how many guest pages hold each: guest mappings of the same
	// memory share its lock.
	std::unordered_map<uint64_t, uint32_t> m_holders;
	bool                                   m_prepared     = false;
	uint64_t                               m_locked_pages = 0;
	uint64_t                               m_over_budget  = 0;
	uint64_t                               m_failed       = 0;
	// Release()'s pending unlock: a run of backing pages.
	uint64_t             m_unlock_offset = 0;
	uint64_t             m_unlock_pages  = 0;
	std::vector<uint8_t> m_query;
	// Read without the caller's mutex.
	std::atomic<uint64_t> m_budget_pages {0};
	std::atomic<uint64_t> m_held_count {0};
	std::atomic<uint64_t> m_stats_locked {0};
	std::atomic<uint64_t> m_stats_over_budget {0};
	std::atomic<uint64_t> m_stats_failed {0};
};

} // namespace Libs::LibKernel::Memory

#endif /* KYTY_KERNEL_READ_WATCH_LOCKS_H_ */
