#ifndef KYTY_COMMON_RAM_OPTIONS_H_
#define KYTY_COMMON_RAM_OPTIONS_H_

// gta6-prep: ram
// --ram-profile and what it amounts to, and the text of the "RAM" lines of the log contract
// (Common::RamLog writes them). Pure and header-only (ram_policy_tests checks it without a GPU).

#include "common/vramOptions.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Config {

// --ram-profile: Normal keeps every built-in value; Low bundles the settings below for a PC with
// about 8 GB of RAM. The emulator never picks it by itself: a game that works today on such a PC
// keeps its behaviour (tools/preflight passes low on a PC with less than 14 GiB).
enum class RamProfile : uint8_t { Normal, Low };

// The settings in effect. The defaults are the values the emulator had before the profile
// existed; each place that reads one static_asserts its old constant against them.
struct RamSettings {
	RamProfile profile = RamProfile::Normal;
	// BufferCache's host-visible rings in MiB: uploads (the staging ring) and readbacks.
	uint32_t upload_ring_mb   = 512;
	uint32_t download_ring_mb = 64;
	// PipelineCache's graphics program compile workers and async pipeline threads.
	uint32_t compile_workers        = 6;
	uint32_t async_pipeline_threads = 3;
	// The threads of the shader store's WarmUp and of the pipeline manifest at most; 0: no bound
	// (Common::ParallelThreads(), half the hardware threads).
	uint32_t parallel_threads = 0;
	// Upper bound of the automatic pipeline helper count, applied after the VRAM profile's;
	// nullopt: none. --pipeline-helpers and KYTY_PIPELINE_HELPERS still win.
	std::optional<uint32_t> max_pipeline_helpers;
	// How often the driver pipeline cache is written, and how soon after a slow pipeline
	// creation at the earliest, in seconds.
	uint32_t cache_save_seconds      = 60;
	uint32_t slow_cache_save_seconds = 20;
	// Guest stacks the emulator maps fresh are cleared as reused ones are. A fresh one is a new
	// commit, which the host hands out zeroed; reused stacks are always cleared.
	bool clear_fresh_stacks = true;
	// A deleted image that VMA placed outside device-local memory may go into the recycled-image
	// pool, which keeps its (pinned host) memory.
	bool recycle_spilled_images = true;

	constexpr bool operator==(const RamSettings&) const = default;
};

constexpr RamSettings ResolveRamSettings(RamProfile profile) {
	RamSettings settings;
	settings.profile = profile;
	if (profile == RamProfile::Low) {
		settings.upload_ring_mb          = 128;
		settings.download_ring_mb        = 32;
		settings.compile_workers         = 3;
		settings.async_pipeline_threads  = 2;
		settings.parallel_threads        = 2;
		settings.max_pipeline_helpers    = 0;
		settings.cache_save_seconds      = 180;
		settings.slow_cache_save_seconds = 60;
		settings.clear_fresh_stacks      = false;
		settings.recycle_spilled_images  = false;
	}
	return settings;
}

constexpr std::string_view RamProfileName(RamProfile profile) {
	return profile == RamProfile::Low ? "low" : "normal";
}

constexpr std::optional<RamProfile> ParseRamProfile(std::string_view text) {
	if (text == "normal") return RamProfile::Normal;
	if (text == "low") return RamProfile::Low;
	return std::nullopt;
}

// --ram-profile low implies --oom-recovery: a failed allocation frees what it can and retries past
// the budget instead of closing the emulator. --no-oom-recovery still wins (ResolveVramSettings()).
constexpr VramRequest ApplyRamProfile(VramRequest request, RamProfile profile) {
	if (profile == RamProfile::Low) {
		request.oom_recovery = true;
	}
	return request;
}

// The automatic pipeline helper count once the RAM profile bounds it (nullopt: no bound), after
// MemoryPolicy::BoundPipelineHelpers() applied the VRAM profile's.
constexpr uint32_t BoundHelpersByRam(uint32_t count, std::optional<uint32_t> bound) {
	return bound.has_value() ? std::min(count, *bound) : count;
}

