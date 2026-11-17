// gta6-prep: ram
// --ram-profile, the settings behind it and the text of the "RAM" lines, without a GPU or a game.
#include "common/parallelFor.h"
#include "common/ramOptions.h"
#include "graphics/host_gpu/renderer/cache/memoryPolicy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace {

using Config::RamGiB;
using Config::RamMiB;
using Config::RamProfile;
using Config::RamSettings;
using Config::ResolveRamSettings;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "RamPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

void CheckEqual(const std::string& actual, std::string_view expected, const char* message) {
	if (actual != expected) {
		std::fprintf(stderr, "RamPolicyTests: failed: %s: got \"%s\", expected \"%.*s\"\n", message,
		             actual.c_str(), static_cast<int>(expected.size()), expected.data());
		std::abort();
	}
}

constexpr uint64_t MiB(uint64_t count) {
	return count * RamMiB;
}

// The texts the triage rules of real failures match (tools/triage/rules.json): a RAM line must
// not trip them.
void CheckClearOfTriageRules(const std::string& text, const char* message) {
	Check(text.find("ErrorOutOf") == std::string::npos &&
	          text.find("out of device memory") == std::string::npos &&
	          text.find("could not reserve") == std::string::npos && !text.starts_with("Memory: "),
	      message);
}

// Normal is today's emulator: every value is the literal it replaced (each is also
// static_asserted where the code reads it).
void TestNormalKeepsToday() {
	constexpr RamSettings builtin {};
	static_assert(ResolveRamSettings(RamProfile::Normal) == builtin,
	              "the normal profile changes a built-in value");
	Check(builtin.profile == RamProfile::Normal && builtin.upload_ring_mb == 512 &&
	          builtin.download_ring_mb == 64 && builtin.compile_workers == 6 &&
	          builtin.async_pipeline_threads == 3 && builtin.parallel_threads == 0 &&
	          !builtin.max_pipeline_helpers.has_value() && builtin.cache_save_seconds == 60 &&
	          builtin.slow_cache_save_seconds == 20 && builtin.clear_fresh_stacks &&
	          builtin.recycle_spilled_images,
	      "the built-in RAM settings are not today's values");
}

void TestLowProfile() {
	constexpr auto low = ResolveRamSettings(RamProfile::Low);
	Check(low.profile == RamProfile::Low, "the low profile is not reported");
	Check(low.upload_ring_mb == 128 && low.download_ring_mb == 32,
	      "low does not shrink the host rings to 128 and 32 MiB");
	Check(low.compile_workers == 3, "low does not run 3 compile workers");
	Check(low.async_pipeline_threads == 2, "low does not run 2 async pipeline threads");
	Check(low.parallel_threads == 2, "low does not bound WarmUp and the manifest to 2 threads");
	Check(low.max_pipeline_helpers == 0u, "low does not turn the automatic pipeline helpers off");
	Check(low.cache_save_seconds == 180 && low.slow_cache_save_seconds == 60,
	      "low does not save the driver cache every 180 s (60 s after a slow creation)");
	Check(!low.clear_fresh_stacks, "low still clears fresh guest stacks");
	Check(!low.recycle_spilled_images, "low still recycles spilled images");
	Check(!(low == RamSettings {}), "low equals normal");
}

void TestParsing() {
	Check(Config::ParseRamProfile("low") == RamProfile::Low &&
	          Config::ParseRamProfile("normal") == RamProfile::Normal,
	      "--ram-profile low and normal do not parse");
	Check(!Config::ParseRamProfile("Low").has_value() &&
	          !Config::ParseRamProfile("8gb").has_value() &&
	          !Config::ParseRamProfile("auto").has_value() &&
	          !Config::ParseRamProfile("").has_value(),
	      "--ram-profile takes an unknown value");
	Check(Config::RamProfileName(RamProfile::Low) == "low" &&
	          Config::RamProfileName(RamProfile::Normal) == "normal",
	      "RAM profile names");
}

