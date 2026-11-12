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
        # A stand-in vulkaninfo first on PATH, so the result does not depend on the host's vulkan-tools
        # (or System32\\vulkaninfo.exe). It warns on stderr, as the loader does about stale layer
        # entries, and exits nonzero.
        self.bin = os.path.join(self.tmp, "bin")
        os.makedirs(self.bin)
        fake = os.path.join(self.bin, "vulkaninfo")
        with open(fake, "w", newline="\n") as f:
            f.write("#!/bin/sh\necho 'FAKE vulkaninfo summary'\necho 'WARNING: [Loader Message] stale layer' >&2\nexit 1\n")
        os.chmod(fake, 0o755)
        with open(fake + ".cmd", "w", newline="\r\n") as f:
            f.write("@echo FAKE vulkaninfo summary\n@echo WARNING: [Loader Message] stale layer 1>&2\n@exit /b 1\n")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def check_zip(self, split=False):
        zips = glob.glob(os.path.join(self.out, "kyty-logs-*.zip"))
        self.assertEqual(len(zips), 1)
        with zipfile.ZipFile(zips[0]) as z:
            names = sorted(os.path.basename(n) for n in z.namelist())
            vk = [n for n in z.namelist() if os.path.basename(n) == "vulkaninfo.txt"]
            vk_text = z.read(vk[0]).decode("utf-8-sig") if vk else ""
        printf = ["_kyty.head.txt", "_kyty.tail.txt"] if split else ["_kyty.txt"]
        self.assertEqual(names, sorted(["PPSA04263_2026-11-10_18-12-48.log", "collect_info.txt", "vulkaninfo.txt"] +
                                       printf))
        self.assertIn("FAKE vulkaninfo summary", vk_text)
        self.assertIn("WARNING: [Loader Message] stale layer", vk_text)
        rep = u.kt.triage(zips, rules=u.RULES)
        self.assertEqual(rep["primary"]["category"], "DEVICE_LOST")
        self.assertIn("sampler border color word changed", [h["hack"] for h in rep["workarounds"]["hacks"]])
        self.assertEqual(os.listdir(self.scratch), [], "staging folder removed")

    def run_sh(self, **env):
        env = dict(os.environ, TMPDIR=self.scratch, PATH=self.bin + os.pathsep + "/usr/bin:/bin", **env)
        r = subprocess.run(["sh", os.path.join(u.TOOL_DIR, "collect_logs.sh"), self.kyty, self.out],
                           capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)

    def run_pwsh(self, *args):
        env = dict(os.environ, TMPDIR=self.scratch, TEMP=self.scratch, TMP=self.scratch,
                   PATH=self.bin + os.pathsep + os.environ.get("PATH", ""))
        r = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", os.path.join(u.TOOL_DIR, "collect_logs.ps1"),
                            "-KytyDir", self.kyty, "-OutDir", self.out] + list(args), capture_output=True, text=True,
                           env=env, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(shutil.which("sh"), "no sh")
    def test_sh(self):
        self.run_sh()
        self.check_zip()

    @unittest.skipUnless(shutil.which("sh"), "no sh")
    def test_sh_splits_a_printf_log_over_the_limit(self):
        self.run_sh(MAX_PRINTF_MB="0")
        self.check_zip(split=True)

    @unittest.skipUnless(shutil.which("pwsh"), "no pwsh")
    def test_powershell(self):
        self.run_pwsh()
        self.check_zip()

    @unittest.skipUnless(shutil.which("pwsh"), "no pwsh")
    def test_powershell_splits_a_printf_log_over_the_limit(self):
        # -MaxPrintfMB below the 128 MiB tail: the tail starts at offset 0.
        self.run_pwsh("-MaxPrintfMB", "0")
        self.check_zip(split=True)

if __name__ == "__main__":
    unittest.main()