// The "RAM" lines. Sizes are bytes here and MiB (truncated) in the text; "?" is unknown.
inline constexpr uint64_t RamMiB = uint64_t {1} << 20u;
inline constexpr uint64_t RamGiB = uint64_t {1} << 30u;

// A line built in a caller's buffer, so that the report of a fatal error (RamLog::WriteExitLine())
// formats one without allocating. Text past the buffer is cut.
class RamLineWriter {
public:
	RamLineWriter(char* data, size_t size) noexcept: m_data(data), m_size(size) {}

	void Text(std::string_view text) noexcept {
		const auto count = std::min(text.size(), m_size - m_length);
		std::copy_n(text.data(), count, m_data + m_length);
		m_length += count;
	}
	void Number(uint64_t value) noexcept {
		char       digits[24];
		const auto result = std::to_chars(digits, digits + sizeof(digits), value);
		Text(std::string_view(digits, static_cast<size_t>(result.ptr - digits)));
	}
	void MiB(std::optional<uint64_t> bytes) noexcept {
		if (bytes.has_value()) {
			Number(*bytes / RamMiB);
		} else {
			Text("?");
		}
	}
	// A rate per second, rounded.
	void Rate(std::optional<double> value) noexcept {
		if (value.has_value() && *value >= 0.0) {
			Number(static_cast<uint64_t>(*value + 0.5));
		} else {
			Text("?");
		}
	}
	[[nodiscard]] std::string_view View() const noexcept { return {m_data, m_length}; }

private:
	char*  m_data;
	size_t m_size;
	size_t m_length = 0;
};

// What a commit charge `used` leaves below `limit`: 0 past it (Linux overcommits), nullopt when
// either is unknown.
constexpr std::optional<uint64_t> CommitHeadroom(std::optional<uint64_t> used,
                                                 std::optional<uint64_t> limit) {
	if (!used.has_value() || !limit.has_value()) {
		return std::nullopt;
	}
	return *limit > *used ? *limit - *used : 0;
}

// The "RAM:" line, once at startup with the guest memory section committed.
struct RamStartup {
	std::optional<uint64_t> physical;     // the RAM the OS sees
	std::optional<uint64_t> available;    // of it, available now
	std::optional<uint64_t> commit_used;  // the system's commit charge
	std::optional<uint64_t> commit_limit; // Windows: RAM plus the page files
	std::optional<uint64_t> page_file;    // the page files (swap off Windows)
	// Windows has no figure for the page files' size: EstimatePageFile().
	bool       page_file_estimated = false;
	uint64_t   guest               = 0; // the guest memory section
	RamProfile profile             = RamProfile::Normal;
};

// Windows' page files from the commit limit less the RAM; 0 when the limit is below the RAM, as it
// is without a page file (part of the RAM never counts towards the limit). nullopt: either unknown.
constexpr std::optional<uint64_t> EstimatePageFile(std::optional<uint64_t> commit_limit,
                                                   std::optional<uint64_t> physical) {
	if (!commit_limit.has_value() || !physical.has_value()) {
		return std::nullopt;
	}
	return *commit_limit > *physical ? *commit_limit - *physical : 0;
}

// "RAM: phys=8090 MiB avail=4712 commit=18242/24474 headroom=6232 pagefile=~16384 guest=12736
// profile=low\n"
inline std::string FormatRamStartup(const RamStartup& ram) {
	char          buffer[320];
	RamLineWriter line(buffer, sizeof(buffer));
	line.Text("RAM: phys=");
	line.MiB(ram.physical);
	line.Text(" MiB avail=");
	line.MiB(ram.available);
	line.Text(" commit=");
	line.MiB(ram.commit_used);
	line.Text("/");
	line.MiB(ram.commit_limit);
	line.Text(" headroom=");
	line.MiB(CommitHeadroom(ram.commit_used, ram.commit_limit));
	line.Text(" pagefile=");
	if (ram.page_file.has_value() && ram.page_file_estimated) {
		line.Text("~");
	}
	line.MiB(ram.page_file);
	line.Text(" guest=");
	line.MiB(ram.guest);
	line.Text(" profile=");
	line.Text(RamProfileName(ram.profile));
	line.Text("\n");
	return std::string(line.View());
}

