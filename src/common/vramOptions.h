#ifndef KYTY_COMMON_VRAM_OPTIONS_H_
#define KYTY_COMMON_VRAM_OPTIONS_H_

// gta6-prep: vram
// The video memory options of the command line and what they amount to once --vram-profile is
// applied. Pure and header-only (vram_policy_tests checks it without a GPU).

#include <cstdint>
#include <optional>
#include <string_view>

namespace Config {

// --vram-profile: Auto keeps every built-in value; Low bundles the 8 GB settings below.
enum class VramProfile : uint8_t { Auto, Low };
// --vram-gc: how the texture and buffer caches derive their collection thresholds from the
// memory budget (see Libs::Graphics::MemoryPolicy::ComputeGcThresholds()).
enum class VramGcPolicy : uint8_t { Legacy, Proportional };
// --video-out-resolution: the output resolution VideoOutGetOutputStatus reports (see
// Libs::VideoOut::ReportedOutputResolution()).
enum class VideoOutResolution : uint8_t { Auto, R1080p, R4k };

// The options as the command line gave them.
struct VramRequest {
	VramProfile                 profile = VramProfile::Auto;
	std::optional<VramGcPolicy> gc_policy;
	// --oom-recovery and --no-oom-recovery; the off form wins when both are given.
	bool                        oom_recovery    = false;
	bool                        no_oom_recovery = false;
	// --rt-arena-mb (0: no limit). KYTY_RT_ARENA_MB still wins over it.
	std::optional<uint32_t>     rt_arena_mb;
};

// The settings in effect: the profile's values where the command line set nothing.
struct VramSettings {
	VramProfile             profile   = VramProfile::Auto;
	VramGcPolicy            gc_policy = VramGcPolicy::Legacy;
	bool                    oom_recovery = false;
	// The recycled-image pool keeps min(256 MiB, budget / 32) instead of 256 MiB.
	bool                    small_recycle_pool = false;
	// The pool is dropped whenever the texture collection runs pressured.
	bool                    trim_recycle_pool = false;
	// Images a depth/color conversion replaced that the texture cache keeps for a conversion back.
	uint32_t                max_parked_images = 8;
	// The native ray tracing BLAS arena defaults to clamp(heap / 8, 512 MiB, 1 GiB) instead of
	// clamp(heap / 4, 512 MiB, 4 GiB).
	bool                    small_rt_arena = false;
	std::optional<uint32_t> rt_arena_mb;
	// Upper bound of the automatic pipeline helper count; 0 is no bound. KYTY_PIPELINE_HELPERS
	// still wins.
	uint32_t                max_pipeline_helpers = 0;

	constexpr bool operator==(const VramSettings&) const = default;
};

constexpr VramSettings ResolveVramSettings(const VramRequest& request) {
	VramSettings settings;
	settings.profile = request.profile;
	const bool low   = request.profile == VramProfile::Low;
	if (low) {
		settings.gc_policy            = VramGcPolicy::Proportional;
		settings.oom_recovery         = true;
		settings.small_recycle_pool   = true;
		settings.trim_recycle_pool    = true;
		settings.max_parked_images    = 4;
		settings.small_rt_arena       = true;
		settings.max_pipeline_helpers = 2;
	}
	if (request.gc_policy.has_value()) {
		settings.gc_policy = *request.gc_policy;
	}
	if (request.no_oom_recovery) {
		settings.oom_recovery = false;
	} else if (request.oom_recovery) {
		settings.oom_recovery = true;
	}
	settings.rt_arena_mb = request.rt_arena_mb;
	return settings;
}

constexpr std::string_view VramProfileName(VramProfile profile) {
	return profile == VramProfile::Low ? "low" : "auto";
}

constexpr std::string_view VramGcPolicyName(VramGcPolicy policy) {
	return policy == VramGcPolicy::Proportional ? "proportional" : "legacy";
}

constexpr std::string_view VideoOutResolutionName(VideoOutResolution resolution) {
	switch (resolution) {
		case VideoOutResolution::R1080p: return "1080p";
		case VideoOutResolution::R4k: return "4k";
		case VideoOutResolution::Auto: break;
	}
	return "auto";
}

constexpr std::optional<VramProfile> ParseVramProfile(std::string_view text) {
	if (text == "auto") return VramProfile::Auto;
	if (text == "low") return VramProfile::Low;
	return std::nullopt;
}

constexpr std::optional<VramGcPolicy> ParseVramGcPolicy(std::string_view text) {
	if (text == "legacy") return VramGcPolicy::Legacy;
	if (text == "proportional") return VramGcPolicy::Proportional;
	return std::nullopt;
}

constexpr std::optional<VideoOutResolution> ParseVideoOutResolution(std::string_view text) {
	if (text == "auto") return VideoOutResolution::Auto;
	if (text == "1080p") return VideoOutResolution::R1080p;
	if (text == "4k") return VideoOutResolution::R4k;
	return std::nullopt;
}

} // namespace Config

#endif /* KYTY_COMMON_VRAM_OPTIONS_H_ */
