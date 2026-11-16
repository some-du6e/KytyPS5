"""nid.py against SHA-1 test vectors, hashlib and NID/name pairs confirmed outside this code."""

import base64
import contextlib
import hashlib
import io
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nid_test_util as u  # noqa: E402

nid = u.nid

# FIPS 180-2 appendix A and the empty message.
SHA1_VECTORS = [
    (b"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"),
    (b"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"),
    (b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1"),
    (b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
     "a49b2446a02c645bf419f995b67091253a04a259"),
]


class Sha1Tests(unittest.TestCase):
    def test_vectors(self):
        for message, digest in SHA1_VECTORS:
            with self.subTest(length=len(message)):
                self.assertEqual(nid.sha1_digest(message).hex(), digest)

    def test_port_matches_hashlib_at_every_padding_length(self):
        # 55/56 bytes is where the length field stops fitting in the first block, 119/120 the second.
        for n in range(0, 260):
            data = bytes((i * 7 + n) & 0xFF for i in range(n))
            with self.subTest(length=n):
                self.assertEqual(nid.sha1_digest(data), hashlib.sha1(data).digest())


class NidTests(unittest.TestCase):
    def test_confirmed_pairs(self):
        pairs = u.confirmed_pairs()
        self.assertGreaterEqual(len(pairs), 20)
        for nid_text, name, _where, _name_from in pairs:
            with self.subTest(name=name):
                self.assertEqual(nid.symbol_to_nid(name), nid_text)
                self.assertEqual(nid.symbol_to_nid(name.encode("ascii")), nid_text)
                self.assertEqual(nid.symbol_to_nid(name, sha1=nid.sha1_digest), nid_text)
                self.assertEqual(nid.nid_value(name), nid.decode_nid(nid_text))

    def test_pairs_cover_one_two_and_three_sha1_blocks(self):
        # name + 16 bytes of salt: up to 55 bytes is one block, up to 119 two, more is three or more.
        sizes = [len(name) + len(nid.NID_SALT) for _n, name, _w, _f in u.confirmed_pairs()]
        self.assertIn(55, sizes)
        self.assertIn(56, sizes)
        self.assertTrue(any(s > 119 for s in sizes))
        nids = "".join(n for n, _name, _w, _f in u.confirmed_pairs())
        self.assertIn("+", nids)
        self.assertIn("-", nids)

    @unittest.skipUnless(u.in_checkout(), "not inside a KytyPS5 checkout")
    def test_pair_provenance(self):
        """Each NID is where the fixture says, and each 'comment' name is in that LIB_FUNC's comment."""
        for nid_text, name, where, name_from in u.confirmed_pairs():
            with self.subTest(nid=nid_text):
                if where.startswith("src/"):
                    with open(os.path.join(u.REPO, where), "r", encoding="utf-8") as f:
                        text = f.read()
                    m = re.search(r'LIB_(?:FUNC|OBJECT)\(\s*"%s"[^\n]*' % re.escape(nid_text), text)
                    self.assertIsNotNone(m, "%s has no LIB_FUNC for %s" % (where, nid_text))
                    if "comment" in name_from.split():
                        self.assertRegex(m.group(0), r"//\s*%s\s*$" % re.escape(name))
                else:
                    self.assertIn(nid_text, u.read_bytes(os.path.join(u.TRIAGE_FIXTURES, where)).decode("utf-8", "replace"))

    @unittest.skipUnless(u.in_checkout(), "not inside a KytyPS5 checkout")
    def test_constants_match_the_cpp(self):
        with open(os.path.join(u.REPO, "src", "loader", "nid.cpp"), "r", encoding="utf-8") as f:
            cpp = f.read()
        salt = re.search(r"salt\s*=\s*\{([^}]*)\}", cpp).group(1)
        self.assertEqual(bytes(int(x, 16) for x in salt.replace(",", " ").split()), nid.NID_SALT)
        self.assertEqual(re.search(r'codes\[\]\s*=\s*"([^"]*)"', cpp).group(1), nid.NID_CODES)

    def test_encoding(self):
        self.assertEqual(nid.encode_nid(0), "AAAAAAAAAAA")
        self.assertEqual(nid.encode_nid((1 << 64) - 1), "----------8")
        for value in (0, 1, 0x3F, 0x8A5D379E5B8A7CC3, (1 << 64) - 1):
            with self.subTest(value=value):
                # The same text as base64 of the 8 big-endian bytes with '+' and '-'.
                expect = base64.b64encode(value.to_bytes(8, "big"), b"+-").decode("ascii").rstrip("=")
                self.assertEqual(nid.encode_nid(value), expect)
                self.assertEqual(nid.decode_nid(expect), value)

    def test_decode_rejects_what_encode_cannot_produce(self):
        for bad in ("AAAAAAAAAAB", "AAAAAAAAAA", "AAAAAAAAAAAA", "AAAAAAAAAA*", "AAAAAAAAA=A"):
            with self.subTest(text=bad):
                self.assertFalse(nid.is_nid(bad))
                with self.assertRaises(ValueError):
                    nid.decode_nid(bad)
        self.assertTrue(nid.is_nid("1jfXLRVzisc"))

    def test_nid_from_name(self):
        self.assertEqual(nid.nid_from_name("9T2pDF2Ryqg[Net_v1][Net_v1.1][Func]"), "9T2pDF2Ryqg")
        self.assertEqual(nid.nid_from_name("9T2pDF2Ryqg#A#B"), "9T2pDF2Ryqg")
        self.assertEqual(nid.nid_from_name("9T2pDF2Ryqg"), "9T2pDF2Ryqg")


class NameTableTests(unittest.TestCase):
    def test_cpp_name_table_case(self):
        # tests/HleSoftFailTests.cpp TestNameTable, same text.
        table = nid.NameTable()
        added = table.add_names("# comment\n\nsceNetHtonl\r\n  sceKernelUsleep  \nsceNetInetPton")
        self.assertEqual(added, 3)
        self.assertEqual(len(table), 3)
        self.assertEqual(table.find("1jfXLRVzisc"), "sceKernelUsleep")
        self.assertEqual(table.find("9T2pDF2Ryqg"), "sceNetHtonl")
        self.assertEqual(table.find("8Kcp5d-q1Uo"), "sceNetInetPton")
        self.assertIsNone(table.find("AAAAAAAAAAA"))

    def test_trimming_and_comments(self):
        table = nid.NameTable()
        added = table.add_names(b"\t sceKernelUsleep\t \r\n   # sceNetHtonl\n#sceNetInetPton\n \t\r\n")
        self.assertEqual(added, 1)
        self.assertEqual(table.find("1jfXLRVzisc"), "sceKernelUsleep")
        self.assertIsNone(table.find("9T2pDF2Ryqg"))  # '#' after leading blanks: a comment
        self.assertIsNone(table.find("8Kcp5d-q1Uo"))

    def test_only_newline_ends_a_line(self):
        # The C++ splits at '\n' only and keeps the BOM, so neither line below names anything.
        table = nid.NameTable()
        self.assertEqual(table.add_names("sceKernelUsleep\rsceNetHtonl\n﻿sceNetInetPton\n"), 2)
        self.assertIsNone(table.find("1jfXLRVzisc"))
        self.assertIsNone(table.find("9T2pDF2Ryqg"))
        self.assertIsNone(table.find("8Kcp5d-q1Uo"))

    def test_duplicates_count_as_added(self):
        table = nid.NameTable()
        self.assertEqual(table.add_names("sceKernelUsleep\nsceKernelUsleep\n"), 2)
        self.assertEqual(len(table), 1)

    def test_load_file_and_find_name_for_import(self):
        table = nid.load_names(u.data("names.txt"))
        self.assertEqual(len(table), len(u.confirmed_pairs()))
        for nid_text, name, _w, _f in u.confirmed_pairs():
            with self.subTest(name=name):
                self.assertEqual(table.find(nid_text), name)
        self.assertEqual(nid.find_name_for_import(table, "1jfXLRVzisc[libkernel_v1][libkernel_v1.1][Func]"),
                         "sceKernelUsleep")
        self.assertEqual(nid.find_name_for_import(table, "1jfXLRVzisc#A#B"), "sceKernelUsleep")
        self.assertIsNone(nid.find_name_for_import(table, "AAAAAAAAAAA[Net_v1][Net_v1.1][Func]"))
        with self.assertRaises(OSError):
            nid.load_names(u.data("no-such-file.txt"))


class CommandTests(unittest.TestCase):
    def run_main(self, argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(nid.main(argv), 0)
        return out.getvalue()

    def test_names_to_nids(self):
        self.assertEqual(self.run_main(["sceKernelUsleep", "sceNetHtonl"]),
                         "1jfXLRVzisc sceKernelUsleep\n9T2pDF2Ryqg sceNetHtonl\n")

    def test_lookup(self):
        self.assertEqual(self.run_main(["--names", u.data("names.txt"), "Fc8qxlKINYQ[VideoRecordingP_v1]", "AAAAAAAAAAA"]),
                         "Fc8qxlKINYQ[VideoRecordingP_v1] sceVideoRecordingSetInfo\nAAAAAAAAAAA ?\n")

    def test_lookup_of_nids_that_start_with_a_dash(self):
        # argparse would take these for options; -h... would even print the help.
        names = u.data("names.txt")
        self.assertEqual(self.run_main(["--names", names, "-2IRUCO--PM"]), "-2IRUCO--PM sceKernelReadTsc\n")
        self.assertEqual(self.run_main(["1jfXLRVzisc", "-2IRUCO--PM[libkernel_v1][libkernel_v1.1][Func]", "--names", names,
                                        "-hAbcdefghk"]),
                         "1jfXLRVzisc sceKernelUsleep\n-2IRUCO--PM[libkernel_v1][libkernel_v1.1][Func] sceKernelReadTsc\n"
                         "-hAbcdefghk ?\n")
        self.assertEqual(self.run_main(["--names", names, "--", "-2IRUCO--PM"]), "-2IRUCO--PM sceKernelReadTsc\n")
        for bad in (["--nmaes", names, "1jfXLRVzisc"], ["--names", names], ["--names"]):
            with self.subTest(argv=bad), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                nid.main(bad)


if __name__ == "__main__":
    unittest.main()
