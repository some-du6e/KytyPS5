// gta6-prep: ram-lock
// ReadWatchLocks, the bookkeeping that keeps the guest memory of read watches locked in RAM, with
// the OS calls counted instead of made: the budget and the room the working set makes for it, the
// pages refused a lock and the budget an unlock offers them, the pages several guest mappings
// share, a page mapped over other memory, Windows' limit, and that nothing is ever unlocked that
// was not locked (VirtualUnlock of such a page takes it out of the working set).
#include "kernel/readWatchLocks.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using Libs::LibKernel::Memory::ReadWatchLockResult;
using Libs::LibKernel::Memory::ReadWatchLocks;

constexpr uint64_t Page = Libs::LibKernel::Memory::ReadWatchPageSize;
constexpr uint64_t MiB  = uint64_t {1} << 20u;

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
// half way might leave them. Past `quota` locked pages, a call fails as Windows' limit does.
struct FakeOs {
	std::set<uint64_t>                                    locked;
	std::set<uint64_t>                                    refused;
	bool                                                  partial = false;
	std::optional<uint64_t>                               quota;
	std::optional<uint64_t>                               prepare_limit;
	std::vector<uint64_t>                                 prepares;
	std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> prepared;
	std::vector<std::pair<uint64_t, uint64_t>>            capped;
	uint64_t                                              queries = 0;
	std::vector<Call>                                     locks;
	std::vector<Call>                                     unlocks;

	bool Prepare(uint64_t bytes) {
		prepares.push_back(bytes);
		return !prepare_limit.has_value() || bytes <= *prepare_limit;
	}

	void Prepared(uint64_t budget, uint64_t refused, uint64_t room) {
		prepared.emplace_back(budget, refused, room);
	}

	ReadWatchLockResult Lock(uint64_t offset, uint64_t size) {
		Check(offset % Page == 0 && size % Page == 0 && size != 0, "a lock of partial pages");
		locks.push_back({offset, size});
		std::vector<uint64_t> added;
		for (auto page = offset; page < offset + size; page += Page) {
			const bool over_quota =
			    quota.has_value() && !locked.contains(page) && locked.size() >= *quota;
			if (refused.contains(page) || over_quota) {
				if (!partial) {
					for (const auto undo: added) {
						locked.erase(undo);
					}
				}
				return over_quota ? ReadWatchLockResult::Quota : ReadWatchLockResult::Failed;
			}
			if (locked.insert(page).second) {
				added.push_back(page);
			}
		}
		return ReadWatchLockResult::Locked;
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

	void Capped(uint64_t locked_bytes, uint64_t budget) {
		capped.emplace_back(locked_bytes, budget);
	}

	void Forget() {
		locks.clear();
		unlocks.clear();
	}
};

using Locks    = ReadWatchLocks<FakeOs>;
using Prepared = std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>;

// The guest addresses and backing offsets the cases use.
constexpr uint64_t GuestA  = 0x0000000200000000ull;
constexpr uint64_t GuestB  = 0x0000000300000000ull;
constexpr uint64_t Backing = 0x10000;

// The guest address space's translation, as GuestBackingStore::ForEachBackedRun() walks it: the
// backing of each mapped guest page; runs of contiguous backing, unbacked pages skipped.
struct Translate {
	std::map<uint64_t, uint64_t> backing;

	void Map(uint64_t guest, uint64_t offset, uint64_t pages) {
		for (uint64_t index = 0; index < pages; index++) {
			backing[guest + index * Page] = offset + index * Page;
		}
	}

	template <typename F>
	void operator()(uint64_t start, uint64_t bytes, F&& f) const {
		const auto end = start + bytes;
		for (auto page = start; page < end;) {
			const auto found = backing.find(page);
			if (found == backing.end()) {
				page += Page;
				continue;
			}
			auto stop = page + Page;
			while (stop < end && backing.contains(stop) &&
			       backing.at(stop) == found->second + (stop - page)) {
				stop += Page;
			}
			f(found->second, page, stop - page);
			page = stop;
		}
	}

