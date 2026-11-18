#ifndef KYTY_KERNEL_READ_WATCH_LOCKS_H_
#define KYTY_KERNEL_READ_WATCH_LOCKS_H_

// gta6-prep: ram-lock
// Keeps the guest memory of the PageManager's read watches in RAM (Windows,
// --lock-read-watched-mb). A read watch makes guest pages PAGE_NOACCESS so that the CPU's reads of
// what the GPU wrote fault. Guest memory is one pagefile-backed section, mapped at the guest
// addresses and once more as a whole (the backing view, always read-write). Another fork found that
// Windows mishandles a no-access page of a mapped view once it has paged the page out: a later
// protection change on it can bug check (MEMORY_MANAGEMENT 0x1A/0x41202 in nt!MiGetPageProtection;
// chenxiao07/KytyPS5#5, reproduced there, not here). Their fix makes those pages guard pages
// instead (91a15d6c) and keeps no access only on pages it locked in RAM (d8f99391a5). Here, before
// a run of guest pages loses its access, their pages of the backing view are locked (VirtualLock
// cannot lock a no-access page), and once the access is back they are unlocked. A page locked in
// RAM never leaves it.
//
// The pages the budget or Windows refuses get no lock. They are remembered until their watch ends
// ("unlocked" on the "RAM usage:" lines), and an unlock offers the budget it frees to them.
// gta6-prep: read-guard: without --read-watch-guard-pages they stay no access, as they were before
// the lock existed. With it, the guest address space makes them guard pages instead, and keeps no
// access only where ForEachLockRun() reports a lock (GuestAddressSpace::ProtectTransient()).
//
// Pure: the OS calls go through `Os` (read_watch_lock_tests counts them). Not thread-safe: the
// guest address space calls it under a mutex of its own, with its state lock held shared, so the
// mapping and the translation from guest page to backing page cannot change during a call.

#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::LibKernel::Memory {

// The pages of the bookkeeping: the PageManager's (TRACKER_PAGE_SIZE) and the host's.
inline constexpr uint64_t ReadWatchPageSize = 0x1000;
// What the raise of the working set adds to the budget: VirtualLock locks no more than the minimum
// working set holds, less a few pages.
inline constexpr uint64_t ReadWatchLockMargin = uint64_t {16} << 20u;

// gta6-prep: read-guard: a set of guest pages, kept as page-aligned runs [begin, end) that neither
// overlap nor touch. ReadWatchLocks keeps the pages refused a lock in one, and the guest address
// space the read watches' guard pages in another. Not thread-safe.
class ReadWatchPageRuns {
public:
	static constexpr uint64_t PageSize = ReadWatchPageSize;

	// The pages of the page-aligned [begin, end) join the set.
	void Add(uint64_t begin, uint64_t end) {
		if (begin >= end) {
			return;
		}
		Erase(begin, end);
		m_pages += (end - begin) / PageSize;
		auto next = m_runs.lower_bound(begin);
		if (next != m_runs.begin()) {
			if (const auto previous = std::prev(next); previous->second == begin) {
				begin = previous->first;
				m_runs.erase(previous);
			}
		}
		if (next != m_runs.end() && next->first == end) {
			end  = next->second;
			next = m_runs.erase(next);
		}
		m_runs.emplace_hint(next, begin, end);
	}

	// The pages of the page-aligned [begin, end) leave it. (An empty range would split the run
	// holding it into two that touch.)
	void Erase(uint64_t begin, uint64_t end) {
		if (begin >= end) {
			return;
		}
		auto run = m_runs.lower_bound(begin);
		if (run != m_runs.begin()) {
			if (const auto previous = std::prev(run); previous->second > begin) {
				run = previous;
			}
		}
		while (run != m_runs.end() && run->first < end) {
			const auto first = run->first;
			const auto last  = run->second;
			run              = m_runs.erase(run);
			m_pages -= (std::min(last, end) - std::max(first, begin)) / PageSize;
			if (first < begin) {
				m_runs.emplace_hint(run, first, begin);
			}
			if (last > end) {
				m_runs.emplace_hint(run, end, last);
				break;
			}
		}
	}

	[[nodiscard]] uint64_t Pages() const noexcept { return m_pages; }
	[[nodiscard]] bool     Empty() const noexcept { return m_runs.empty(); }

