// gta6-prep: vram
// The video memory arithmetic of the renderer and the options behind it, without a GPU.
#include "graphics/host_gpu/renderer/cache/memoryPolicy.h"
#include "graphics/presentation/videoOutResolution.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace {

namespace Policy = Libs::Graphics::MemoryPolicy;

using Policy::GcThresholds;
using Policy::GiB;
using Policy::MiB;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "VramPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

// The thresholds TextureCache and BufferCache computed before MemoryPolicy existed, worked out by
// hand (integer arithmetic, truncating divisions) for the budgets of common cards: the legacy
// policy must keep them bit for bit.
struct LegacyCase {
	uint64_t     budget;
	GcThresholds expected;
	const char*  message;
};

constexpr LegacyCase LegacyCases[] = {
    {0,
     {0, 1610612736, 3221225472, 1073741824, 2147483648},
     "legacy thresholds of a zero budget changed"},
    {2 * GiB,
     {0, 1610612736, 3221225472, 1073741824, 2147483648},
     "legacy thresholds of a 2 GiB budget changed"},
    {4 * GiB,
     {0, 1717986919, 3435973837, 1717986919, 3435973837},
     "legacy thresholds of a 4 GiB budget changed"},
    {13 * GiB / 2,
     {0, 2791728743, 5583457485, 2791728743, 5583457485},
     "legacy thresholds of a 6.5 GiB budget (RTX 3050 8 GB) changed"},
    {8 * GiB,
     {0, 3435973837, 6871947674, 3435973837, 6871947674},
     "legacy thresholds of an 8 GiB budget changed"},
    {12 * GiB,
     {2147483648, 7730941133, 11166914970, 7730941133, 11166914970},
     "legacy thresholds of a 12 GiB budget changed"},
    {45 * GiB / 2,
     {7784628224, 19005230285, 22441204122, 19005230285, 22441204122},
     "legacy thresholds of a 22.5 GiB budget (RTX 3090) changed"},
};

constexpr bool LegacyCasesHold() {
	for (const auto& test: LegacyCases) {
		if (Policy::LegacyGcThresholds(test.budget) != test.expected) {
			return false;
		}
	}
	return true;
}
// Also checked at compile time, so a syntax check catches a changed formula.
static_assert(LegacyCasesHold());

void TestLegacyThresholdsPinned() {
	for (const auto& test: LegacyCases) {
		Check(Policy::LegacyGcThresholds(test.budget) == test.expected, test.message);
		Check(Policy::ComputeGcThresholds(test.budget, Config::VramGcPolicy::Legacy) ==
		          test.expected,
		      "ComputeGcThresholds(Legacy) is not LegacyGcThresholds");
	}
	// Without a budget the caches keep their member initializers, which are what a zero budget
	// gives.
	Check(Policy::LegacyGcThresholds(0) == Policy::DefaultGcThresholds,
	      "the default thresholds are not the legacy ones of a zero budget");
	// A budget past INT64_MAX is clamped, not wrapped.
	const auto huge = Policy::LegacyGcThresholds(UINT64_MAX);
	Check(huge.texture_trigger > 0 && huge.texture_pressure > huge.texture_trigger &&
	          huge.texture_critical > huge.texture_pressure,
	      "an oversized budget wrapped the legacy thresholds");
}

