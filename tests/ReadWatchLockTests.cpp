// gta6-prep: ram-lock
// ReadWatchLocks, the bookkeeping that keeps the guest memory of read watches locked in RAM, with
// the OS calls counted instead of made: the budget, the pages several guest mappings share, a page
// mapped over other memory, and that nothing is ever unlocked that was not locked (VirtualUnlock of
// such a page takes it out of the working set).
#include "kernel/readWatchLocks.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

namespace {

using Libs::LibKernel::Memory::ReadWatchLocks;

constexpr uint64_t Page = Libs::LibKernel::Memory::ReadWatchPageSize;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ReadWatchLockTests: failed: %s\n", message);
		std::abort();
	}
}

struct Call {
	uint64_t offset                        = 0;
	uint64_t size                          = 0;
	bool     operator==(const Call&) const = default;
};

// The backing view's pages as the tests' Windows sees them. A page VirtualLock refuses fails its
// call; with `partial`, the pages before it in the call stay locked, as a VirtualLock that gave up
// half way might leave them.
struct FakeOs {
	std::set<uint64_t> locked;
	std::set<uint64_t> refused;
	bool               partial        = false;
	bool               prepare_result = true;
	uint64_t           prepares       = 0;
	uint64_t           prepared       = 0;
	uint64_t           queries        = 0;
	std::vector<Call>  locks;
	std::vector<Call>  unlocks;

	bool Prepare(uint64_t budget) {
		prepares++;
		prepared = budget;
		return prepare_result;
	}

	bool Lock(uint64_t offset, uint64_t size) {
		Check(offset % Page == 0 && size % Page == 0 && size != 0, "a lock of partial pages");
		locks.push_back({offset, size});
		std::vector<uint64_t> added;
		for (auto page = offset; page < offset + size; page += Page) {
			if (refused.contains(page)) {
				if (!partial) {
					for (const auto undo: added) {
						locked.erase(undo);
					}
				}
				return false;
			}
			if (locked.insert(page).second) {
				added.push_back(page);
			}
		}
		return true;
	}

	void Unlock(uint64_t offset, uint64_t size) {
		Check(offset % Page == 0 && size % Page == 0 && size != 0, "an unlock of partial pages");
		unlocks.push_back({offset, size});
		for (auto page = offset; page < offset + size; page += Page) {
			Check(locked.erase(page) == 1, "unlocked a page that is not locked");
		}
	}

	void QueryLocked(uint64_t offset, uint64_t pages, std::vector<uint8_t>& out) {
		queries++;
		Check(out.size() == pages, "the query's answer has the wrong size");
		for (uint64_t index = 0; index < pages; index++) {
			out[index] = locked.contains(offset + index * Page) ? 1 : 0;
		}
	}

	void Forget() {
		locks.clear();
		unlocks.clear();
	}
};

using Locks = ReadWatchLocks<FakeOs>;

// The guest addresses and backing offsets the cases use.
constexpr uint64_t GuestA  = 0x0000000200000000ull;
constexpr uint64_t GuestB  = 0x0000000300000000ull;
constexpr uint64_t Backing = 0x10000;

// Without a budget nothing happens: no OS call, no count.
void TestOff() {
	Locks locks;
	Check(!locks.Enabled() && !locks.HasLocks(), "the locks start on");
	locks.Lock(Backing, GuestA, Page * 4);
	locks.Unlock(GuestA, Page * 4);
	const auto& os    = locks.GetOs();
	const auto  stats = locks.Stats();
	Check(os.prepares == 0 && os.locks.empty() && os.unlocks.empty(),
	      "the locks called the OS without a budget");
	Check(stats.budget_bytes == 0 && stats.locked_bytes == 0 && stats.over_budget_pages == 0 &&
	          stats.failed_pages == 0 && locks.HeldPages() == 0,
	      "the locks counted something without a budget");
}

