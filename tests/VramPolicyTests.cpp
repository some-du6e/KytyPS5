// gta6-prep: vram
// The video memory arithmetic of the renderer and the options behind it, without a GPU.
#include "graphics/host_gpu/renderer/cache/memoryPolicy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

namespace Policy = Libs::Graphics::MemoryPolicy;

using Policy::GcThresholds;
using Policy::GiB;
using Policy::MiB;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "VramPolicyTests: failed: %s\n", message);
		std::abort();
	}
}

// The thresholds TextureCache and BufferCache computed before MemoryPolicy existed, worked out by
// hand (integer arithmetic, truncating divisions) for the budgets of common cards: the legacy
// policy must keep them bit for bit.
struct LegacyCase {
	uint64_t     budget;
	GcThresholds expected;
	const char*  message;
};

constexpr LegacyCase LegacyCases[] = {
    {0,
     {0, 1610612736, 3221225472, 1073741824, 2147483648},
     "legacy thresholds of a zero budget changed"},
    {2 * GiB,
     {0, 1610612736, 3221225472, 1073741824, 2147483648},
     "legacy thresholds of a 2 GiB budget changed"},
    {4 * GiB,
     {0, 1717986919, 3435973837, 1717986919, 3435973837},
     "legacy thresholds of a 4 GiB budget changed"},
    {13 * GiB / 2,
     {0, 2791728743, 5583457485, 2791728743, 5583457485},
     "legacy thresholds of a 6.5 GiB budget (RTX 3050 8 GB) changed"},
    {8 * GiB,
     {0, 3435973837, 6871947674, 3435973837, 6871947674},
     "legacy thresholds of an 8 GiB budget changed"},
    {12 * GiB,
     {2147483648, 7730941133, 11166914970, 7730941133, 11166914970},
     "legacy thresholds of a 12 GiB budget changed"},
    {45 * GiB / 2,
     {7784628224, 19005230285, 22441204122, 19005230285, 22441204122},
     "legacy thresholds of a 22.5 GiB budget (RTX 3090) changed"},
};

constexpr bool LegacyCasesHold() {
	for (const auto& test: LegacyCases) {
		if (Policy::LegacyGcThresholds(test.budget) != test.expected) {
			return false;
		}
	}
	return true;
}
// Also checked at compile time, so a syntax check catches a changed formula.
static_assert(LegacyCasesHold());

void TestLegacyThresholdsPinned() {
	for (const auto& test: LegacyCases) {
		Check(Policy::LegacyGcThresholds(test.budget) == test.expected, test.message);
		Check(Policy::ComputeGcThresholds(test.budget, Config::VramGcPolicy::Legacy) ==
		          test.expected,
		      "ComputeGcThresholds(Legacy) is not LegacyGcThresholds");
	}
	// Without a budget the caches keep their member initializers, which are what a zero budget
	// gives.
	Check(Policy::LegacyGcThresholds(0) == Policy::DefaultGcThresholds,
	      "the default thresholds are not the legacy ones of a zero budget");
	// A budget past INT64_MAX is clamped, not wrapped.
	const auto huge = Policy::LegacyGcThresholds(UINT64_MAX);
	Check(huge.texture_trigger > 0 && huge.texture_pressure > huge.texture_trigger &&
	          huge.texture_critical > huge.texture_pressure,
	      "an oversized budget wrapped the legacy thresholds");
}

void TestProportionalThresholds() {
	for (const uint64_t budget: {uint64_t {1} * GiB, uint64_t {4} * GiB, uint64_t {13} * GiB / 2,
	                             uint64_t {8} * GiB, uint64_t {45} * GiB / 2,
	                             uint64_t {80} * GiB}) {
		const auto t = Policy::ProportionalGcThresholds(budget);
		Check(t.texture_trigger <= t.texture_pressure && t.texture_pressure <= t.texture_critical &&
		          t.texture_critical < budget,
		      "proportional texture thresholds are not ordered below the budget");
		Check(t.buffer_trigger <= t.buffer_critical && t.buffer_critical < budget,
		      "proportional buffer thresholds are not ordered below the budget");
		Check(Policy::ComputeGcThresholds(budget, Config::VramGcPolicy::Proportional) == t,
		      "ComputeGcThresholds(Proportional) is not ProportionalGcThresholds");
	}
	// The pressured and critical levels are what a 24 GB card gets today, within 5%.
	const uint64_t budget       = 45 * GiB / 2;
	const auto     legacy       = Policy::LegacyGcThresholds(budget);
	const auto     proportional = Policy::ProportionalGcThresholds(budget);
	const auto     close        = [](uint64_t a, uint64_t b) {
		const auto difference = a > b ? a - b : b - a;
		return difference * 20 <= b;
	};
	Check(close(proportional.texture_pressure, legacy.texture_pressure) &&
	          close(proportional.texture_critical, legacy.texture_critical) &&
	          close(proportional.buffer_critical, legacy.buffer_critical),
	      "proportional thresholds stray from the 24 GB card's");
	// An 8 GB card no longer runs pressured from 40% of its budget.
	const auto small = Policy::ProportionalGcThresholds(13 * GiB / 2);
	Check(small.texture_pressure > Policy::LegacyGcThresholds(13 * GiB / 2).texture_pressure,
	      "proportional pressure on 6.5 GiB is not later than legacy");
	Check(small.texture_pressure == Policy::Permille(13 * GiB / 2, 780),
	      "proportional pressure is not 78% of the budget");
	Check(Policy::Permille(1000, 550) == 550 && Policy::Permille(UINT64_MAX, 1000) == UINT64_MAX,
	      "Permille rounds or overflows");
}

} // namespace

int main() {
	TestLegacyThresholdsPinned();
	TestProportionalThresholds();
	std::printf("VramPolicyTests: ok\n");
	return 0;
}