void TestProportionalThresholds() {
	for (const uint64_t budget: {uint64_t {1} * GiB, uint64_t {4} * GiB, uint64_t {13} * GiB / 2,
	                             uint64_t {8} * GiB, uint64_t {45} * GiB / 2,
	                             uint64_t {80} * GiB}) {
		const auto t = Policy::ProportionalGcThresholds(budget);
		Check(t.texture_trigger <= t.texture_pressure && t.texture_pressure <= t.texture_critical &&
		          t.texture_critical < budget,
		      "proportional texture thresholds are not ordered below the budget");
		Check(t.buffer_trigger <= t.buffer_critical && t.buffer_critical < budget,
		      "proportional buffer thresholds are not ordered below the budget");
		Check(Policy::ComputeGcThresholds(budget, Config::VramGcPolicy::Proportional) == t,
		      "ComputeGcThresholds(Proportional) is not ProportionalGcThresholds");
	}
	// The pressured and critical levels are what a 24 GB card gets today, within 5%.
	const uint64_t budget       = 45 * GiB / 2;
	const auto     legacy       = Policy::LegacyGcThresholds(budget);
	const auto     proportional = Policy::ProportionalGcThresholds(budget);
	const auto     close        = [](uint64_t a, uint64_t b) {
		const auto difference = a > b ? a - b : b - a;
		return difference * 20 <= b;
	};
	Check(close(proportional.texture_pressure, legacy.texture_pressure) &&
	          close(proportional.texture_critical, legacy.texture_critical) &&
	          close(proportional.buffer_critical, legacy.buffer_critical),
	      "proportional thresholds stray from the 24 GB card's");
	// An 8 GB card no longer runs pressured from 40% of its budget.
	const auto small = Policy::ProportionalGcThresholds(13 * GiB / 2);
	Check(small.texture_pressure > Policy::LegacyGcThresholds(13 * GiB / 2).texture_pressure,
	      "proportional pressure on 6.5 GiB is not later than legacy");
	Check(small.texture_pressure == Policy::Permille(13 * GiB / 2, 780),
	      "proportional pressure is not 78% of the budget");
	Check(Policy::Permille(1000, 550) == 550 && Policy::Permille(UINT64_MAX, 1000) == UINT64_MAX,
	      "Permille rounds or overflows");
}

// --vram-limit-mb on a 24 GB card without resizable BAR: heap 0 (VRAM) is limited, the 256 MiB BAR
// window and the host heap stay whole; 0 limits nothing.
void TestHeapSizeLimits() {
	using Policy::HeapInfo;
	using Policy::NoHeapLimit;
	constexpr HeapInfo rtx3090[] = {
	    {.size = 24 * GiB, .device_local = true},
	    {.size = 32 * GiB, .device_local = false},
	    {.size = 256 * MiB, .device_local = true},
	};
	const auto off = Policy::BuildHeapSizeLimits(rtx3090, 0);
	Check(!off.any, "a zero limit limited a heap");
	for (const auto limit: off.limits) {
		Check(limit == NoHeapLimit, "a zero limit left a heap limit");
	}
	const auto limited = Policy::BuildHeapSizeLimits(rtx3090, 8192 * MiB);
	Check(limited.any, "an 8 GiB limit limited nothing");
	Check(limited.limits[0] == 8 * GiB, "the VRAM heap is not limited to 8 GiB");
	Check(limited.limits[1] == NoHeapLimit, "the host heap was limited");
	Check(limited.limits[2] == NoHeapLimit, "the BAR heap was limited");
	for (size_t i = 3; i < limited.limits.size(); i++) {
		Check(limited.limits[i] == NoHeapLimit, "a heap past the count was limited");
	}
	// A limit at or above the heap's size emulates nothing.
	Check(!Policy::BuildHeapSizeLimits(rtx3090, 24 * GiB).any,
	      "a limit equal to the heap size limited it");
	// A UMA device's single device-local heap is limited like VRAM.
	constexpr HeapInfo uma[] = {{.size = 16 * GiB, .device_local = true}};
	Check(Policy::BuildHeapSizeLimits(uma, 4 * GiB).limits[0] == 4 * GiB,
	      "the UMA heap was not limited");
	// A heap of exactly 1 GiB counts as a BAR-sized heap.
	constexpr HeapInfo small[] = {{.size = 1 * GiB, .device_local = true}};
	Check(!Policy::BuildHeapSizeLimits(small, 512 * MiB).any, "a 1 GiB heap was limited");
}

