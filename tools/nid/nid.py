#!/usr/bin/env python3
"""NIDs as KytyPS5 computes them: a Python port of src/loader/nid.cpp.

symbol_to_nid() is Loader::Nid::SymbolToNid, NameTable is Loader::Nid::NameTable and
find_name_for_import() is Loader::Nid::FindNameForImport. They follow the C++ rules exactly, so a
names file gives the same NID -> name map here as under --nid-names in the emulator.

As a command:
  python3 -I tools/nid/nid.py sceKernelUsleep ...          print the NID of each name
  python3 -I tools/nid/nid.py --names FILE 1jfXLRVzisc ...  look NIDs up in a names file

Standard library only.
"""

import argparse
import hashlib
import sys

# SymbolToNid's salt, appended to the symbol name before hashing, and its alphabet: base64 with
# '+' and '-' as the last two characters.
NID_SALT = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                  0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])
NID_CODES = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
NID_LENGTH = 11

_MASK32 = 0xFFFFFFFF


def _rotl(x, n):
    return ((x << n) | (x >> (32 - n))) & _MASK32


def sha1_digest(data):
    """Loader::Nid::Sha1Digest, line for line: plain SHA-1 (FIPS 180-4) of `data`, 20 bytes.

    symbol_to_nid() uses hashlib by default, which is much faster; the tests check that this port
    and hashlib agree on every padding case, so the choice does not change any NID."""
    data = bytes(data)
    h0, h1, h2, h3, h4 = 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0

    msg = bytearray(data)
    bit_len = (len(data) * 8) & 0xFFFFFFFFFFFFFFFF
    msg.append(0x80)
    while len(msg) % 64 != 56:
        msg.append(0)
    for i in range(7, -1, -1):
        msg.append((bit_len >> (i * 8)) & 0xFF)

    for offset in range(0, len(msg), 64):
        w = [0] * 80
        for i in range(16):
            j = offset + i * 4
            w[i] = (msg[j] << 24) | (msg[j + 1] << 16) | (msg[j + 2] << 8) | msg[j + 3]
        for i in range(16, 80):
            w[i] = _rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1)

        a, b, c, d, e = h0, h1, h2, h3, h4
        for i in range(80):
            if i < 20:
                f = (b & c) | ((~b & _MASK32) & d)
                k = 0x5A827999
            elif i < 40:
                f = b ^ c ^ d
                k = 0x6ED9EBA1
            elif i < 60:
                f = (b & c) | (b & d) | (c & d)
                k = 0x8F1BBCDC
            else:
                f = b ^ c ^ d
                k = 0xCA62C1D6
            temp = (_rotl(a, 5) + f + e + k + w[i]) & _MASK32
            e = d
            d = c
            c = _rotl(b, 30)
            b = a
            a = temp

        h0 = (h0 + a) & _MASK32
        h1 = (h1 + b) & _MASK32
        h2 = (h2 + c) & _MASK32
        h3 = (h3 + d) & _MASK32
        h4 = (h4 + e) & _MASK32

    return b"".join(h.to_bytes(4, "big") for h in (h0, h1, h2, h3, h4))


def _hashlib_sha1(data):
    try:
        return hashlib.sha1(data, usedforsecurity=False).digest()
    except TypeError:  # Python < 3.9 has no usedforsecurity
        return hashlib.sha1(data).digest()


def _pick_sha1():
    try:
        if _hashlib_sha1(b"abc") == sha1_digest(b"abc"):
            return _hashlib_sha1
    except (ValueError, AttributeError):  # a build without SHA-1 in hashlib
        pass
    return sha1_digest


_sha1 = _pick_sha1()


def to_bytes(symbol):
    """A symbol name as the bytes the emulator hashes: str is UTF-8, with surrogateescape so that
    names read with that error handler hash to their original bytes."""
    if isinstance(symbol, str):
        return symbol.encode("utf-8", "surrogateescape")
    return bytes(symbol)


def nid_value(symbol, sha1=None):
    """The 64-bit number behind a NID: the first 8 bytes of SHA-1(symbol + NID_SALT), read
    little-endian. SymbolToNid memcpy()s them into a uint64_t, and every host it runs on is
    little-endian."""
    digest = (sha1 or _sha1)(to_bytes(symbol) + NID_SALT)
    return int.from_bytes(digest[:8], "little")


def encode_nid(value):
    """SymbolToNid's text form of a 64-bit value: 10 characters of 6 bits from the top, then the
    low 4 bits times 4 (base64 of the 8 big-endian bytes without padding)."""
    chars = [NID_CODES[(value >> (58 - i * 6)) & 0x3F] for i in range(10)]
    chars.append(NID_CODES[(value & 0xF) * 4])
    return "".join(chars)