// A watch locks its run once, the working set made room for the budget first; its end unlocks the
// same pages, once.
void TestLockAndUnlock() {
	Locks locks;
	locks.SetBudget(Page * 16);
	Check(locks.Enabled() && locks.Stats().budget_bytes == Page * 16, "the budget is not kept");
	locks.Lock(Backing, GuestA, Page * 4);
	auto& os = locks.GetOs();
	Check(os.prepares == 1 && os.prepared == Page * 16, "the working set was not prepared once");
	Check(os.locks == std::vector<Call> {{Backing, Page * 4}},
	      "the run was not locked in one call");
	Check(locks.HasLocks() && locks.HeldPages() == 4 && locks.Stats().locked_bytes == Page * 4,
	      "the locked pages are not counted");
	// The same run protected again (a run through pages denied already): nothing new.
	os.Forget();
	locks.Lock(Backing, GuestA, Page * 4);
	Check(os.locks.empty() && os.prepares == 1, "pages locked already were locked again");
	locks.Unlock(GuestA, Page * 4);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 4}},
	      "the run was not unlocked in one call");
	Check(os.locked.empty() && !locks.HasLocks() && locks.Stats().locked_bytes == 0,
	      "pages stayed locked after their watch ended");
	// A second end, and an end where nothing was locked: no call.
	os.Forget();
	locks.Unlock(GuestA, Page * 4);
	locks.Unlock(GuestB, Page * 64);
	Check(os.unlocks.empty(), "unlocked pages that hold no lock");
}

// Past the budget pages stay unlocked and are counted; they are never unlocked.
void TestBudget() {
	Locks locks;
	locks.SetBudget(Page * 2);
	locks.Lock(Backing, GuestA, Page * 5);
	auto& os = locks.GetOs();
	Check(os.locks == std::vector<Call> {{Backing, Page * 2}},
	      "the budget's pages were not locked");
	auto stats = locks.Stats();
	Check(stats.locked_bytes == Page * 2 && stats.over_budget_pages == 3,
	      "the pages past the budget are not counted");
	// A run protected again through them asks again, and counts again: a count of refusals.
	locks.Lock(Backing + Page * 2, GuestA + Page * 2, Page * 3);
	Check(locks.Stats().over_budget_pages == 6, "the refusals are not counted");
	locks.Unlock(GuestA, Page * 5);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locked.empty(),
	      "the unlock touched pages past the budget");
	// Room again: the next watch is locked.
	os.Forget();
	locks.Lock(Backing + Page * 8, GuestB, Page);
	Check(os.locks == std::vector<Call> {{Backing + Page * 8, Page}} &&
	          locks.Stats().over_budget_pages == 6,
	      "the budget did not come back after the unlock");
	locks.Unlock(GuestB, Page);
	// A smaller budget keeps what is locked until its watch ends.
	locks.Lock(Backing, GuestA, Page * 2);
	locks.SetBudget(Page);
	locks.Lock(Backing + Page * 8, GuestB, Page);
	stats = locks.Stats();
	Check(stats.locked_bytes == Page * 2 && stats.over_budget_pages == 7,
	      "a smaller budget unlocked pages or locked more");
	locks.SetBudget(0);
	Check(!locks.Enabled() && locks.HasLocks(), "turning the budget off dropped the locks");
	os.Forget();
	locks.Unlock(GuestA, Page * 2);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locked.empty(),
	      "the locks taken before the budget went off were not unlocked");
}

// Two guest mappings of the same memory share its lock: it is locked once, counts once against the
// budget, and is unlocked when the last of them ends its watch.
void TestAliases() {
	Locks locks;
	locks.SetBudget(Page * 4);
	locks.Lock(Backing, GuestA, Page * 4);
	locks.Lock(Backing, GuestB, Page * 4);
	auto& os = locks.GetOs();
	Check(os.locks.size() == 1 && locks.HeldPages() == 8 &&
	          locks.Stats().locked_bytes == Page * 4 && locks.Stats().over_budget_pages == 0,
	      "an alias was locked again or counted against the budget");
	locks.Unlock(GuestA, Page * 4);
	Check(os.unlocks.empty() && os.locked.size() == 4, "an alias's end unlocked shared pages");
	// GuestB is the last holder now: its ends unlock, in runs of contiguous pages.
	locks.Unlock(GuestB + Page, Page);
	Check(os.unlocks == std::vector<Call> {{Backing + Page, Page}},
	      "the last holder's end did not unlock its page");
	os.Forget();
	locks.Unlock(GuestB, Page * 4);
	Check(os.unlocks == (std::vector<Call> {{Backing, Page}, {Backing + Page * 2, Page * 2}}) &&
	          os.locked.empty() && !locks.HasLocks(),
	      "the last holder's end did not unlock the shared pages, in runs");
	// A run that crosses pages an alias holds: the others are locked around them.
	os.Forget();
	locks.Lock(Backing + Page, GuestB, Page);
	locks.Lock(Backing, GuestA, Page * 3);
	Check(os.locks == (std::vector<Call> {
	                      {Backing + Page, Page}, {Backing, Page}, {Backing + Page * 2, Page}}),
	      "a run was not split around a page an alias holds");
	locks.Unlock(GuestA, Page * 3);
	locks.Unlock(GuestB, Page);
	Check(os.locked.empty(), "pages stayed locked");
}

