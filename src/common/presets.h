#ifndef KYTY_COMMON_PRESETS_H_
#define KYTY_COMMON_PRESETS_H_

#include <span>
#include <string>
#include <string_view>

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

} // namespace Config

#endif /* KYTY_COMMON_PRESETS_H_ */