	// Lock() of each backed run of a range, as ProtectTransient() calls it.
	void Lock(Locks& locks, uint64_t start, uint64_t bytes) const {
		(*this)(start, bytes, [&](uint64_t offset, uint64_t run, uint64_t size) {
			locks.Lock(offset, run, size);
		});
	}
};

// Without a budget nothing happens: no OS call, no count.
void TestOff() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 4);
	Check(!locks.Enabled() && !locks.HasWork() && !locks.HasRoom(), "the locks start on");
	translate.Lock(locks, GuestA, Page * 4);
	locks.Unlock(GuestA, Page * 4, translate);
	const auto& os    = locks.GetOs();
	const auto  stats = locks.Stats();
	Check(os.prepares.empty() && os.prepared.empty() && os.locks.empty() && os.unlocks.empty(),
	      "the locks called the OS without a budget");
	Check(stats.budget_bytes == 0 && stats.locked_bytes == 0 && stats.unlocked_bytes == 0 &&
	          stats.failed_pages == 0 && locks.HeldPages() == 0 && locks.RefusedPages() == 0,
	      "the locks counted something without a budget");
}

// The budget makes room in the working set once; a watch locks its run in one call, and its end
// unlocks the same pages, once.
void TestLockAndUnlock() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 4);
	auto& os = locks.GetOs();
	Check(locks.SetBudget(Page * 16) == Page * 16, "the budget is not kept");
	Check(os.prepares == std::vector<uint64_t> {Page * 16} &&
	          os.prepared == (Prepared {{Page * 16, 0, Page * 16}}),
	      "the working set was not prepared once, for the budget");
	Check(locks.Enabled() && locks.HasRoom() && locks.Stats().budget_bytes == Page * 16,
	      "the budget is not reported");
	translate.Lock(locks, GuestA, Page * 4);
	Check(os.locks == std::vector<Call> {{Backing, Page * 4}},
	      "the run was not locked in one call");
	Check(locks.HasWork() && locks.HeldPages() == 4 && locks.Stats().locked_bytes == Page * 4,
	      "the locked pages are not counted");
	// The same run protected again (a run through pages denied already): nothing new.
	os.Forget();
	translate.Lock(locks, GuestA, Page * 4);
	Check(os.locks.empty() && os.prepares.size() == 1, "pages locked already were locked again");
	locks.Unlock(GuestA, Page * 4, translate);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 4}},
	      "the run was not unlocked in one call");
	Check(os.locked.empty() && !locks.HasWork() && locks.Stats().locked_bytes == 0,
	      "pages stayed locked after their watch ended");
	// A second end, and an end where nothing was locked: no call.
	os.Forget();
	locks.Unlock(GuestA, Page * 4, translate);
	locks.Unlock(GuestB, Page * 64, translate);
	Check(os.unlocks.empty() && os.locks.empty(), "unlocked pages that hold no lock");
	// A smaller budget needs no more room; a larger one asks for it.
	locks.SetBudget(Page * 8);
	Check(os.prepares.size() == 1, "a smaller budget prepared the working set again");
	locks.SetBudget(Page * 32);
	Check(os.prepares.size() == 2 && os.prepares.back() == Page * 32,
	      "a larger budget did not prepare the working set for it");
}

// Past the budget pages stay unlocked and are refused: counted once while their watch lasts, never
// unlocked.
void TestBudget() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 5);
	translate.Map(GuestB, Backing + Page * 8, 1);
	locks.SetBudget(Page * 2);
	translate.Lock(locks, GuestA, Page * 5);
	auto& os = locks.GetOs();
	Check(os.locks == std::vector<Call> {{Backing, Page * 2}},
	      "the budget's pages were not locked");
	auto stats = locks.Stats();
	Check(stats.locked_bytes == Page * 2 && stats.unlocked_bytes == Page * 3 &&
	          locks.RefusedPages() == 3,
	      "the pages past the budget are not counted");
	// A run protected again through them asks again, makes no call, and counts them once.
	os.Forget();
	translate.Lock(locks, GuestA + Page * 2, Page * 3);
	Check(os.locks.empty() && locks.Stats().unlocked_bytes == Page * 3,
	      "the refused pages were counted twice or locked past the budget");
	locks.Unlock(GuestA, Page * 5, translate);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locked.empty() &&
	          locks.Stats().unlocked_bytes == 0 && !locks.HasWork(),
	      "the unlock touched pages past the budget, or kept them refused");
	// Room again: the next watch is locked.
	os.Forget();
	translate.Lock(locks, GuestB, Page);
	Check(os.locks == std::vector<Call> {{Backing + Page * 8, Page}},
	      "the budget did not come back after the unlock");
	locks.Unlock(GuestB, Page, translate);
	// A smaller budget keeps what is locked until its watch ends.
	translate.Lock(locks, GuestA, Page * 2);
	locks.SetBudget(Page);
	translate.Lock(locks, GuestB, Page);
	stats = locks.Stats();
	Check(stats.locked_bytes == Page * 2 && stats.unlocked_bytes == Page &&
	          stats.budget_bytes == Page,
	      "a smaller budget unlocked pages or locked more");
	locks.Unlock(GuestB, Page, translate);
	locks.SetBudget(0);
	Check(!locks.Enabled() && locks.HasWork(), "turning the budget off dropped the locks");
	os.Forget();
	locks.Unlock(GuestA, Page * 2, translate);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locked.empty(),
	      "the locks taken before the budget went off were not unlocked");
}

