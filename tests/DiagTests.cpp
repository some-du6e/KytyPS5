// Pure checks of the crash-log diagnostics text (gta6-prep log contract): no GPU, no files
// outside a temporary folder.
#include "graphics/host_gpu/vulkanDiag.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "DiagTests: failed: %s\n", message);
    std::abort();
  }
}

void CheckEqual(const std::string &actual, std::string_view expected,
                const char *message) {
  if (actual != expected) {
    std::fprintf(stderr, "DiagTests: failed: %s: got \"%s\", expected \"%.*s\"\n",
                 message, actual.c_str(), static_cast<int>(expected.size()),
                 expected.data());
    std::abort();
  }
}

void TestVulkanFailureText() {
  using Libs::Graphics::VulkanFailureText;
  CheckEqual(VulkanFailureText("vkWaitSemaphores", vk::Result::eErrorDeviceLost),
             "vkWaitSemaphores failed: ErrorDeviceLost (-4)",
             "device lost text");
  CheckEqual(VulkanFailureText("vmaCreateImage",
                               vk::Result::eErrorOutOfDeviceMemory),
             "vmaCreateImage failed: ErrorOutOfDeviceMemory (-2)",
             "out of device memory text");
  CheckEqual(VulkanFailureText("vkQueueSubmit", vk::Result::eErrorOutOfHostMemory),
             "vkQueueSubmit failed: ErrorOutOfHostMemory (-1)",
             "out of host memory text");
  CheckEqual(VulkanFailureText("vkAcquireNextImageKHR", vk::Result::eTimeout),
             "vkAcquireNextImageKHR failed: Timeout (2)",
             "positive result text");
}

void TestDriverVersion() {
  using Libs::Graphics::DriverVersionString;
  // NVIDIA 566.36 as the driver reports it: 566 << 22 | 36 << 14.
  CheckEqual(DriverVersionString(0x10de, (566u << 22u) | (36u << 14u)),
             "566.36.0.0", "NVIDIA 566.36");
  CheckEqual(DriverVersionString(0x10de, (581u << 22u) | (57u << 14u) |
                                             (3u << 6u) | 5u),
             "581.57.3.5", "NVIDIA with all four fields");
  CheckEqual(DriverVersionString(0x10de, 0xffffffffu), "1023.255.255.63",
             "NVIDIA field widths");
  // AMD and Mesa use VK_MAKE_VERSION.
  CheckEqual(DriverVersionString(0x1002, VK_MAKE_VERSION(2, 0, 341)),
             "2.0.341", "AMD driver version");
  CheckEqual(DriverVersionString(0x8086, VK_MAKE_VERSION(25, 2, 7)),
             "25.2.7", "Mesa driver version");
  CheckEqual(Libs::Graphics::ApiVersionString(VK_MAKE_API_VERSION(0, 1, 4, 312)),
             "1.4.312", "API version");
}

vk::PhysicalDeviceMemoryProperties MakeMemory(uint64_t vram, uint64_t bar,
                                              bool resizable_bar) {
  constexpr uint64_t MiB = 1024ull * 1024;
  vk::PhysicalDeviceMemoryProperties memory{};
  memory.memoryHeapCount = 3;
  memory.memoryHeaps[0].size = vram * MiB;
  memory.memoryHeaps[0].flags = vk::MemoryHeapFlagBits::eDeviceLocal;
  memory.memoryHeaps[1].size = 16384 * MiB;
  memory.memoryHeaps[2].size = bar * MiB;
  memory.memoryHeaps[2].flags = vk::MemoryHeapFlagBits::eDeviceLocal;
  memory.memoryTypeCount = 3;
  memory.memoryTypes[0].heapIndex = 0;
  memory.memoryTypes[0].propertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
  memory.memoryTypes[1].heapIndex = 1;
  memory.memoryTypes[1].propertyFlags = vk::MemoryPropertyFlagBits::eHostVisible |
                                        vk::MemoryPropertyFlagBits::eHostCoherent;
  memory.memoryTypes[2].heapIndex = resizable_bar ? 0 : 2;
  memory.memoryTypes[2].propertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal |
                                        vk::MemoryPropertyFlagBits::eHostVisible;
  return memory;
}

void TestGpuMemorySummary() {
  using Libs::Graphics::SummarizeGpuMemory;
  // RTX 3050 8 GB without Resizable BAR: the 256 MiB BAR heap is not video memory.
  auto summary = SummarizeGpuMemory(MakeMemory(8192, 256, false));
  Check(summary.vram_mib == 8192, "vram without ReBAR");
  Check(summary.bar_mib == 256, "bar without ReBAR");
  summary = SummarizeGpuMemory(MakeMemory(8192, 256, true));
  Check(summary.vram_mib == 8192, "vram with ReBAR");
  Check(summary.bar_mib == 8192, "bar with ReBAR");
  vk::PhysicalDeviceMemoryProperties none{};
  summary = SummarizeGpuMemory(none);
  Check(summary.vram_mib == 0 && summary.bar_mib == 0, "no heaps");
}

void TestGpuIdentityLine() {
  Libs::Graphics::GpuIdentity gpu;
  gpu.name = "NVIDIA GeForce RTX 3050";
  gpu.vendor_id = 0x10de;
  gpu.device_id = 0x2507;
  gpu.driver_version = (566u << 22u) | (36u << 14u);
  gpu.api_version = VK_MAKE_API_VERSION(0, 1, 4, 303);
  gpu.driver_id = "NvidiaProprietary";
  gpu.memory = {8192, 256};
  CheckEqual(Libs::Graphics::GpuIdentityLine(gpu),
             "GPU: NVIDIA GeForce RTX 3050 vendor=0x10de device=0x2507 "
             "driver=566.36.0.0 api=1.4.303 driverID=NvidiaProprietary "
             "vram=8192 MiB bar=256 MiB\n",
             "GPU line");
  CheckEqual(vk::to_string(vk::DriverId::eNvidiaProprietary),
             "NvidiaProprietary", "VkDriverId name");
}

} // namespace

int main() {
  TestVulkanFailureText();
  TestDriverVersion();
  TestGpuMemorySummary();
  TestGpuIdentityLine();
  std::printf("DiagTests: ok\n");
  return 0;
}
