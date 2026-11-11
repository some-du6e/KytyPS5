// Pure helpers behind the gta6-prep HLE hardening: the NID hash and names table, the return value
// policy and log lines of unresolved imports, the --dump-imports report and the param size check.

#include "common/softFail.h"
#include "loader/importDiagnostics.h"
#include "loader/nid.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Diag = Loader::ImportDiagnostics;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "HleSoftFailTests: failed: %s\n", message);
		std::abort();
	}
}

void CheckEqual(std::string_view actual, std::string_view expected, const char* message) {
	if (actual != expected) {
		std::fprintf(stderr, "HleSoftFailTests: failed: %s\n  actual:   \"%.*s\"\n  expected: \"%.*s\"\n",
		             message, static_cast<int>(actual.size()), actual.data(),
		             static_cast<int>(expected.size()), expected.data());
		std::abort();
	}
}

// Every pair is registered in src/libs under the NID with the function named after the symbol
// (LIB_FUNC("9T2pDF2Ryqg", LibNet::NetHtonl) and so on), so the hash must reproduce them.
void TestNidHash() {
	struct Pair {
		const char* symbol;
		const char* nid;
	};
	constexpr Pair pairs[] = {
	    {"sceNetHtonl", "9T2pDF2Ryqg"},             // libNet.cpp
	    {"sceNetInetNtop", "9vA2aW+CHuA"},          // '+' in the alphabet
	    {"sceNetInetPton", "8Kcp5d-q1Uo"},          // '-' in the alphabet
	    {"sceKernelUsleep", "1jfXLRVzisc"},         // libKernel.cpp
	    {"sceKernelGetProcessTime", "4J2sUJmuHZQ"}, // libKernel.cpp
	    {"sceHttpGetLastErrno", "0onIrKx9NIE"},     // libHttp
	};
	for (const auto& pair: pairs) {
		CheckEqual(Loader::Nid::SymbolToNid(pair.symbol), pair.nid, pair.symbol);
	}

	// SHA-1 itself, against the FIPS 180 "abc" vector.
	constexpr uint8_t abc[]    = {'a', 'b', 'c'};
	constexpr uint8_t expect[] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
	                              0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
	const auto        digest   = Loader::Nid::Sha1Digest(abc, sizeof(abc));
	for (size_t i = 0; i < sizeof(expect); i++) {
		Check(digest[i] == expect[i], "SHA-1 of \"abc\"");
	}
}

void TestNameTable() {
	Loader::Nid::NameTable table;
	const auto added = table.AddNames("# comment\n\nsceNetHtonl\r\n  sceKernelUsleep  \nsceNetInetPton");
	Check(added == 3, "names file: three names, comments and blank lines skipped");
	Check(table.Size() == 3, "table size");
	const auto* name = table.Find("1jfXLRVzisc");
	Check(name != nullptr && *name == "sceKernelUsleep", "whitespace around a name is trimmed");
	Check(table.Find("9T2pDF2Ryqg") != nullptr && *table.Find("9T2pDF2Ryqg") == "sceNetHtonl",
	      "CRLF line end is trimmed");
	Check(table.Find("8Kcp5d-q1Uo") != nullptr, "last line without a line end");
	Check(table.Find("AAAAAAAAAAA") == nullptr, "unknown NID");
}

void TestLibraryFromGeneratedName() {
	CheckEqual(Diag::LibraryFromGeneratedName("abc[NpSessionSignaling_v1][NpSessionSignaling_v1.1][Func]"),
	           "NpSessionSignaling", "Np library");
	CheckEqual(Diag::LibraryFromGeneratedName("x[SaveData_native_v2][SaveData_v1.1][Func]"),
	           "SaveData_native", "library with an underscore keeps it");
	CheckEqual(Diag::LibraryFromGeneratedName("x[libSceNpManager_v1][NpManager_v1.1][Func]"),
	           "NpManager", "libSce prefix stripped");
	CheckEqual(Diag::LibraryFromGeneratedName("plain#1#1"), "", "raw name has no library segment");
	CheckEqual(Diag::LibraryDisplayName("libSceNpWebApi2"), "NpWebApi2", "raw library name");
	CheckEqual(Diag::LibraryDisplayName("libkernel"), "libkernel", "libkernel keeps its name");
	CheckEqual(Diag::NidFromName("9T2pDF2Ryqg[Net_v1][Net_v1.1][Func]"), "9T2pDF2Ryqg",
	           "NID of a generated name");
	CheckEqual(Diag::NidFromName("9T2pDF2Ryqg#A#B"), "9T2pDF2Ryqg", "NID of a raw name");
}

