"""build_names.py: parsers, filter, cache and download checks, HLE scan and the whole run, offline."""

import contextlib
import hashlib
import io
import os
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nid_test_util as u  # noqa: E402

bn = u.load_tool("build_names")


def parse(fmt, data):
    return list(bn.PARSERS[fmt](io.BytesIO(data)))


class FakeResponse(io.BytesIO):
    def __init__(self, data, headers=None):
        super().__init__(data)
        self.headers = headers or {}

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class FakeOpener:
    """urllib.request.urlopen stand-in that serves fixed bytes per URL, or (bytes, headers), and
    counts the requests."""

    def __init__(self, files):
        self.files = files
        self.requests = []

    def __call__(self, req, timeout=None):
        url = req.full_url
        self.requests.append(url)
        if url not in self.files:
            raise OSError("404 for %s" % url)
        body = self.files[url]
        return FakeResponse(*body) if isinstance(body, tuple) else FakeResponse(body)


def fake_source(sid, fmt, data, **kw):
    s = bn.Source(sid, "example/%s" % sid, "0123456789abcdef0123456789abcdef01234567", "list.txt", fmt,
                  kw.pop("license", "GPL-2.0-or-later"), kw.pop("platform", "PS5"), hashlib.sha256(data).hexdigest(),
                  len(data), **kw)
    return s


class ParserTests(unittest.TestCase):
    def test_lines(self):
        rows = parse("lines", b"\xef\xbb\xbfsceKernelUsleep\r\n\r\nsceNetHtonl\n  cos  \n")
        self.assertEqual([r[0] for r in rows], [b"sceKernelUsleep", b"", b"sceNetHtonl", b"cos"])
        self.assertTrue(all(r[1] is None and r[2] is None for r in rows))

    def test_pairs(self):
        rows = parse("pairs", b"1jfXLRVzisc sceKernelUsleep\r\n9T2pDF2Ryqg sceNetHtonl\nnot a pair\n")
        self.assertEqual(rows, [(b"sceKernelUsleep", "1jfXLRVzisc", None), (b"sceNetHtonl", "9T2pDF2Ryqg", None)])

    def test_xml(self):
        rows = parse("xml", b'<?xml version="1.0"?>\n<DynlibDatabase>\n\t<Entry obf="1jfXLRVzisc" sym="sceKernelUsleep"/>\n'
                            b'\t<Entry obf="AAAAAAAAAAA" sym="a&amp;b&#x41;&lt;"/>\n</DynlibDatabase>\n')
        self.assertEqual(rows, [(b"sceKernelUsleep", "1jfXLRVzisc", None), (b"a&bA<", "AAAAAAAAAAA", None)])

    def test_pascal(self):
        rows = parse("pascal", b" FunctData:array[0..1] of TFunctData=(\n  (i:$000009F49DC1282D;n:'x''y'),\n"
                               b"  (i:$0;n:'cos'));\n")
        self.assertEqual(rows, [(b"x'y", None, 0x9F49DC1282D), (b"cos", None, 0)])

    def test_usable_name(self):
        for good in (b"sceKernelUsleep", b"_ZN3sce2Np9CppWebApi6Common6StringD1Ev", b".L.str", b"a#b"):
            self.assertTrue(bn.usable_name(good), good)
        for bad in (b"", b"#define", b"a b", b"a\tb", b"\x7f", "naïve".encode("utf-8"), b"x\x00"):
            self.assertFalse(bn.usable_name(bad), bad)


class ReadSourceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-nid-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_pair_check(self):
        pairs = u.confirmed_pairs()[:3]
        lines = ["%s %s" % (n, name) for n, name, _w, _f in pairs]
        lines.append("AAAAAAAAAAA %s" % pairs[0][1])  # the right name under a wrong NID
        lines.append("BBBBBBBBBBB two words")
        path = os.path.join(self.tmp, "pairs.csv")
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        r = bn.read_source(fake_source("p", "pairs", b""), path)
        self.assertEqual((r.entries, r.pairs, r.pairs_matching), (5, 5, 3))
        self.assertEqual(r.mismatches, [("AAAAAAAAAAA", pairs[0][1]), ("BBBBBBBBBBB", "two")])
        self.assertEqual(len(r.names), 4)

    def test_rejected_names_are_counted(self):
        path = os.path.join(self.tmp, "names.txt")
        with open(path, "wb") as f:
            f.write(b"sceKernelUsleep\n# comment\nwith space\n\n")
        r = bn.read_source(fake_source("l", "lines", b""), path)
        self.assertEqual((r.entries, r.rejected, sorted(r.names)), (4, 3, [b"sceKernelUsleep"]))


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-nid-test-")
        self.data = b"sceKernelUsleep\nsceNetHtonl\n"
        self.source = fake_source("one", "lines", self.data)
        self.log = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_download_then_cache(self):
        opener = FakeOpener({self.source.url: self.data})
        path, digest = bn.fetch(self.source, self.tmp, opener=opener, log=self.log.append)
        self.assertEqual(u.read_bytes(path), self.data)
        self.assertEqual(digest, self.source.sha256)
        self.assertTrue(path.startswith(os.path.join(self.tmp, "one", self.source.rev[:12])))
        bn.fetch(self.source, self.tmp, opener=opener, log=self.log.append)
        self.assertEqual(len(opener.requests), 1)
        bn.fetch(self.source, self.tmp, opener=opener, refresh=True, log=self.log.append)
        self.assertEqual(len(opener.requests), 2)
        self.assertEqual([n for n in os.listdir(os.path.dirname(path))], ["list.txt"])

    def test_hash_mismatch(self):
        opener = FakeOpener({self.source.url: self.data.replace(b"Usleep", b"Usleeq")})
        with self.assertRaises(bn.FetchError) as cm:
            bn.fetch(self.source, self.tmp, opener=opener, log=self.log.append)
        self.assertIn("SHA-256", str(cm.exception))
        self.assertTrue(os.path.exists(self.source.cache_path(self.tmp) + ".bad"))
        self.assertFalse(os.path.exists(self.source.cache_path(self.tmp)))

    def test_size_mismatch_and_offline(self):
        with self.assertRaises(bn.FetchError):
            bn.fetch(self.source, self.tmp, offline=True, log=self.log.append)
        opener = FakeOpener({self.source.url: self.data + b"x\n"})
        with self.assertRaises(bn.FetchError) as cm:
            bn.fetch(self.source, self.tmp, opener=opener, log=self.log.append)
        self.assertIn("bytes, expected", str(cm.exception))

    def test_failed_download_leaves_nothing(self):
        with self.assertRaises(bn.FetchError):
            bn.fetch(self.source, self.tmp, opener=FakeOpener({}), log=self.log.append)
        self.assertEqual(os.listdir(os.path.dirname(self.source.cache_path(self.tmp))), [])

    def test_short_body_and_html_page(self):
        # A source being re-pinned has no size or hash yet, so download() has to catch these itself.
        unpinned = bn.Source("new", "example/new", self.source.rev, "list.txt", "lines", "none", "PS5", None, None)
        for headers, error in (({"Content-Length": str(2 * len(self.data))}, "got %d of the" % len(self.data)),
                               ({"Content-Type": "text/html; charset=utf-8"}, "HTML page")):
            with self.subTest(headers=headers):
                with self.assertRaises(bn.FetchError) as cm:
                    bn.fetch(unpinned, self.tmp, opener=FakeOpener({unpinned.url: (self.data, headers)}),
                             log=self.log.append)
                self.assertIn(error, str(cm.exception))
                self.assertEqual(os.listdir(os.path.dirname(unpinned.cache_path(self.tmp))), [])
        headers = {"Content-Length": str(len(self.data)), "Content-Type": "text/plain; charset=utf-8"}
        path, _digest = bn.fetch(unpinned, self.tmp, opener=FakeOpener({unpinned.url: (self.data, headers)}),
                                 log=self.log.append)
        self.assertEqual(u.read_bytes(path), self.data)

    def test_failed_refresh_keeps_the_cached_copy(self):
        path, _digest = bn.fetch(self.source, self.tmp, opener=FakeOpener({self.source.url: self.data}),
                                 log=self.log.append)
        # Offline or rate-limited, then a changed file: the verified copy stays.
        for opener in (FakeOpener({}), FakeOpener({self.source.url: self.data.replace(b"Usleep", b"Usleeq")})):
            with self.assertRaises(bn.FetchError):
                bn.fetch(self.source, self.tmp, opener=opener, refresh=True, log=self.log.append)
            self.assertEqual(u.read_bytes(path), self.data)
        self.assertEqual(u.read_bytes(path + ".bad"), self.data.replace(b"Usleep", b"Usleeq"))
        self.assertEqual(sorted(os.listdir(os.path.dirname(path))), ["list.txt", "list.txt.bad"])
        self.assertEqual(bn.fetch(self.source, self.tmp, offline=True, log=self.log.append), (path, self.source.sha256))


