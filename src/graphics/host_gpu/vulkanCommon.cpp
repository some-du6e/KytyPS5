#include "graphics/host_gpu/vulkanCommon.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/vulkanDiag.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Libs::Graphics {
namespace {

struct FormatMapping {
	Prospero::BufferFormat guest;
	vk::Format             host;
};

constexpr FormatMapping kFormatMappings[] = {
    {Prospero::BufferFormat::k8UNorm, vk::Format::eR8Unorm},
    {Prospero::BufferFormat::k8SNorm, vk::Format::eR8Snorm},
    {Prospero::BufferFormat::k8UScaled, vk::Format::eR8Uscaled},
    {Prospero::BufferFormat::k8SScaled, vk::Format::eR8Sscaled},
    {Prospero::BufferFormat::k8UInt, vk::Format::eR8Uint},
    {Prospero::BufferFormat::k8SInt, vk::Format::eR8Sint},
    {Prospero::BufferFormat::k16UNorm, vk::Format::eR16Unorm},
    {Prospero::BufferFormat::k16SNorm, vk::Format::eR16Snorm},
    {Prospero::BufferFormat::k16UScaled, vk::Format::eR16Uscaled},
    {Prospero::BufferFormat::k16SScaled, vk::Format::eR16Sscaled},
    {Prospero::BufferFormat::k16UInt, vk::Format::eR16Uint},
    {Prospero::BufferFormat::k16SInt, vk::Format::eR16Sint},
    {Prospero::BufferFormat::k16Float, vk::Format::eR16Sfloat},
    {Prospero::BufferFormat::k8_8UNorm, vk::Format::eR8G8Unorm},
    {Prospero::BufferFormat::k8_8SNorm, vk::Format::eR8G8Snorm},
    {Prospero::BufferFormat::k8_8UScaled, vk::Format::eR8G8Uscaled},
    {Prospero::BufferFormat::k8_8SScaled, vk::Format::eR8G8Sscaled},
    {Prospero::BufferFormat::k8_8UInt, vk::Format::eR8G8Uint},
    {Prospero::BufferFormat::k8_8SInt, vk::Format::eR8G8Sint},
    {Prospero::BufferFormat::k32UInt, vk::Format::eR32Uint},
    {Prospero::BufferFormat::k32SInt, vk::Format::eR32Sint},
    {Prospero::BufferFormat::k32Float, vk::Format::eR32Sfloat},
    {Prospero::BufferFormat::k16_16UNorm, vk::Format::eR16G16Unorm},
    {Prospero::BufferFormat::k16_16SNorm, vk::Format::eR16G16Snorm},
    {Prospero::BufferFormat::k16_16UScaled, vk::Format::eR16G16Uscaled},
    {Prospero::BufferFormat::k16_16SScaled, vk::Format::eR16G16Sscaled},
    {Prospero::BufferFormat::k16_16UInt, vk::Format::eR16G16Uint},
    {Prospero::BufferFormat::k16_16SInt, vk::Format::eR16G16Sint},
    {Prospero::BufferFormat::k16_16Float, vk::Format::eR16G16Sfloat},
    {Prospero::BufferFormat::k11_11_10Float, vk::Format::eB10G11R11UfloatPack32},
    {Prospero::BufferFormat::k10_10_10_2UNorm, vk::Format::eA2B10G10R10UnormPack32},
    {Prospero::BufferFormat::k10_10_10_2SNorm, vk::Format::eA2B10G10R10SnormPack32},
    {Prospero::BufferFormat::k10_10_10_2UScaled, vk::Format::eA2B10G10R10UscaledPack32},
    {Prospero::BufferFormat::k10_10_10_2UInt, vk::Format::eA2B10G10R10UintPack32},
    {Prospero::BufferFormat::k8_8_8_8UNorm, vk::Format::eR8G8B8A8Unorm},
    {Prospero::BufferFormat::k8_8_8_8SNorm, vk::Format::eR8G8B8A8Snorm},
    {Prospero::BufferFormat::k8_8_8_8UScaled, vk::Format::eR8G8B8A8Uscaled},
    {Prospero::BufferFormat::k8_8_8_8SScaled, vk::Format::eR8G8B8A8Sscaled},
    {Prospero::BufferFormat::k8_8_8_8UInt, vk::Format::eR8G8B8A8Uint},
    {Prospero::BufferFormat::k8_8_8_8SInt, vk::Format::eR8G8B8A8Sint},
    {Prospero::BufferFormat::k32_32UInt, vk::Format::eR32G32Uint},
    {Prospero::BufferFormat::k32_32SInt, vk::Format::eR32G32Sint},
    {Prospero::BufferFormat::k32_32Float, vk::Format::eR32G32Sfloat},
    {Prospero::BufferFormat::k16_16_16_16UNorm, vk::Format::eR16G16B16A16Unorm},
    {Prospero::BufferFormat::k16_16_16_16SNorm, vk::Format::eR16G16B16A16Snorm},
    {Prospero::BufferFormat::k16_16_16_16UScaled, vk::Format::eR16G16B16A16Uscaled},
    {Prospero::BufferFormat::k16_16_16_16SScaled, vk::Format::eR16G16B16A16Sscaled},
    {Prospero::BufferFormat::k16_16_16_16UInt, vk::Format::eR16G16B16A16Uint},
    {Prospero::BufferFormat::k16_16_16_16SInt, vk::Format::eR16G16B16A16Sint},
    {Prospero::BufferFormat::k16_16_16_16Float, vk::Format::eR16G16B16A16Sfloat},
    {Prospero::BufferFormat::k32_32_32UInt, vk::Format::eR32G32B32Uint},
    {Prospero::BufferFormat::k32_32_32SInt, vk::Format::eR32G32B32Sint},
    {Prospero::BufferFormat::k32_32_32Float, vk::Format::eR32G32B32Sfloat},
    {Prospero::BufferFormat::k32_32_32_32UInt, vk::Format::eR32G32B32A32Uint},
    {Prospero::BufferFormat::k32_32_32_32SInt, vk::Format::eR32G32B32A32Sint},
    {Prospero::BufferFormat::k32_32_32_32Float, vk::Format::eR32G32B32A32Sfloat},
    // 8 and 8_8 sRGB views are optional in Vulkan, so these images store UNorm.
    {Prospero::BufferFormat::k8Srgb, vk::Format::eR8Unorm},
    {Prospero::BufferFormat::k8_8Srgb, vk::Format::eR8G8Unorm},
    {Prospero::BufferFormat::k8_8_8_8Srgb, vk::Format::eR8G8B8A8Srgb},
    {Prospero::BufferFormat::k9_9_9_5Float, vk::Format::eE5B9G9R9UfloatPack32},
    {Prospero::BufferFormat::k5_6_5UNorm, vk::Format::eB5G6R5UnormPack16},
    {Prospero::BufferFormat::k5_5_5_1UNorm, vk::Format::eA1R5G5B5UnormPack16},
    {Prospero::BufferFormat::k1_5_5_5UNorm, vk::Format::eR5G5B5A1UnormPack16},
    {Prospero::BufferFormat::k4_4_4_4UNorm, vk::Format::eR4G4B4A4UnormPack16},
    {Prospero::BufferFormat::kBc1UNorm, vk::Format::eBc1RgbaUnormBlock},
    {Prospero::BufferFormat::kBc1Srgb, vk::Format::eBc1RgbaSrgbBlock},
    {Prospero::BufferFormat::kBc2UNorm, vk::Format::eBc2UnormBlock},
    {Prospero::BufferFormat::kBc2Srgb, vk::Format::eBc2SrgbBlock},
    {Prospero::BufferFormat::kBc3UNorm, vk::Format::eBc3UnormBlock},
    {Prospero::BufferFormat::kBc3Srgb, vk::Format::eBc3SrgbBlock},
    {Prospero::BufferFormat::kBc4UNorm, vk::Format::eBc4UnormBlock},
    {Prospero::BufferFormat::kBc4SNorm, vk::Format::eBc4SnormBlock},
    {Prospero::BufferFormat::kBc5UNorm, vk::Format::eBc5UnormBlock},
    {Prospero::BufferFormat::kBc5SNorm, vk::Format::eBc5SnormBlock},
    {Prospero::BufferFormat::kBc6UFloat, vk::Format::eBc6HUfloatBlock},
    {Prospero::BufferFormat::kBc6SFloat, vk::Format::eBc6HSfloatBlock},
    {Prospero::BufferFormat::kBc7UNorm, vk::Format::eBc7UnormBlock},
    {Prospero::BufferFormat::kBc7Srgb, vk::Format::eBc7SrgbBlock},
};

constexpr auto MakeFormatLookup() {
	constexpr auto kMaxFormat = static_cast<size_t>(Prospero::BufferFormat::kBc7Srgb);
	std::array<vk::Format, kMaxFormat + 1> lookup {};
	lookup.fill(vk::Format::eUndefined);
	for (const auto& mapping: kFormatMappings) {
		lookup[static_cast<size_t>(mapping.guest)] = mapping.host;
	}
	return lookup;
}

constexpr auto kFormatLookup = MakeFormatLookup();

} // namespace

