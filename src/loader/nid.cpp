#include "loader/nid.h"

#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace Loader::Nid {

std::array<uint8_t, 20> Sha1Digest(const uint8_t* data, size_t size) {
	uint32_t h0 = 0x67452301u;
	uint32_t h1 = 0xefcdab89u;
	uint32_t h2 = 0x98badcfeu;
	uint32_t h3 = 0x10325476u;
	uint32_t h4 = 0xc3d2e1f0u;

	std::vector<uint8_t> msg(data, data + size);
	const uint64_t       bit_len = static_cast<uint64_t>(size) * 8u;

	msg.push_back(0x80u);
	while ((msg.size() % 64u) != 56u) {
		msg.push_back(0u);
	}
	for (int i = 7; i >= 0; i--) {
		msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xffu));
	}

	for (size_t offset = 0; offset < msg.size(); offset += 64u) {
		uint32_t w[80] = {};
		for (int i = 0; i < 16; i++) {
			const auto j = offset + static_cast<size_t>(i) * 4u;
			w[i]         = (static_cast<uint32_t>(msg[j + 0]) << 24u) |
			       (static_cast<uint32_t>(msg[j + 1]) << 16u) |
			       (static_cast<uint32_t>(msg[j + 2]) << 8u) | static_cast<uint32_t>(msg[j + 3]);
		}
		for (int i = 16; i < 80; i++) {
			w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}

		uint32_t a = h0;
		uint32_t b = h1;
		uint32_t c = h2;
		uint32_t d = h3;
		uint32_t e = h4;

		for (int i = 0; i < 80; i++) {
			uint32_t f = 0;
			uint32_t k = 0;
			if (i < 20) {
				f = (b & c) | ((~b) & d);
				k = 0x5a827999u;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1u;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdcu;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6u;
			}

			const auto temp = std::rotl(a, 5) + f + e + k + w[i];
			e               = d;
			d               = c;
			c               = std::rotl(b, 30);
			b               = a;
			a               = temp;
		}

		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
	}

	std::array<uint8_t, 20> digest = {};
	const uint32_t          h[5]   = {h0, h1, h2, h3, h4};
	for (int i = 0; i < 5; i++) {
		digest[static_cast<size_t>(i) * 4u + 0u] = static_cast<uint8_t>((h[i] >> 24u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 1u] = static_cast<uint8_t>((h[i] >> 16u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 2u] = static_cast<uint8_t>((h[i] >> 8u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 3u] = static_cast<uint8_t>(h[i] & 0xffu);
	}
	return digest;
}

std::string SymbolToNid(std::string_view symbol) {
	static constexpr std::array<uint8_t, 16> salt = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde,
	                                                 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1,
	                                                 0xc3, 0xe5, 0x52, 0x30};
	static constexpr char                    codes[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

	std::vector<uint8_t> input(symbol.size() + salt.size());
	if (!symbol.empty()) {
		std::memcpy(input.data(), symbol.data(), symbol.size());
	}
	std::memcpy(input.data() + symbol.size(), salt.data(), salt.size());

	const auto hash = Sha1Digest(input.data(), input.size());

	uint64_t digest = 0;
	std::memcpy(&digest, hash.data(), sizeof(digest));

	char nid[12] = {};
	for (int i = 0; i < 10; i++) {
		nid[i] = codes[(digest >> (58 - i * 6)) & 0x3fu];
	}
	nid[10] = codes[(digest & 0xfu) * 4u];
	return std::string(nid);
}

size_t NameTable::AddNames(std::string_view text) {
	size_t added = 0;
	while (!text.empty()) {
		const auto end  = text.find('\n');
		auto       line = text.substr(0, end);
		text = (end == std::string_view::npos ? std::string_view {} : text.substr(end + 1));

		while (!line.empty() &&
		       (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.remove_suffix(1);
		}
		while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
			line.remove_prefix(1);
		}
		if (line.empty() || line.front() == '#') {
			continue;
		}
		m_names.insert_or_assign(SymbolToNid(line), std::string(line));
		added++;
	}
	return added;
}

bool NameTable::LoadFile(const std::filesystem::path& path, size_t* added) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(file)),
	                       std::istreambuf_iterator<char>());
	const auto        count = AddNames(text);
	if (added != nullptr) {
		*added = count;
	}
	return true;
}

const std::string* NameTable::Find(std::string_view nid) const {
	if (m_names.empty()) {
		return nullptr;
	}
	auto it = m_names.find(std::string(nid));
	return it != m_names.end() ? &it->second : nullptr;
}

NameTable& GlobalNames() {
	static NameTable table;
	return table;
}

const std::string* FindNameForImport(std::string_view name) {
	const auto end = name.find_first_of("[#");
	return GlobalNames().Find(name.substr(0, end));
}

} // namespace Loader::Nid