	// The first run that starts at `from` or later.
	[[nodiscard]] std::optional<std::pair<uint64_t, uint64_t>> First(uint64_t from) const {
		const auto run = m_runs.lower_bound(from);
		if (run == m_runs.end()) {
			return std::nullopt;
		}
		return std::pair<uint64_t, uint64_t> {run->first, run->second};
	}

	// Calls f(begin, end) for the part of each run within [begin, end), lowest first.
	template <typename F>
	void ForEach(uint64_t begin, uint64_t end, F&& f) const {
		auto run = m_runs.lower_bound(begin);
		if (run != m_runs.begin()) {
			if (const auto previous = std::prev(run); previous->second > begin) {
				run = previous;
			}
		}
		for (; run != m_runs.end() && run->first < end; ++run) {
			f(std::max(run->first, begin), std::min(run->second, end));
		}
	}

private:
	std::map<uint64_t, uint64_t> m_runs;
	uint64_t                     m_pages = 0;
};

// What Os::Lock() reports. Quota: Windows' limit of locked pages (ERROR_WORKING_SET_QUOTA).
enum class ReadWatchLockResult : uint8_t { Locked, Quota, Failed };

// `Os` works on the backing view, where `offset` and `size` are page-aligned byte offsets:
//   bool Prepare(uint64_t bytes): makes room for `bytes` of locks in all (the working set). False:
//     it made no more.
//   void Prepared(uint64_t budget, uint64_t refused, uint64_t room): after the Prepare() calls of
//     a SetBudget() that needed more room: the budget in bytes, the room asked for if Windows
//     refused it (else 0), and the room made (0: none, and the lock stays off).
//   ReadWatchLockResult Lock(uint64_t offset, uint64_t size): VirtualLock.
//   void Unlock(uint64_t offset, uint64_t size): VirtualUnlock, only ever of pages Lock() locked.
//     VirtualUnlock of a page that is not locked takes it out of the working set instead.
//   void QueryLocked(uint64_t offset, uint64_t pages, std::vector<uint8_t>& locked): after a failed
//     Lock() of those pages, which of them are locked all the same (`locked` has `pages` zeros).
//   void Capped(uint64_t limit, uint64_t budget): once, when Windows first refused more locks or
//     more room: the bytes it stopped the locks at, below the `budget`.
template <typename Os>
class ReadWatchLocks {
public:
	static constexpr uint64_t PageSize = ReadWatchPageSize;
	// An unlock offers the budget it frees to this many refused pages at most: their VirtualLock
	// runs in the unlock's call (1 MiB).
	static constexpr uint64_t RetryPages = 256;
	// The room the working set makes for the locks grows with them, so that it reserves little
	// more than what is locked: SetBudget() makes room for RoomStep (or the budget, if smaller) at
	// once, and a lock that needs more grows it by RoomStep, up to the budget.
	static constexpr uint64_t RoomStep = uint64_t {64} << 20u;
	// SetBudget() halves what it asks for while Windows refuses, down to this.
	static constexpr uint64_t SmallestAsk = uint64_t {1} << 20u;

	// The room step, whole pages, at least one.
	static constexpr uint64_t StepOf(uint64_t room_step) {
		return std::max(room_step - room_step % PageSize, PageSize);
	}

	// `room_step`: RoomStep, but in tests. The OS side is built in place: the Windows one holds
	// atomics, so it can be neither copied nor moved.
	explicit ReadWatchLocks(uint64_t room_step = RoomStep): m_room_step(StepOf(room_step)) {}
	// The tests' OS side, handed in (only instantiated for an OS side that can be moved).
	explicit ReadWatchLocks(Os os, uint64_t room_step = RoomStep)
	    : m_os(std::move(os)), m_room_step(StepOf(room_step)) {}

