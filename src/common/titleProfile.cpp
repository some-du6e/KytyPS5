#include "common/titleProfile.h"

#include <array>
#include <cstddef>

namespace Common {

namespace {

// GTA V for PS5: the PlayStation 5 releases of 2022. PPSA04263 and PPSA04264 are the two the fork
// was tested with; PPSA03420 is listed as Grand Theft Auto V by prosperopatches.com and
// serialstation.com. (PPSA04288, which comments next to PPSA04264 mention, is Resident Evil 2.)
constexpr std::string_view Gta5Ids[]   = {"PPSA03420", "PPSA04263", "PPSA04264"};
constexpr std::string_view Gta5Names[] = {"grand theft auto v", "grand theft auto v (playstation 5)"};

// GTA 6: no dump exists before its release, so it is recognized by its name (or forced with
// --title-profile gta6, or by listing its IDs in KYTY_GTA6_TITLE_IDS). No quirk and no default
// preset: nothing is known about the title yet, and a guessed load-heavy option would be worse
// than the defaults every other title runs with.
constexpr std::string_view Gta6Names[] = {"grand theft auto vi", "grand theft auto 6"};

constexpr TitleProfile Profiles[] = {
    {.key            = "gta5",
     .display        = "Grand Theft Auto V",
     .ids            = Gta5Ids,
     .names          = Gta5Names,
     .quirks         = QuirkBit(Quirk::RockstarLegacyEntitlements),
     .default_preset = ""},
    {.key            = "gta6",
     .display        = "Grand Theft Auto VI",
     .ids            = {},
     .names          = Gta6Names,
     .quirks         = 0,
     .default_preset = ""},
};

constexpr std::array<std::string_view, static_cast<size_t>(Quirk::Count)> QuirkNamesTable = {
    "rockstar-legacy-entitlements",
    "playgo-all-chunks",
};

constexpr char ToLowerAscii(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool IsSpaceAscii(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
			return false;
		}
	}
	return true;
}

bool IsIdSeparator(char c) {
	return c == ',' || c == ';' || IsSpaceAscii(c);
}

// Whether `id` is one of the IDs in the separated `list`.
bool ListContainsId(std::string_view list, std::string_view id) {
	size_t pos = 0;
	while (pos < list.size()) {
		while (pos < list.size() && IsIdSeparator(list[pos])) {
			pos++;
		}
		size_t end = pos;
		while (end < list.size() && !IsIdSeparator(list[end])) {
			end++;
		}
		if (end > pos && EqualsIgnoreCase(list.substr(pos, end - pos), id)) {
			return true;
		}
		pos = end;
	}
	return false;
}

} // namespace

TitleInfo MakeTitleInfo(std::string_view title_id, std::string_view content_id,
                        std::string_view name, std::string_view version) {
	TitleInfo info;
	info.name       = name;
	info.version    = version;
	info.content_id = content_id;
	if (!title_id.empty()) {
		info.id = title_id;
		return info;
	}
	const auto dash = content_id.find('-');
	if (dash == std::string_view::npos) {
		return info;
	}
	const auto underscore = content_id.find('_', dash + 1);
	if (underscore == std::string_view::npos || underscore <= dash + 1) {
		return info;
	}
	info.id                 = content_id.substr(dash + 1, underscore - dash - 1);
	info.id_from_content_id = true;
	return info;
}

std::string_view QuirkName(Quirk quirk) {
	const auto index = static_cast<size_t>(quirk);
	return index < QuirkNamesTable.size() ? QuirkNamesTable[index] : std::string_view {"?"};
}

std::optional<Quirk> FindQuirk(std::string_view name) {
	for (size_t index = 0; index < QuirkNamesTable.size(); index++) {
		if (QuirkNamesTable[index] == name) {
			return static_cast<Quirk>(index);
		}
	}
	return std::nullopt;
}

std::string QuirkList(QuirkMask mask) {
	std::string list;
	for (size_t index = 0; index < QuirkNamesTable.size(); index++) {
		if ((mask & QuirkBit(static_cast<Quirk>(index))) == 0) {
			continue;
		}
		if (!list.empty()) {
			list += ',';
		}
		list += QuirkNamesTable[index];
	}
	return list.empty() ? std::string {"none"} : list;
}

std::string QuirkNames() {
	std::string names;
	for (const auto name: QuirkNamesTable) {
		if (!names.empty()) {
			names += ", ";
		}
		names += name;
	}
	return names;
}

std::span<const TitleProfile> TitleProfiles() {
	return Profiles;
}

const TitleProfile* FindTitleProfile(std::string_view key) {
	for (const auto& profile: Profiles) {
		if (profile.key == key) {
			return &profile;
		}
	}
	return nullptr;
}

