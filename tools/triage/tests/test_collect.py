"""collect_logs.sh / collect_logs.ps1 build the zip the triage tool reads (pwsh test skips without pwsh)."""

import glob
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage_test_util as u  # noqa: E402


class CollectTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-collect-test-")
        self.kyty = os.path.join(self.tmp, "kyty")
        self.out = os.path.join(self.tmp, "out")
        self.scratch = os.path.join(self.tmp, "scratch")
        for d in (os.path.join(self.kyty, "_Logs"), self.out, self.scratch):
            os.makedirs(d)
        old = os.path.join(self.kyty, "_Logs", "PPSA00001_2026-11-18_10-00-00.log")
        with open(old, "wb") as f:
            f.write(b"--- Session 2026-11-18 10:00:00 ---\n")
        os.utime(old, (1, 1))
        shutil.copy(u.fixture("gta5-829-v22-session-log.txt"),
                    os.path.join(self.kyty, "_Logs", "PPSA04263_2026-11-10_18-12-48.log"))
        shutil.copy(u.fixture("synthetic-contract-device-lost.txt"), os.path.join(self.kyty, "_kyty.txt"))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def check_zip(self):
        zips = glob.glob(os.path.join(self.out, "kyty-logs-*.zip"))
        self.assertEqual(len(zips), 1)
        with zipfile.ZipFile(zips[0]) as z:
            names = sorted(os.path.basename(n) for n in z.namelist())
        self.assertEqual(names, ["PPSA04263_2026-11-10_18-12-48.log", "_kyty.txt", "collect_info.txt"])
        rep = u.kt.triage(zips, rules=u.RULES)
        self.assertEqual(rep["primary"]["category"], "DEVICE_LOST")
        self.assertIn("sampler border color word changed", [h["hack"] for h in rep["workarounds"]["hacks"]])
        self.assertEqual(os.listdir(self.scratch), [], "staging folder removed")

    @unittest.skipUnless(shutil.which("sh"), "no sh")
    def test_sh(self):
        env = dict(os.environ, TMPDIR=self.scratch, PATH="/usr/bin:/bin")
        r = subprocess.run(["sh", os.path.join(u.TOOL_DIR, "collect_logs.sh"), self.kyty, self.out],
                           capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.check_zip()

    @unittest.skipUnless(shutil.which("pwsh"), "no pwsh")
    def test_powershell(self):
        env = dict(os.environ, TMPDIR=self.scratch, TEMP=self.scratch, TMP=self.scratch)
        r = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", os.path.join(u.TOOL_DIR, "collect_logs.ps1"),
                            "-KytyDir", self.kyty, "-OutDir", self.out], capture_output=True, text=True, env=env,
                           timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.check_zip()


if __name__ == "__main__":
    unittest.main()