	// The most bytes locked at once; 0: no new locks. The first room for them is made at once:
	// when Windows refuses it, SetBudget() asks for half as much, down to SmallestAsk, and the room
	// it got is the budget from then on (none: the lock stays off). Pages locked already stay
	// locked until their watch ends. Returns the budget in bytes.
	uint64_t SetBudget(uint64_t bytes) {
		bytes -= bytes % PageSize;
		m_budget_pages.store(bytes / PageSize, std::memory_order_relaxed);
		if (const auto first = std::min(bytes, m_room_step); first > m_room) {
			auto room = m_room;
			for (auto ask = first; ask > m_room; ask = Halved(ask)) {
				if (m_os.Prepare(ask)) {
					room = ask;
					break;
				}
			}
			m_room_final = room < first;
			m_room       = room;
			if (room == 0) {
				m_budget_pages.store(0, std::memory_order_relaxed);
			}
			m_os.Prepared(room == 0 ? bytes : Budget() * PageSize, room < first ? first : 0, room);
		}
		Publish();
		return Budget() * PageSize;
	}
	[[nodiscard]] bool Enabled() const noexcept {
		return m_budget_pages.load(std::memory_order_relaxed) != 0;
	}
	// Whether the budget has room for more locks (a hint: read without the caller's mutex).
	[[nodiscard]] bool HasRoom() const noexcept {
		return m_stats_locked.load(std::memory_order_relaxed) <
		       m_stats_budget.load(std::memory_order_relaxed);
	}
	// Whether some guest page holds a lock or was refused one (Unlock() has something to do).
	[[nodiscard]] bool HasWork() const noexcept {
		return m_tracked_count.load(std::memory_order_relaxed) != 0;
	}

	// A read watch starts on the guest pages [start, start + size), which the backing view holds
	// from `offset` on (one run of contiguous backing). Locks their backing pages while the budget
	// allows; the pages past it are refused.
	void Lock(uint64_t offset, uint64_t start, uint64_t size) {
		if (m_budget_pages.load(std::memory_order_relaxed) == 0 || size == 0) {
			return;
		}
		LockPages(offset, start, size);
		Publish();
	}

	// The read watch ends on the guest pages of [vaddr, vaddr + size), which have their access
	// back: unlocks the backing pages Lock() locked for them that no other guest page holds, and
	// forgets those it refused. The budget that frees goes to pages refused earlier, RetryPages of
	// them at most: `translate(start, bytes, f)` calls f(offset, start, bytes) for each run of
	// contiguous backing of a guest range, as Lock() takes them, and skips the unbacked pages.
	template <typename Translate>
	void Unlock(uint64_t vaddr, uint64_t size, Translate&& translate) {
		if (size == 0 || (m_held.empty() && m_refused.Empty())) {
			return;
		}
		const auto begin  = vaddr & ~(PageSize - 1);
		const auto end    = (vaddr + size + PageSize - 1) & ~(PageSize - 1);
		const auto locked = m_locked_pages;
		for (auto held = m_held.lower_bound(begin); held != m_held.end() && held->first < end;) {
			held = Release(held, false);
		}
		FlushUnlocks();
		EraseRefused(begin, end);
		if (m_locked_pages < locked) {
			Retry(translate);
		}
		Publish();
	}

	// Any thread.
	[[nodiscard]] ReadWatchLockStats Stats() const noexcept {
		ReadWatchLockStats stats;
		stats.budget_bytes   = m_stats_budget.load(std::memory_order_relaxed) * PageSize;
		stats.locked_bytes   = m_stats_locked.load(std::memory_order_relaxed) * PageSize;
		stats.unlocked_bytes = m_stats_refused.load(std::memory_order_relaxed) * PageSize;
		stats.failed_pages   = m_stats_failed.load(std::memory_order_relaxed);
		return stats;
	}

	// The guest pages that hold a lock, and those refused one.
	[[nodiscard]] uint64_t HeldPages() const noexcept { return m_held.size(); }
	[[nodiscard]] uint64_t RefusedPages() const noexcept { return m_refused.Pages(); }

	// gta6-prep: read-guard: calls f(start, end, locked) for the parts of [begin, end) in address
	// order: the runs of guest pages that hold a lock (locked) and the runs between them. A page
	// counts whole, cut to [begin, end). Right after Lock() of a range the parts of that range are
	// up to date: a page mapped over other memory since its lock has been seen to.
	template <typename F>
	void ForEachLockRun(uint64_t begin, uint64_t end, F&& f) const {
		auto cursor = begin;
		for (auto held = m_held.lower_bound(begin & ~(PageSize - 1));
		     held != m_held.end() && held->first < end;) {
			const auto run_begin = held->first;
			auto       run_end   = run_begin + PageSize;
			for (++held; held != m_held.end() && held->first == run_end; ++held) {
				run_end += PageSize;
			}
			const auto from = std::max(run_begin, begin);
			const auto to   = std::min(run_end, end);
			if (cursor < from) {
				f(cursor, from, false);
			}
			f(from, to, true);
			cursor = to;
		}
		if (cursor < end) {
			f(cursor, end, false);
		}
	}

