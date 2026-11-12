#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAG_H_

// The text of the Vulkan diagnostics of crash logs (gta6-prep log contract). Header-only and free
// of device state, so that tests check it without a GPU.

#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace Libs::Graphics {

// "<operation> failed: <VkResult name> (<value>)", e.g. "vkWaitSemaphores failed: ErrorDeviceLost
// (-4)".
[[nodiscard]] inline std::string VulkanFailureText(std::string_view operation, vk::Result result) {
	return fmt::format("{} failed: {} ({})", operation, vk::to_string(result),
	                   static_cast<int>(result));
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAG_H_ */