// K9: low implies --oom-recovery; --no-oom-recovery still wins; normal changes no VRAM setting.
void TestOomRecoveryImplied() {
	using Config::ApplyRamProfile;
	using Config::ResolveVramSettings;
	using Config::VramProfile;
	using Config::VramRequest;
	Check(ResolveVramSettings(ApplyRamProfile({}, RamProfile::Normal)) == ResolveVramSettings({}),
	      "the normal RAM profile changes a VRAM setting");
	Check(ResolveVramSettings(ApplyRamProfile({}, RamProfile::Low)).oom_recovery,
	      "the low RAM profile does not turn on OOM recovery");
	Check(!ResolveVramSettings(ApplyRamProfile({.no_oom_recovery = true}, RamProfile::Low))
	           .oom_recovery,
	      "--no-oom-recovery does not win over the low RAM profile");
	Check(ResolveVramSettings(ApplyRamProfile({.oom_recovery = true}, RamProfile::Normal))
	          .oom_recovery,
	      "--oom-recovery is lost under the normal RAM profile");
	// The VRAM profile's own values stay as they were.
	const auto both =
	    ResolveVramSettings(ApplyRamProfile({.profile = VramProfile::Low}, RamProfile::Low));
	Check(both.profile == VramProfile::Low && both.oom_recovery && both.max_pipeline_helpers == 2 &&
	          both.max_parked_images == 4,
	      "the low RAM profile changed the low VRAM profile");
	const VramRequest request {.gc_policy = Config::VramGcPolicy::Legacy, .rt_arena_mb = 768};
	const auto        applied = ApplyRamProfile(request, RamProfile::Low);
	Check(applied.gc_policy == Config::VramGcPolicy::Legacy && applied.rt_arena_mb == 768u &&
	          !applied.no_oom_recovery,
	      "the low RAM profile changed another VRAM option");
}

// K1: the RAM bound comes after the VRAM bound; no bound keeps the count.
void TestHelperBound() {
	namespace Policy = Libs::Graphics::MemoryPolicy;
	using Config::BoundHelpersByRam;
	static_assert(BoundHelpersByRam(4, std::nullopt) == 4);
	static_assert(BoundHelpersByRam(4, 0) == 0);
	static_assert(BoundHelpersByRam(1, 3) == 1);
	static_assert(BoundHelpersByRam(5, 3) == 3);
	// 12 logical processors: 12 / 2 - 2 = 4 automatic helpers, 2 under --vram-profile low, none
	// once --ram-profile low applies too.
	const uint32_t automatic = 12 / 2 - 2;
	const auto     vram_low  = Config::ResolveVramSettings({.profile = Config::VramProfile::Low});
	Check(BoundHelpersByRam(Policy::BoundPipelineHelpers(automatic, 0),
	                        RamSettings {}.max_pipeline_helpers) == 4,
	      "the normal profiles bound the automatic helpers");
	Check(BoundHelpersByRam(Policy::BoundPipelineHelpers(automatic, vram_low.max_pipeline_helpers),
	                        RamSettings {}.max_pipeline_helpers) == 2,
	      "the normal RAM profile changed the VRAM bound");
	Check(BoundHelpersByRam(Policy::BoundPipelineHelpers(automatic, vram_low.max_pipeline_helpers),
	                        ResolveRamSettings(RamProfile::Low).max_pipeline_helpers) == 0,
	      "the low RAM profile leaves automatic helpers");
}

// K4: ParallelThreads(cap) is ParallelThreads() bounded by the cap; 0 is no bound.
void TestParallelThreads() {
	using Common::CapThreads;
	static_assert(CapThreads(6, 0) == 6);
	static_assert(CapThreads(6, 2) == 2);
	static_assert(CapThreads(1, 2) == 1);
	static_assert(CapThreads(12, 12) == 12);
	static_assert(CapThreads(0, 2) == 1);
	Check(Common::ParallelThreads(0) == Common::ParallelThreads(),
	      "no cap changed the thread count");
	Check(Common::ParallelThreads(2) >= 1 && Common::ParallelThreads(2) <= 2,
	      "a cap of 2 is not 1 or 2 threads");
	Check(Common::ParallelThreads(ResolveRamSettings(RamProfile::Normal).parallel_threads) ==
	          Common::ParallelThreads(),
	      "the normal profile changes the WarmUp and manifest threads");
}