std::string TitleProfileKeys() {
	std::string keys;
	for (const auto& profile: Profiles) {
		if (!keys.empty()) {
			keys += ", ";
		}
		keys += profile.key;
	}
	return keys;
}

std::string NormalizeTitleName(std::string_view name) {
	// UTF-8 sequences that read as a space: TM, SM, (R), (C) and the no-break space.
	constexpr std::string_view Spaces[] = {"\xE2\x84\xA2", "\xE2\x84\xA0", "\xC2\xAE", "\xC2\xA9",
	                                       "\xC2\xA0"};
	std::string normalized;
	normalized.reserve(name.size());
	bool pending_space = false;
	for (size_t pos = 0; pos < name.size();) {
		bool space = IsSpaceAscii(name[pos]);
		size_t length = 1;
		if (!space) {
			for (const auto sequence: Spaces) {
				if (name.substr(pos).starts_with(sequence)) {
					space  = true;
					length = sequence.size();
					break;
				}
			}
		}
		if (space) {
			pending_space = !normalized.empty();
		} else {
			if (pending_space) {
				normalized += ' ';
				pending_space = false;
			}
			normalized += ToLowerAscii(name[pos]);
		}
		pos += length;
	}
	return normalized;
}

std::string_view TitleMatchReasonName(TitleMatchReason reason) {
	switch (reason) {
		case TitleMatchReason::None: return "no match";
		case TitleMatchReason::Disabled: return "--title-profile none";
		case TitleMatchReason::Forced: return "--title-profile";
		case TitleMatchReason::EnvId: return "KYTY_GTA6_TITLE_IDS";
		case TitleMatchReason::Id: return "title ID";
		case TitleMatchReason::Name: return "title name";
	}
	return "?";
}

TitleMatch MatchTitle(const TitleInfo& title, std::string_view forced_key,
                      std::string_view gta6_ids) {
	if (forced_key == "none") {
		return {.profile = nullptr, .reason = TitleMatchReason::Disabled};
	}
	if (!forced_key.empty()) {
		const auto* profile = FindTitleProfile(forced_key);
		return {.profile = profile,
		        .reason  = profile != nullptr ? TitleMatchReason::Forced : TitleMatchReason::None};
	}
	if (!title.id.empty() && ListContainsId(gta6_ids, title.id)) {
		if (const auto* profile = FindTitleProfile("gta6"); profile != nullptr) {
			return {.profile = profile, .reason = TitleMatchReason::EnvId};
		}
	}
	if (!title.id.empty()) {
		for (const auto& profile: Profiles) {
			for (const auto id: profile.ids) {
				if (EqualsIgnoreCase(id, title.id)) {
					return {.profile = &profile, .reason = TitleMatchReason::Id};
				}
			}
		}
	}
	const auto match_name = [](std::string_view raw) -> const TitleProfile* {
		const auto name = NormalizeTitleName(raw);
		if (name.empty()) {
			return nullptr;
		}
		for (const auto& profile: Profiles) {
			for (const auto profile_name: profile.names) {
				if (profile_name == name) {
					return &profile;
				}
			}
		}
		return nullptr;
	};
	if (const auto* profile = match_name(title.name); profile != nullptr) {
		return {.profile = profile, .reason = TitleMatchReason::Name};
	}
	for (const auto& localized: title.localized_names) {
		if (const auto* profile = match_name(localized); profile != nullptr) {
			return {.profile = profile, .reason = TitleMatchReason::Name};
		}
	}
	return {};
}

bool NeedsTitleProfileHint(const TitleInfo& title, const TitleMatch& match) {
	if (match.profile != nullptr || match.reason != TitleMatchReason::None) {
		return false;
	}
	const auto named_like_gta = [](std::string_view name) {
		return NormalizeTitleName(name).starts_with("grand theft auto");
	};
	if (named_like_gta(title.name)) {
		return true;
	}
	for (const auto& localized: title.localized_names) {
		if (named_like_gta(localized)) {
			return true;
		}
	}
	return false;
}

std::string FormatTitleProfileLine(const TitleInfo& title, const TitleProfile* profile,
                                   QuirkMask quirks, std::string_view preset) {
	std::string line = "Title profile: ";
	line += profile != nullptr ? profile->key : std::string_view {"none"};
	line += " (";
	line += profile != nullptr ? profile->display : std::string_view {"none"};
	line += ") id=";
	line += title.id.empty() ? std::string_view {"?"} : std::string_view {title.id};
	line += " version=";
	line += title.version.empty() ? std::string_view {"?"} : std::string_view {title.version};
	line += " name=\"";
	line += title.name;
	line += "\" quirks=";
	line += QuirkList(quirks);
	line += " preset=";
	line += preset.empty() ? std::string_view {"none"} : preset;
	return line;
}

} // namespace Common
