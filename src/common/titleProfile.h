#ifndef KYTY_COMMON_TITLE_PROFILE_H_
#define KYTY_COMMON_TITLE_PROFILE_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Per-title profiles: which known title is running, the title-specific workarounds ("quirks") it
// needs and the preset of options it defaults to. Pure data and string logic, so that it can be
// tested without the loader (Loader::SystemContentTitle() fills TitleInfo from param.json).
namespace Common {

// The identity of the running title, from its param.json.
struct TitleInfo {
	std::string id;         // TITLE_ID, else the title segment of CONTENT_ID (see MakeTitleInfo())
	std::string name;       // TITLE: the localized titleName
	// gta6-prep: config. Every localizedParameters titleName of the param.json, in file order;
	// `name` is the default language's (else en-US's, else the first). Profiles match any of them.
	std::vector<std::string> localized_names;
	std::string version;    // APP_VER
	std::string content_id; // CONTENT_ID
	// The param.json has no TITLE_ID: `id` comes from CONTENT_ID.
	bool id_from_content_id = false;
};

// `id` is `title_id` when it is not empty, else the characters between the first '-' of
// `content_id` and the next '_' after it ("UP1004-PPSA04264_00-..." gives "PPSA04264"), else empty.
[[nodiscard]] TitleInfo MakeTitleInfo(std::string_view title_id, std::string_view content_id,
                                      std::string_view name, std::string_view version);

// Title-specific behaviour that a profile turns on and `--quirk <name>=on|off` overrides.
enum class Quirk : uint32_t {
	// libNpEntitlementAccess reports GTA V's three legacy add-on labels as installed.
	RockstarLegacyEntitlements,
	// gta6-prep: config. With --playgo-hack and no chunk metadata, PlayGo reports chunk ids at or
	// above the made-up count as installed instead of PLAYGO_ERROR_BAD_CHUNK_ID. No profile turns
	// it on: titles may use BAD_CHUNK_ID to find the end of the layout.
	PlayGoAllChunks,
	Count
};

using QuirkMask = uint64_t;

[[nodiscard]] constexpr QuirkMask QuirkBit(Quirk quirk) {
	return QuirkMask {1} << static_cast<uint32_t>(quirk);
}

inline constexpr QuirkMask AllQuirks = (QuirkMask {1} << static_cast<uint32_t>(Quirk::Count)) - 1u;

// The CLI name of a quirk, e.g. "rockstar-legacy-entitlements".
[[nodiscard]] std::string_view QuirkName(Quirk quirk);
[[nodiscard]] std::optional<Quirk> FindQuirk(std::string_view name);
// The names of the quirks in `mask`, separated by commas, or "none".
[[nodiscard]] std::string QuirkList(QuirkMask mask);
// Every quirk name, separated by ", ", for error messages.
[[nodiscard]] std::string QuirkNames();

struct TitleProfile {
	std::string_view                  key;     // --title-profile <key>, e.g. "gta5"
	std::string_view                  display; // e.g. "Grand Theft Auto V"
	std::span<const std::string_view> ids;     // exact TITLE_IDs
	// Exact names after NormalizeTitleName(). Never a prefix or substring match: "grand theft auto
	// v" is a prefix of "grand theft auto vi".
	std::span<const std::string_view> names;
	QuirkMask                         quirks = 0;
	// A preset (see Config presets) applied where the user set no option; empty: none.
	std::string_view                  default_preset;
};

[[nodiscard]] std::span<const TitleProfile> TitleProfiles();
[[nodiscard]] const TitleProfile*           FindTitleProfile(std::string_view key);
// Every profile key, separated by ", ", for error messages.
[[nodiscard]] std::string TitleProfileKeys();

// ASCII case folding, the trademark, registered and copyright signs (and no-break spaces) become
// spaces, runs of whitespace collapse to one space, and leading and trailing spaces are dropped.
// Other bytes are kept as they are.
[[nodiscard]] std::string NormalizeTitleName(std::string_view name);

enum class TitleMatchReason {
	None,     // no profile matches the title
	Disabled, // --title-profile none
	Forced,   // --title-profile <key>
	EnvId,    // the title ID is listed in KYTY_GTA6_TITLE_IDS
	Id,       // the title ID is in the profile's IDs
	Name      // the normalized title name is one of the profile's names
};

struct TitleMatch {
	const TitleProfile* profile = nullptr;
	TitleMatchReason    reason  = TitleMatchReason::None;
};

[[nodiscard]] std::string_view TitleMatchReasonName(TitleMatchReason reason);

// `forced_key`: empty picks the profile by the title, "none" picks none, any other value picks the
// profile with that key (none when there is no such profile). `gta6_ids`: the value of
// KYTY_GTA6_TITLE_IDS, title IDs separated by commas, semicolons or whitespace that select the
// gta6 profile (its title ID is not known yet). Otherwise an exact ID match comes before an exact
// normalized name match, which tries `name` first and then each of `localized_names` (a SKU whose
// default language is not English still has its en-US titleName). IDs compare without regard to
// ASCII case.
[[nodiscard]] TitleMatch MatchTitle(const TitleInfo& title, std::string_view forced_key,
                                    std::string_view gta6_ids = {});

// Whether a title that matched no profile by itself (not --title-profile none) has a name (`name`
// or one of `localized_names`) that starts with "grand theft auto": another SKU or a localized
// name of GTA V or GTA 6 that the user may want to run with --title-profile gta5|gta6.
[[nodiscard]] bool NeedsTitleProfileHint(const TitleInfo& title, const TitleMatch& match);

// The quirks in effect: the profile's, plus `on`, minus `off`.
[[nodiscard]] constexpr QuirkMask ResolveQuirks(const TitleProfile* profile, QuirkMask on,
                                                QuirkMask off) {
	return ((profile != nullptr ? profile->quirks : 0u) | on) & ~off & AllQuirks;
}

// The "Title profile: ..." line of the log contract, without the newline.
[[nodiscard]] std::string FormatTitleProfileLine(const TitleInfo& title, const TitleProfile* profile,
                                                 QuirkMask quirks, std::string_view preset);

} // namespace Common

#endif /* KYTY_COMMON_TITLE_PROFILE_H_ */