// "VRAM spill:" lines: the first spill of a kind, then counts 2, 4, 8, ...
void TestSpillLogCadence() {
	uint32_t logged = 0;
	for (uint64_t count = 1; count <= 1024; count++) {
		logged += Policy::LogSpillAt(count) ? 1u : 0u;
	}
	Check(logged == 11, "spill lines are not logged at 1, 2, 4, ..., 1024");
	Check(Policy::LogSpillAt(1) && Policy::LogSpillAt(2) && !Policy::LogSpillAt(3) &&
	          Policy::LogSpillAt(4) && !Policy::LogSpillAt(6) && !Policy::LogSpillAt(0),
	      "spill log cadence is not the first and every power of two");
}

// --oom-recovery and --no-oom-recovery: off by default, the off form wins.
void TestOomRecoveryFlags() {
	using Config::ResolveVramSettings;
	using Config::VramRequest;
	Check(!ResolveVramSettings({}).oom_recovery, "OOM recovery is on by default");
	Check(ResolveVramSettings({.oom_recovery = true}).oom_recovery, "--oom-recovery is ignored");
	Check(!ResolveVramSettings({.oom_recovery = true, .no_oom_recovery = true}).oom_recovery,
	      "--no-oom-recovery does not win over --oom-recovery");
	Check(!ResolveVramSettings({.no_oom_recovery = true}).oom_recovery,
	      "--no-oom-recovery turned recovery on");
}

// --vram-profile: auto changes nothing, low sets each 8 GB value, explicit options win over it.
void TestProfileExpansion() {
	using Config::ResolveVramSettings;
	using Config::VramGcPolicy;
	using Config::VramProfile;
	using Config::VramSettings;
	constexpr VramSettings builtin {};
	static_assert(ResolveVramSettings({}) == builtin, "the auto profile changes a built-in value");
	Check(ResolveVramSettings({.profile = VramProfile::Auto}) == builtin,
	      "the auto profile changes a built-in value");
	Check(builtin.gc_policy == VramGcPolicy::Legacy && !builtin.oom_recovery &&
	          !builtin.small_recycle_pool && !builtin.trim_recycle_pool &&
	          builtin.max_parked_images == 8 && !builtin.small_rt_arena &&
	          !builtin.rt_arena_mb.has_value() && builtin.max_pipeline_helpers == 0,
	      "the built-in VRAM settings are not today's values");

	const auto low = ResolveVramSettings({.profile = VramProfile::Low});
	Check(low.profile == VramProfile::Low, "the low profile is not reported");
	Check(low.gc_policy == VramGcPolicy::Proportional, "low does not collect proportionally");
	Check(low.oom_recovery, "low does not recover from allocation failures");
	Check(low.small_recycle_pool && low.trim_recycle_pool, "low keeps the full recycle pool");
	Check(low.max_parked_images == 4, "low keeps 8 parked images");
	Check(low.small_rt_arena && !low.rt_arena_mb.has_value(), "low keeps the large BLAS arena");
	Check(low.max_pipeline_helpers == 2, "low does not bound the pipeline helpers");

	const auto overridden = ResolveVramSettings({.profile         = VramProfile::Low,
	                                             .gc_policy       = VramGcPolicy::Legacy,
	                                             .no_oom_recovery = true,
	                                             .rt_arena_mb     = 768});
	Check(overridden.gc_policy == VramGcPolicy::Legacy, "--vram-gc does not win over low");
	Check(!overridden.oom_recovery, "--no-oom-recovery does not win over low");
	Check(overridden.rt_arena_mb == 768u, "--rt-arena-mb is lost under low");
	Check(overridden.max_parked_images == 4 && overridden.max_pipeline_helpers == 2,
	      "an explicit option reset the rest of the profile");
	// Explicit options without a profile.
	Check(ResolveVramSettings({.gc_policy = VramGcPolicy::Proportional}).gc_policy ==
	          VramGcPolicy::Proportional,
	      "--vram-gc proportional is ignored");
	Check(ResolveVramSettings({.rt_arena_mb = 0}).rt_arena_mb == 0u,
	      "--rt-arena-mb 0 (no limit) is lost");

	Check(Config::ParseVramProfile("low") == VramProfile::Low &&
	          Config::ParseVramProfile("auto") == VramProfile::Auto &&
	          !Config::ParseVramProfile("Low").has_value(),
	      "--vram-profile parsing");
	Check(Config::ParseVramGcPolicy("proportional") == VramGcPolicy::Proportional &&
	          Config::ParseVramGcPolicy("legacy") == VramGcPolicy::Legacy &&
	          !Config::ParseVramGcPolicy("").has_value(),
	      "--vram-gc parsing");
	Check(Config::VramProfileName(VramProfile::Low) == "low" &&
	          Config::VramGcPolicyName(VramGcPolicy::Proportional) == "proportional",
	      "VRAM option names");
}