// "RAM profile: low -> upload_ring=128 download_ring=32 compile_workers=3 async_threads=2
// parallel_threads=2 helpers<=0 cache_save=180s/60s fresh_stack_clear=off recycle_spilled=off
// oom_recovery=on\n". `oom_recovery`: the VramSettings in effect.
inline std::string FormatRamProfile(const RamSettings& s, bool oom_recovery) {
	const auto  on_off = [](bool value) { return std::string(value ? "on" : "off"); };
	std::string line   = "RAM profile: " + std::string(RamProfileName(s.profile)) +
	                   " -> upload_ring=" + std::to_string(s.upload_ring_mb) +
	                   " download_ring=" + std::to_string(s.download_ring_mb) +
	                   " compile_workers=" + std::to_string(s.compile_workers) +
	                   " async_threads=" + std::to_string(s.async_pipeline_threads);
	line += " parallel_threads=" +
	        (s.parallel_threads != 0 ? std::to_string(s.parallel_threads) : std::string("auto"));
	line += s.max_pipeline_helpers.has_value()
	            ? " helpers<=" + std::to_string(*s.max_pipeline_helpers)
	            : std::string(" helpers=auto");
	line += " cache_save=" + std::to_string(s.cache_save_seconds) + "s/" +
	        std::to_string(s.slow_cache_save_seconds) + "s";
	line += " fresh_stack_clear=" + on_off(s.clear_fresh_stacks) +
	        " recycle_spilled=" + on_off(s.recycle_spilled_images) +
	        " oom_recovery=" + on_off(oom_recovery) + "\n";
	return line;
}

// Below this much commit headroom at startup (the guest memory section committed) a session is
// likely to reach the commit limit: host-visible memory and the emulator's own heap come on top,
// and the GPU driver reportedly charges commit for the video memory it allocates (unmeasured here).
inline constexpr uint64_t RamHeadroomWarning = 12 * RamGiB;
// The headroom a suggested page file aims at: up to the card's video memory, 1-4 GiB of
// host-visible memory and 2-4 GiB of emulator heap.
inline constexpr uint64_t RamHeadroomTarget = 20 * RamGiB;
inline constexpr uint64_t PageFileStep      = 4 * RamGiB;
// The "RAM usage:" thread warns once when the commit charge reaches this share of the limit.
inline constexpr uint64_t CommitWarningPercent = 95;
// After a "RAM usage:" sample with less available RAM than this, or with the commit charge at this
// share of an enforced limit, the thread syncs the session log to the disk (SyncAfterSample()).
inline constexpr uint64_t SyncAvailableBelow = 512 * RamMiB;
inline constexpr uint64_t SyncCommitPercent  = 90;

// The page file to add, in whole PageFileSteps, so that `wanted` more bytes of commit than `used`
// still leave RamHeadroomTarget below `limit`; 0 when they already do.
constexpr uint64_t PageFileToAdd(uint64_t used, uint64_t limit, uint64_t wanted) {
	const uint64_t needed = used + wanted + RamHeadroomTarget;
	if (limit >= needed) {
		return 0;
	}
	return (needed - limit + PageFileStep - 1) / PageFileStep * PageFileStep;
}

inline constexpr std::string_view PageFileHowTo =
    "fixed size, on any drive with space (System Properties > Advanced > Performance Settings > "
    "Advanced > Virtual memory > Change, untick Automatically manage paging file size for all "
    "drives, pick the drive, Custom size), then restart Windows";

// The commit limit counts the page files' current size, not what a page file that Windows manages
// may still grow to: the warnings say so.
inline constexpr std::string_view PageFileGrowth =
    "A page file that Windows manages may grow on its own; a fixed size avoids the stall while it "
    "grows.";