def decode_nid(nid):
    """encode_nid() backwards. ValueError for text that encode_nid() cannot produce."""
    if len(nid) != NID_LENGTH:
        raise ValueError("a NID has %d characters: %r" % (NID_LENGTH, nid))
    try:
        digits = [NID_CODES.index(ch) for ch in nid]
    except ValueError:
        raise ValueError("not a NID: %r" % (nid,)) from None
    if digits[10] & 3:
        raise ValueError("not a NID (low bits of the last character): %r" % (nid,))
    value = 0
    for digit in digits[:10]:
        value = (value << 6) | digit
    return (value << 4) | (digits[10] >> 2)


def symbol_to_nid(symbol, sha1=None):
    """Loader::Nid::SymbolToNid: the 11-character NID of a symbol name (str or bytes)."""
    return encode_nid(nid_value(symbol, sha1))


def is_nid(text):
    """True for an 11-character string that SymbolToNid could have produced."""
    try:
        decode_nid(text)
    except (ValueError, TypeError):
        return False
    return True


def nid_from_name(name):
    """Loader::ImportDiagnostics::NidFromName: the NID at the start of a generated import name
    ("<nid>[<Lib>_v1][<Mod>_v1.1][Func]") or a raw one ("<nid>#<lib>#<mod>")."""
    end = len(name)
    for sep in "[#":
        i = name.find(sep)
        if i != -1:
            end = min(end, i)
    return name[:end]


class NameTable:
    """Loader::Nid::NameTable: a NID -> symbol name map built from a names file."""

    def __init__(self):
        self.names = {}

    def add_names(self, text):
        """AddNames: one symbol name per line. Lines end at '\\n'; trailing '\\r', ' ' and '\\t' and
        leading ' ' and '\\t' are trimmed; empty lines and lines starting with '#' after that are
        skipped. A later name with the same NID replaces the earlier one. Returns the number of
        names read, duplicates included, like the C++."""
        added = 0
        for line in to_bytes(text).split(b"\n"):
            line = line.rstrip(b"\r \t").lstrip(b" \t")
            if not line or line.startswith(b"#"):
                continue
            self.names[symbol_to_nid(line)] = line.decode("utf-8", "surrogateescape")
            added += 1
        return added

    def load_file(self, path):
        """LoadFile: adds the names in the file. Returns the count; OSError when it cannot be read."""
        with open(path, "rb") as f:
            return self.add_names(f.read())

    def find(self, nid):
        """Find: the symbol name of a NID, or None."""
        return self.names.get(nid)

    def __len__(self):
        return len(self.names)


def find_name_for_import(table, name):
    """Loader::Nid::FindNameForImport: the table's name for the NID at the start of a generated or
    raw import name, or None."""
    return table.find(nid_from_name(name))


def load_names(path):
    """A NameTable filled from one names file."""
    table = NameTable()
    table.load_file(path)
    return table


def _items_after_dashes(argv):
    """argv with the options first and the other arguments after '--', in their order. argparse
    takes an argument that starts with '-' for an option, and about one NID in 64 does
    (-Rjp3-YViXc). One that starts with '-' and is not a NID or an import name stays an option, so
    a mistyped option still fails."""
    options, items = [], []
    args = iter(argv)
    for arg in args:
        if arg == "--":
            items.extend(args)
        elif arg == "--names":
            options.append(arg)
            value = next(args, None)
            if value is not None:
                options.append(value)
        elif arg.startswith("-") and not is_nid(nid_from_name(arg)):
            options.append(arg)
        else:
            items.append(arg)
    return options + ["--"] + items


def main(argv=None):
    ap = argparse.ArgumentParser(prog="nid.py", description="Compute NIDs of symbol names, or look NIDs up "
                                 "in a names file (the --nid-names format).", allow_abbrev=False)
    ap.add_argument("items", nargs="+", help="symbol names; with --names, NIDs or import names to look up")
    ap.add_argument("--names", metavar="FILE", help="names file to look the items up in")
    args = ap.parse_args(_items_after_dashes(sys.argv[1:] if argv is None else argv))
    out = []
    if args.names:
        table = load_names(args.names)
        for item in args.items:
            name = find_name_for_import(table, item)
            out.append("%s %s" % (item, name if name is not None else "?"))
    else:
        for item in args.items:
            out.append("%s %s" % (symbol_to_nid(item), item))
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
