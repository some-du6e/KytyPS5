#include "common/ramLog.h"

#include "common/config.h"
#include "common/emulatorConfig.h"
#include "common/hostStats.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "common/timer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// clang-format off
#include <psapi.h>
// clang-format on
#elif !defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Common::RamLog {

namespace {

// How often the thread writes a "RAM usage:" line.
constexpr auto UsageInterval = std::chrono::seconds {30};

// Windows refuses a commit past its limit (RAM plus the page files). Linux overcommits, so its
// Committed_AS may pass CommitLimit harmlessly: the warnings are for Windows.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
constexpr bool CommitLimitEnforced = true;
#else
constexpr bool CommitLimitEnforced = false;
#endif

// Never destroyed: the thread of the lines may still run while exit() destroys statics, as the
// session log is never closed either (Log).
struct State {
	std::mutex                              fields_mutex;
	std::function<Config::RendererMemory()> fields;
};

State& GetState() {
	static auto* state = new State;
	return *state;
}

// SteadyNs() at Start(); 0 before.
std::atomic<uint64_t> g_started_ns {0};

uint64_t SecondsSinceStart() {
	const auto started = g_started_ns.load(std::memory_order_relaxed);
	const auto now     = SteadyNs();
	return now > started ? (now - started) / 1'000'000'000u : 0;
}

std::optional<uint64_t> Available(const HostStats& stats) {
	if (!stats.system_memory_total.has_value() || !stats.system_memory_used.has_value() ||
	    *stats.system_memory_used > *stats.system_memory_total) {
		return std::nullopt;
	}
	return *stats.system_memory_total - *stats.system_memory_used;
}

Config::RamUsage UsageOf(const HostStats& stats) {
	Config::RamUsage usage;
	usage.seconds           = SecondsSinceStart();
	usage.working_set       = stats.process_resident;
	usage.private_bytes     = stats.process_private;
	usage.commit_used       = stats.commit_used;
	usage.commit_limit      = stats.commit_limit;
	usage.available         = Available(stats);
	usage.faults_per_s      = stats.page_faults_per_s;
	usage.hard_faults_per_s = stats.page_ins_per_s;
	return usage;
}

[[noreturn]] void RunUsageThread() {
	Thread::LowerCurrentPriority();
	HostStatsSampler sampler;
	// The rates of the first line come from this sample.
	(void)sampler.Sample();
	bool warned = false;
	for (;;) {
		std::this_thread::sleep_for(UsageInterval);
		auto usage = UsageOf(sampler.Sample());
		{
			auto&           state = GetState();
			std::lock_guard lock(state.fields_mutex);
			if (state.fields) {
				usage.renderer = state.fields();
			}
		}
		Log::WriteToConsoleAndLog(Config::FormatRamUsage(usage));
		if (CommitLimitEnforced && !warned &&
		    Config::CommitNearLimit(usage.commit_used, usage.commit_limit)) {
			warned = true;
			Log::WriteToConsoleAndLog(
			    Config::FormatCommitWarning(*usage.commit_used, *usage.commit_limit));
		}
		// Through the OS cache to the disk when memory is short, so that the last line before a
		// crash of the whole machine (a blue screen) stays in the file. Not after every sample: a
		// sync may hold up the threads that log meanwhile.
		if (Config::SyncAfterSample(usage, CommitLimitEnforced)) {
			Log::SyncSessionLog();
		}
	}
}

// What the process and the host have now, without allocating (WriteExitLine()).
Config::RamUsage SampleNow() {
	Config::RamUsage usage;
	usage.seconds = SecondsSinceStart();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	PROCESS_MEMORY_COUNTERS_EX process {};
	if (K32GetProcessMemoryInfo(GetCurrentProcess(),
	                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),
	                            sizeof(process)) != 0) {
		usage.working_set   = process.WorkingSetSize;
		usage.private_bytes = process.PrivateUsage;
	}
	MEMORYSTATUSEX system {};
	system.dwLength = sizeof(system);
	if (GlobalMemoryStatusEx(&system) != 0) {
		usage.available    = system.ullAvailPhys;
		usage.commit_limit = system.ullTotalPageFile;
		usage.commit_used  = system.ullTotalPageFile - system.ullAvailPageFile;
	}
#elif !defined(__APPLE__)
	char       text[4096];
	const auto read_file = [&text](const char* path) {
		const int file = ::open(path, O_RDONLY | O_CLOEXEC);
		if (file < 0) {
			return std::string_view {};
		}
		const auto size = ::read(file, text, sizeof(text));
		::close(file);
		return size > 0 ? std::string_view(text, static_cast<size_t>(size)) : std::string_view {};
	};
	const auto meminfo = read_file("/proc/meminfo");
	usage.available    = Config::ParseProcKiB(meminfo, "MemAvailable");
	usage.commit_limit = Config::ParseProcKiB(meminfo, "CommitLimit");
	usage.commit_used  = Config::ParseProcKiB(meminfo, "Committed_AS");
	// Reuses the buffer: meminfo is read above.
	usage.working_set = Config::ParseProcKiB(read_file("/proc/self/status"), "VmRSS");
#endif
	return usage;
}

} // namespace

void Start(uint64_t guest) {
	uint64_t expected = 0;
	if (!g_started_ns.compare_exchange_strong(expected, std::max<uint64_t>(SteadyNs(), 1))) {
		return;
	}
	HostStatsSampler   sampler;
	const auto         stats = sampler.Sample();
	const auto&        ram   = Config::GetRamSettings();
	Config::RamStartup startup;
	startup.physical     = stats.system_memory_total;
	startup.available    = Available(stats);
	startup.commit_used  = stats.commit_used;
	startup.commit_limit = stats.commit_limit;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// "~0" without a page file.
	startup.page_file = Config::EstimatePageFile(stats.commit_limit, stats.system_memory_total);
	startup.page_file_estimated = startup.page_file.has_value();
#else
	startup.page_file = stats.swap_total;
#endif
	startup.guest   = guest;
	startup.profile = ram.profile;
	Log::WriteToConsoleAndLog(Config::FormatRamStartup(startup));
	if (ram.profile == Config::RamProfile::Low) {
		Log::WriteToConsoleAndLog(
		    Config::FormatRamProfile(ram, Config::GetVramSettings().oom_recovery));
	}
	if constexpr (CommitLimitEnforced) {
		if (const auto warning =
		        Config::FormatHeadroomWarning(stats.commit_used, stats.commit_limit);
		    warning.has_value()) {
			Log::WriteToConsoleAndLog(*warning);
		}
	}
	if (Log::SessionLogOpen()) {
		std::thread(RunUsageThread).detach();
	}
}

void SetExtraFields(std::function<Config::RendererMemory()> fields) {
	auto&           state = GetState();
	std::lock_guard lock(state.fields_mutex);
	state.fields = std::move(fields);
}

void WriteExitLine() {
	if (g_started_ns.load(std::memory_order_relaxed) == 0) {
		return;
	}
	char       line[Config::RamUsageLineSize];
	const auto size = Config::FormatRamUsage(SampleNow(), line, sizeof(line));
	Log::WriteToConsoleAndLog(std::string_view(line, size));
}

} // namespace Common::RamLog
