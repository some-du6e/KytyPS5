"""kyty_triage.py --nid-names: names appear with the option and nothing changes without it."""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nid_test_util as u  # noqa: E402

TRIAGE = os.path.join(u.TRIAGE_DIR, "kyty_triage.py")
GTA5 = os.path.join(u.TRIAGE_FIXTURES, "gta5-729-guest-write-rtx3060.txt")
NAMES = u.data("names.txt")


@unittest.skipUnless(os.path.isfile(TRIAGE) and os.path.isfile(GTA5), "tools/triage is not next to tools/nid")
class TriageNamesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kt = u.load("kyty_triage_nid_test", TRIAGE)

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-nid-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_triage(self, *args):
        out = os.path.join(self.tmp, "out")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(self.kt.main(list(args) + ["-o", out]), 0)
        with open(out, "r", encoding="utf-8") as f:
            return f.read()

    def test_markdown(self):
        text = self.run_triage("--nid-names", NAMES, GTA5)
        self.assertIn("- `BYIZGKm6bO4[NpUtility_v1][NpUtility_v1.1][Func]` x1 (sceNpBandwidthTestGetStatus)", text)
        self.assertIn("(sceVideoRecordingSetInfo, known harmless)", text)
        self.assertIn("Names from names.txt (--nid-names, 26 names): 5 imports named here.", text)
        plain = self.run_triage(GTA5)
        self.assertNotIn("sceNpBandwidthTestGetStatus", plain)
        self.assertNotIn("--nid-names", plain)
        # The option only adds: drop what it added and the plain report is left.
        stripped = text.replace("Names from names.txt (--nid-names, 26 names): 5 imports named here.\n\n", "")
        for name in ("sceNpBandwidthTestGetStatus", "sceNpBandwidthTestInitStartDownload", "sceNpBandwidthTestAbort",
                     "sceNpBandwidthTestShutdown"):
            stripped = stripped.replace(" (%s)" % name, "")
        stripped = stripped.replace("(sceVideoRecordingSetInfo, known harmless)", "(known harmless)")
        self.assertEqual(stripped, plain)

    def test_json_and_agent_prompt(self):
        rep = json.loads(self.run_triage("--json", "--nid-names", NAMES, GTA5))
        names = {c["nid"]: c.get("name") for c in rep["imports"]["called"]}
        self.assertEqual(names["kvdMF48mB3Y"], "sceNpBandwidthTestAbort")
        self.assertEqual(rep["imports"]["nid_names"], {"file": "names.txt", "names": 26, "named": 5})
        plain = json.loads(self.run_triage("--json", GTA5))
        self.assertNotIn("nid_names", plain["imports"])
        self.assertTrue(all("name" not in c for c in plain["imports"]["called"]))
        prompt = self.run_triage("--agent-prompt", "--nid-names", NAMES, GTA5)
        self.assertIn("pLr1fEQS1z8[NpUtility_v1][NpUtility_v1.1][Func] (sceNpBandwidthTestShutdown) x1", prompt)

    def test_log_names_win_and_objects_get_names(self):
        log = os.path.join(self.tmp, "session.log")
        with open(log, "w") as f:
            f.write("Unresolved import stub called: 1jfXLRVzisc[libkernel_v1][libkernel_v1.1][Func] (program=eboot.bin, "
                    "first call, returns=0x00000000, policy=zero)\n"
                    "Unresolved import name: 1jfXLRVzisc[libkernel_v1][libkernel_v1.1][Func] = nameFromTheLog\n"
                    "Unresolved object import: djxxOmW6-aw[libkernel_v1][libkernel_v1.1][Object] (program=eboot.bin) "
                    "-> invalid page 0x0000000ffff00000\n")
        rep = json.loads(self.run_triage("--json", "--nid-names", NAMES, log))
        self.assertEqual(rep["imports"]["called"][0]["name"], "nameFromTheLog")
        self.assertEqual(rep["imports"]["objects"][0]["name"], "__progname")
        self.assertEqual(rep["imports"]["nid_names"]["named"], 1)
        text = self.run_triage("--nid-names", NAMES, log)
        self.assertIn("Unresolved object imports: `djxxOmW6-aw[libkernel_v1][libkernel_v1.1][Object]` (__progname)", text)

    def test_missing_names_file(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.kt.main(["--nid-names", os.path.join(self.tmp, "none.txt"), GTA5])


if __name__ == "__main__":
    unittest.main()
