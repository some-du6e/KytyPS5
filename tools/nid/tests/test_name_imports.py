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
                                               "Fc8qxlKINYQ", "AAAAAAAAAAA", "3ZWa3AoyWZQ",
                                               "RolXukwAUco", "djxxOmW6-aw", "1jfXLRVzisc"]))
        # Stubbed at load, then bound by a module loaded later: Rebind: lines.
        self.assertEqual((imps["RolXukwAUco"]["kind"], imps["RolXukwAUco"]["kinds"]), ("rebound", ["load", "rebound"]))
        self.assertEqual((imps["djxxOmW6-aw"]["kind"], imps["djxxOmW6-aw"]["kinds"]), ("rebound", ["object", "rebound"]))
        self.assertEqual(imps["1jfXLRVzisc"]["kind"], "rebound")
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

    def write(self, name, lines):
        path = os.path.join(self.tmp, name)
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        return path

    STUB = ("Relocate: unresolved PLT import patched to stub [432] [0000000903cdd458] <- 00000002000003fc, "
            "%s[%s_v1][%s_v1.1][Func], Func, Global, <game>\\eboot.bin")
    WEBAPI = STUB % ("RolXukwAUco", "NpCppWebApi", "NpCppWebApi")
    UTILITY = STUB % ("pLr1fEQS1z8", "NpUtility", "NpUtility")
    PRX = "Loading: <game>\\sce_module/libSceNpCppWebApi.prx"

    def test_stub_then_rebind(self):
        # A gta6-prep printf log: the stub at the first relocation, the Rebind: line after the PRX.
        log = self.write("_kyty.txt", [self.WEBAPI, self.UTILITY, self.PRX,
                                       "Rebind: RolXukwAUco[NpCppWebApi_v1][NpCppWebApi_v1.1][Func] -> "
                                       "RolXukwAUco[NpCppWebApi_v1][NpCppWebApi_v1.1][Func] (program=eboot.bin)"])
        text = self.output(log).decode("ascii")
        self.assertIn("# totals: unresolved=1 named=1 called=0 object=0 load=1 rebound=1\n", text)
        self.assertIn("library NpCppWebApi: unresolved=0 named=0 rebound=1\n  rebound RolXukwAUco Func eboot.bin ", text)
        # A Rebind: line before the stub line leaves it unresolved: the last line wins.
        log = self.write("again.txt", ["Rebind: RolXukwAUco[NpCppWebApi_v1][NpCppWebApi_v1.1][Func] -> x (program=eboot.bin)",
                                       self.WEBAPI])
        self.assertEqual(self.imports(log)["RolXukwAUco"]["kind"], "load")

    def test_prx_loaded_later(self):
        # A build without Rebind: lines: the game loads its PRX after the first relocation.
        imps = self.imports(self.write("v22.txt", [self.WEBAPI, self.UTILITY, self.PRX]))
        self.assertEqual((imps["RolXukwAUco"]["kind"], imps["RolXukwAUco"]["prx"]), ("prx", "libSceNpCppWebApi.prx"))
        self.assertEqual((imps["pLr1fEQS1z8"]["kind"], imps["pLr1fEQS1z8"]["prx"]), ("load", None))
        text = self.output(self.write("v22.txt", [self.WEBAPI, self.PRX])).decode("ascii")
        self.assertIn("# totals: unresolved=0 named=0 called=0 object=0 load=0 prx=1\n", text)
        self.assertIn("  prx RolXukwAUco Func eboot.bin _ZN3sce2Np9CppWebApi7Matches2V125RequestMatchPlayerFactory"
                      "6createEPNS1_6Common10LibContextEPKcNS3_10PlayerTypeEPNS5_12IntrusivePtrINS3_18RequestMatchPlayerEEE"
                      " (libSceNpCppWebApi.prx loaded later)\n", text)
        # Loaded before the relocation (builds before 2026-10-04 loaded every PRX first): still a gap.
        self.assertEqual(self.imports(self.write("old.txt", [self.PRX, self.WEBAPI]))["RolXukwAUco"]["kind"], "load")
        # A build that writes Rebind: lines (it also writes the summary) said nothing: still a gap.
        log = self.write("prep.txt", [self.WEBAPI, "Unresolved imports: 1 functions, 0 objects; by library: NpCppWebApi=1",
                                      self.PRX])
        self.assertEqual(self.imports(log)["RolXukwAUco"]["kind"], "load")

    def test_least_resolved_log_wins(self):
        bound = self.write("bound.txt", [self.WEBAPI, self.PRX])
        stuck = self.write("stuck.txt", [self.WEBAPI])
        imp = self.imports(bound, stuck)["RolXukwAUco"]
        self.assertEqual((imp["kind"], imp["sources"]), ("load", ["bound.txt", "stuck.txt"]))
        self.assertEqual(self.imports(bound, self.write("bound2.txt", [self.WEBAPI, self.PRX]))["RolXukwAUco"]["kind"], "prx")
        # Two inputs with one file name are still two logs.
        os.makedirs(os.path.join(self.tmp, "other"))
        same = os.path.join(self.tmp, "other", "bound.txt")
        shutil.copy(stuck, same)
        self.assertEqual(self.imports(bound, same)["RolXukwAUco"]["kind"], "load")

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
