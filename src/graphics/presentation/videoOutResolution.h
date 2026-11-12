#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTRESOLUTION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTRESOLUTION_H_

// gta6-prep: vram
// The output resolution class VideoOutGetOutputStatus reports (VideoOutOutputStatus::resolution):
// 1 below 4K, 2 for 4K. Pure (vram_policy_tests checks it).

#include "common/vramOptions.h"

#include <cstdint>

namespace Libs::VideoOut {

inline constexpr uint32_t OUTPUT_RESOLUTION_BELOW_4K = 1;
inline constexpr uint32_t OUTPUT_RESOLUTION_4K       = 2;

// Auto: 4K unless param.json's ATTRIBUTE3 bit 2 (the title detects the output resolution) is set
// and the output is smaller than 3840x2160. --video-out-resolution 1080p and 4k report that class
// whatever the title and the window are.
constexpr uint32_t ReportedOutputResolution(int32_t attribute3, uint32_t width, uint32_t height,
                                            Config::VideoOutResolution override_resolution) {
	switch (override_resolution) {
		case Config::VideoOutResolution::R1080p: return OUTPUT_RESOLUTION_BELOW_4K;
		case Config::VideoOutResolution::R4k: return OUTPUT_RESOLUTION_4K;
		case Config::VideoOutResolution::Auto: break;
	}
	return (attribute3 & 4) != 0 && width < 3840 && height < 2160 ? OUTPUT_RESOLUTION_BELOW_4K
	                                                              : OUTPUT_RESOLUTION_4K;
}

} // namespace Libs::VideoOut

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VIDEOOUTRESOLUTION_H_