// The budgets the low profile derives: the recycle pool, the BLAS arena and the helper bound.
void TestProfileBudgets() {
	Check(Policy::RecycledImageBytes(13 * GiB / 2, false) == 256 * MiB,
	      "the auto recycle pool is not 256 MiB");
	Check(Policy::RecycledImageBytes(13 * GiB / 2, true) == 208 * MiB,
	      "the low recycle pool on 6.5 GiB is not budget/32");
	Check(Policy::RecycledImageBytes(45 * GiB / 2, true) == 256 * MiB,
	      "the low recycle pool passes 256 MiB");
	Check(Policy::RecycledImageBytes(0, true) == 0, "the low recycle pool without a budget");

	// Auto: heap / 4 within 512 MiB - 4 GiB, as NativeRayTracing computed it.
	Check(Policy::RtArenaBudget(8 * GiB, std::nullopt, false) == 2 * GiB,
	      "the auto BLAS arena of an 8 GB card is not 2 GiB");
	Check(Policy::RtArenaBudget(24 * GiB, std::nullopt, false) == 4 * GiB,
	      "the auto BLAS arena of a 24 GB card is not 4 GiB");
	Check(Policy::RtArenaBudget(1 * GiB, std::nullopt, false) == 512 * MiB,
	      "the auto BLAS arena is below 512 MiB");
	// Low: heap / 8 within 512 MiB - 1 GiB.
	Check(Policy::RtArenaBudget(8 * GiB, std::nullopt, true) == 1 * GiB,
	      "the low BLAS arena of an 8 GB card is not 1 GiB");
	Check(Policy::RtArenaBudget(6 * GiB, std::nullopt, true) == 768 * MiB,
	      "the low BLAS arena of a 6 GB card is not heap/8");
	Check(Policy::RtArenaBudget(2 * GiB, std::nullopt, true) == 512 * MiB,
	      "the low BLAS arena is below 512 MiB");
	Check(Policy::RtArenaBudget(24 * GiB, std::nullopt, true) == 1 * GiB,
	      "the low BLAS arena passes 1 GiB");
	// A request (KYTY_RT_ARENA_MB or --rt-arena-mb) wins; 0 is no limit.
	Check(Policy::RtArenaBudget(8 * GiB, 768, true) == 768 * MiB, "an arena request is ignored");
	Check(Policy::RtArenaBudget(8 * GiB, 0, false) == 0, "a zero arena request is not unlimited");

	Check(Policy::BoundPipelineHelpers(10, 0) == 10, "no bound changed the helper count");
	Check(Policy::BoundPipelineHelpers(10, 2) == 2, "the helper bound is not applied");
	Check(Policy::BoundPipelineHelpers(1, 2) == 1, "the helper bound raised the count");
}

