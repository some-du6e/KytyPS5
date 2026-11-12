// Pure checks of the crash-log diagnostics text (gta6-prep log contract): no GPU, no files
// outside a temporary folder.
#include "graphics/host_gpu/vulkanDiag.h"

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

} // namespace

int main() {
  TestVulkanFailureText();
  std::printf("DiagTests: ok\n");
  return 0;
}