// The budget an unlock frees goes to the pages refused earlier, lowest first, RetryPages of them at
// most per unlock; refused pages that are no longer backed are dropped.
void TestRetry() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 2);
	translate.Map(GuestB, Backing + Page * 8, 3);
	locks.SetBudget(Page * 2);
	translate.Lock(locks, GuestA, Page * 2);
	translate.Lock(locks, GuestB, Page * 3);
	auto& os = locks.GetOs();
	Check(locks.RefusedPages() == 3, "the second watch was not refused");
	os.Forget();
	locks.Unlock(GuestA, Page * 2, translate);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} &&
	          os.locks == std::vector<Call> {{Backing + Page * 8, Page * 2}},
	      "the freed budget did not go to the refused pages, in one call");
	Check(locks.Stats().locked_bytes == Page * 2 && locks.Stats().unlocked_bytes == Page,
	      "the pages the retry locked are not counted");
	// Their end unlocks them; the last refused page needs nothing.
	os.Forget();
	locks.Unlock(GuestB, Page * 3, translate);
	Check(os.unlocks == std::vector<Call> {{Backing + Page * 8, Page * 2}} && os.locked.empty() &&
	          !locks.HasWork(),
	      "the pages the retry locked were not unlocked at their end");

	// A refused page that lost its backing is dropped, not locked. (It takes its place in the
	// first run the retry takes, so the other two come one by one.)
	translate.Lock(locks, GuestA, Page * 2);
	translate.Lock(locks, GuestB, Page * 3);
	translate.backing.erase(GuestB);
	os.Forget();
	locks.Unlock(GuestA, Page * 2, translate);
	Check(os.locks ==
	              (std::vector<Call> {{Backing + Page * 9, Page}, {Backing + Page * 10, Page}}) &&
	          locks.RefusedPages() == 0 && locks.HeldPages() == 2,
	      "an unbacked refused page was locked or kept");
	locks.Unlock(GuestB, Page * 3, translate);
	Check(os.locked.empty() && !locks.HasWork(), "pages stayed locked");

	// RetryPages at most per unlock.
	constexpr auto Many = Locks::RetryPages + 100;
	translate.Map(GuestA, Backing, Many);
	translate.Map(GuestB, Backing + Page * 1024, Many);
	locks.SetBudget(Page * Many);
	translate.Lock(locks, GuestA, Page * Many);
	translate.Lock(locks, GuestB, Page * Many);
	Check(locks.RefusedPages() == Many, "the second large watch was not refused");
	os.Forget();
	locks.Unlock(GuestA, Page * Many, translate);
	Check(locks.HeldPages() == Locks::RetryPages && locks.RefusedPages() == 100 &&
	          os.locks == std::vector<Call> {{Backing + Page * 1024, Page * Locks::RetryPages}},
	      "an unlock retried more than RetryPages pages");
	// The next unlock that frees budget offers it to the rest.
	os.Forget();
	locks.Unlock(GuestB, Page, translate);
	Check(locks.RefusedPages() == 0 && locks.HeldPages() == Locks::RetryPages - 1 + 100 &&
	          os.locks ==
	              std::vector<Call> {{Backing + Page * (1024 + Locks::RetryPages), Page * 100}},
	      "the next unlock did not retry the rest");
	locks.Unlock(GuestB, Page * Many, translate);
	Check(os.locked.empty() && !locks.HasWork(), "pages stayed locked");
}