	[[nodiscard]] Os&       GetOs() noexcept { return m_os; }
	[[nodiscard]] const Os& GetOs() const noexcept { return m_os; }

private:
	using Held = std::map<uint64_t, uint64_t>;

	static constexpr uint64_t NoCap = ~uint64_t {0};

	// SetBudget()'s next ask: half, in whole pages; 0 (no more asks) below 2 * SmallestAsk.
	static constexpr uint64_t Halved(uint64_t ask) {
		return ask >= SmallestAsk * 2 ? ask / 2 - ask / 2 % PageSize : 0;
	}

	// The budget in pages: SetBudget()'s, or less once Windows refused more locks (the cap) or
	// more room.
	[[nodiscard]] uint64_t Budget() const noexcept {
		const auto budget = std::min(m_budget_pages.load(std::memory_order_relaxed), m_cap_pages);
		return m_room_final ? std::min(budget, RoomPages()) : budget;
	}
	[[nodiscard]] uint64_t RoomPages() const noexcept { return m_room / PageSize; }

	// A lock needs more room than the working set has, and the budget allows it: RoomStep more.
	// When Windows refuses, the room is the budget from then on.
	void Grow() {
		const auto ask = std::min(Budget(), RoomPages() + m_room_step / PageSize) * PageSize;
		if (m_os.Prepare(ask)) {
			m_room = ask;
			return;
		}
		m_room_final = true;
		ReportCap();
	}

	// Windows' limit lowered the budget: said once.
	void ReportCap() {
		if (!std::exchange(m_capped, true)) {
			m_os.Capped(Budget() * PageSize,
			            m_budget_pages.load(std::memory_order_relaxed) * PageSize);
		}
	}

	// Lock() of one run, without Publish().
	void LockPages(uint64_t offset, uint64_t start, uint64_t size) {
		// Whole pages: the start and its backing share their offset within a page.
		const auto skew = start & (PageSize - 1);
		start -= skew;
		offset -= skew;
		const auto pages = (size + skew + PageSize - 1) / PageSize;
		// The pages are held, locked or refused again below.
		EraseRefused(start, start + pages * PageSize);

		// The pages to lock with one call: contiguous in the guest and in the backing view.
		uint64_t pending_start  = 0;
		uint64_t pending_offset = 0;
		uint64_t pending_pages  = 0;
		// The guest pages refused in a row.
		uint64_t refused_start = 0;
		uint64_t refused_end   = 0;

		const auto flush = [&] {
			if (pending_pages != 0) {
				LockRun(pending_start, pending_offset, pending_pages);
				pending_pages = 0;
			}
		};
		const auto refuse = [&](uint64_t guest) {
			if (refused_end != guest) {
				AddRefused(refused_start, refused_end);
				refused_start = guest;
			}
			refused_end = guest + PageSize;
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
			if (m_holders.contains(backing)) {
				// Another guest mapping of the same memory holds the lock. (Looked up again after
				// the flush, whose inserts may rehash.)
				flush();
				m_holders.find(backing)->second++;
				held = std::next(m_held.emplace_hint(held, guest, backing));
				continue;
			}
			if (!m_room_final && m_locked_pages + pending_pages >= RoomPages() &&
			    RoomPages() < Budget()) {
				// The budget allows more than the room holds.
				flush();
				if (m_locked_pages >= RoomPages() && RoomPages() < Budget()) {
					Grow();
				}
			}
			if (m_locked_pages + pending_pages >= Budget()) {
				flush();
				refuse(guest);
				continue;
			}
			if (pending_pages == 0) {
				pending_start  = guest;
				pending_offset = backing;
			}
			pending_pages++;
		}
		flush();
		AddRefused(refused_start, refused_end);
	}