void TestStartupLine() {
	// 8 GB of RAM and a 16 GiB page file on Windows, with the guest memory section committed.
	using Config::EstimatePageFile;
	static_assert(EstimatePageFile(MiB(24474), MiB(8090)) == MiB(16384));
	// No page file: the limit is below the RAM, and the estimate is 0, not unknown.
	static_assert(EstimatePageFile(MiB(7900), MiB(8090)) == 0u);
	static_assert(!EstimatePageFile(std::nullopt, MiB(8090)).has_value() &&
	              !EstimatePageFile(MiB(24474), std::nullopt).has_value());
	Config::RamStartup ram;
	ram.physical            = MiB(8090);
	ram.available           = MiB(4712);
	ram.commit_used         = MiB(18242);
	ram.commit_limit        = MiB(24474);
	ram.page_file           = EstimatePageFile(ram.commit_limit, ram.physical);
	ram.page_file_estimated = true;
	ram.guest               = MiB(12736);
	ram.profile             = RamProfile::Low;
	CheckEqual(Config::FormatRamStartup(ram),
	           "RAM: phys=8090 MiB avail=4712 commit=18242/24474 headroom=6232 pagefile=~16384 "
	           "guest=12736 profile=low\n",
	           "the RAM line does not follow the log contract");
	ram.commit_limit = MiB(7900);
	ram.commit_used  = MiB(5000);
	ram.page_file    = EstimatePageFile(ram.commit_limit, ram.physical);
	CheckEqual(Config::FormatRamStartup(ram),
	           "RAM: phys=8090 MiB avail=4712 commit=5000/7900 headroom=2900 pagefile=~0 "
	           "guest=12736 profile=low\n",
	           "the RAM line of a Windows PC without a page file");
	// Linux: swap, exactly, and a commit charge past its limit (overcommit; the limit is the swap
	// and half the RAM) leaves no headroom.
	ram.commit_used         = MiB(30000);
	ram.commit_limit        = MiB(6093);
	ram.page_file_estimated = false;
	ram.page_file           = MiB(2048);
	ram.profile             = RamProfile::Normal;
	CheckEqual(Config::FormatRamStartup(ram),
	           "RAM: phys=8090 MiB avail=4712 commit=30000/6093 headroom=0 pagefile=2048 "
	           "guest=12736 profile=normal\n",
	           "the RAM line of an overcommitted host");
	Config::RamStartup unknown;
	unknown.guest = MiB(12736);
	CheckEqual(Config::FormatRamStartup(unknown),
	           "RAM: phys=? MiB avail=? commit=?/? headroom=? pagefile=? guest=12736 "
	           "profile=normal\n",
	           "the RAM line without host figures");
}

void TestProfileLine() {
	CheckEqual(Config::FormatRamProfile(ResolveRamSettings(RamProfile::Low), true),
	           "RAM profile: low -> upload_ring=128 download_ring=32 compile_workers=3 "
	           "async_threads=2 parallel_threads=2 helpers<=0 cache_save=180s/60s "
	           "fresh_stack_clear=off recycle_spilled=off oom_recovery=on\n",
	           "the RAM profile line does not follow the log contract");
	CheckEqual(Config::FormatRamProfile(RamSettings {}, false),
	           "RAM profile: normal -> upload_ring=512 download_ring=64 compile_workers=6 "
	           "async_threads=3 parallel_threads=auto helpers=auto cache_save=60s/20s "
	           "fresh_stack_clear=on recycle_spilled=on oom_recovery=off\n",
	           "the RAM profile line of the normal profile");
}

