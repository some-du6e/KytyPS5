#include "loader/importDiagnostics.h"

#include "libs/errno.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <tuple>

namespace Loader::ImportDiagnostics {

// network.cpp np_error_signed_out: what Kyty's Np functions return while PSN emulation is off.
constexpr int32_t NP_ERROR_SIGNED_OUT = -2141913082; /* 0x80550006 */

static std::string_view StripLibScePrefix(std::string_view library) {
	constexpr std::string_view prefix = "libSce";
	return library.starts_with(prefix) ? library.substr(prefix.size()) : library;
}

std::string_view LibraryDisplayName(std::string_view library) {
	return StripLibScePrefix(library);
}

std::string_view LibraryFromGeneratedName(std::string_view name) {
	const auto open = name.find('[');
	if (open == std::string_view::npos) {
		return {};
	}
	const auto close = name.find(']', open + 1);
	if (close == std::string_view::npos) {
		return {};
	}
	auto segment = name.substr(open + 1, close - open - 1);
	// "<Lib>_v<digits>": library names may contain '_' themselves (SaveData_native).
	const auto version = segment.rfind("_v");
	if (version != std::string_view::npos && version + 2 < segment.size() &&
	    std::all_of(segment.begin() + static_cast<std::ptrdiff_t>(version + 2), segment.end(),
	                [](char c) { return c >= '0' && c <= '9'; })) {
		segment = segment.substr(0, version);
	}
	return StripLibScePrefix(segment);
}

std::string_view NidFromName(std::string_view name) {
	return name.substr(0, name.find_first_of("[#"));
}

const char* PolicyName(UnresolvedPolicy policy) {
	switch (policy) {
		case UnresolvedPolicy::Zero: return "zero";
		case UnresolvedPolicy::NpOffline: return "np-offline";
		case UnresolvedPolicy::NetDown: return "net-down";
		case UnresolvedPolicy::HttpDown: return "http-down";
	}
	return "zero";
}

static bool IsFamily(std::string_view library, std::string_view family) {
	return library.starts_with(family);
}

UnresolvedReturn GetUnresolvedReturn(std::string_view library) {
	library = StripLibScePrefix(library);

	const auto as_code = [](int32_t code) { return static_cast<uint32_t>(code); };

	// "Np" followed by an upper-case letter: NpManager, NpWebApi2, NpCppWebApi, ... but not
	// a hypothetical "Npxyz".
	if (library.size() > 2 && library.starts_with("Np") && library[2] >= 'A' && library[2] <= 'Z') {
		if (IsFamily(library, "NpTrophy") || IsFamily(library, "NpEntitlementAccess")) {
			return {};
		}
		return {as_code(NP_ERROR_SIGNED_OUT), UnresolvedPolicy::NpOffline};
	}
	if (library == "Net" || IsFamily(library, "NetCtl")) {
		return {as_code(Libs::Network::NET_ERROR_ENETDOWN), UnresolvedPolicy::NetDown};
	}
	if (IsFamily(library, "Http")) {
		return {as_code(Libs::Network::HTTP_ERROR_NETWORK), UnresolvedPolicy::HttpDown};
	}
	if (IsFamily(library, "Ssl") || library == "Rudp") {
		return {as_code(Libs::Network::NET_ERROR_ENETDOWN), UnresolvedPolicy::NetDown};
	}
	return {};
}

static std::string Printf(const char* format, auto... args) {
	const int size = std::snprintf(nullptr, 0, format, args...);
	if (size <= 0) {
		return {};
	}
	std::string text(static_cast<size_t>(size), '\0');
	std::snprintf(text.data(), text.size() + 1, format, args...);
	return text;
}

static std::string Str(std::string_view text) {
	return std::string(text);
}

std::string FormatFirstCall(std::string_view name, std::string_view program,
                            const UnresolvedReturn& ret) {
	return Printf("Unresolved import stub called: %s (program=%s, first call, returns=0x%08" PRIx32
	              ", policy=%s)\n",
	              Str(name).c_str(), Str(program).c_str(), ret.value, PolicyName(ret.policy));
}

std::string FormatKnownName(std::string_view name, std::string_view symbol) {
	return "Unresolved import name: " + Str(name) + " = " + Str(symbol) + "\n";
}

std::string FormatObjectImport(std::string_view name, std::string_view program, uint64_t page) {
	return Printf("Unresolved object import: %s (program=%s) -> invalid page 0x%016" PRIx64 "\n",
	              Str(name).c_str(), Str(program).c_str(), page);
}

std::string FormatRebind(std::string_view name, std::string_view resolved,
                         std::string_view program) {
	return "Rebind: " + Str(name) + " -> " + Str(resolved) + " (program=" + Str(program) + ")\n";
}

std::string FormatSummary(uint32_t functions, uint32_t objects,
                          std::vector<std::pair<std::string, uint32_t>> by_library) {
	std::sort(by_library.begin(), by_library.end(), [](const auto& a, const auto& b) {
		return a.second != b.second ? a.second > b.second : a.first < b.first;
	});
	std::string text = Printf("Unresolved imports: %" PRIu32 " functions, %" PRIu32
	                          " objects; by library: ",
	                          functions, objects);
	if (by_library.empty()) {
		text += "none";
	}
	for (size_t i = 0; i < by_library.size(); i++) {
		if (i != 0) {
			text += ", ";
		}
		text += (by_library[i].first.empty() ? std::string("?") : by_library[i].first) + "=" +
		        std::to_string(by_library[i].second);
	}
	text += "\n";
	return text;
}

static const char* StateName(ImportState state) {
	switch (state) {
		case ImportState::Hle: return "hle";
		case ImportState::Lle: return "lle";
		case ImportState::Unresolved: return "unresolved";
	}
	return "unresolved";
}

/*
 * --dump-imports report, format 1. Plain text, '\n' line ends:
 *
 *   # Kyty import report, format 1
 *   # programs: <file> <file> ...                (eboot.bin first, then the PRXs in load order)
 *   # totals: imported=<n> hle=<n> lle=<n> unresolved=<n>
 *   <blank line>
 *   library <Lib>: imported=<n> hle=<n> lle=<n> unresolved=<n>
 *     hle <nid> <type> <importer> <emulator function>
 *     lle <nid> <type> <importer> <exporting file>
 *     unresolved <nid> <type> <importer> <symbol name from --nid-names, or ?>
 *   <blank line>
 *   library ...
 *
 * - One "library" section per library the programs import from, sorted by name. <Lib> is the
 *   name the importer gives it, without "libSce" (as in the linker's generated names).
 * - An import counts once per (importer, library, NID, type), however many relocations use it.
 * - Inside a section, lines are sorted by state (hle, lle, unresolved), then NID, then importer.
 * - <type> is Func, Object or NoType; NoType imports resolved through the Func/Object retry are
 *   listed with their own type.
 * - Fields are separated by one space; only the last field may contain spaces.
 */
std::string FormatImportReport(const std::vector<std::string>& programs,
                               std::vector<ImportReportEntry>  entries) {
	const auto key = [](const ImportReportEntry& e) {
		return std::tie(e.library, e.state, e.nid, e.importer, e.type);
	};
	std::sort(entries.begin(), entries.end(),
	          [&](const auto& a, const auto& b) { return key(a) < key(b); });
	entries.erase(std::unique(entries.begin(), entries.end(),
	                          [&](const auto& a, const auto& b) { return key(a) == key(b); }),
	              entries.end());

	struct Counts {
		uint32_t imported   = 0;
		uint32_t hle        = 0;
		uint32_t lle        = 0;
		uint32_t unresolved = 0;

		void Add(ImportState state) {
			imported++;
			switch (state) {
				case ImportState::Hle: hle++; break;
				case ImportState::Lle: lle++; break;
				case ImportState::Unresolved: unresolved++; break;
			}
		}
	};
	const auto counts_text = [](const Counts& c) {
		return Printf("imported=%" PRIu32 " hle=%" PRIu32 " lle=%" PRIu32 " unresolved=%" PRIu32,
		              c.imported, c.hle, c.lle, c.unresolved);
	};

	Counts                        totals;
	std::map<std::string, Counts> per_library;
	for (const auto& e: entries) {
		totals.Add(e.state);
		per_library[e.library].Add(e.state);
	}

	std::string text = "# Kyty import report, format 1\n# programs:";
	for (const auto& program: programs) {
		text += " " + program;
	}
	text += "\n# totals: " + counts_text(totals) + "\n";

	std::string current;
	bool        first = true;
	for (const auto& e: entries) {
		if (first || e.library != current) {
			current = e.library;
			first   = false;
			text += "\nlibrary " + (current.empty() ? std::string("?") : current) + ": " +
			        counts_text(per_library[current]) + "\n";
		}
		text += std::string("  ") + StateName(e.state) + " " + e.nid + " " + e.type + " " +
		        e.importer + " " + (e.target.empty() ? std::string("?") : e.target) + "\n";
	}
	return text;
}

} // namespace Loader::ImportDiagnostics