	// Locks the backing pages [offset, offset + pages) for the guest pages from `start` on, none of
	// which any guest page holds yet.
	void LockRun(uint64_t start, uint64_t offset, uint64_t pages) {
		const auto result = m_os.Lock(offset, pages * PageSize);
		if (result == ReadWatchLockResult::Locked) {
			for (uint64_t index = 0; index < pages; index++) {
				Hold(start + index * PageSize, offset + index * PageSize);
			}
			m_locked_pages += pages;
			return;
		}
		// VirtualLock may have locked some of them before it failed: those are held like the
		// others, so that they are unlocked once their watch ends. The others are refused.
		m_query.assign(pages, 0);
		m_os.QueryLocked(offset, pages, m_query);
		uint64_t refused_start = 0;
		uint64_t refused_end   = 0;
		for (uint64_t index = 0; index < pages; index++) {
			const auto guest = start + index * PageSize;
			if (m_query[index] != 0) {
				Hold(guest, offset + index * PageSize);
				m_locked_pages++;
				continue;
			}
			m_failed++;
			if (refused_end != guest) {
				AddRefused(refused_start, refused_end);
				refused_start = guest;
			}
			refused_end = guest + PageSize;
		}
		AddRefused(refused_start, refused_end);
		// Windows' limit: no more locks than there are now, so no more calls that fail, until some
		// are unlocked.
		if (result == ReadWatchLockResult::Quota && m_locked_pages < Budget()) {
			m_cap_pages = m_locked_pages;
			ReportCap();
		}
	}

	// Unlock()'s offer of the budget it freed: the refused pages from the lowest address on, as
	// many as the budget and RetryPages allow, as Lock() takes them. Their pages that are no longer
	// backed are dropped.
	template <typename Translate>
	void Retry(Translate& translate) {
		uint64_t left   = RetryPages;
		uint64_t cursor = 0;
		while (left != 0 && m_locked_pages < Budget()) {
			const auto refused = m_refused.First(cursor);
			if (!refused.has_value()) {
				break;
			}
			const auto pages = std::min(left, Budget() - m_locked_pages);
			const auto begin = refused->first;
			const auto end   = std::min(refused->second, begin + pages * PageSize);
			EraseRefused(begin, end);
			left -= (end - begin) / PageSize;
			// Pages refused again go below the cursor: each page is tried once per call.
			cursor = end;
			translate(begin, end - begin, [&](uint64_t offset, uint64_t start, uint64_t bytes) {
				LockPages(offset, start, bytes);
			});
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

	// The refused guest pages, in runs (m_refused).
	void AddRefused(uint64_t begin, uint64_t end) { m_refused.Add(begin, end); }
	void EraseRefused(uint64_t begin, uint64_t end) { m_refused.Erase(begin, end); }

	void Publish() noexcept {
		m_tracked_count.store(m_held.size() + m_refused.Pages(), std::memory_order_relaxed);
		m_stats_budget.store(Budget(), std::memory_order_relaxed);
		m_stats_locked.store(m_locked_pages, std::memory_order_relaxed);
		m_stats_refused.store(m_refused.Pages(), std::memory_order_relaxed);
		m_stats_failed.store(m_failed, std::memory_order_relaxed);
	}

	Os             m_os;
	const uint64_t m_room_step;
	// The guest pages that hold a lock, and their backing page.
	Held m_held;
	// The backing pages locked, and how many guest pages hold each: guest mappings of the same
	// memory share its lock.
	std::unordered_map<uint64_t, uint32_t> m_holders;
	// The guest pages refused a lock, while their watch lasts.
	ReadWatchPageRuns m_refused;
	// The room the working set has for locks, in bytes, and whether Windows refused more.
	uint64_t m_room       = 0;
	bool     m_room_final = false;
	// Windows' limit of locked pages once a lock hit it, and whether a limit was reported.
	uint64_t m_cap_pages    = NoCap;
	bool     m_capped       = false;
	uint64_t m_locked_pages = 0;
	uint64_t m_failed       = 0;
	// Release()'s pending unlock: a run of backing pages.
	uint64_t             m_unlock_offset = 0;
	uint64_t             m_unlock_pages  = 0;
	std::vector<uint8_t> m_query;
	// Read without the caller's mutex.
	std::atomic<uint64_t> m_budget_pages {0};
	std::atomic<uint64_t> m_tracked_count {0};
	std::atomic<uint64_t> m_stats_budget {0};
	std::atomic<uint64_t> m_stats_locked {0};
	std::atomic<uint64_t> m_stats_refused {0};
	std::atomic<uint64_t> m_stats_failed {0};
};

} // namespace Libs::LibKernel::Memory

#endif /* KYTY_KERNEL_READ_WATCH_LOCKS_H_ */