void TestUnresolvedPolicy() {
	struct Case {
		const char*            library;
		uint32_t               value;
		Diag::UnresolvedPolicy policy;
	};
	constexpr Case cases[] = {
	    {"NpManager", 0x80550006u, Diag::UnresolvedPolicy::NpOffline},
	    {"NpWebApi2", 0x80550006u, Diag::UnresolvedPolicy::NpOffline},
	    {"NpSessionSignaling", 0x80550006u, Diag::UnresolvedPolicy::NpOffline},
	    {"NpMatching2", 0x80550006u, Diag::UnresolvedPolicy::NpOffline},
	    {"NpCppWebApi", 0x80550006u, Diag::UnresolvedPolicy::NpOffline},
	    {"libSceNpAuth", 0x80550006u, Diag::UnresolvedPolicy::NpOffline},
	    {"NpTrophy2", 0, Diag::UnresolvedPolicy::Zero},
	    {"NpEntitlementAccess", 0, Diag::UnresolvedPolicy::Zero},
	    {"Net", 0x80410132u, Diag::UnresolvedPolicy::NetDown},
	    {"NetCtl", 0x80410132u, Diag::UnresolvedPolicy::NetDown},
	    {"Http", 0x80431063u, Diag::UnresolvedPolicy::HttpDown},
	    {"Http2", 0x80431063u, Diag::UnresolvedPolicy::HttpDown},
	    {"Ssl", 0x80410132u, Diag::UnresolvedPolicy::NetDown},
	    {"Rudp", 0x80410132u, Diag::UnresolvedPolicy::NetDown},
	    {"VideoOut", 0, Diag::UnresolvedPolicy::Zero},
	    {"libkernel", 0, Diag::UnresolvedPolicy::Zero},
	    {"Ngs2", 0, Diag::UnresolvedPolicy::Zero},
	    {"Netflix", 0, Diag::UnresolvedPolicy::Zero},
	    {"", 0, Diag::UnresolvedPolicy::Zero},
	};
	for (const auto& c: cases) {
		const auto ret = Diag::GetUnresolvedReturn(c.library);
		Check(ret.value == c.value && ret.policy == c.policy, c.library);
	}
	Check(Diag::StubReturnRegister(0x80550006u) == 0xffffffff80550006ull,
	      "error codes are sign-extended into rax");
	Check(Diag::StubReturnRegister(0) == 0, "zero stays zero");
	CheckEqual(Diag::PolicyName(Diag::UnresolvedPolicy::HttpDown), "http-down", "policy name");
}

void TestLogLines() {
	CheckEqual(Diag::FormatFirstCall("abc[NpManager_v1][NpManager_v1.1][Func]", "eboot.bin",
	                                 {0x80550006u, Diag::UnresolvedPolicy::NpOffline}),
	           "Unresolved import stub called: abc[NpManager_v1][NpManager_v1.1][Func] "
	           "(program=eboot.bin, first call, returns=0x80550006, policy=np-offline)\n",
	           "first call line");
	CheckEqual(Diag::FormatFirstCall("x[VideoOut_v1][VideoOut_v1.1][NoType]", "a.prx", {}),
	           "Unresolved import stub called: x[VideoOut_v1][VideoOut_v1.1][NoType] "
	           "(program=a.prx, first call, returns=0x00000000, policy=zero)\n",
	           "first call line, zero policy");
	CheckEqual(Diag::FormatObjectImport("o[libc_v1][libc_v1.1][Object]", "eboot.bin", 0x840000000ull),
	           "Unresolved object import: o[libc_v1][libc_v1.1][Object] (program=eboot.bin) -> "
	           "invalid page 0x0000000840000000\n",
	           "object import line");
	CheckEqual(Diag::FormatRebind("n[Fios2_v1][Fios2_v1.1][Func]", "n[Fios2_v1][Fios2_v1.1][Func]",
	                              "eboot.bin"),
	           "Rebind: n[Fios2_v1][Fios2_v1.1][Func] -> n[Fios2_v1][Fios2_v1.1][Func] "
	           "(program=eboot.bin)\n",
	           "rebind line");
	CheckEqual(Diag::FormatSummary(5, 1, {{"Net", 1}, {"NpManager", 3}, {"Ampr", 1}, {"", 1}}),
	           "Unresolved imports: 5 functions, 1 objects; by library: NpManager=3, ?=1, Ampr=1, "
	           "Net=1\n",
	           "summary sorted by count, then name");
	CheckEqual(Diag::FormatSummary(0, 0, {}),
	           "Unresolved imports: 0 functions, 0 objects; by library: none\n", "empty summary");
}

