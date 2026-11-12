"""Input handling: .gz, .zip members in memory, stdin, CRLF, truncated lines, bad UTF-8, big files."""

import gzip
import io
import os
import shutil
import sys
import tempfile
import time
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage_test_util as u  # noqa: E402

kt = u.kt

FATAL = (b"--- Build ---\nOfficial build KytyPS5-2026-11-19-abcdef1\n--- Error ---\n"
         b"TextureCache: invalid depth upload\n in D:\\a\\KytyPS5\\KytyPS5\\src\\graphics\\host_gpu\\renderer\\cache"
         b"\\textureCache.cpp:1081\n")
NOISE = b"\t cmd  = 0x0000000242d976d8\n\t data = 0x000000000002e8ab\nEndOfPipe Signal!!! [0x0000000203446080]\n"


class InputTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-triage-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def path(self, name):
        return os.path.join(self.tmp, name)

    def write(self, name, data):
        with open(self.path(name), "wb") as f:
            f.write(data)
        return self.path(name)

    def run_paths(self, *paths, **kw):
        return kt.triage(list(paths), rules=u.RULES, repo=u.REPO, **kw)

    def test_gzip_same_as_plain(self):
        with open(u.fixture("gta5-810-depth-upload-rx9070xt-linux.txt"), "rb") as f:
            data = f.read()
        plain = self.write("_kyty.txt", data)
        gz = self.path("_kyty.txt.gz")
        with gzip.open(gz, "wb") as f:
            f.write(data)
        a = u.golden_summary(self.run_paths(plain))
        b = u.golden_summary(self.run_paths(gz))
        self.assertEqual(a, b)
        self.assertEqual(b["crash_site"], "textureCache.cpp:1081")

    def test_zip_members_are_read_in_memory(self):
        with open(u.fixture("gta5-829-v22-session-log.txt"), "rb") as f:
            session = f.read()
        z = self.path("Kyty-log.zip")
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("PPSA04263_2026-11-10_18-12-48.log", session)
            zf.writestr("_kyty.txt", FATAL)
            zf.writestr("dump/shader.spv", b"\x03\x02\x23\x07" + b"\x00" * 64)
            zf.writestr("gpu_info.bin", b"\x00\x01\x02")
        before = set(os.listdir(self.tmp))
        rep = self.run_paths(z)
        self.assertEqual(set(os.listdir(self.tmp)), before, "nothing extracted")
        names = [s["name"] for s in rep["sources"]]
        self.assertEqual(len(names), 2, names)
        self.assertTrue(any(n.endswith(":_kyty.txt") for n in names))
        self.assertEqual(rep["primary"]["crash_site"], "textureCache.cpp:1081")
        self.assertEqual(rep["identity"]["title_id"], "PPSA04263")
        self.assertIn("sampler border color word changed", [h["hack"] for h in rep["workarounds"]["hacks"]])

    def test_stdin(self):
        rep = kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(NOISE + FATAL))
        self.assertEqual(rep["sources"][0]["name"], "<stdin>")
        self.assertEqual(rep["primary"]["category"], "RENDER_STATE")

    def test_crlf_truncated_and_invalid_utf8(self):
        data = (b"--- Session 2026-11-19 20:05:11 ---\r\nOfficial build KytyPS5-2026-11-19-abcdef1\r\n"
                b"Title ID: PPSA00001\r\n"
                b"[HACK] null SRT table read as zero: first at ahead resolution plain read, shader=0x01182642842237f5, "
                b"where=0x70\r\n"
                b"Soft-fail: sceNpFoo: bad bytes \xff\xfe\xc3\x28 here\r\n"
                + FATAL.replace(b"\n", b"\r\n") +
                b"\tEquWait: half a li")
        p = self.write("crlf.log", data)
        rep = self.run_paths(p)
        self.assertTrue(rep["sources"][0]["truncated_last_line"])
        self.assertEqual(rep["outcome"], "crash")
        self.assertEqual(rep["primary"]["crash_site"], "textureCache.cpp:1081")
        self.assertEqual(rep["primary"]["message"], "TextureCache: invalid depth upload")
        self.assertEqual(rep["identity"]["build_label"], "Official build KytyPS5-2026-11-19-abcdef1")
        self.assertEqual(rep["identity"]["session_start"], "2026-11-19 20:05:11")
        self.assertEqual(rep["workarounds"]["hacks"][0]["hack"], "null SRT table read as zero")
        self.assertIn("\ufffd", rep["soft_fails"][0]["message"])

    def test_truncated_without_fatal(self):
        p = self.write("cut.txt", b"Title ID: PPSA00001\n--- Execute: Main\n" + NOISE + b"\t cmd = 0x00000002")
        rep = self.run_paths(p)
        self.assertEqual(rep["outcome"], "truncated")
        self.assertEqual(rep["primary"]["category"], "NO_FATAL_TRUNCATED")

    def test_directory_picks_newest_session_log_and_printf_log(self):
        os.mkdir(self.path("_Logs"))
        old = self.write(os.path.join("_Logs", "PPSA00001_2026-11-18_10-00-00.log"),
                         b"--- Session 2026-11-18 10:00:00 ---\nTitle ID: PPSA00001\n")
        new = self.write(os.path.join("_Logs", "PPSA00002_2026-11-19_10-00-00.log"),
                         b"--- Session 2026-11-19 10:00:00 ---\nTitle ID: PPSA00002\n")
        os.utime(old, (time.time() - 100, time.time() - 100))
        self.write("_kyty.txt", FATAL)
        rep = self.run_paths(self.tmp)
        self.assertEqual(rep["identity"]["title_id"], "PPSA00002")
        self.assertEqual(len(rep["sources"]), 2)
        self.assertEqual(rep["primary"]["category"], "RENDER_STATE")

    def test_session_and_printf_report_deduplicated(self):
        a = self.write("session.log", b"--- Session 2026-11-19 10:00:00 ---\n" + FATAL)
        b = self.write("_kyty.txt", NOISE * 10 + FATAL + NOISE)
        rep = self.run_paths(a, b)
        self.assertEqual(len(rep["fatal_blocks"]), 1)

    def test_big_file_streams_and_quick_mode_reads_the_tail_first(self):
        p = self.path("big_kyty.txt")
        chunk = NOISE * 4000
        with open(p, "wb") as f:
            f.write(b"TITLE_ID, string = PPSA00009\nVulkan device: NVIDIA GeForce RTX 3050\n")
            for _ in range(int((40 << 20) / len(chunk))):
                f.write(chunk)
            f.write(b"Unresolved import stub called: Zx9cVb7nM3k[NpAuth_v1][NpAuth_v1.1][Func]\n")
            f.write(FATAL)
            f.write(NOISE * 50)
        t0 = time.time()
        full = self.run_paths(p)
        t_full = time.time() - t0
        self.assertEqual(full["primary"]["crash_site"], "textureCache.cpp:1081")
        self.assertEqual(full["identity"]["title_id"], "PPSA00009")
        self.assertEqual(full["sources"][0]["scanned"], "full")
        quick = self.run_paths(p, quick=True)
        self.assertTrue(quick["sources"][0]["scanned"].startswith("quick"))
        self.assertEqual(quick["primary"]["crash_site"], "textureCache.cpp:1081")
        self.assertEqual(quick["identity"]["gpu"]["name"], "NVIDIA GeForce RTX 3050")
        self.assertEqual(quick["imports"]["called_total"], 1)
        # generous bound: CI runners are slow, but a quadratic or per-line-decode slip shows up here
        self.assertLess(t_full, 60.0)

    def test_small_printf_log_keeps_indented_build_block(self):
        # Under LENIENT_MAX_BYTES a file is read leniently; the indented printf prefixes must still match.
        p = self.write("_kyty.txt", b"Build\n  version: Official build KytyPS5-2026-10-02-39e23ac\n"
                                    b"  os: Windows 10\n  threads: 20\n\t flip_mode = 1\n\t flip_mode = 1\n")
        rep = self.run_paths(p)
        self.assertTrue(rep["sources"][0]["lenient"])
        self.assertEqual(rep["identity"]["build_label"], "Official build KytyPS5-2026-10-02-39e23ac")
        self.assertEqual(rep["identity"]["host"].get("threads"), "20")
        self.assertEqual(rep["progress"]["flips"], 2)

    def test_lenient_and_strict_scans_agree_on_log_files(self):
        # Logs over LENIENT_MAX_BYTES are scanned strictly; the goldens only cover the lenient path.
        for name in sorted(os.listdir(u.FIXTURES)):
            if not name.endswith(".txt"):
                continue
            lenient = u.report(name)
            if not all(s["kind"] in ("printf-log", "session-log", "console-capture") for s in lenient["sources"]):
                continue
            old = kt.LENIENT_MAX_BYTES
            kt.LENIENT_MAX_BYTES = 0
            try:
                strict = kt.triage([u.fixture(name)], rules=u.RULES)
            finally:
                kt.LENIENT_MAX_BYTES = old
            with self.subTest(case=name):
                self.assertFalse(strict["sources"][0]["lenient"])
                self.assertEqual(u.golden_summary(strict), u.golden_summary(lenient))
                self.assertEqual(strict["identity"].get("host"), lenient["identity"].get("host"))
                self.assertEqual(strict["progress"]["flips"], lenient["progress"]["flips"])

    def test_pasted_markdown_quotes_and_backticks(self):
        text = (b"### GPU\n\nNVIDIA GeForce RTX 3050 8GB\n\n### OS\n\nWindows 11\n\n"
                b"> `Unhandled host exception: type=1 code=3221225477 pc=0x0000000901c2d3e4 access=1 "
                b"address=0x0000000000000010 in D:\\a\\KytyPS5\\KytyPS5\\src\\loader\\runtimeLinker.cpp:833`\n")
        rep = kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(text))
        self.assertEqual(rep["primary"]["category"], "HOST_FAULT_GUEST")
        self.assertEqual(rep["primary"]["crash_site"], "runtimeLinker.cpp:833")
        self.assertEqual(rep["identity"]["gpu"]["name"], "NVIDIA GeForce RTX 3050 8GB")
        self.assertEqual(rep["identity"]["gpu"]["vendor"], "NVIDIA")
        self.assertEqual(rep["identity"]["host"]["os"], "Windows 11")


if __name__ == "__main__":
    unittest.main()