// gta6-prep: read-guard: with guard pages the refused pages are guarded, which needs no lock: an
// unlock with `retry` off offers them nothing, and the budget it frees goes to the next watch.
void TestNoRetry() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 2);
	translate.Map(GuestB, Backing + Page * 8, 3);
	locks.SetBudget(Page * 2);
	translate.Lock(locks, GuestA, Page * 2);
	translate.Lock(locks, GuestB, Page * 3);
	auto& os = locks.GetOs();
	Check(locks.RefusedPages() == 3, "the second watch was not refused");
	os.Forget();
	locks.Unlock(GuestA, Page * 2, translate, false);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locks.empty(),
	      "an unlock without retry locked the refused pages, or did not unlock its own");
	Check(locks.HeldPages() == 0 && locks.RefusedPages() == 3 && locks.Stats().locked_bytes == 0 &&
	          locks.Stats().unlocked_bytes == Page * 3,
	      "an unlock without retry changed the refused pages");
	// The next watch gets the budget.
	os.Forget();
	translate.Lock(locks, GuestA, Page * 2);
	Check(os.locks == std::vector<Call> {{Backing, Page * 2}} && locks.HeldPages() == 2,
	      "the budget an unlock without retry freed did not go to the next watch");
	// The refused pages' end has nothing to unlock.
	locks.Unlock(GuestB, Page * 3, translate, false);
	Check(os.unlocks.empty() && locks.RefusedPages() == 0 && locks.HeldPages() == 2,
	      "the refused pages' end unlocked something, or kept them refused");
	locks.Unlock(GuestA, Page * 2, translate, false);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locked.empty() &&
	          !locks.HasWork(),
	      "pages stayed locked");
}

// Two guest mappings of the same memory share its lock: it is locked once, counts once against the
// budget, and is unlocked when the last of them ends its watch.
void TestAliases() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 4);
	translate.Map(GuestB, Backing, 4);
	locks.SetBudget(Page * 4);
	translate.Lock(locks, GuestA, Page * 4);
	translate.Lock(locks, GuestB, Page * 4);
	auto& os = locks.GetOs();
	Check(os.locks.size() == 1 && locks.HeldPages() == 8 &&
	          locks.Stats().locked_bytes == Page * 4 && locks.Stats().unlocked_bytes == 0,
	      "an alias was locked again or counted against the budget");
	locks.Unlock(GuestA, Page * 4, translate);
	Check(os.unlocks.empty() && os.locked.size() == 4, "an alias's end unlocked shared pages");
	// GuestB is the last holder now: its ends unlock, in runs of contiguous pages.
	locks.Unlock(GuestB + Page, Page, translate);
	Check(os.unlocks == std::vector<Call> {{Backing + Page, Page}},
	      "the last holder's end did not unlock its page");
	os.Forget();
	locks.Unlock(GuestB, Page * 4, translate);
	Check(os.unlocks == (std::vector<Call> {{Backing, Page}, {Backing + Page * 2, Page * 2}}) &&
	          os.locked.empty() && !locks.HasWork(),
	      "the last holder's end did not unlock the shared pages, in runs");
	// A run that crosses pages an alias holds: the others are locked around them.
	os.Forget();
	translate.Lock(locks, GuestB + Page, Page);
	translate.Lock(locks, GuestA, Page * 3);
	Check(os.locks == (std::vector<Call> {
	                      {Backing + Page, Page}, {Backing, Page}, {Backing + Page * 2, Page}}),
	      "a run was not split around a page an alias holds");
	locks.Unlock(GuestA, Page * 3, translate);
	locks.Unlock(GuestB + Page, Page, translate);
	Check(os.locked.empty() && !locks.HasWork(), "pages stayed locked");
	// With the budget full, an alias of a locked page still shares its lock.
	locks.SetBudget(Page);
	translate.Lock(locks, GuestA, Page);
	translate.Lock(locks, GuestB, Page * 2);
	Check(locks.HeldPages() == 2 && locks.RefusedPages() == 1,
	      "a full budget refused an alias of a locked page");
	locks.Unlock(GuestA, Page, translate);
	locks.Unlock(GuestB, Page * 2, translate);
	Check(os.locked.empty() && !locks.HasWork(), "pages stayed locked");
}