void TestImportReport() {
	using Diag::ImportState;
	std::vector<Diag::ImportReportEntry> entries = {
	    {"eboot.bin", "Net", "9T2pDF2Ryqg", "Func", ImportState::Hle, "LibNet::NetHtonl"},
	    {"eboot.bin", "Net", "9T2pDF2Ryqg", "Func", ImportState::Hle, "LibNet::NetHtonl"},
	    {"eboot.bin", "Net", "zzzzzzzzzzz", "Func", ImportState::Unresolved, ""},
	    {"eboot.bin", "Fios2", "aaaaaaaaaaa", "Func", ImportState::Lle, "libSceFios2.prx"},
	    {"libc.prx", "Net", "yyyyyyyyyyy", "NoType", ImportState::Unresolved, "sceNetSomething"},
	};
	const auto text = Diag::FormatImportReport({"eboot.bin", "libc.prx"}, entries);
	CheckEqual(text,
	           "# Kyty import report, format 1\n"
	           "# programs: eboot.bin libc.prx\n"
	           "# totals: imported=4 hle=1 lle=1 unresolved=2\n"
	           "\n"
	           "library Fios2: imported=1 hle=0 lle=1 unresolved=0\n"
	           "  lle aaaaaaaaaaa Func eboot.bin libSceFios2.prx\n"
	           "\n"
	           "library Net: imported=3 hle=1 lle=0 unresolved=2\n"
	           "  hle 9T2pDF2Ryqg Func eboot.bin LibNet::NetHtonl\n"
	           "  unresolved yyyyyyyyyyy NoType libc.prx sceNetSomething\n"
	           "  unresolved zzzzzzzzzzz Func eboot.bin ?\n",
	           "import report");
}

void TestParamSize() {
	using Common::SoftFail::CheckParamSize;
	using Common::SoftFail::ParamSize;
	static_assert(CheckParamSize(0x28, 0x28) == ParamSize::Exact);
	static_assert(CheckParamSize(0x30, 0x28) == ParamSize::Larger);
	static_assert(CheckParamSize(0x20, 0x28) == ParamSize::Smaller);
	CheckEqual(Common::SoftFail::FormatParamSizeMessage("param", 0x30, 0x28),
	           "param size 0x30 larger than 0x28, ignoring tail", "larger size message");
	CheckEqual(Common::SoftFail::FormatParamSizeMessage("option", 0x10, 0x58),
	           "option size 0x10 smaller than 0x58", "smaller size message");
	CheckEqual(Common::SoftFail::FormatParamSizeMessage("param", 0x28, 0x28), "",
	           "no message for the exact size");
	CheckEqual(Common::SoftFail::FormatLine("sceNpSetContentRestriction",
	                                        "param size 0x30 larger than 0x28, ignoring tail"),
	           "Soft-fail: sceNpSetContentRestriction: param size 0x30 larger than 0x28, ignoring "
	           "tail\n",
	           "soft-fail line");
}

} // namespace

int main() {
	TestNidHash();
	TestNameTable();
	TestLibraryFromGeneratedName();
	TestUnresolvedPolicy();
	TestLogLines();
	TestImportReport();
	TestParamSize();
	std::printf("HleSoftFailTests: all passed\n");
	return 0;
}