vk::Format VulkanFormat(Prospero::BufferFormat guest_format) {
	const auto index = static_cast<size_t>(guest_format);
	return index < kFormatLookup.size() ? kFormatLookup[index] : vk::Format::eUndefined;
}

namespace {

std::atomic<VkDevice> g_fault_device {VK_NULL_HANDLE};
// The device fault report is read from the driver once (it may not keep it) and the text is given
// to every caller: several threads see the loss together and any one of them may be the report
// that reaches the log first, or be inside a SoftExitScope that drops its report. The others wait
// for the first reader up to FAULT_REPORT_WAIT, so a driver stuck in the query cannot hold every
// fatal report back.
std::timed_mutex           g_fault_report_mutex;
std::optional<std::string> g_fault_report;
constexpr auto             FAULT_REPORT_WAIT = std::chrono::seconds(2);

std::string_view DescriptionText(const char* text, size_t capacity) {
	return {text, strnlen(text, capacity)};
}

std::string DeviceFaultReport(vk::Device device) {
	vk::DeviceFaultCountsEXT counts {};
	auto                     result = device.getFaultInfoEXT(&counts, nullptr);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		return fmt::format("Device fault: {}\n", VulkanFailureText("vkGetDeviceFaultInfoEXT", result));
	}
	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	// The vendor's binary crash dump is not read.
	counts.vendorBinarySize = 0;
	vk::DeviceFaultInfoEXT info {};
	info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos  = vendors.empty() ? nullptr : vendors.data();
	result             = device.getFaultInfoEXT(&counts, &info);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		return fmt::format("Device fault: {}\n", VulkanFailureText("vkGetDeviceFaultInfoEXT", result));
	}
	std::string report = fmt::format(
	    "Device fault: {}\n", DescriptionText(info.description.data(), VK_MAX_DESCRIPTION_SIZE));
	for (uint32_t i = 0; i < counts.addressInfoCount && i < addresses.size(); i++) {
		const auto& address = addresses[i];
		report += fmt::format("Device fault: address {} 0x{:016x} precision=0x{:x}\n",
		                      vk::to_string(address.addressType), address.reportedAddress,
		                      address.addressPrecision);
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount && i < vendors.size(); i++) {
		const auto& vendor = vendors[i];
		report += fmt::format("Device fault: vendor \"{}\" code=0x{:x} data=0x{:x}\n",
		                      DescriptionText(vendor.description.data(), VK_MAX_DESCRIPTION_SIZE),
		                      vendor.vendorFaultCode, vendor.vendorFaultData);
	}
	return report;
}

} // namespace