// A guest page mapped over other memory while it held a lock: the old lock goes, the new memory is
// locked, and its end unlocks the new memory only.
void TestRemap() {
	Locks locks;
	locks.SetBudget(Page * 4);
	locks.Lock(Backing, GuestA, Page);
	locks.Lock(Backing + Page * 8, GuestA, Page);
	auto& os = locks.GetOs();
	Check(os.unlocks == std::vector<Call> {{Backing, Page}} &&
	          os.locks.back() == Call {Backing + Page * 8, Page} &&
	          locks.Stats().locked_bytes == Page,
	      "a page mapped over other memory kept its old lock");
	os.Forget();
	locks.Unlock(GuestA, Page);
	Check(os.unlocks == std::vector<Call> {{Backing + Page * 8, Page}} && os.locked.empty(),
	      "the end of the watch did not unlock the new memory");
}

// A preparation that fails turns the locks off: nothing is locked, nothing counted.
void TestPrepareFails() {
	Locks locks;
	locks.GetOs().prepare_result = false;
	locks.SetBudget(Page * 4);
	locks.Lock(Backing, GuestA, Page * 2);
	locks.Lock(Backing, GuestB, Page * 2);
	const auto& os    = locks.GetOs();
	const auto  stats = locks.Stats();
	Check(os.prepares == 1 && os.locks.empty() && !locks.Enabled() && stats.budget_bytes == 0 &&
	          stats.over_budget_pages == 0 && stats.failed_pages == 0,
	      "a failed preparation left the locks on");
}

// A VirtualLock that fails: the pages it left unlocked are counted, those it locked anyway are held
// like the others, and only those are unlocked.
void TestLockFails() {
	Locks locks;
	locks.SetBudget(Page * 8);
	auto& os = locks.GetOs();
	os.refused.insert(Backing + Page * 2);
	locks.Lock(Backing, GuestA, Page * 4);
	Check(os.queries == 1 && os.locked.empty() && locks.Stats().failed_pages == 4 &&
	          locks.Stats().locked_bytes == 0 && !locks.HasLocks(),
	      "a failed lock was held or not counted");
	locks.Unlock(GuestA, Page * 4);
	Check(os.unlocks.empty(), "pages a failed lock left unlocked were unlocked");
	// Half way: the first two pages stay locked.
	os.partial = true;
	locks.Lock(Backing, GuestA, Page * 4);
	Check(os.locked.size() == 2 && locks.HeldPages() == 2 && locks.Stats().failed_pages == 6 &&
	          locks.Stats().locked_bytes == Page * 2,
	      "the pages a failed lock locked are not held");
	locks.Unlock(GuestA, Page * 4);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locked.empty(),
	      "the pages a failed lock locked were not unlocked, or more were");
}

// The PageManager's runs are whole 4 KiB pages; a range that is not still covers the pages it
// touches.
void TestPartialPages() {
	Locks locks;
	locks.SetBudget(Page * 4);
	locks.Lock(Backing + 0x800, GuestA + 0x800, Page);
	auto& os = locks.GetOs();
	Check(os.locks == std::vector<Call> {{Backing, Page * 2}}, "the touched pages were not locked");
	locks.Unlock(GuestA + Page + 0x10, 0x10);
	Check(os.unlocks == std::vector<Call> {{Backing + Page, Page}},
	      "the touched page was not unlocked");
	locks.Unlock(GuestA, 1);
	Check(os.locked.empty(), "a page stayed locked");
}

} // namespace

int main() {
	TestOff();
	TestLockAndUnlock();
	TestBudget();
	TestAliases();
	TestRemap();
	TestPrepareFails();
	TestLockFails();
	TestPartialPages();
	std::printf("ReadWatchLockTests: ok\n");
	return 0;
}
