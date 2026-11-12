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

inline constexpr uint32_t VENDOR_ID_NVIDIA = 0x10de;

// VkPhysicalDeviceProperties::driverVersion as the vendor's own tools show it: NVIDIA packs
// 10.8.8.6 bits (566.36 is 566.36.0.0); the others use the VK_MAKE_VERSION layout.
[[nodiscard]] inline std::string DriverVersionString(uint32_t vendor_id, uint32_t version) {
	if (vendor_id == VENDOR_ID_NVIDIA) {
		return fmt::format("{}.{}.{}.{}", version >> 22u, (version >> 14u) & 0xffu,
		                   (version >> 6u) & 0xffu, version & 0x3fu);
	}
	return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
	                   VK_VERSION_PATCH(version));
}

[[nodiscard]] inline std::string ApiVersionString(uint32_t version) {
	return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
	                   VK_API_VERSION_PATCH(version));
}

struct GpuMemorySummary {
	// The DEVICE_LOCAL heaps larger than 1 GiB together (the video memory of a discrete GPU, not
	// its 256 MiB BAR window).
	uint64_t vram_mib = 0;
	// The largest heap with a memory type both DEVICE_LOCAL and HOST_VISIBLE: 256 MiB without
	// Resizable BAR, about the whole video memory with it.
	uint64_t bar_mib = 0;
};

[[nodiscard]] inline GpuMemorySummary SummarizeGpuMemory(
    const vk::PhysicalDeviceMemoryProperties& memory) {
	constexpr uint64_t MiB = 1024ull * 1024;
	constexpr uint64_t GiB = 1024ull * MiB;
	GpuMemorySummary   summary;
	uint64_t           vram  = 0;
	const auto         heaps = std::min<uint32_t>(memory.memoryHeapCount, VK_MAX_MEMORY_HEAPS);
	for (uint32_t i = 0; i < heaps; i++) {
		const auto& heap = memory.memoryHeaps[i];
		if ((heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal) && heap.size > GiB) {
			vram += heap.size;
		}
	}
	uint64_t   bar   = 0;
	const auto types = std::min<uint32_t>(memory.memoryTypeCount, VK_MAX_MEMORY_TYPES);
	for (uint32_t i = 0; i < types; i++) {
		const auto& type = memory.memoryTypes[i];
		constexpr auto mapped =
		    vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible;
		if ((type.propertyFlags & mapped) == mapped && type.heapIndex < heaps) {
			bar = std::max<uint64_t>(bar, memory.memoryHeaps[type.heapIndex].size);
		}
	}
	summary.vram_mib = vram / MiB;
	summary.bar_mib  = bar / MiB;
	return summary;
}

struct GpuIdentity {
	std::string_view name;
	uint32_t         vendor_id      = 0;
	uint32_t         device_id      = 0;
	uint32_t         driver_version = 0;
	uint32_t         api_version    = 0;
	// The VkDriverId name (vk::to_string), or "?" when the driver does not report one.
	std::string_view driver_id = "?";
	GpuMemorySummary memory;
};

// The "GPU: " line of the log contract.
[[nodiscard]] inline std::string GpuIdentityLine(const GpuIdentity& gpu) {
	return fmt::format("GPU: {} vendor=0x{:04x} device=0x{:04x} driver={} api={} driverID={} "
	                   "vram={} MiB bar={} MiB\n",
	                   gpu.name, gpu.vendor_id, gpu.device_id,
	                   DriverVersionString(gpu.vendor_id, gpu.driver_version),
	                   ApiVersionString(gpu.api_version), gpu.driver_id, gpu.memory.vram_mib,
	                   gpu.memory.bar_mib);
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_VULKANDIAG_H_ */