// The "RAM warning:" of a small commit headroom at startup; nullopt: enough headroom, or the
// commit is unknown. RamLog writes it on Windows only, where every commit counts against the limit.
inline std::optional<std::string> FormatHeadroomWarning(std::optional<uint64_t> used,
                                                        std::optional<uint64_t> limit) {
	const auto headroom = CommitHeadroom(used, limit);
	if (!headroom.has_value() || *headroom >= RamHeadroomWarning) {
		return std::nullopt;
	}
	return "RAM warning: commit headroom " + std::to_string(*headroom / RamMiB) + " MiB is below " +
	       std::to_string(RamHeadroomWarning / RamMiB) + " MiB. Add about " +
	       std::to_string(PageFileToAdd(*used, *limit, 0) / RamMiB) + " MiB of page file, " +
	       std::string(PageFileHowTo) + ". " + std::string(PageFileGrowth) +
	       " The GPU driver may also charge commit for the video memory it allocates.\n";
}

// Whether the commit charge `used` is at `percent` of `limit` or more; false when either is
// unknown.
constexpr bool CommitAtPercent(std::optional<uint64_t> used, std::optional<uint64_t> limit,
                               uint64_t percent) {
	return used.has_value() && limit.has_value() && *limit != 0 && *used * 100 >= *limit * percent;
}

constexpr bool CommitNearLimit(std::optional<uint64_t> used, std::optional<uint64_t> limit) {
	return CommitAtPercent(used, limit, CommitWarningPercent);
}

// The "RAM warning:" of the first "RAM usage:" sample at CommitNearLimit().
inline std::string FormatCommitWarning(uint64_t used, uint64_t limit) {
	const auto percent = limit != 0 ? used * 100 / limit : 0;
	return "RAM warning: commit " + std::to_string(used / RamMiB) + " MiB is " +
	       std::to_string(percent) + "% of the " + std::to_string(limit / RamMiB) +
	       " MiB limit, past which allocations fail. Close other programs, or add about " +
	       std::to_string(PageFileToAdd(used, limit, 0) / RamMiB) + " MiB of page file, " +
	       std::string(PageFileHowTo) + ". " + std::string(PageFileGrowth) + "\n";
}

// What follows "could not reserve <n> MB for guest direct memory" (GuestBackingStore): the free
// commit, the limit and the page file that fits the section `guest` with RamHeadroomTarget to
// spare. Without the page file part when the commit already has that room, and empty when the
// commit is unknown.
inline std::string FormatReserveAdvice(uint64_t guest, std::optional<uint64_t> used,
                                       std::optional<uint64_t> limit) {
	const auto headroom = CommitHeadroom(used, limit);
	if (!headroom.has_value()) {
		return {};
	}
	std::string advice = "Commit: " + std::to_string(*headroom / RamMiB) + " MiB free of a " +
	                     std::to_string(*limit / RamMiB) + " MiB limit, " +
	                     std::to_string(guest / RamMiB) + " MiB needed.";
	if (const auto add = PageFileToAdd(*used, *limit, guest); add != 0) {
		advice += " Add about " + std::to_string(add / RamMiB) + " MiB of page file, " +
		          std::string(PageFileHowTo) + ".";
	}
	return advice + "\n";
}

// The renderer's part of a "RAM usage:" line (RenderContext hands it to RamLog::SetExtraFields()).
struct RendererMemory {
	// VK_EXT_memory_budget's usage and budget of the device-local heaps and of the others.
	uint64_t local_usage  = 0;
	uint64_t local_budget = 0;
	uint64_t host_usage   = 0;
	uint64_t host_budget  = 0;
	// Allocated outside device-local memory so far (the "VRAM spill:" lines' bytes).
	uint64_t spilled = 0;
	// Guest memory the read watchers keep inaccessible now (printed rounded up to a MiB).
	uint64_t noaccess = 0;
};

// `private_bytes` is the process's private commit (Windows private bytes). The guest memory section
// is a shared section, so it is not in it.
struct RamUsage {
	uint64_t                      seconds = 0; // since RamLog::Start()
	std::optional<uint64_t>       working_set;
	std::optional<uint64_t>       private_bytes;
	std::optional<uint64_t>       commit_used;
	std::optional<uint64_t>       commit_limit;
	std::optional<uint64_t>       available;         // physical memory
	std::optional<double>         faults_per_s;      // the process's page faults, soft and hard
	std::optional<double>         hard_faults_per_s; // those that read from the disk
	std::optional<RendererMemory> renderer;
};

