#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_NID_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_NID_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Loader::Nid {

std::array<uint8_t, 20> Sha1Digest(const uint8_t* data, size_t size);

// The 11-character NID the PS4/PS5 toolchain derives from a symbol name: the SHA-1 of the name
// followed by a fixed salt, its first 8 bytes read little-endian and written in a base64 variant
// that uses '+' and '-'.
std::string SymbolToNid(std::string_view symbol);

// A NID -> symbol name map built from a names file: one symbol name per line. Blank lines, lines
// starting with '#' and trailing whitespace (including '\r') are ignored.
class NameTable {
public:
	// Adds the names in `text`. Returns how many names were added.
	size_t AddNames(std::string_view text);
	// Reads the file and adds its names. Returns false when it cannot be read.
	bool LoadFile(const std::filesystem::path& path, size_t* added = nullptr);

	// nullptr when the NID has no known name.
	[[nodiscard]] const std::string* Find(std::string_view nid) const;
	[[nodiscard]] size_t             Size() const { return m_names.size(); }

private:
	std::unordered_map<std::string, std::string> m_names;
};

// The table --nid-names loaded, shared by the linker logs and --dump-imports. Filled once on the
// main thread before any guest code runs and only read afterwards.
NameTable& GlobalNames();
// GlobalNames().Find() of the NID at the start of a generated or raw import name
// ("<nid>[...]" or "<nid>#lib#mod").
const std::string* FindNameForImport(std::string_view name);

} // namespace Loader::Nid

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_NID_H_ */
