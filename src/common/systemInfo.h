#ifndef KYTY_COMMON_SYSTEM_INFO_H_
#define KYTY_COMMON_SYSTEM_INFO_H_

#include <cstdint>
#include <string>

namespace Common {

struct SystemInfo {
	std::string ProcessorName;
	// Physical memory of the host; 0 when unknown.
	uint64_t    TotalMemoryBytes = 0;
	// The host OS and its version, e.g. "Windows 10.0.22631" or "Linux 6.8.0-45-generic".
	std::string OsName;
};

[[nodiscard]] SystemInfo GetSystemInfo();

} // namespace Common

#endif /* KYTY_COMMON_SYSTEM_INFO_H_ */
