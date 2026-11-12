#include "common/presets.h"

#include <algorithm>
#include <cstddef>

namespace Config {

namespace {

// For bisecting a regression of a title that worked before: every switch of the fork's global
// behaviour changes is turned off (each one also has its own --no- flag).
constexpr std::string_view SafeTokens[] = {
    "--no-async-pipelines",
    "--no-overlap-dispatches",
    "--no-defer-draw-barriers",
    "--no-masked-select-elimination",
    "--no-spirv-forwarding",
    "--no-uniform-buffers",
    "--no-lod-stats",
    "--no-keep-small-layer-targets",
    "--no-tlas-cache",
    "--pipeline-helpers",
    "0",
    "--gpu-thread-cpus",
    "off",
};

// Options that use less video memory.
// --vram-profile low already implies proportional collection thresholds, --oom-recovery, a
// smaller recycled-image pool, 4 parked images, a smaller BLAS arena and at most 2 automatic
// pipeline helpers (Config::ResolveVramSettings()); --vram-gc and --oom-recovery are repeated so
// that the "Preset:" line names them and --vram-profile auto after the preset keeps them.
// --video-out-resolution stays auto: 1080p changes what the guest is told, and no title is known
// to size its render targets by it. --rt-arena-mb is left to the profile, as native ray tracing
// is off.
constexpr std::string_view LowVramTokens[] = {
    "--no-native-raytracing",
    "--no-trinity",
    "--no-readback-tiled-images",
    "--no-readback-storage-images",
    "--no-keep-small-layer-targets",
    "--vram-profile",
    "low",
    "--vram-gc",
    "proportional",
    "--oom-recovery",
};

// An RTX 3050 with 8 GB: low-vram's options (a preset cannot name another one, so they are
// repeated here), then the NVIDIA machine's. The pipeline helpers (NVIDIA on Windows only) stay
// automatic, which --vram-profile low caps at 2: a fixed --pipeline-helpers 2 would also start 2
// on a CPU whose automatic count is 1.
constexpr std::string_view Rtx3050Tokens[] = {
    "--no-native-raytracing",
    "--no-trinity",
    "--no-readback-tiled-images",
    "--no-readback-storage-images",
    "--no-keep-small-layer-targets",
    "--vram-profile",
    "low",
    "--vram-gc",
    "proportional",
    "--oom-recovery",
    // The built-in CPUs suit a Ryzen 9 5900X; on the CPUs such a machine has they can land on
    // E-cores or on cores the game threads need.
    "--gpu-thread-cpus",
    "off",
};

constexpr Preset PresetTable[] = {
    {.name        = "safe",
     .description = "turn off the fork's global behaviour changes, to bisect a regression",
     .tokens      = SafeTokens},
    {.name = "low-vram", .description = "use less video memory", .tokens = LowVramTokens},
    {.name = "rtx3050", .description = "an RTX 3050 with 8 GB", .tokens = Rtx3050Tokens},
};

} // namespace

std::span<const Preset> Presets() {
	return PresetTable;
}

const Preset* FindPreset(std::string_view name) {
	for (const auto& preset: PresetTable) {
		if (preset.name == name) {
			return &preset;
		}
	}
	return nullptr;
}

std::string PresetNames() {
	std::string names;
	for (const auto& preset: PresetTable) {
		if (!names.empty()) {
			names += ", ";
		}
		names += preset.name;
	}
	return names;
}

std::string PresetTokenList(const Preset& preset) {
	std::string list;
	for (const auto token: preset.tokens) {
		if (!list.empty()) {
			list += ' ';
		}
		list += token;
	}
	return list;
}

std::string_view OptionKey(std::string_view option) {
	if (option.starts_with("--")) {
		option.remove_prefix(2);
	}
	if (option.starts_with("no-")) {
		option.remove_prefix(3);
	}
	return option;
}

std::string ExpandPresets(std::vector<std::string>& args, std::vector<std::string>& presets) {
	for (size_t i = 1; i < args.size(); i++) {
		if (args[i] == "--guest-exec") {
			break;
		}
		if (args[i] != "--preset") {
			continue;
		}
		if (i + 1 >= args.size()) {
			return "missing value for --preset";
		}
		const auto* preset = FindPreset(args[i + 1]);
		if (preset == nullptr) {
			return "unknown preset: " + args[i + 1] + " (presets: " + PresetNames() + ")";
		}
		presets.emplace_back(preset->name);
		args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
		           args.begin() + static_cast<std::ptrdiff_t>(i + 2));
		args.insert(args.begin() + static_cast<std::ptrdiff_t>(i), preset->tokens.begin(),
		            preset->tokens.end());
		// i >= 1, so this does not wrap; the loop continues after the inserted tokens.
		i = i + preset->tokens.size() - 1;
	}
	return {};
}

std::vector<std::string> TitlePresetTokens(const Preset&                 preset,
                                           std::span<const std::string> explicit_options,
                                           bool (*is_flag)(std::string_view option)) {
	std::vector<std::string> tokens;
	for (size_t i = 0; i < preset.tokens.size(); i++) {
		const auto token    = preset.tokens[i];
		const bool flag     = is_flag(token);
		const bool user_set = std::ranges::find(explicit_options, OptionKey(token)) !=
		                      explicit_options.end();
		if (!user_set) {
			tokens.emplace_back(token);
		}
		if (!flag && i + 1 < preset.tokens.size()) {
			i++;
			if (!user_set) {
				tokens.emplace_back(preset.tokens[i]);
			}
		}
	}
	return tokens;
}

} // namespace Config
