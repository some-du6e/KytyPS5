#include "common/systemInfo.h"

#include "common/assert.h"
#include "common/config.h"
#include "cpuinfo.h"

#include <fmt/format.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#else
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

namespace Common {

static uint64_t TotalMemory() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORYSTATUSEX status {};
	status.dwLength = sizeof(status);
	return GlobalMemoryStatusEx(&status) != 0 ? status.ullTotalPhys : 0;
#elif defined(__APPLE__)
	uint64_t size   = 0;
	size_t   length = sizeof(size);
	return sysctlbyname("hw.memsize", &size, &length, nullptr, 0) == 0 ? size : 0;
#else
	const long pages     = sysconf(_SC_PHYS_PAGES);
	const long page_size = sysconf(_SC_PAGESIZE);
	return pages > 0 && page_size > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size)
	                                  : 0;
#endif
}

static std::string OsName() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// GetVersionEx reports the version the manifest asks for; ntdll reports the real one.
	struct OsVersion {
		ULONG size;
		ULONG major;
		ULONG minor;
		ULONG build;
		ULONG platform;
		WCHAR service_pack[128];
	};
	using RtlGetVersionFunc = LONG(WINAPI*)(OsVersion*);
	using WineVersionFunc   = const char*(__cdecl*)();
	const auto ntdll        = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr) {
		return "Windows";
	}
	std::string name = "Windows";
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
	if (auto rtl_get_version =
	        reinterpret_cast<RtlGetVersionFunc>(GetProcAddress(ntdll, "RtlGetVersion"));
	    rtl_get_version != nullptr) {
		OsVersion version {};
		version.size = sizeof(version);
		if (rtl_get_version(&version) == 0) {
			name += fmt::format(" {}.{}.{}", version.major, version.minor, version.build);
		}
	}
	// Wine and Proton users report as Windows.
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
	if (auto wine_version = reinterpret_cast<WineVersionFunc>(GetProcAddress(ntdll, "wine_get_version"));
	    wine_version != nullptr) {
		name += fmt::format(" (Wine {})", wine_version());
	}
	return name;
#else
	utsname host {};
	if (uname(&host) != 0) {
		return "unknown";
	}
	return fmt::format("{} {}", host.sysname, host.release);
#endif
}

SystemInfo GetSystemInfo() {
	static const bool initialized = cpuinfo_initialize();
	EXIT_IF(!initialized);

	const auto* package = cpuinfo_get_package(0);
	EXIT_IF(package == nullptr);

	return {.ProcessorName = package->name, .TotalMemoryBytes = TotalMemory(), .OsName = OsName()};
}

} // namespace Common