// A guest page mapped over other memory while it held a lock: the old lock goes, the new memory is
// locked, and its end unlocks the new memory only.
void TestRemap() {
	Locks     locks;
	Translate translate;
	locks.SetBudget(Page * 4);
	locks.Lock(Backing, GuestA, Page);
	locks.Lock(Backing + Page * 8, GuestA, Page);
	auto& os = locks.GetOs();
	Check(os.unlocks == std::vector<Call> {{Backing, Page}} &&
	          os.locks.back() == Call {Backing + Page * 8, Page} &&
	          locks.Stats().locked_bytes == Page,
	      "a page mapped over other memory kept its old lock");
	os.Forget();
	locks.Unlock(GuestA, Page, translate);
	Check(os.unlocks == std::vector<Call> {{Backing + Page * 8, Page}} && os.locked.empty(),
	      "the end of the watch did not unlock the new memory");
}

// The first room the working set makes: halved while Windows refuses it, down to 1 MiB; the
// budget is what it got, and nothing is locked when it got none.
void TestPrepare() {
	{
		Locks locks;
		auto& os         = locks.GetOs();
		os.prepare_limit = MiB * 3 / 2;
		Check(locks.SetBudget(MiB * 4) == MiB,
		      "the budget is not what the working set made room for");
		Check(os.prepares == (std::vector<uint64_t> {MiB * 4, MiB * 2, MiB}) &&
		          os.prepared == (Prepared {{MiB, MiB * 4, MiB}}),
		      "the asks were not halved, or not reported once");
		Check(locks.SetBudget(MiB / 2) == MiB / 2 && os.prepares.size() == 3,
		      "a budget within the room asked for more");
		Check(locks.SetBudget(MiB * 4) == MiB && os.prepares.size() == 5 &&
		          os.prepared.back() ==
		              std::tuple<uint64_t, uint64_t, uint64_t> {MiB, MiB * 4, MiB},
		      "a refused raise lost the room made before");
	}
	{
		Locks     locks;
		Translate translate;
		translate.Map(GuestA, Backing, 2);
		auto& os         = locks.GetOs();
		os.prepare_limit = 0;
		Check(locks.SetBudget(MiB * 4) == 0 && !locks.Enabled(),
		      "a refused room left the locks on");
		Check(os.prepares == (std::vector<uint64_t> {MiB * 4, MiB * 2, MiB}) &&
		          os.prepared == (Prepared {{MiB * 4, MiB * 4, 0}}),
		      "a refused room was not reported once");
		translate.Lock(locks, GuestA, Page * 2);
		Check(os.locks.empty() && locks.Stats().unlocked_bytes == 0 && !locks.HasWork(),
		      "pages were locked or counted without room");
		// A budget under 2 MiB is asked for once.
		os.prepares.clear();
		locks.SetBudget(Page * 4);
		Check(os.prepares == std::vector<uint64_t> {Page * 4}, "a small budget was halved");
	}
}

// The room grows with the locks: SetBudget() makes room for one step, and a lock that needs more
// grows it a step at a time, up to the budget. When Windows refuses a step, the room is the budget
// from then on, reported once, and the pages past it are refused without a call.
void TestRoom() {
	{
		Locks     locks(FakeOs {}, Page * 2);
		Translate translate;
		translate.Map(GuestA, Backing, 5);
		auto& os = locks.GetOs();
		Check(locks.SetBudget(Page * 5) == Page * 5 &&
		          os.prepares == std::vector<uint64_t> {Page * 2},
		      "the first room is not one step");
		translate.Lock(locks, GuestA, Page * 5);
		Check(os.prepares == (std::vector<uint64_t> {Page * 2, Page * 4, Page * 5}),
		      "the room did not grow a step at a time, up to the budget");
		Check(os.locks == (std::vector<Call> {{Backing, Page * 2},
		                                      {Backing + Page * 2, Page * 2},
		                                      {Backing + Page * 4, Page}}),
		      "a run was not locked within the room");
		Check(locks.HeldPages() == 5 && locks.RefusedPages() == 0 && os.capped.empty(),
		      "the pages are not locked");
		locks.Unlock(GuestA, Page * 5, translate);
		Check(os.locked.empty() && !locks.HasWork(), "pages stayed locked");
	}
	{
		Locks     locks(FakeOs {}, Page * 2);
		Translate translate;
		translate.Map(GuestA, Backing, 5);
		auto& os         = locks.GetOs();
		os.prepare_limit = Page * 3;
		locks.SetBudget(Page * 5);
		translate.Lock(locks, GuestA, Page * 5);
		Check(os.prepares == (std::vector<uint64_t> {Page * 2, Page * 4}) &&
		          os.locks == std::vector<Call> {{Backing, Page * 2}},
		      "a refused room was asked again, or pages were locked past it");
		Check(locks.HeldPages() == 2 && locks.RefusedPages() == 3 &&
		          locks.Stats().budget_bytes == Page * 2 &&
		          os.capped == (std::vector<std::pair<uint64_t, uint64_t>> {{Page * 2, Page * 5}}),
		      "the room Windows refused to grow is not the budget, or not reported");
		// An unlock offers the room to the refused pages, without asking for more.
		os.Forget();
		locks.Unlock(GuestA, Page, translate);
		Check(os.locks == std::vector<Call> {{Backing + Page * 2, Page}} &&
		          os.prepares.size() == 2 && os.capped.size() == 1,
		      "the refused pages did not get the room an unlock freed");
		locks.Unlock(GuestA, Page * 5, translate);
		Check(os.locked.empty() && !locks.HasWork(), "pages stayed locked");
	}
}