// Room for the longest "RAM usage:" line.
inline constexpr size_t RamUsageLineSize = 512;

// "RAM usage: t=630 ws=2843 private=9875 commit=31790/40858 avail=187 faults=3412 hard=?
// vk_local=5402/5560 vk_host=1873/3711 spilled=1504 noaccess=312\n" into `out`, cut to `size`.
// Returns the length. Allocates nothing.
inline size_t FormatRamUsage(const RamUsage& usage, char* out, size_t size) noexcept {
	RamLineWriter line(out, size);
	line.Text("RAM usage: t=");
	line.Number(usage.seconds);
	line.Text(" ws=");
	line.MiB(usage.working_set);
	line.Text(" private=");
	line.MiB(usage.private_bytes);
	line.Text(" commit=");
	line.MiB(usage.commit_used);
	line.Text("/");
	line.MiB(usage.commit_limit);
	line.Text(" avail=");
	line.MiB(usage.available);
	line.Text(" faults=");
	line.Rate(usage.faults_per_s);
	line.Text(" hard=");
	line.Rate(usage.hard_faults_per_s);
	if (usage.renderer.has_value()) {
		const auto& renderer = *usage.renderer;
		line.Text(" vk_local=");
		line.MiB(renderer.local_usage);
		line.Text("/");
		line.MiB(renderer.local_budget);
		line.Text(" vk_host=");
		line.MiB(renderer.host_usage);
		line.Text("/");
		line.MiB(renderer.host_budget);
		line.Text(" spilled=");
		line.MiB(renderer.spilled);
		line.Text(" noaccess=");
		line.Number(renderer.noaccess / RamMiB + (renderer.noaccess % RamMiB != 0 ? 1 : 0));
	}
	line.Text("\n");
	return line.View().size();
}

inline std::string FormatRamUsage(const RamUsage& usage) {
	char buffer[RamUsageLineSize];
	return std::string(buffer, FormatRamUsage(usage, buffer, sizeof(buffer)));
}

// Whether the "RAM usage:" thread syncs the session log to the disk after `usage`: when memory is
// short, which is when the whole machine is likeliest to go down with the log's last lines in the
// OS cache. Less than SyncAvailableBelow available, or the commit at SyncCommitPercent of a limit
// the OS enforces (`commit_enforced`: Windows). A sync can hold up other threads that log.
constexpr bool SyncAfterSample(const RamUsage& usage, bool commit_enforced) {
	if (usage.available.has_value() && *usage.available < SyncAvailableBelow) {
		return true;
	}
	return commit_enforced &&
	       CommitAtPercent(usage.commit_used, usage.commit_limit, SyncCommitPercent);
}

// The value of a "<key>: <n> kB" line of /proc/meminfo or /proc/self/status, in bytes; nullopt
// when no line has the key. The exit line reads them without allocating.
constexpr std::optional<uint64_t> ParseProcKiB(std::string_view text, std::string_view key) {
	for (size_t start = 0; start < text.size();) {
		const auto end  = std::min(text.find('\n', start), text.size());
		auto       line = text.substr(start, end - start);
		start           = end + 1;
		if (line.size() <= key.size() || line.substr(0, key.size()) != key ||
		    line[key.size()] != ':') {
			continue;
		}
		line.remove_prefix(key.size() + 1);
		while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
			line.remove_prefix(1);
		}
		uint64_t value  = 0;
		bool     digits = false;
		while (!line.empty() && line.front() >= '0' && line.front() <= '9') {
			value  = value * 10 + static_cast<uint64_t>(line.front() - '0');
			digits = true;
			line.remove_prefix(1);
		}
		if (!digits) {
			return std::nullopt;
		}
		return value * 1024;
	}
	return std::nullopt;
}

} // namespace Config

#endif /* KYTY_COMMON_RAM_OPTIONS_H_ */
