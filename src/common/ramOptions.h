#ifndef KYTY_COMMON_RAM_OPTIONS_H_
#define KYTY_COMMON_RAM_OPTIONS_H_

// gta6-prep: ram
// --ram-profile and what it amounts to. Pure and header-only (ram_policy_tests checks it without a
// GPU).

#include "common/vramOptions.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Config {

// --ram-profile: Normal keeps every built-in value; Low bundles the settings below for a PC with
// about 8 GB of RAM. The emulator never picks it by itself: a game that works today on such a PC
// keeps its behaviour (tools/preflight passes low on a PC with less than 14 GiB).
enum class RamProfile : uint8_t { Normal, Low };

// The settings in effect. The defaults are the values the emulator had before the profile
// existed; each place that reads one static_asserts its old constant against them.
struct RamSettings {
	RamProfile profile = RamProfile::Normal;
	// BufferCache's host-visible rings in MiB: uploads (the staging ring) and readbacks.
	uint32_t upload_ring_mb   = 512;
	uint32_t download_ring_mb = 64;
	// PipelineCache's graphics program compile workers and async pipeline threads.
	uint32_t compile_workers        = 6;
	uint32_t async_pipeline_threads = 3;
	// The threads of the shader store's WarmUp and of the pipeline manifest at most; 0: no bound
	// (Common::ParallelThreads(), half the hardware threads).
	uint32_t parallel_threads = 0;
	// Upper bound of the automatic pipeline helper count, applied after the VRAM profile's;
	// nullopt: none. --pipeline-helpers and KYTY_PIPELINE_HELPERS still win.
	std::optional<uint32_t> max_pipeline_helpers;
	// How often the driver pipeline cache is written, and how soon after a slow pipeline
	// creation at the earliest, in seconds.
	uint32_t cache_save_seconds      = 60;
	uint32_t slow_cache_save_seconds = 20;
	// Guest stacks the emulator maps fresh are cleared as reused ones are. A fresh one is a new
	// commit, which the host hands out zeroed; reused stacks are always cleared.
	bool clear_fresh_stacks = true;
	// A deleted image that VMA placed outside device-local memory may go into the recycled-image
	// pool, which keeps its (pinned host) memory.
	bool recycle_spilled_images = true;

	constexpr bool operator==(const RamSettings&) const = default;
};

constexpr RamSettings ResolveRamSettings(RamProfile profile) {
	RamSettings settings;
	settings.profile = profile;
	if (profile == RamProfile::Low) {
		settings.upload_ring_mb          = 128;
		settings.download_ring_mb        = 32;
		settings.compile_workers         = 3;
		settings.async_pipeline_threads  = 2;
		settings.parallel_threads        = 2;
		settings.max_pipeline_helpers    = 0;
		settings.cache_save_seconds      = 180;
		settings.slow_cache_save_seconds = 60;
		settings.clear_fresh_stacks      = false;
		settings.recycle_spilled_images  = false;
	}
	return settings;
}

constexpr std::string_view RamProfileName(RamProfile profile) {
	return profile == RamProfile::Low ? "low" : "normal";
}

constexpr std::optional<RamProfile> ParseRamProfile(std::string_view text) {
	if (text == "normal") return RamProfile::Normal;
	if (text == "low") return RamProfile::Low;
	return std::nullopt;
}

// --ram-profile low implies --oom-recovery: a failed allocation frees what it can and retries past
// the budget instead of closing the emulator. --no-oom-recovery still wins (ResolveVramSettings()).
constexpr VramRequest ApplyRamProfile(VramRequest request, RamProfile profile) {
	if (profile == RamProfile::Low) {
		request.oom_recovery = true;
	}
	return request;
}

// The automatic pipeline helper count once the RAM profile bounds it (nullopt: no bound), after
// MemoryPolicy::BoundPipelineHelpers() applied the VRAM profile's.
constexpr uint32_t BoundHelpersByRam(uint32_t count, std::optional<uint32_t> bound) {
	return bound.has_value() ? std::min(count, *bound) : count;
}

} // namespace Config

#endif /* KYTY_COMMON_RAM_OPTIONS_H_ */