// A VirtualLock that fails: the pages it left unlocked are refused and counted, those it locked
// anyway are held like the others, and only those are unlocked.
void TestLockFails() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 4);
	locks.SetBudget(Page * 8);
	auto& os = locks.GetOs();
	os.refused.insert(Backing + Page * 2);
	translate.Lock(locks, GuestA, Page * 4);
	Check(os.queries == 1 && os.locked.empty() && locks.Stats().failed_pages == 4 &&
	          locks.Stats().locked_bytes == 0 && locks.Stats().unlocked_bytes == Page * 4 &&
	          locks.HeldPages() == 0,
	      "a failed lock was held or not counted");
	Check(os.capped.empty() && locks.Stats().budget_bytes == Page * 8,
	      "a failure that is not Windows' limit lowered the budget");
	locks.Unlock(GuestA, Page * 4, translate);
	Check(os.unlocks.empty() && !locks.HasWork(),
	      "pages a failed lock left unlocked were unlocked, or stayed refused");
	// Half way: the first two pages stay locked.
	os.partial = true;
	translate.Lock(locks, GuestA, Page * 4);
	Check(os.locked.size() == 2 && locks.HeldPages() == 2 && locks.Stats().failed_pages == 6 &&
	          locks.Stats().locked_bytes == Page * 2 && locks.RefusedPages() == 2,
	      "the pages a failed lock locked are not held");
	locks.Unlock(GuestA, Page * 4, translate);
	Check(os.unlocks == std::vector<Call> {{Backing, Page * 2}} && os.locked.empty(),
	      "the pages a failed lock locked were not unlocked, or more were");
}

// Windows' limit (ERROR_WORKING_SET_QUOTA): the budget drops to what is locked, reported once, and
// the pages past it make no more calls.
void TestQuota() {
	Locks     locks;
	Translate translate;
	translate.Map(GuestA, Backing, 5);
	translate.Map(GuestB, Backing + Page * 8, 2);
	locks.SetBudget(Page * 8);
	auto& os   = locks.GetOs();
	os.partial = true;
	os.quota   = 3;
	translate.Lock(locks, GuestA, Page * 5);
	Check(locks.HeldPages() == 3 && locks.RefusedPages() == 2 && locks.Stats().failed_pages == 2,
	      "the pages under Windows' limit are not held, or those past it not refused");
	Check(os.capped == (std::vector<std::pair<uint64_t, uint64_t>> {{Page * 3, Page * 8}}) &&
	          locks.Stats().budget_bytes == Page * 3,
	      "Windows' limit did not lower the budget, or was not reported");
	os.Forget();
	translate.Lock(locks, GuestB, Page * 2);
	Check(os.locks.empty() && locks.RefusedPages() == 4 && locks.Stats().failed_pages == 2,
	      "a lock past Windows' limit called the OS");
	// An unlock frees budget under the limit: the refused pages get it, and the limit is reported
	// no more.
	locks.Unlock(GuestA, Page * 5, translate);
	Check(locks.HeldPages() == 2 && locks.RefusedPages() == 0 && os.locked.size() == 2 &&
	          os.capped.size() == 1,
	      "the budget under Windows' limit did not go to the refused pages");
	locks.Unlock(GuestB, Page * 2, translate);
	Check(os.locked.empty() && !locks.HasWork(), "pages stayed locked");
}

