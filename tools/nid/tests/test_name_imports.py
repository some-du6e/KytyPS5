"""name_imports.py: report naming, log lists, input forms and --hle, offline."""

import contextlib
import gzip
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nid_test_util as u  # noqa: E402

ni = u.load_tool("name_imports")
NAMES = u.data("names.txt")


def run(*args, stdin=None):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = ni.main(["--names", NAMES] + list(args), stdin=stdin)
    return code, err.getvalue()


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-nid-test-")
        self.table = u.nid.load_names(NAMES)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_name_report(self):
        text = u.read_bytes(u.data("synthetic-report-unnamed.txt")).decode("ascii")
        named, stats = ni.name_report(text, self.table)
        self.assertEqual(named, u.read_bytes(u.data("synthetic-report-named.txt")).decode("ascii"))
        self.assertEqual((stats["unresolved"], stats["named"], stats["named_before"], stats["differs"]), (5, 3, 1, 0))
        again, stats = ni.name_report(named, self.table)
        self.assertEqual(again, named)
        self.assertEqual(stats["named"], 0)

    def test_crlf_is_kept(self):
        text = u.read_bytes(u.data("synthetic-report-unnamed.txt")).decode("ascii").replace("\n", "\r\n")
        named, _stats = ni.name_report(text, self.table)
        self.assertEqual(named, u.read_bytes(u.data("synthetic-report-named.txt")).decode("ascii").replace("\n", "\r\n"))

    def test_main_writes_the_named_report(self):
        out = os.path.join(self.tmp, "named.txt")
        code, err = run(u.data("synthetic-report-unnamed.txt"), "-o", out)
        self.assertEqual(code, 0)
        self.assertEqual(u.read_bytes(out), u.read_bytes(u.data("synthetic-report-named.txt")))
        self.assertIn("named 3 of 5 unresolved imports (1 had a name already)", err)

    def test_report_with_other_inputs_is_refused(self):
        with self.assertRaises(SystemExit):
            run(u.data("synthetic-log.txt"), u.data("synthetic-report-unnamed.txt"))


class LogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-nid-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def output(self, *args, stdin=None):
        path = os.path.join(self.tmp, "out")
        code, _err = run(*(list(args) + ["-o", path]), stdin=stdin)
        self.assertEqual(code, 0)
        return u.read_bytes(path)

    def imports(self, *args, stdin=None):
        data = json.loads(self.output(*(list(args) + ["--json"]), stdin=stdin))
        return {i["nid"]: i for i in data["imports"]}

    def test_list_golden(self):
        self.assertEqual(self.output(u.data("synthetic-log.txt")), u.read_bytes(u.data("synthetic-log.expected.txt")))

    def test_every_line_form(self):
        imps = self.imports(u.data("synthetic-log.txt"))
        self.assertEqual(sorted(imps), sorted(["pLr1fEQS1z8", "kvdMF48mB3Y", "BYIZGKm6bO4", "hqzi1IHdQQQ",
                                               "Fc8qxlKINYQ", "AAAAAAAAAAA", "3ZWa3AoyWZQ"]))
        self.assertEqual(imps["pLr1fEQS1z8"]["kinds"], ["load"])        # Relocate: unresolved
        self.assertEqual(imps["pLr1fEQS1z8"]["programs"], ["eboot.bin"])
        self.assertEqual(imps["kvdMF48mB3Y"]["kinds"], ["called", "load"])  # Stubbed: + pasted console line
        self.assertEqual(imps["BYIZGKm6bO4"]["name_from"], "log")       # Unresolved import name:
        self.assertEqual(imps["hqzi1IHdQQQ"]["name_from"], "names")
        self.assertEqual(imps["Fc8qxlKINYQ"]["calls"], 2)               # printf + console form
        self.assertEqual(imps["Fc8qxlKINYQ"]["programs"], ["eboot.bin"])
        self.assertEqual(imps["Fc8qxlKINYQ"]["library"], "VideoRecordingP")
        self.assertEqual(imps["AAAAAAAAAAA"]["kind"], "object")
        self.assertIsNone(imps["AAAAAAAAAAA"]["name"])
        self.assertEqual(imps["3ZWa3AoyWZQ"]["kind"], "called")         # --strict-imports EXIT text
        self.assertEqual(imps["3ZWa3AoyWZQ"]["name"], "sceAgcAsyncCondExecPatchSetCommandAddress")

    def test_gz_zip_and_stdin(self):
        raw = u.read_bytes(u.data("synthetic-log.txt"))
        gz = os.path.join(self.tmp, "log.txt.gz")
        with gzip.open(gz, "wb") as f:
            f.write(raw)
        zp = os.path.join(self.tmp, "logs.zip")
        with zipfile.ZipFile(zp, "w") as z:
            z.writestr("_Logs/session.log", raw)

        def strip(imps):
            return {k: {f: v for f, v in i.items() if f != "sources"} for k, i in imps.items()}
        want = strip(self.imports(u.data("synthetic-log.txt")))
        self.assertEqual(strip(self.imports(gz)), want)
        self.assertEqual(strip(self.imports(zp)), want)
        self.assertEqual(strip(self.imports("-", stdin=io.BytesIO(raw))), want)
        self.assertEqual(self.imports(zp)["BYIZGKm6bO4"]["sources"], ["logs.zip:_Logs/session.log"])

    def test_hle(self):
        repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(repo, "src", "libs"))
        with open(os.path.join(repo, "src", "libs", "lib.cpp"), "w") as f:
            f.write('LIB_FUNC("hqzi1IHdQQQ", NpBandwidthTestInitStartDownload);\n'
                    'LIB_FUNC("AAAAAAAAAAA", NotAnObject);\n')
        imps = self.imports(u.data("synthetic-log.txt"), "--hle", "--repo", repo)
        self.assertEqual(imps["hqzi1IHdQQQ"]["hle"],
                         [{"kind": "FUNC", "file": "src/libs/lib.cpp", "line": 1,
                           "function": "NpBandwidthTestInitStartDownload"}])
        self.assertEqual(imps["AAAAAAAAAAA"]["hle"], [])  # an Object import needs a LIB_OBJECT
        text = self.output(u.data("synthetic-log.txt"), "--hle", "--repo", repo).decode("ascii")
        self.assertIn("# hle: 1 of them registered by a LIB_FUNC/LIB_OBJECT in this checkout now\n", text)
        self.assertIn("  called hqzi1IHdQQQ Func eboot.bin sceNpBandwidthTestInitStartDownload"
                      " (hle now: src/libs/lib.cpp:1 NpBandwidthTestInitStartDownload)\n", text)

    @unittest.skipUnless(os.path.isdir(u.TRIAGE_FIXTURES), "no triage fixtures")
    def test_gta5_fixture(self):
        imps = self.imports(os.path.join(u.TRIAGE_FIXTURES, "gta5-729-guest-write-rtx3060.txt"))
        called = {n: i["name"] for n, i in imps.items() if i["kind"] == "called"}
        self.assertEqual(called, {
            "BYIZGKm6bO4": "sceNpBandwidthTestGetStatus", "hqzi1IHdQQQ": "sceNpBandwidthTestInitStartDownload",
            "kvdMF48mB3Y": "sceNpBandwidthTestAbort", "pLr1fEQS1z8": "sceNpBandwidthTestShutdown",
            "Fc8qxlKINYQ": "sceVideoRecordingSetInfo"})

    def test_missing_names_file(self):
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                ni.main(["--names", os.path.join(self.tmp, "none.txt"), u.data("synthetic-log.txt")])


if __name__ == "__main__":
    unittest.main()