// The "VRAM:" line, exactly as the log contract (and tools/triage) reads it.
void TestBreakdownLine() {
	Policy::VramBreakdown breakdown;
	breakdown.budget       = 13 * GiB / 2;
	breakdown.limit_mb     = 8192;
	breakdown.profile      = Config::VramProfile::Low;
	breakdown.gc_policy    = Config::VramGcPolicy::Proportional;
	breakdown.bda_table    = 805306376; // BufferCache::BDA_PAGETABLE_SIZE
	breakdown.fault_buffer = 37748736 + 8192;
	breakdown.rings        = 192 * MiB;
	breakdown.recycle_cap  = 208 * MiB;
	breakdown.rt_arena     = 1 * GiB;
	breakdown.thresholds   = Policy::ProportionalGcThresholds(13 * GiB / 2);
	Check(Policy::FormatVramBreakdown(breakdown) ==
	          "VRAM: budget=6656 MiB limit=8192 profile=low gc=proportional fixed: bda_table=768 "
	          "fault=36 rings=192 recycle_cap=208 rt_arena=1024 thresholds: tex_trigger=3660 "
	          "tex_pressure=5191 tex_critical=6123 buf_trigger=4659 buf_critical=6123\n",
	      "the VRAM line does not follow the log contract");
	breakdown.limit_mb  = 0;
	breakdown.rt_arena  = std::nullopt;
	breakdown.profile   = Config::VramProfile::Auto;
	breakdown.gc_policy = Config::VramGcPolicy::Legacy;
	const auto line     = Policy::FormatVramBreakdown(breakdown);
	Check(line.find(" limit=off ") != std::string::npos &&
	          line.find(" rt_arena=off ") != std::string::npos &&
	          line.find(" profile=auto gc=legacy ") != std::string::npos,
	      "the VRAM line does not print off and the auto/legacy names");
}

// VideoOutGetOutputStatus's resolution: auto keeps the expression it had, 1080p and 4k fix it.
void TestReportedOutputResolution() {
	using Config::VideoOutResolution;
	using Libs::VideoOut::OUTPUT_RESOLUTION_4K;
	using Libs::VideoOut::OUTPUT_RESOLUTION_BELOW_4K;
	using Libs::VideoOut::ReportedOutputResolution;
	struct Size {
		uint32_t width;
		uint32_t height;
	};
	constexpr Size sizes[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160},
	                          {3840, 1080}, {1920, 2160}, {7680, 4320}};
	for (const int32_t attribute3: {0, 4, 1, 5, 0x7fffffff, -1}) {
		for (const auto size: sizes) {
			const uint32_t before = (attribute3 & 4) != 0 && size.width < 3840 &&
			                                size.height < 2160
			                            ? 1u
			                            : 2u;
			Check(ReportedOutputResolution(attribute3, size.width, size.height,
			                               VideoOutResolution::Auto) == before,
			      "auto output resolution differs from the expression it replaced");
			Check(ReportedOutputResolution(attribute3, size.width, size.height,
			                               VideoOutResolution::R1080p) ==
			          OUTPUT_RESOLUTION_BELOW_4K,
			      "--video-out-resolution 1080p does not report below 4K");
			Check(ReportedOutputResolution(attribute3, size.width, size.height,
			                               VideoOutResolution::R4k) == OUTPUT_RESOLUTION_4K,
			      "--video-out-resolution 4k does not report 4K");
		}
	}
	static_assert(ReportedOutputResolution(0, 1920, 1080, VideoOutResolution::Auto) ==
	              OUTPUT_RESOLUTION_4K);
	static_assert(ReportedOutputResolution(4, 1920, 1080, VideoOutResolution::Auto) ==
	              OUTPUT_RESOLUTION_BELOW_4K);
	Check(Config::ParseVideoOutResolution("1080p") == VideoOutResolution::R1080p &&
	          Config::ParseVideoOutResolution("4k") == VideoOutResolution::R4k &&
	          Config::ParseVideoOutResolution("auto") == VideoOutResolution::Auto &&
	          !Config::ParseVideoOutResolution("1440p").has_value(),
	      "--video-out-resolution parsing");
}

} // namespace

int main() {
	TestLegacyThresholdsPinned();
	TestProportionalThresholds();
	TestHeapSizeLimits();
	TestSpillLogCadence();
	TestOomRecoveryFlags();
	TestProfileExpansion();
	TestProfileBudgets();
	TestBreakdownLine();
	TestReportedOutputResolution();
	std::printf("VramPolicyTests: ok\n");
	return 0;
}