// gta6-prep: read-guard: the run set of the refused pages and of the guard pages: runs that touch
// merge, an erase splits a run, the page count follows, and the queries cut runs to their range.
void TestPageRuns() {
	using Libs::LibKernel::Memory::ReadWatchPageRuns;
	using Runs = std::vector<std::pair<uint64_t, uint64_t>>;
	ReadWatchPageRuns runs;

	const auto within = [&](uint64_t begin, uint64_t end) {
		Runs found;
		runs.ForEach(begin, end, [&](uint64_t from, uint64_t to) { found.emplace_back(from, to); });
		return found;
	};
	const auto all = [&] { return within(0, ~uint64_t {0}); };
	Check(runs.Empty() && runs.Pages() == 0 && !runs.First(0).has_value(),
	      "the set does not start empty");
	runs.Add(GuestA, GuestA + Page * 2);
	runs.Add(GuestA + Page * 4, GuestA + Page * 5);
	runs.Add(GuestA + Page * 2, GuestA + Page * 3);
	Check(all() == (Runs {{GuestA, GuestA + Page * 3}, {GuestA + Page * 4, GuestA + Page * 5}}) &&
	          runs.Pages() == 4,
	      "touching runs did not merge, or the pages are miscounted");
	// Adding pages already in the set counts them once; an empty range adds nothing.
	runs.Add(GuestA + Page, GuestA + Page * 5);
	runs.Add(GuestA + Page * 9, GuestA + Page * 9);
	Check(all() == (Runs {{GuestA, GuestA + Page * 5}}) && runs.Pages() == 5,
	      "an overlapping add counted pages twice, or an empty one added a run");
	runs.Erase(GuestA + Page, GuestA + Page * 2);
	Check(all() == (Runs {{GuestA, GuestA + Page}, {GuestA + Page * 2, GuestA + Page * 5}}) &&
	          runs.Pages() == 4,
	      "an erase in a run did not split it");
	Check(
	    within(GuestA + Page * 3, GuestA + Page * 3 + 0x10) ==
	            (Runs {{GuestA + Page * 3, GuestA + Page * 3 + 0x10}}) &&
	        within(GuestA + Page, GuestA + Page * 2).empty() &&
	        within(GuestA + Page / 2, GuestA + Page * 3) ==
	            (Runs {{GuestA + Page / 2, GuestA + Page}, {GuestA + Page * 2, GuestA + Page * 3}}),
	    "ForEach() did not cut the runs to its range");
	Check(runs.First(0) == std::optional<std::pair<uint64_t, uint64_t>> {{GuestA, GuestA + Page}} &&
	          runs.First(GuestA + 1) ==
	              std::optional<std::pair<uint64_t, uint64_t>> {
	                  {GuestA + Page * 2, GuestA + Page * 5}} &&
	          !runs.First(GuestA + Page * 2 + 1).has_value(),
	      "First() did not find the first run that starts at or after its address");
	// An erase across runs and gaps, and one past them all.
	runs.Erase(GuestA, GuestA + Page * 4);
	Check(all() == (Runs {{GuestA + Page * 4, GuestA + Page * 5}}) && runs.Pages() == 1,
	      "an erase across runs left pages or miscounted them");
	runs.Erase(0, ~uint64_t {0} & ~(Page - 1));
	Check(runs.Empty() && runs.Pages() == 0, "an erase of everything left a run");
}