void TestHeadroomWarning() {
	using Config::FormatHeadroomWarning;
	using Config::PageFileToAdd;
	// 8 GB of RAM, a 16 GiB page file, the guest section committed: 6232 MiB of headroom. 20 GiB
	// less that is 14248 MiB, rounded up to 4 GiB: 16384 MiB.
	const auto warning = FormatHeadroomWarning(MiB(18242), MiB(24474));
	Check(warning.has_value(), "no warning with 6232 MiB of commit headroom");
	CheckEqual(*warning,
	           "RAM warning: commit headroom 6232 MiB is below 12288 MiB. Add about 16384 MiB of "
	           "page file, fixed size, on any drive with space (System Properties > Advanced > "
	           "Performance Settings > Advanced > Virtual memory > Change, untick Automatically "
	           "manage paging file size for all drives, pick the drive, Custom size), then restart "
	           "Windows. A page file that Windows manages may grow on its own; a fixed size avoids "
	           "the stall while it grows. The GPU driver may also charge commit for the video "
	           "memory it allocates.\n",
	           "the RAM warning does not follow the log contract");
	CheckClearOfTriageRules(*warning, "the headroom warning matches a failure's triage rule");
	// 32 GB of RAM with a system-managed page file: 18 GiB of headroom, no warning.
	Check(!FormatHeadroomWarning(MiB(19000), MiB(37432)).has_value(),
	      "a warning with 18 GiB of commit headroom");
	// The boundary: 12288 MiB is enough, a byte less is not (and asks for 12 GiB more).
	Check(!FormatHeadroomWarning(MiB(10000), MiB(10000) + 12 * RamGiB).has_value(),
	      "a warning at exactly 12288 MiB of headroom");
	const auto edge = FormatHeadroomWarning(MiB(10000), MiB(10000) + 12 * RamGiB - 1);
	Check(edge.has_value() && edge->find("headroom 12287 MiB") != std::string::npos &&
	          edge->find("Add about 12288 MiB") != std::string::npos,
	      "no warning, or not 12 GiB more, just below 12288 MiB of headroom");
	// An unknown commit charge or limit: no warning.
	Check(!FormatHeadroomWarning(std::nullopt, MiB(24474)).has_value() &&
	          !FormatHeadroomWarning(MiB(18242), std::nullopt).has_value(),
	      "a warning without the commit figures");
	// No headroom at all (the charge at the limit) asks for the whole 20 GiB.
	static_assert(PageFileToAdd(MiB(24474), MiB(24474), 0) == 20 * RamGiB);
	static_assert(PageFileToAdd(MiB(18242), MiB(24474), 0) == MiB(16384));
	static_assert(PageFileToAdd(0, 40 * RamGiB, 0) == 0);
	static_assert(PageFileToAdd(0, 20 * RamGiB, 0) == 0);
}

void TestCommitWarning() {
	using Config::CommitNearLimit;
	static_assert(!CommitNearLimit(MiB(31000), MiB(33462)));
	static_assert(CommitNearLimit(MiB(31790), MiB(33462)));
	static_assert(CommitNearLimit(MiB(33462), MiB(33462)));
	static_assert(!CommitNearLimit(std::nullopt, MiB(33462)) &&
	              !CommitNearLimit(MiB(1), std::nullopt) && !CommitNearLimit(MiB(1), 0));
	const auto warning = Config::FormatCommitWarning(MiB(31790), MiB(33462));
	CheckEqual(warning,
	           "RAM warning: commit 31790 MiB is 95% of the 33462 MiB limit, past which "
	           "allocations fail. Close other programs, or add about 20480 MiB of page file, fixed "
	           "size, on any drive with space (System Properties > Advanced > Performance "
	           "Settings > Advanced > Virtual memory > Change, untick Automatically manage paging "
	           "file size for all drives, pick the drive, Custom size), then restart Windows. A "
	           "page file that Windows manages may grow on its own; a fixed size avoids the stall "
	           "while it grows.\n",
	           "the commit warning does not follow the log contract");
	CheckClearOfTriageRules(warning, "the commit warning matches a failure's triage rule");
	static_assert(Config::CommitAtPercent(MiB(90), MiB(100), 90) &&
	              !Config::CommitAtPercent(MiB(89), MiB(100), 90));
}

