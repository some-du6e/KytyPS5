#ifndef KYTY_COMMON_PRESETS_H_
#define KYTY_COMMON_PRESETS_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Named bundles of command-line options. `--preset <name>` is replaced by the preset's tokens where
// it stands, so options after it override the preset; a title profile's default preset is applied
// the same way, but only to the options the user did not set (see ConfigOptions::explicit_options).
namespace Config {

struct Preset {
	std::string_view                  name;
	std::string_view                  description;
	std::span<const std::string_view> tokens; // command-line tokens, values included
};

[[nodiscard]] std::span<const Preset> Presets();
[[nodiscard]] const Preset*           FindPreset(std::string_view name);
// Every preset name, separated by ", ", for error messages.
[[nodiscard]] std::string PresetNames();
// The preset's tokens separated by spaces (the "Preset: <name> -> <tokens>" log line).
[[nodiscard]] std::string PresetTokenList(const Preset& preset);

// The key an option is tracked by in ConfigOptions::explicit_options: its name without the leading
// "--" and "no-", e.g. "native-raytracing" for both --native-raytracing and --no-native-raytracing.
[[nodiscard]] std::string_view OptionKey(std::string_view option);

// Replaces each `--preset <name>` of `args` (args[0] is the program) with the preset's tokens where
// it stands, so that the options after it override the preset's, and appends the name to
// `presets`. The tokens of a preset are not expanded again, and everything from --guest-exec on
// belongs to the guest. Returns an empty string, or the error message to print.
[[nodiscard]] std::string ExpandPresets(std::vector<std::string>& args,
                                        std::vector<std::string>& presets);

// The tokens of `preset` that a title profile's default preset applies: those whose OptionKey() is
// not in `explicit_options` (the options the user set). A token for which `is_flag` is false takes
// the next token as its value, and the two are kept or dropped together.
[[nodiscard]] std::vector<std::string> TitlePresetTokens(const Preset&                 preset,
                                                         std::span<const std::string> explicit_options,
                                                         bool (*is_flag)(std::string_view option));

} // namespace Config

#endif /* KYTY_COMMON_PRESETS_H_ */
