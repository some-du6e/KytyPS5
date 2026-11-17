// gta6-prep: ram
// --ram-profile and the settings behind it, without a GPU or a game.
#include "common/parallelFor.h"
#include "common/ramOptions.h"
#include "graphics/host_gpu/renderer/cache/memoryPolicy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace {

using Config::RamProfile;
using Config::RamSettings;
using Config::ResolveRamSettings;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "RamPolicyTests: failed: %s\n", message);
		std::abort();
	}
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

} // namespace

int main() {
	TestNormalKeepsToday();
	TestLowProfile();
	TestParsing();
	TestOomRecoveryImplied();
	TestHelperBound();
	TestParallelThreads();
	std::printf("RamPolicyTests: ok\n");
	return 0;
}