// The "RAM usage:" thread syncs the session log only when memory is short (a sync may hold up the
// threads that log meanwhile).
void TestSyncAfterSample() {
	using Config::SyncAfterSample;
	Config::RamUsage usage;
	Check(!SyncAfterSample(usage, true), "a sync without figures");
	usage.available    = MiB(18800);
	usage.commit_used  = MiB(26650);
	usage.commit_limit = MiB(52000);
	Check(!SyncAfterSample(usage, true), "a sync with plenty of RAM and commit");
	usage.available = MiB(511);
	Check(SyncAfterSample(usage, true) && SyncAfterSample(usage, false),
	      "no sync with less than 512 MiB available");
	usage.available    = MiB(512);
	usage.commit_used  = MiB(46800);
	usage.commit_limit = MiB(52000);
	Check(SyncAfterSample(usage, true), "no sync with the commit at 90% of an enforced limit");
	Check(!SyncAfterSample(usage, false), "a sync for the commit of an unenforced limit (Linux)");
	usage.commit_used = MiB(46799);
	Check(!SyncAfterSample(usage, true), "a sync below 90% of the commit limit");
}

// The text after "could not reserve <n> MB for guest direct memory" (a triage rule anchors on that
// prefix, which the emulator keeps).
void TestReserveAdvice() {
	const auto advice = Config::FormatReserveAdvice(MiB(12736), MiB(15474), MiB(24474));
	CheckEqual(advice,
	           "Commit: 9000 MiB free of a 24474 MiB limit, 12736 MiB needed. Add about 24576 MiB "
	           "of page file, fixed size, on any drive with space (System Properties > Advanced > "
	           "Performance Settings > Advanced > Virtual memory > Change, untick Automatically "
	           "manage paging file size for all drives, pick the drive, Custom size), then "
	           "restart Windows.\n",
	           "the reserve failure advice does not follow the log contract");
	CheckClearOfTriageRules(advice, "the reserve advice repeats a triage rule's text");
	Check(Config::FormatReserveAdvice(MiB(12736), std::nullopt, MiB(24474)).empty(),
	      "advice without the commit figures");
	// The commit has room for the section and 20 GiB more: something else failed, and the advice
	// asks for no page file ("Add about 0 MiB").
	CheckEqual(Config::FormatReserveAdvice(MiB(12736), MiB(10000), MiB(65536)),
	           "Commit: 55536 MiB free of a 65536 MiB limit, 12736 MiB needed.\n",
	           "the reserve failure advice with enough commit");
}

