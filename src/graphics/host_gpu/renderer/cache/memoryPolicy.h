#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_MEMORYPOLICY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_MEMORYPOLICY_H_

// gta6-prep: vram
// The video memory arithmetic of the renderer, without Vulkan: collection thresholds, the heap
// limits of --vram-limit-mb, the recycled-image pool and BLAS arena budgets, the spill log
// cadence and the size check of merged guest buffers. vram_policy_tests checks it.

#include "common/vramOptions.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Libs::Graphics::MemoryPolicy {

inline constexpr uint64_t MiB = uint64_t {1} << 20u;
inline constexpr uint64_t GiB = uint64_t {1} << 30u;

// Device memory (in bytes, whole process) from which each cache collects: the texture cache
// above `texture_trigger`, pressured (reading GPU-written images back before it frees them)
// above `texture_pressure`, aggressive above `texture_critical`; the buffer cache above
// `buffer_trigger`, aggressive (freeing GPU-written buffers too) above `buffer_critical`.
struct GcThresholds {
	uint64_t texture_trigger  = 0;
	uint64_t texture_pressure = 0;
	uint64_t texture_critical = 0;
	uint64_t buffer_trigger   = 0;
	uint64_t buffer_critical  = 0;

	constexpr bool operator==(const GcThresholds&) const = default;
};

// Without VK_EXT_memory_budget the caches keep these (their member initializers).
inline constexpr GcThresholds DefaultGcThresholds {
    .texture_trigger  = 0,
    .texture_pressure = 1536 * MiB,
    .texture_critical = 3 * GiB,
    .buffer_trigger   = 1 * GiB,
    .buffer_critical  = 2 * GiB,
};

// The formulas TextureCache and BufferCache used before the policy existed, unchanged: around an
// 8 GiB anchor, so a budget B <= 8 GiB triggers at 0, is pressured from 0.4 B and critical from
// 0.8 B (floors aside).
constexpr GcThresholds LegacyGcThresholds(uint64_t total_budget) {
	constexpr int64_t gib    = static_cast<int64_t>(GiB);
	const auto        budget = static_cast<int64_t>(std::min<uint64_t>(total_budget, INT64_MAX));
	const auto        threshold = std::min<int64_t>(budget, 8 * gib);
	const auto        expected  = std::min(budget - 6 * threshold / 10, budget - gib);
	const auto        critical  = std::min(budget - 2 * threshold / 10, budget - gib / 2);
	GcThresholds      result;
	result.texture_pressure = static_cast<uint64_t>(std::max<int64_t>(expected, gib + gib / 2));
	result.texture_critical = static_cast<uint64_t>(std::max<int64_t>(critical, 3 * gib));
	result.texture_trigger  = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, 0));
	result.buffer_trigger   = static_cast<uint64_t>(std::max<int64_t>(expected, gib));
	result.buffer_critical  = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * gib));
	return result;
}

// Thousandths of the budget. Pressure and critical are what the legacy formulas give a 24 GB card
// (22.5 GiB budget: 0.79 and 0.93); the triggers start the cheap collections earlier than that card
// does but later than legacy on an 8 GB one (which collects from 0).
inline constexpr uint64_t ProportionalTextureTrigger  = 550;
inline constexpr uint64_t ProportionalTexturePressure = 780;
inline constexpr uint64_t ProportionalTextureCritical = 920;
inline constexpr uint64_t ProportionalBufferTrigger   = 700;
inline constexpr uint64_t ProportionalBufferCritical  = 920;

constexpr uint64_t Permille(uint64_t value, uint64_t permille) {
	return value / 1000 * permille + value % 1000 * permille / 1000;
}

constexpr GcThresholds ProportionalGcThresholds(uint64_t budget) {
	return {
	    .texture_trigger  = Permille(budget, ProportionalTextureTrigger),
	    .texture_pressure = Permille(budget, ProportionalTexturePressure),
	    .texture_critical = Permille(budget, ProportionalTextureCritical),
	    .buffer_trigger   = Permille(budget, ProportionalBufferTrigger),
	    .buffer_critical  = Permille(budget, ProportionalBufferCritical),
	};
}

constexpr GcThresholds ComputeGcThresholds(uint64_t budget, Config::VramGcPolicy policy) {
	return policy == Config::VramGcPolicy::Proportional ? ProportionalGcThresholds(budget)
	                                                    : LegacyGcThresholds(budget);
}