class HleScanTests(unittest.TestCase):
    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="kyty-nid-repo-")
        os.makedirs(os.path.join(self.repo, "src", "libs"))
        with open(os.path.join(self.repo, "src", "libs", "libExample.cpp"), "w") as f:
            f.write('#define LIB_FUNC(n, f) LIB_ADD(n, f, Func)\n'
                    'LIB_DEFINE(InitExample) {\n'
                    '\tLIB_FUNC("1jfXLRVzisc", Example::KernelUsleep);\n'
                    '\tLIB_FUNC("9T2pDF2Ryqg",\n\t         Example::NetHtonl);\n'
                    '\tLIB_OBJECT("djxxOmW6-aw", &Example::g_progname);\n'
                    '\tLIB_FUNC("AAAAAAAAAAA", Example::Unknown);\n'
                    '}\n')

    def tearDown(self):
        shutil.rmtree(self.repo, ignore_errors=True)

    def test_hle_nids(self):
        hle = bn.hle_nids(self.repo)
        self.assertEqual(list(hle), ["1jfXLRVzisc", "9T2pDF2Ryqg", "djxxOmW6-aw", "AAAAAAAAAAA"])
        self.assertEqual(hle["9T2pDF2Ryqg"], [("FUNC", "src/libs/libExample.cpp", 4, "Example::NetHtonl")])
        self.assertEqual(hle["djxxOmW6-aw"][0][0], "OBJECT")

    def test_share_counts_functions_only(self):
        table = u.nid.load_names(u.data("names.txt"))
        named, total, unnamed = bn.hle_share(table, bn.hle_nids(self.repo))
        self.assertEqual((named, total, unnamed), (2, 3, ["AAAAAAAAAAA"]))

    @unittest.skipUnless(u.in_checkout(), "not inside a KytyPS5 checkout")
    def test_real_tables(self):
        hle = bn.hle_nids(u.REPO)
        funcs = [n for n, regs in hle.items() if any(k == "FUNC" for k, _f, _l, _fn in regs)]
        self.assertGreater(len(funcs), 1000)
        self.assertTrue(all(u.nid.is_nid(n) for n in hle), [n for n in hle if not u.nid.is_nid(n)][:5])
        for nid_text, _name, where, _f in u.confirmed_pairs():
            if where.startswith("src/"):
                self.assertEqual(hle[nid_text][0][1], where)


class RegistryTests(unittest.TestCase):
    def test_sources_are_pinned(self):
        ids = [s.id for s in bn.SOURCES]
        self.assertEqual(len(ids), len(set(ids)))
        for s in bn.SOURCES:
            with self.subTest(source=s.id):
                self.assertRegex(s.rev, r"^[0-9a-f]{40}$")
                self.assertRegex(s.sha256 or "", r"^[0-9a-f]{64}$")
                self.assertIsInstance(s.size, int)
                self.assertIsInstance(s.names, int)
                self.assertIn(s.fmt, bn.PARSERS)
                self.assertIn(s.license, ("none", "GPL-2.0-or-later", "LGPL-2.1", "GPL-3.0"))
                self.assertEqual(s.url, "https://raw.githubusercontent.com/%s/%s/%s" % (s.repo, s.rev, s.path))

    def test_sources_md_lists_every_source(self):
        with open(os.path.join(u.TOOL_DIR, "SOURCES.md"), "r", encoding="utf-8") as f:
            text = f.read()
        for s in bn.SOURCES:
            with self.subTest(source=s.id):
                self.assertIn("`%s`" % s.id, text)
                self.assertIn(s.rev[:12], text)