// gta6-prep: read-guard: the parts a read watch's start splits its pages into: those that hold a
// lock keep NOACCESS, the others get guard pages (GuestAddressSpace::ProtectTransient()). Past the
// budget, around an alias's lock, with a range that is not page-aligned, after a page lost its
// lock to a remap, and after the watch ends.
void TestLockRuns() {
	using Parts = std::vector<std::tuple<uint64_t, uint64_t, bool>>;
	Locks      locks;
	Translate  translate;
	const auto parts = [&](uint64_t begin, uint64_t end) {
		Parts found;
		locks.ForEachLockRun(begin, end, [&](uint64_t from, uint64_t to, bool locked) {
			found.emplace_back(from, to, locked);
		});
		return found;
	};
	translate.Map(GuestA, Backing, 6);
	// Nothing locked: one part, unlocked.
	Check(parts(GuestA, GuestA + Page * 6) == (Parts {{GuestA, GuestA + Page * 6, false}}),
	      "a range without locks is not one unlocked part");
	// A budget of two pages: pages 1 and 2 locked, 3 and 4 refused, 0 and 5 not watched.
	locks.SetBudget(Page * 2);
	translate.Lock(locks, GuestA + Page, Page * 4);
	Check(locks.HeldPages() == 2 && locks.RefusedPages() == 2, "the budget did not split the run");
	Check(parts(GuestA, GuestA + Page * 6) ==
	          (Parts {{GuestA, GuestA + Page, false},
	                  {GuestA + Page, GuestA + Page * 3, true},
	                  {GuestA + Page * 3, GuestA + Page * 6, false}}),
	      "the locked pages are not one part between the others");
	Check(parts(GuestA + Page * 2, GuestA + Page * 4) ==
	          (Parts {{GuestA + Page * 2, GuestA + Page * 3, true},
	                  {GuestA + Page * 3, GuestA + Page * 4, false}}),
	      "a range that starts in the locked run is not cut to it");
	// Not page-aligned: whole pages, cut to the range.
	Check(parts(GuestA + Page + 0x10, GuestA + Page + 0x20) ==
	          (Parts {{GuestA + Page + 0x10, GuestA + Page + 0x20, true}}),
	      "a range within a locked page is not locked");
	Check(parts(GuestA + Page / 2, GuestA + Page * 3 + 0x10) ==
	          (Parts {{GuestA + Page / 2, GuestA + Page, false},
	                  {GuestA + Page, GuestA + Page * 3, true},
	                  {GuestA + Page * 3, GuestA + Page * 3 + 0x10, false}}),
	      "an unaligned range is not cut at its ends");
	// An alias of a locked page shares its lock: locked for both guest pages.
	translate.Map(GuestB, Backing + Page, 1);
	translate.Lock(locks, GuestB, Page);
	Check(parts(GuestB, GuestB + Page) == (Parts {{GuestB, GuestB + Page, true}}),
	      "an alias of a locked page is not locked");
	// Page 2 is mapped over other memory: its next Lock() drops the old lock, and the budget it
	// frees takes the new memory.
	translate.Map(GuestA + Page * 2, Backing + Page * 16, 1);
	translate.Lock(locks, GuestA + Page * 2, Page);
	Check(parts(GuestA + Page, GuestA + Page * 3) ==
	          (Parts {{GuestA + Page, GuestA + Page * 3, true}}),
	      "a page mapped over other memory did not get its new lock");
	// The ends of the watches: nothing is locked.
	locks.Unlock(GuestB, Page, translate);
	locks.Unlock(GuestA, Page * 6, translate);
	Check(parts(GuestA, GuestA + Page * 6) == (Parts {{GuestA, GuestA + Page * 6, false}}) &&
	          !locks.HasWork() && locks.GetOs().locked.empty(),
	      "pages stayed locked after their watch ended");
}

// The PageManager's runs are whole 4 KiB pages; a range that is not still covers the pages it
// touches.
void TestPartialPages() {
	Locks     locks;
	Translate translate;
	locks.SetBudget(Page * 4);
	locks.Lock(Backing + 0x800, GuestA + 0x800, Page);
	auto& os = locks.GetOs();
	Check(os.locks == std::vector<Call> {{Backing, Page * 2}}, "the touched pages were not locked");
	locks.Unlock(GuestA + Page + 0x10, 0x10, translate);
	Check(os.unlocks == std::vector<Call> {{Backing + Page, Page}},
	      "the touched page was not unlocked");
	locks.Unlock(GuestA, 1, translate);
	Check(os.locked.empty(), "a page stayed locked");
}

} // namespace

int main() {
	TestOff();
	TestLockAndUnlock();
	TestBudget();
	TestRetry();
	TestNoRetry();
	TestAliases();
	TestRemap();
	TestPrepare();
	TestRoom();
	TestLockFails();
	TestQuota();
	TestPageRuns();
	TestLockRuns();
	TestPartialPages();
	std::printf("ReadWatchLockTests: ok\n");
	return 0;
}
