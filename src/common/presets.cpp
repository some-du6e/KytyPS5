#include "common/presets.h"

namespace Config {

namespace {

// For bisecting a regression of a title that worked before: every switch of the fork's global
// behaviour changes is turned off (each one also has its own --no- flag).
constexpr std::string_view SafeTokens[] = {
    "--no-async-pipelines",
};

// Options that use less video memory. Only options that exist are listed.
// TODO(gta6-prep VRAM workstream): add, once their flags exist,
//   "--vram-profile", "low",
//   "--vram-gc", "proportional",
//   "--oom-recovery",
//   "--video-out-resolution", "1080p",
//   "--rt-arena-mb", "<MiB>",
constexpr std::string_view LowVramTokens[] = {
    "--no-native-raytracing",
    "--no-trinity",
    "--no-readback-tiled-images",
    "--no-readback-storage-images",
};

// An RTX 3050 with 8 GB: low-vram's options (a preset cannot name another one, so they are
// repeated here).
// TODO(gta6-prep VRAM workstream): the same additions as low-vram.
constexpr std::string_view Rtx3050Tokens[] = {
    "--no-native-raytracing",
    "--no-trinity",
    "--no-readback-tiled-images",
    "--no-readback-storage-images",
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

} // namespace Config