// What the next collection of the caches is asked to do on top of their thresholds (see
// GraphicContext::RequestGcPressure()). Ordered: a stronger request replaces a weaker one.
enum class GcPressure : uint8_t {
	None,
	// An allocation landed outside device-local memory (proportional policy only): the texture
	// collection runs pressured.
	Spill,
	// An allocation failed (--oom-recovery): both caches collect aggressively, down to images and
	// buffers unused for two frames.
	OutOfMemory,
};

// --vram-limit-mb: every device-local heap larger than 1 GiB is limited to `limit` bytes (VMA's
// pHeapSizeLimit). Smaller device-local heaps are the 256 MiB BAR window of a card without
// resizable BAR and stay whole, as do host heaps.
inline constexpr uint64_t NoHeapLimit     = ~uint64_t {0}; // VK_WHOLE_SIZE
inline constexpr size_t   MaxMemoryHeaps  = 16;            // VK_MAX_MEMORY_HEAPS
inline constexpr uint64_t SmallHeapBytes  = 1 * GiB;

struct HeapInfo {
	uint64_t size         = 0;
	bool     device_local = false;
};

struct HeapLimits {
	std::array<uint64_t, MaxMemoryHeaps> limits {};
	// Some heap is limited: pass `limits` to VMA.
	bool                                 any = false;
};

constexpr HeapLimits BuildHeapSizeLimits(std::span<const HeapInfo> heaps, uint64_t limit) {
	HeapLimits result;
	result.limits.fill(NoHeapLimit);
	if (limit == 0) {
		return result;
	}
	const auto count = std::min(heaps.size(), MaxMemoryHeaps);
	for (size_t i = 0; i < count; i++) {
		if (heaps[i].device_local && heaps[i].size > SmallHeapBytes && limit < heaps[i].size) {
			result.limits[i] = limit;
			result.any       = true;
		}
	}
	return result;
}

// The recycled-image pool (GraphicContext::CreateImage()).
inline constexpr uint64_t DefaultRecycledImageBytes = 256 * MiB;

constexpr uint64_t RecycledImageBytes(uint64_t budget, bool small_pool) {
	return small_pool ? std::min(DefaultRecycledImageBytes, budget / 32) : DefaultRecycledImageBytes;
}

// The BLAS memory of native ray tracing (NativeRayTracing). `requested_mb`: KYTY_RT_ARENA_MB, else
// --rt-arena-mb (0: no limit, returned as 0).
inline constexpr uint64_t RtArenaMin      = 512 * MiB;
inline constexpr uint64_t RtArenaMax      = 4 * GiB;
inline constexpr uint64_t RtArenaSmallMax = 1 * GiB;

constexpr uint64_t RtArenaBudget(uint64_t largest_device_heap, std::optional<uint64_t> requested_mb,
                                 bool small_arena) {
	if (requested_mb.has_value()) {
		return *requested_mb << 20u;
	}
	return small_arena ? std::clamp(largest_device_heap / 8, RtArenaMin, RtArenaSmallMax)
	                   : std::clamp(largest_device_heap / 4, RtArenaMin, RtArenaMax);
}

// The pipeline helper count once --vram-profile low bounds it (0: no bound).
constexpr uint32_t BoundPipelineHelpers(uint32_t automatic, uint32_t bound) {
	return bound == 0 ? automatic : std::min(automatic, bound);
}

// "VRAM spill" lines: the first spill of a kind, then each power-of-two count.
constexpr bool LogSpillAt(uint64_t count) {
	return std::has_single_bit(count);
}

// A guest buffer that merges overlapping cached buffers (BufferCache::CreateBuffer()) against the
// device limits. 0 for a limit: unknown, no check. Ordered: each value passes more limits than
// the one before (PlanMergedBuffer() compares them).
enum class MergedBufferFit : uint8_t {
	Fits,
	// Past maxMemoryAllocationSize: the driver may still allocate it.
	OverAllocationSize,
	// Past maxBufferSize: the buffer cannot be created.
	OverBufferSize,
};

struct MergedBufferPlan {
	// The growth reserved for a streaming buffer is what passes a limit: create the buffer
	// without it (the merge stays correct, later growth merges again).
	bool            drop_stream_leap = false;
	MergedBufferFit fit              = MergedBufferFit::Fits;