void EnableDeviceFaultReports(vk::Device device) {
	g_fault_device.store(static_cast<VkDevice>(device), std::memory_order_release);
}

std::string DeviceLostDetails(vk::Result result) {
	if (result != vk::Result::eErrorDeviceLost) {
		return {};
	}
	const auto device = g_fault_device.load(std::memory_order_acquire);
	if (device == VK_NULL_HANDLE || VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT == nullptr) {
		return "Device lost: no VK_EXT_device_fault report (--device-fault-report enables it on "
		       "drivers that support it)\n";
	}
	std::unique_lock lock(g_fault_report_mutex, std::defer_lock);
	if (!lock.try_lock_for(FAULT_REPORT_WAIT)) {
		return "Device lost: another thread is still reading the device fault report\n";
	}
	if (!g_fault_report.has_value()) {
		g_fault_report = DeviceFaultReport(vk::Device(device));
	}
	return *g_fault_report;
}

void RequireVulkanSuccess(vk::Result result, const char* operation) {
	if (result != vk::Result::eSuccess) {
		EXIT("%s\n%s", VulkanFailureText(operation, result).c_str(),
		     DeviceLostDetails(result).c_str());
	}
}

vk::ShaderModule CompileSPV(std::span<const uint32_t> code, vk::Device device) {
	vk::ShaderModuleCreateInfo create_info {};
	create_info.codeSize    = code.size_bytes();
	create_info.pCode       = code.data();
	vk::ShaderModule module = nullptr;
	RequireVulkanSuccess(device.createShaderModule(&create_info, nullptr, &module),
	                     "create SPIR-V shader module");
	return module;
}

vk::Pipeline CreateComputePipeline(vk::Device device, std::span<const uint32_t> code,
                                   vk::PipelineLayout layout, const char* operation) {
	const auto                    shader = CompileSPV(code, device);
	vk::ComputePipelineCreateInfo create {};
	create.stage.stage    = vk::ShaderStageFlagBits::eCompute;
	create.stage.module   = shader;
	create.stage.pName    = "main";
	create.layout         = layout;
	vk::Pipeline pipeline = nullptr;
	const auto   result   = device.createComputePipelines(nullptr, 1, &create, nullptr, &pipeline);
	device.destroyShaderModule(shader, nullptr);
	RequireVulkanSuccess(result, operation);
	return pipeline;
}

} // namespace Libs::Graphics