void TestUsageLine() {
	Config::RamUsage usage;
	usage.seconds       = 630;
	usage.working_set   = MiB(2843);
	usage.private_bytes = MiB(9875);
	usage.commit_used   = MiB(31790);
	usage.commit_limit  = MiB(40858);
	usage.available     = MiB(187);
	usage.faults_per_s  = 3411.6;
	usage.renderer      = Config::RendererMemory {.local_usage  = MiB(5402),
	                                              .local_budget = MiB(5560),
	                                              .host_usage   = MiB(1873),
	                                              .host_budget  = MiB(3711),
	                                              .spilled      = MiB(1504),
	                                              .noaccess     = MiB(312)};
	CheckEqual(Config::FormatRamUsage(usage),
	           "RAM usage: t=630 ws=2843 private=9875 commit=31790/40858 avail=187 faults=3412 "
	           "hard=? vk_local=5402/5560 vk_host=1873/3711 spilled=1504 noaccess=312\n",
	           "the RAM usage line does not follow the log contract");
	// A single inaccessible page shows: noaccess rounds up.
	usage.renderer->noaccess = 4096;
	usage.hard_faults_per_s  = 12.4;
	const auto line          = Config::FormatRamUsage(usage);
	Check(line.find(" hard=12 ") != std::string::npos &&
	          line.find(" noaccess=1\n") != std::string::npos,
	      "the hard faults or one inaccessible page are not shown");
	usage.renderer->noaccess = 0;
	Check(Config::FormatRamUsage(usage).find(" noaccess=0\n") != std::string::npos,
	      "no inaccessible page is not 0");
	// The exit line: no renderer fields, nothing sampled yet.
	Config::RamUsage exit_line;
	exit_line.seconds = 5;
	CheckEqual(Config::FormatRamUsage(exit_line),
	           "RAM usage: t=5 ws=? private=? commit=?/? avail=? faults=? hard=?\n",
	           "the RAM usage line without figures");
	// A buffer that is too small cuts the line and writes nothing past it.
	char small[16];
	for (auto& c: small) {
		c = '#';
	}
	const auto cut = Config::FormatRamUsage(usage, small, 10);
	Check(cut == 10 && std::string_view(small, 10) == "RAM usage:" && small[10] == '#',
	      "a small buffer is overrun or not filled");
	// The longest line fits the buffer the exit path uses.
	constexpr auto   most = ~uint64_t {0};
	Config::RamUsage widest;
	widest.seconds           = most;
	widest.working_set       = most;
	widest.private_bytes     = most;
	widest.commit_used       = most;
	widest.commit_limit      = most;
	widest.available         = most;
	widest.faults_per_s      = 1e15;
	widest.hard_faults_per_s = 1e15;
	widest.renderer          = Config::RendererMemory {most, most, most, most, most, most};
	char       buffer[Config::RamUsageLineSize];
	const auto size = Config::FormatRamUsage(widest, buffer, sizeof(buffer));
	Check(size < sizeof(buffer) && buffer[size - 1] == '\n', "the widest RAM usage line is cut");
	CheckClearOfTriageRules(Config::FormatRamUsage(usage),
	                        "a RAM usage line matches a triage rule");
}

// The exit line reads /proc without allocating.
void TestProcParser() {
	using Config::ParseProcKiB;
	constexpr std::string_view meminfo = "MemTotal:       16318460 kB\n"
	                                     "MemFree:         1205672 kB\n"
	                                     "MemAvailable:    9876544 kB\n"
	                                     "CommitLimit:    12345678 kB\n"
	                                     "Committed_AS:   23456789 kB";
	static_assert(ParseProcKiB(meminfo, "MemAvailable") == uint64_t {9876544} * 1024);
	static_assert(ParseProcKiB(meminfo, "CommitLimit") == uint64_t {12345678} * 1024);
	// The last line has no newline.
	static_assert(ParseProcKiB(meminfo, "Committed_AS") == uint64_t {23456789} * 1024);
	// A key is a whole name: "Mem" is the start of three, and none of them.
	static_assert(!ParseProcKiB(meminfo, "Mem").has_value());
	static_assert(!ParseProcKiB(meminfo, "SwapTotal").has_value());
	static_assert(!ParseProcKiB("", "MemAvailable").has_value());
	// /proc/self/status separates with a tab.
	constexpr std::string_view status = "Name:\tkyty_emulator\nVmRSS:\t  123456 kB\nThreads:\t9\n";
	static_assert(ParseProcKiB(status, "VmRSS") == uint64_t {123456} * 1024);
	static_assert(!ParseProcKiB(status, "Name").has_value(), "a value that is not a number");
	Check(ParseProcKiB(meminfo, "MemFree") == uint64_t {1205672} * 1024,
	      "/proc/meminfo parsing at run time");
}

} // namespace

int main() {
	TestNormalKeepsToday();
	TestLowProfile();
	TestParsing();
	TestOomRecoveryImplied();
	TestHelperBound();
	TestParallelThreads();
	TestStartupLine();
	TestProfileLine();
	TestHeadroomWarning();
	TestCommitWarning();
	TestSyncAfterSample();
	TestReserveAdvice();
	TestUsageLine();
	TestProcParser();
	std::printf("RamPolicyTests: ok\n");
	return 0;
}
