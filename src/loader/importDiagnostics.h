#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_IMPORTDIAGNOSTICS_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_IMPORTDIAGNOSTICS_H_

// Pure helpers behind the linker's unresolved-import handling: the return value policy of the
// unresolved-import stub, the "Imports / HLE" lines of the gta6-prep log contract, and the
// --dump-imports report. Nothing here reads the configuration or writes a log, so the tests can
// call it directly (hle_softfail_tests).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Loader::ImportDiagnostics {

// "<nid>[<Lib>_v<n>][<Mod>_v<a>.<b>][<Type>]" (SymbolDatabase::GenerateName) -> "<Lib>". A raw
// library name ("libSceNpManager") loses its "libSce" prefix the way GenerateName strips it.
// Empty when the name has no library segment.
std::string_view LibraryFromGeneratedName(std::string_view name);
// A raw ELF library name as generated names show it: "libSceNpManager" -> "NpManager".
std::string_view LibraryDisplayName(std::string_view library);
// The NID part of a generated ("<nid>[...") or raw ("<nid>#lib#mod") import name.
std::string_view NidFromName(std::string_view name);

enum class UnresolvedPolicy { Zero, NpOffline, NetDown, HttpDown };

struct UnresolvedReturn {
	uint32_t         value  = 0;
	UnresolvedPolicy policy = UnresolvedPolicy::Zero;
};

// "zero", "np-offline", "net-down" or "http-down".
const char* PolicyName(UnresolvedPolicy policy);

// What a call to an unresolved function of `library` returns when the offline policy is on:
// - Np* (NpManager, NpWebApi2, NpSessionSignaling, NpMatching2, NpAuth, ...): signed out,
//   0x80550006, the code Kyty's own Np functions return while PSN emulation is off.
//   NpTrophy* and NpEntitlementAccess* are left at 0: they work offline on hardware.
// - Net, NetCtl*: NET_ERROR_ENETDOWN 0x80410132.
// - Http*: HTTP_ERROR_NETWORK 0x80431063.
// - Ssl*, Rudp: NET_ERROR_ENETDOWN (neither library has a network-down code of its own).
// - everything else: 0, as before.
// `library` is a LibraryFromGeneratedName() result or a raw ELF library name.
UnresolvedReturn GetUnresolvedReturn(std::string_view library);

// The stub returns the 32-bit error sign-extended, so 64-bit result checks see it negative too.
constexpr uint64_t StubReturnRegister(uint32_t value) {
	return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

// "Unresolved import stub called: <name> (program=<file>, first call, returns=0x<8 hex>,
// policy=<policy>)\n"
std::string FormatFirstCall(std::string_view name, std::string_view program,
                            const UnresolvedReturn& ret);
// "Unresolved import name: <name> = <symbol>\n" when --nid-names knows the NID.
std::string FormatKnownName(std::string_view name, std::string_view symbol);
// "Unresolved object import: <name> (program=<file>) -> invalid page 0x<16 hex>\n"
std::string FormatObjectImport(std::string_view name, std::string_view program, uint64_t page);
// "Rebind: <name> -> <resolved> (program=<file>)\n"
std::string FormatRebind(std::string_view name, std::string_view resolved,
                         std::string_view program);
// "Unresolved imports: <F> functions, <O> objects; by library: <Lib>=<n>, ...\n", libraries by
// count (highest first) then name; "by library: none" when there are none.
std::string FormatSummary(uint32_t functions, uint32_t objects,
                          std::vector<std::pair<std::string, uint32_t>> by_library);

enum class ImportState { Hle, Lle, Unresolved };

struct ImportReportEntry {
	std::string importer; // file name of the importing program
	std::string library;  // library as the importer names it, without "libSce"
	std::string nid;
	std::string type; // Func, Object or NoType
	ImportState state = ImportState::Unresolved;
	// HLE: the emulator function; LLE: the file name of the exporting PRX; unresolved: the symbol
	// name from --nid-names, or empty.
	std::string target;
};

// The --dump-imports text. See the format description at the top of the definition.
std::string FormatImportReport(const std::vector<std::string>& programs,
                               std::vector<ImportReportEntry>  entries);

} // namespace Loader::ImportDiagnostics

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_IMPORTDIAGNOSTICS_H_ */