class MainTests(unittest.TestCase):
    """The whole run against fake sources and a fake checkout: no network."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-nid-test-")
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(self.repo, "src", "libs"))
        with open(os.path.join(self.repo, "src", "libs", "lib.cpp"), "w") as f:
            f.write('LIB_FUNC("1jfXLRVzisc", A);\nLIB_FUNC("9T2pDF2Ryqg", B);\nLIB_FUNC("Fc8qxlKINYQ", C);\n')
        self.a = b"sceNetHtonl\r\nsceKernelUsleep\nsceKernelUsleep\nwith space\n"
        self.b = b'<Entry obf="Fc8qxlKINYQ" sym="sceVideoRecordingSetInfo"/>\n<Entry obf="AAAAAAAAAAA" sym="cos"/>\n'
        self.sources = [fake_source("a", "lines", self.a, license="none"), fake_source("b", "xml", self.b)]
        self.opener = FakeOpener({self.sources[0].url: self.a, self.sources[1].url: self.b})
        self._saved = (bn.SOURCES, bn.SOURCES_BY_ID)
        bn.SOURCES = self.sources
        bn.SOURCES_BY_ID = {s.id: s for s in self.sources}

    def tearDown(self):
        bn.SOURCES, bn.SOURCES_BY_ID = self._saved
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bn.main(["--cache", os.path.join(self.tmp, "cache"), "--repo", self.repo] + list(args),
                           opener=self.opener)
        return code, out.getvalue(), err.getvalue()

    def test_build(self):
        output = os.path.join(self.tmp, "names.txt")
        code, out, _err = self.run_main("-o", output)
        self.assertEqual(code, 0)
        text = u.read_bytes(output).decode("ascii")
        header = [line for line in text.splitlines() if line.startswith("#")]
        names = [line for line in text.splitlines() if not line.startswith("#")]
        self.assertEqual(names, ["cos", "sceKernelUsleep", "sceNetHtonl", "sceVideoRecordingSetInfo"])
        self.assertTrue(text.endswith("sceVideoRecordingSetInfo\n"))
        self.assertIn("# source a: example/a rev 0123456789abcdef0123456789abcdef01234567 list.txt (PS5; license none) names=2",
                      header)
        self.assertIn("# names: 4", header)
        self.assertIn("pair check: 1 of 2 NID/name pairs hash to the listed NID (50.00%)", out)
        self.assertIn("HLE check: 3 of 3 LIB_FUNC NIDs", out)
        self.assertIn("1 of 4 entries skipped", out)
        self.assertEqual(u.nid.load_names(output).find("9T2pDF2Ryqg"), "sceNetHtonl")
        self.assertEqual(len(self.opener.requests), 2)
        # Second run: everything from the cache, same bytes.
        code, _out, _err = self.run_main("-o", output + ".2", "--offline")
        self.assertEqual(code, 0)
        self.assertEqual(u.read_bytes(output), u.read_bytes(output + ".2"))
        self.assertEqual(len(self.opener.requests), 2)

    def test_default_output_is_in_the_cache(self):
        code, out, _err = self.run_main("--sources", "a")
        self.assertEqual(code, 0)
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "cache", bn.OUTPUT_NAME)))
        self.assertIn("HLE check: 2 of 3", out)

    def test_low_share_fails(self):
        code, _out, err = self.run_main("-o", os.path.join(self.tmp, "n.txt"), "--sources", "b", "--min-share", "0.9")
        self.assertEqual(code, 3)
        self.assertIn("the hash or the data is broken", err)

    def test_fetch_failure(self):
        self.opener.files.pop(self.sources[1].url)
        output = os.path.join(self.tmp, "n.txt")
        code, _out, err = self.run_main("-o", output)
        self.assertEqual(code, 1)
        self.assertFalse(os.path.exists(output))
        self.assertIn("nothing written", err)
        code, out, _err = self.run_main("-o", output, "--skip-failed")
        self.assertEqual(code, 0)
        self.assertIn("skipped: b", out)

    def test_failed_refresh_keeps_the_cache(self):
        output = os.path.join(self.tmp, "n.txt")
        self.assertEqual(self.run_main("-o", output)[0], 0)
        self.opener.files.clear()  # the network goes away
        code, _out, err = self.run_main("-o", output + ".2", "--refresh")
        self.assertEqual(code, 1)
        self.assertIn("nothing written", err)
        code, _out, _err = self.run_main("-o", output + ".2", "--offline")
        self.assertEqual(code, 0)
        self.assertEqual(u.read_bytes(output), u.read_bytes(output + ".2"))

    def test_source_without_names_fails(self):
        # A newer revision with the attributes the other way round: the parser finds no entry.
        swapped = b'<Entry sym="sceVideoRecordingSetInfo" obf="Fc8qxlKINYQ"/>\n'
        b = fake_source("b", "xml", swapped)
        bn.SOURCES[1] = bn.SOURCES_BY_ID["b"] = b
        self.opener.files[b.url] = swapped
        output = os.path.join(self.tmp, "n.txt")
        code, _out, err = self.run_main("-o", output)
        self.assertEqual(code, 1)
        self.assertIn("error: b: no names in list.txt; has its format changed?", err)
        self.assertIn("could not use b; nothing written", err)
        self.assertFalse(os.path.exists(output))
        code, out, _err = self.run_main("-o", output, "--skip-failed")
        self.assertEqual(code, 0)
        self.assertIn("skipped: b", out)

    def test_source_with_far_fewer_names_fails(self):
        self.sources[0].names = 3  # it gives 2
        code, _out, err = self.run_main("-o", os.path.join(self.tmp, "n.txt"))
        self.assertEqual(code, 1)
        self.assertIn("error: a: 2 names, expected about 3; has its format changed?", err)
        self.sources[0].names = 2
        self.assertEqual(self.run_main("-o", os.path.join(self.tmp, "n.txt"))[0], 0)

    def test_skip_failed_says_when_no_ps5_list_is_left(self):
        self.sources[0].platform = "PS4"
        self.opener.files.pop(self.sources[1].url)
        code, _out, err = self.run_main("-o", os.path.join(self.tmp, "n.txt"), "--skip-failed")
        self.assertEqual(code, 0)
        self.assertIn("warning: no PS5 list left (b skipped)", err)
        self.sources[0].platform = "PS5"
        code, _out, err = self.run_main("-o", os.path.join(self.tmp, "n.txt"), "--skip-failed")
        self.assertNotIn("warning", err)

    def test_extra_names(self):
        extra = os.path.join(self.tmp, "extra.txt")
        with open(extra, "w") as f:
            f.write("# mine\nsceNgs2RackUnlock\n")
        output = os.path.join(self.tmp, "n.txt")
        code, _out, _err = self.run_main("-o", output, "--sources", "a", "--extra", extra)
        self.assertEqual(code, 0)
        text = u.read_bytes(output).decode("ascii")
        self.assertIn("# extra: extra.txt names=1\n", text)
        self.assertIn("\nsceNgs2RackUnlock\n", text)

    def test_unknown_source(self):
        with self.assertRaises(SystemExit):
            self.run_main("--sources", "nope")

    def test_list_sources(self):
        code, out, _err = self.run_main("--list-sources")
        self.assertEqual(code, 0)
        self.assertTrue(re.search(r"^a\s+PS5\s+none", out, re.M))


if __name__ == "__main__":
    unittest.main()