	constexpr bool operator==(const MergedBufferPlan&) const = default;
};

constexpr MergedBufferFit FitMergedBuffer(uint64_t size, uint64_t max_allocation,
                                          uint64_t max_buffer) {
	if (max_buffer != 0 && size > max_buffer) {
		return MergedBufferFit::OverBufferSize;
	}
	if (max_allocation != 0 && size > max_allocation) {
		return MergedBufferFit::OverAllocationSize;
	}
	return MergedBufferFit::Fits;
}

// `size_without_leap`: the size without the stream reserve (equal to `size` without one). The
// reserve is dropped only when it is what passes a limit, i.e. when the buffer without it passes
// fewer limits, and, for maxMemoryAllocationSize, only with `drop_past_allocation_size`
// (--oom-recovery, which --vram-profile low sets). A buffer past maxBufferSize cannot be created;
// one past maxMemoryAllocationSize usually can (drivers allocate past it, VMA does not enforce it),
// and without the reserve every later growth step merges and copies the whole buffer again.
constexpr MergedBufferPlan PlanMergedBuffer(uint64_t size, uint64_t size_without_leap,
                                            uint64_t max_allocation, uint64_t max_buffer,
                                            bool drop_past_allocation_size) {
	const auto fit = FitMergedBuffer(size, max_allocation, max_buffer);
	if (fit == MergedBufferFit::Fits || size_without_leap >= size) {
		return {.drop_stream_leap = false, .fit = fit};
	}
	const auto fit_without_leap = FitMergedBuffer(size_without_leap, max_allocation, max_buffer);
	if (static_cast<uint8_t>(fit_without_leap) >= static_cast<uint8_t>(fit)) {
		return {.drop_stream_leap = false, .fit = fit};
	}
	if (fit == MergedBufferFit::OverAllocationSize && !drop_past_allocation_size) {
		return {.drop_stream_leap = false, .fit = fit};
	}
	return {.drop_stream_leap = true, .fit = fit_without_leap};
}

// The "VRAM:" line of the log contract (RenderContext logs it once after renderer init). Sizes in
// bytes; printed in MiB.
struct VramBreakdown {
	uint64_t                budget        = 0;
	uint32_t                limit_mb      = 0; // the --vram-limit-mb applied to a heap; 0: off
	Config::VramProfile     profile       = Config::VramProfile::Auto;
	Config::VramGcPolicy    gc_policy     = Config::VramGcPolicy::Legacy;
	uint64_t                bda_table     = 0;
	uint64_t                fault_buffer  = 0;
	uint64_t                rings         = 0;
	uint64_t                recycle_cap   = 0;
	std::optional<uint64_t> rt_arena; // nullopt: off (no native RT); 0: no limit (prints 0)
	GcThresholds            thresholds;
};

inline std::string FormatVramBreakdown(const VramBreakdown& v) {
	const auto mib = [](uint64_t bytes) { return std::to_string(bytes >> 20u); };
	std::string line = "VRAM: budget=" + mib(v.budget) + " MiB limit=" +
	                   (v.limit_mb != 0 ? std::to_string(v.limit_mb) : std::string("off"));
	line += " profile=" + std::string(Config::VramProfileName(v.profile));
	line += " gc=" + std::string(Config::VramGcPolicyName(v.gc_policy));
	line += " fixed: bda_table=" + mib(v.bda_table) + " fault=" + mib(v.fault_buffer) +
	        " rings=" + mib(v.rings) + " recycle_cap=" + mib(v.recycle_cap) +
	        " rt_arena=" + (v.rt_arena.has_value() ? mib(*v.rt_arena) : std::string("off"));
	line += " thresholds: tex_trigger=" + mib(v.thresholds.texture_trigger) +
	        " tex_pressure=" + mib(v.thresholds.texture_pressure) +
	        " tex_critical=" + mib(v.thresholds.texture_critical) +
	        " buf_trigger=" + mib(v.thresholds.buffer_trigger) +
	        " buf_critical=" + mib(v.thresholds.buffer_critical) + "\n";
	return line;
}

} // namespace Libs::Graphics::MemoryPolicy

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_MEMORYPOLICY_H_
