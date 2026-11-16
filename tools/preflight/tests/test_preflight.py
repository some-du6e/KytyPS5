"""preflight.ps1 under pwsh, against a fake package in the layout of the Windows artifact:

  install/kyty_emulator              a shell script standing in for the emulator
  install/preflight/preflight.ps1    and collect_logs.ps1, copied from the repo
  install/preflight/gpu-tests/       tests.json and shell scripts as the test executables

The scripts pass, fail, hang past their time limit, flood stdout and stderr, check their
environment, and fail when tests.json says they should. Skips without pwsh, unless
KYTY_REQUIRE_PWSH is set (CI sets it). PWSH names the pwsh to use instead of the one on PATH.

LOCALAPPDATA points into the test's own folder, where game boots keep the shader cache. pwsh on
Linux makes no junction, so here a game boot always takes the fallback; CI's Windows PowerShell 5.1
smoke test checks the junction itself.
"""

import glob
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
REPO = os.path.normpath(os.path.join(TOOL, "..", ".."))
PWSH = os.environ.get("PWSH") or shutil.which("pwsh")

SCRIPTS = {
    "pass.sh": 'echo "pass: $*"\nexit 0\n',
    "fail.sh": "echo 'about to fail'\necho 'FakeTests: check X failed' >&2\nexit 3\n",
    # A child that keeps the pipes open, as a hung GPU test's helper would.
    "hang.sh": 'sleep 600 &\necho "$$ $!" > "$KYTY_FAKE_PIDS"\necho hanging\nwait\n',
    # More than a pipe buffer on both streams: reading one after the other would deadlock.
    "flood.sh": 'i=0\nwhile [ $i -lt 3000 ]; do\n  echo "out line $i, padding to fill the pipe"\n'
                '  echo "err line $i, padding to fill the pipe" >&2\n  i=$((i+1))\ndone\n',
    "env.sh": '[ "$KYTY_FAKE_VALUE" = "a b=c" ] || exit 9\ncase "$(pwd)" in */tests) ;; *) exit 8 ;; esac\n'
              'echo "env ok"\n',
    "willfail.sh": "exit 1\n",
    # What the harness prints after a TDR, before it aborts.
    "lost.sh": "echo 'ShaderRecompilerComputeTests: Gta6Alu failed at dispatch: vkWaitForFences returned "
               "ErrorDeviceLost' >&2\nexit 134\n",
    "args.sh": 'for a in "$@"; do echo "arg:[$a]"; done\n',
}

FAKE_EMULATOR = r"""#!/bin/sh
# Stand-in kyty_emulator: --dump-imports writes the report, anything else "boots".
printf '%s\n' "$*" >> argv.txt
imports=""
session=""
store=""
prev=""
for a in "$@"; do
  case "$prev" in
    --dump-imports) imports="$a" ;;
    --session-log) session="$a" ;;
  esac
  case "$a" in
    --shader-cache) store=1 ;;
    --no-shader-cache) store="" ;;
  esac
  prev="$a"
done
if [ -n "$imports" ]; then
  printf 'eboot.bin\n  libSceFake: 3 imported, 1 unresolved\n' > "$imports"
  echo "Import report: 1 programs written to $imports"
  exit 0
fi
[ -n "$session" ] && echo "--- Session ---" > "$session"
echo "printf log line" > _kyty.txt
# As a Release build: the driver's pipeline cache always, the shader store with --shader-cache.
mkdir -p _PipelineCache
printf 'driver pipeline cache' > _PipelineCache/FAKE00000.bin
[ -n "$store" ] && printf 'translated shaders' >> _PipelineCache/FAKE00000.shaders
# Links to a folder in the results folder, which the zip must not go into.
if [ -n "$KYTY_FAKE_LINK" ]; then
  mkdir -p ../extra/inner
  echo "a note" > ../extra/inner/note.txt
  ln -s "$KYTY_FAKE_LINK" ../linked
  ln -s "$KYTY_FAKE_LINK" ../extra/inner/linked
fi
echo "booting"
if [ "$KYTY_FAKE_EMULATOR" = crash ]; then
  echo "--- Fatal Error ---" >&2
  exit 5
fi
sleep 600 &
[ -n "$KYTY_FAKE_PIDS" ] && echo "$$ $!" > "$KYTY_FAKE_PIDS"
wait
"""

FAKE_VULKANINFO = """#!/bin/sh
echo 'Devices:'
echo '	deviceName         = Fake GPU'
echo '	deviceUUID         = 11111111-2222-3333-4444-555555555555'
echo '	driverUUID         = 66666666-7777-8888-9999-000000000000'
"""


def alive(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().split(") ", 1)[1][0] != "Z"
    except (FileNotFoundError, IndexError):
        return False


class PwshRequired(unittest.TestCase):
    def test_pwsh_is_present_when_required(self):
        if not os.environ.get("KYTY_REQUIRE_PWSH"):
            self.skipTest("KYTY_REQUIRE_PWSH is not set")
        self.assertTrue(PWSH, "KYTY_REQUIRE_PWSH is set, but there is no pwsh on PATH")


@unittest.skipUnless(PWSH, "no pwsh")
class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-preflight-test-")
        # The profile folder, which preflight writes as %USERPROFILE%; the results go inside it.
        self.home = os.path.join(self.tmp, "home")
        self.out = os.path.join(self.home, "out")
        self.scratch = os.path.join(self.tmp, "scratch")
        self.install = os.path.join(self.tmp, "install")
        self.preflight = os.path.join(self.install, "preflight")
        self.package = os.path.join(self.preflight, "gpu-tests")
        self.game = os.path.join(self.tmp, "game")
        self.bin = os.path.join(self.tmp, "bin")
        # Where Windows has it; preflight keeps the shader cache of game boots in it.
        self.localappdata = os.path.join(self.home, "AppData", "Local")
        self.cache = os.path.join(self.localappdata, "KytyPS5", "preflight-cache", "_PipelineCache")
        for folder in (self.home, self.scratch, os.path.join(self.package, "bin"), self.game, self.bin):
            os.makedirs(folder)
        shutil.copy(os.path.join(TOOL, "preflight.ps1"), self.preflight)
        shutil.copy(os.path.join(REPO, "tools", "triage", "collect_logs.ps1"), self.preflight)
        for name, body in SCRIPTS.items():
            self.executable(os.path.join(self.package, "bin", name), "#!/bin/sh\n" + body)
        self.executable(os.path.join(self.install, "kyty_emulator"), FAKE_EMULATOR)
        self.executable(os.path.join(self.bin, "vulkaninfo"), FAKE_VULKANINFO)
        with open(os.path.join(self.game, "eboot.bin"), "wb") as f:
            f.write(b"\x7fELF")
        self.pids = os.path.join(self.tmp, "hang.pids")
        tests = [
            {"name": "pass", "executable": "bin/pass.sh", "args": ["--device-creation-only"], "timeout": 30, "quick": True},
            {"name": "fail", "executable": "bin/fail.sh", "args": [], "timeout": 30, "quick": True},
            {"name": "hang", "executable": "bin/hang.sh", "args": [], "timeout": 2, "quick": False,
             "env": {"KYTY_FAKE_PIDS": self.pids}},
            {"name": "flood", "executable": "bin/flood.sh", "args": [], "timeout": 60, "quick": False},
            {"name": "env", "executable": "bin/env.sh", "args": [], "timeout": 30, "quick": False,
             "env": {"KYTY_FAKE_VALUE": "a b=c"}},
            {"name": "willfail", "executable": "bin/willfail.sh", "args": [], "timeout": 30, "quick": False,
             "will_fail": True},
            {"name": "args", "executable": "bin/args.sh", "args": ["two words", 'quo"te', "back\\slash\\", ""],
             "timeout": 30, "quick": False},
        ]
        self.manifest = {"format": 1, "source": {"commit": "0123456789abcdef", "run_id": "42"}, "tests": tests,
                         "executables": {t["executable"]: {} for t in tests}}
        self.write_manifest()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    @staticmethod
    def executable(path, text):
        with open(path, "w", newline="\n") as f:
            f.write(text)
        os.chmod(path, 0o755)

    def write_manifest(self):
        with open(os.path.join(self.package, "tests.json"), "w", encoding="utf-8") as f:
            json.dump(self.manifest, f)

    def environment(self, **env):
        """preflight's environment; env adds variables, and a None value removes one."""
        environment = dict(os.environ, HOME=self.home, TMPDIR=self.scratch, COMPUTERNAME="FAKE-PC",
                           LOCALAPPDATA=self.localappdata, PATH=self.bin + os.pathsep + os.environ.get("PATH", ""))
        environment.pop("USERPROFILE", None)
        for key, value in env.items():
            if value is None:
                environment.pop(key, None)
            else:
                environment[key] = value
        return environment

    def run_preflight(self, *args, **env):
        started = time.monotonic()
        r = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-File", os.path.join(self.preflight, "preflight.ps1"),
                            "-OutDir", self.out, *args], capture_output=True, text=True, env=self.environment(**env),
                           timeout=240)
        r.seconds = time.monotonic() - started
        return r

    def results(self):
        """(run folder, summary.json, zip names) of the only run in the output folder."""
        runs = glob.glob(os.path.join(self.out, "kyty-preflight-FAKE-PC-*"))
        folders = [p for p in runs if os.path.isdir(p)]
        self.assertEqual(len(folders), 1, runs)
        folder = folders[0]
        self.assertTrue(os.path.isfile(folder + ".zip"), "the zip is next to the folder")
        with open(os.path.join(folder, "summary.json"), encoding="utf-8") as f:
            summary = json.load(f)
        with zipfile.ZipFile(folder + ".zip") as z:
            names = z.namelist()
        return folder, summary, names

    def read(self, *parts):
        with open(os.path.join(*parts), encoding="utf-8") as f:
            return f.read()

    def test_all_tests(self):
        r = self.run_preflight()
        output = r.stdout + r.stderr
        self.assertEqual(r.returncode, 1, output)
        self.assertLess(r.seconds, 120)
        folder, summary, names = self.results()
        base = os.path.basename(folder)
        steps = {s["name"]: s for s in summary["steps"]}
        self.assertEqual([s["name"] for s in summary["steps"]], [t["name"] for t in self.manifest["tests"]])
        self.assertEqual({name: s["status"] for name, s in steps.items()},
                         {"pass": "PASS", "fail": "FAIL", "hang": "TIMEOUT", "flood": "PASS", "env": "PASS",
                          "willfail": "PASS", "args": "PASS"})
        self.assertEqual(summary["result"], "FAIL")
        self.assertEqual(summary["exit_code"], 1)
        self.assertEqual(summary["mode"], "tests")
        self.assertIsNone(summary["cache"], "only a game boot has a shader cache")
        self.assertEqual(summary["counts"], {"PASS": 5, "FAIL": 1, "TIMEOUT": 1})
        self.assertEqual(summary["package"], {"commit": "0123456789abcdef", "run_id": "42"})
        self.assertEqual((steps["fail"]["exit_code"], steps["fail"]["exit_code_hex"]), (3, "0x00000003"))
        self.assertEqual(steps["willfail"]["exit_code"], 1)
        self.assertTrue(steps["pass"]["quick"])
        self.assertGreaterEqual(steps["hang"]["seconds"], 2)
        self.assertLess(steps["hang"]["seconds"], 20)

        # The hung test's process tree is gone.
        pids = [int(p) for p in self.read(self.pids).split()]
        self.assertEqual(len(pids), 2)
        deadline = time.monotonic() + 10
        while any(alive(p) for p in pids) and time.monotonic() < deadline:
            time.sleep(0.2)
        self.assertFalse([p for p in pids if alive(p)], "the hung test and its child were killed")

        self.assertIn("[1/7] pass ... PASS (", output)
        self.assertIn("[2/7] fail ... FAIL (", output)
        self.assertIn("FakeTests: check X failed", output)
        self.assertIn("[3/7] hang ... TIMEOUT (", output)
        self.assertIn("Send this file to Claude", output)

        fail = self.read(folder, "tests", "fail.log")
        self.assertIn("about to fail", fail)
        self.assertIn("--- stderr ---", fail)
        self.assertIn("FakeTests: check X failed", fail)
        self.assertIn("--- fail: FAIL, exit code 0x00000003 (3)", fail)
        flood = self.read(folder, "tests", "flood.log")
        self.assertIn("out line 2999,", flood)
        self.assertIn("err line 2999,", flood)
        self.assertLess(flood.index("out line 2999,"), flood.index("--- stderr ---"))
        self.assertIn("env ok", self.read(folder, "tests", "env.log"))
        arguments = self.read(folder, "tests", "args.log")
        for line in ("arg:[two words]", 'arg:[quo"te]', "arg:[back\\slash\\]", "arg:[]"):
            self.assertIn(line, arguments)
        self.assertIn("pass: --device-creation-only", self.read(folder, "tests", "pass.log"))
        self.assertFalse(glob.glob(os.path.join(folder, "tests", "*.stderr.log")), "stderr went into the logs")

        # The profile path becomes %USERPROFILE%; vulkaninfo loses its UUIDs.
        for name in ("summary.txt", "summary.json", "console.txt", "system.txt"):
            text = self.read(folder, name)
            self.assertNotIn(self.home, text, name)
        self.assertIn("%USERPROFILE%", self.read(folder, "console.txt"))
        system = self.read(folder, "system.txt")
        self.assertIn("deviceName         = Fake GPU", system)
        self.assertIn("deviceUUID         = (removed by preflight)", system)
        self.assertNotIn("11111111", system)
        self.assertNotIn("66666666", system)
        summary_text = self.read(folder, "summary.txt")
        self.assertIn("result: FAIL (exit code 1)", summary_text)
        self.assertIn("| FakeTests: check X failed", summary_text)

        self.assertTrue(all(n.startswith(base + "/") and "\\" not in n for n in names), names)
        for name in ("summary.json", "summary.txt", "system.txt", "console.txt", "tests/fail.log", "tests/hang.log"):
            self.assertIn(f"{base}/{name}", names)
        self.assertEqual(os.listdir(self.scratch), [], "nothing left in the temp folder")

    def test_quick_and_only(self):
        r = self.run_preflight("-Quick")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        _, summary, _ = self.results()
        self.assertEqual([s["name"] for s in summary["steps"]], ["pass", "fail"])
        shutil.rmtree(self.out)
        r = self.run_preflight("-Only", "args,pass")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _, summary, _ = self.results()
        self.assertEqual([s["name"] for s in summary["steps"]], ["args", "pass"])
        self.assertEqual(summary["result"], "PASS")
        self.assertIn("[2/2] pass ... PASS (", r.stdout)

    def test_run_from_a_prompt(self):
        """Not the -File of its process: the arguments come from $args."""
        environment = dict(os.environ, HOME=self.home, TMPDIR=self.scratch, COMPUTERNAME="FAKE-PC",
                           PATH=self.bin + os.pathsep + os.environ.get("PATH", ""))
        script = os.path.join(self.preflight, "preflight.ps1").replace("'", "''")
        out = self.out.replace("'", "''")
        r = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-Command",
                            f"& '{script}' -OutDir '{out}' -Only 'args,pass'; exit $LASTEXITCODE"],
                           capture_output=True, text=True, env=environment, timeout=240)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _, summary, _ = self.results()
        self.assertEqual([s["name"] for s in summary["steps"]], ["args", "pass"])

    def test_wait_after_a_lost_device(self):
        """After a test that lost the GPU device the next one waits 60 s; a failure without it does not."""
        self.manifest["tests"].append({"name": "lost", "executable": "bin/lost.sh", "args": [], "timeout": 30,
                                       "quick": False})
        self.write_manifest()
        r = self.run_preflight("-Only", "fail,lost,pass")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertGreaterEqual(r.seconds, 60)
        self.assertEqual(r.stdout.count("The next test starts in 60 s"), 1, r.stdout)
        _, summary, _ = self.results()
        steps = {s["name"]: s for s in summary["steps"]}
        self.assertEqual([(n, steps[n]["status"]) for n in ("fail", "lost", "pass")],
                         [("fail", "FAIL"), ("lost", "FAIL"), ("pass", "PASS")])
        self.assertTrue(steps["lost"]["device_lost"])
        self.assertNotIn("device_lost", steps["fail"])

    def test_bad_arguments(self):
        cases = [
            (["-Bogus"], "unknown option: -Bogus"),
            (["--title-profile", "gta6"], "Options for the emulator go in -ExtraArgs"),
            (["-Only", "nosuch"], "no test named nosuch in tests.json. The tests: pass, fail, hang"),
            (["-Quick", "-Only", "pass"], "use -Quick or -Only, not both"),
            (["-Game", self.game, "-BootSeconds", "soon"], "-BootSeconds takes a whole number"),
            (["-Game", os.path.join(self.tmp, "nothing-here")], "does not exist"),
            (["-Game", self.game, "-ExtraArgs", "--nid-names 'unclosed"], "a quote is not closed"),
            (["-Game"], "-Game needs a value"),
        ]
        for args, message in cases:
            r = self.run_preflight(*args)
            self.assertEqual(r.returncode, 2, (args, r.stdout + r.stderr))
            self.assertIn(message, r.stdout, args)
            self.assertFalse(os.path.exists(self.out) and os.listdir(self.out), (args, "no results for bad arguments"))
        r = self.run_preflight("-Help")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("Usage: preflight.cmd", r.stdout)

    def test_missing_files(self):
        os.remove(os.path.join(self.package, "tests.json"))
        r = self.run_preflight()
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("Extract the whole zip first", r.stdout)
        r = self.run_preflight("-Game", self.game, "-Emulator", os.path.join(self.tmp, "no-emulator"))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("the emulator is not at", r.stdout)

    def test_game(self):
        # A value that starts with '-' and holds a colon: -File would cut it in two at the colon.
        extra = r"--title-profile gta6 --nid-names C:\PS5\names.txt"
        # What earlier boots left in the shader cache.
        os.makedirs(self.cache)
        with open(os.path.join(self.cache, "marker.txt"), "w", encoding="utf-8") as f:
            f.write("from an earlier boot\n")
        r = self.run_preflight("-Game", self.game, "-BootSeconds", "3", "-ExtraArgs", extra)
        output = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, output)
        folder, summary, names = self.results()
        base = os.path.basename(folder)
        self.assertEqual(summary["mode"], "game")
        # A boot that did not crash has no verdict: RAN, not PASS, and no "every step passed".
        self.assertEqual([(s["name"], s["status"]) for s in summary["steps"]],
                         [("imports", "PASS"), ("boot", "RAN"), ("logs", "PASS")], output)
        self.assertEqual((summary["result"], summary["exit_code"], summary["counts"]), ("RAN", 0, {"PASS": 2, "RAN": 1}))
        self.assertIn("2 of 3 steps passed. The boot ran without crashing", output)
        self.assertIn("result: RAN (exit code 0)", self.read(folder, "summary.txt"))
        boot = summary["steps"][1]
        self.assertIn("still running after 3 s", boot["detail"])
        self.assertLess(boot["seconds"], 20)
        calls = self.read(folder, "emulator", "argv.txt").splitlines()
        # --shader-cache for the boot only, and before -ExtraArgs, where --no-shader-cache wins.
        self.assertEqual(calls, [
            f"--game {self.game} --dump-imports {folder}/imports.txt {extra}",
            f"--game {self.game} --preset rtx3050 --session-log {folder}/session.log --shader-cache {extra}",
        ])
        self.assertEqual(summary["options"]["extra_args"], extra.split(" "))
        self.assertFalse(summary["options"]["cold_cache"])

        # pwsh on Linux makes no junction, so the boot falls back to a cache folder of its own, says
        # so in one line, and the run goes on.
        cache = summary["cache"]
        self.assertEqual((cache["mode"], cache["shader_cache"]), ("fallback", True), cache)
        self.assertIn("made no junction", cache["reason"])
        self.assertEqual(cache["path"], f"%USERPROFILE%/out/{base}/emulator/_PipelineCache")
        self.assertEqual(cache["before"], {"files": 0, "bytes": 0})
        self.assertEqual(cache["after"], {"files": 2, "bytes": len("driver pipeline cache") + len("translated shaders")})
        self.assertEqual(output.count("No shader cache from earlier boots (New-Item made no junction"), 1, output)
        self.assertIn("This boot compiles every shader.", output)
        self.assertIn("shader cache: cold, fallback (New-Item made no junction: junctions need Windows and an NTFS drive): "
                      "no files before the boot, 2 files (39 bytes) after", self.read(folder, "summary.txt"))
        self.assertTrue(os.path.isdir(os.path.join(folder, "emulator", "_PipelineCache")))
        self.assertFalse(os.path.islink(os.path.join(folder, "emulator", "_PipelineCache")))
        # Neither cache is in the zip, and the one of earlier boots is as it was.
        self.assertFalse([n for n in names if "_PipelineCache" in n or "FAKE00000" in n or "marker" in n], names)
        self.assertEqual(os.listdir(self.cache), ["marker.txt"])
        self.assertEqual(self.read(self.cache, "marker.txt"), "from an earlier boot\n")
        self.assertIn("libSceFake: 3 imported, 1 unresolved", self.read(folder, "imports.txt"))
        self.assertIn("booting", self.read(folder, "emulator-boot.log"))
        for name in ("imports.txt", "session.log", "emulator-imports.log", "emulator-boot.log", "summary.json"):
            self.assertIn(f"{base}/{name}", names)
        self.assertFalse([n for n in names if n.startswith(f"{base}/emulator/")], "the emulator's folder stays out")
        logs = [n for n in names if os.path.basename(n).startswith("kyty-logs-")]
        self.assertEqual(len(logs), 1, names)
        with zipfile.ZipFile(os.path.join(folder, os.path.basename(logs[0]))) as z:
            inner = {os.path.basename(n): z.read(n).decode("utf-8-sig") for n in z.namelist()}
        self.assertEqual(sorted(inner), ["_kyty.txt", "collect_info.txt", "vulkaninfo.txt"])
        self.assertIn("printf log line", inner["_kyty.txt"])
        self.assertIn("deviceUUID         = (removed by preflight)", inner["vulkaninfo.txt"])
        self.assertNotIn("11111111", inner["vulkaninfo.txt"])
        self.assertEqual(os.listdir(self.scratch), [], "collect_logs.ps1 removed its staging folder")

    def test_game_crash_and_no_eboot(self):
        os.remove(os.path.join(self.game, "eboot.bin"))
        r = self.run_preflight("-Game", self.game, "-BootSeconds", "20", KYTY_FAKE_EMULATOR="crash")
        output = r.stdout + r.stderr
        self.assertEqual(r.returncode, 1, output)
        self.assertIn("has no eboot.bin", output)
        _, summary, _ = self.results()
        steps = {s["name"]: s for s in summary["steps"]}
        self.assertEqual(steps["boot"]["status"], "FAIL")
        self.assertEqual(steps["boot"]["exit_code"], 5)
        self.assertLess(steps["boot"]["seconds"], 15, "a crash ends the boot early")
        self.assertEqual(steps["imports"]["status"], "PASS")
        self.assertTrue(any("has no eboot.bin" in p for p in summary["problems"]))

    @unittest.skipUnless(sys.platform.startswith("linux"), "needs a Linux pty")
    def test_ctrl_c(self):
        """Ctrl+C in the console stops the running test, and the results so far are zipped."""
        import fcntl
        import pty
        import termios
        self.manifest["tests"][2]["timeout"] = 120
        self.write_manifest()
        master, slave = pty.openpty()
        environment = dict(os.environ, HOME=self.home, TMPDIR=self.scratch, COMPUTERNAME="FAKE-PC",
                           PATH=self.bin + os.pathsep + os.environ.get("PATH", ""))
        process = subprocess.Popen([PWSH, "-NoProfile", "-File", os.path.join(self.preflight, "preflight.ps1"),
                                    "-OutDir", self.out, "-Only", "pass,hang,fail"],
                                   stdin=slave, stdout=slave, stderr=slave, env=environment, start_new_session=True,
                                   preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
        os.close(slave)
        output, sent, started = b"", False, time.monotonic()
        try:
            while time.monotonic() - started < 90 and process.poll() is None:
                if select.select([master], [], [], 0.2)[0]:
                    try:
                        output += os.read(master, 4096)
                    except OSError:
                        break
                if not sent and b"hang ... " in output and os.path.exists(self.pids):
                    os.write(master, b"\x03")
                    sent = True
            rc = process.wait(timeout=60)
        finally:
            if process.poll() is None:
                process.kill()
            os.close(master)
        text = output.decode(errors="replace")
        self.assertTrue(sent, text)
        self.assertEqual(rc, 1, text)
        self.assertLess(time.monotonic() - started, 60, "Ctrl+C did not wait for the 120 s time limit")
        self.assertIn("Stopped early; zipping what ran so far.", text)
        folder, summary, names = self.results()
        self.assertEqual(summary["result"], "INTERRUPTED")
        self.assertEqual([(s["name"], s["status"]) for s in summary["steps"]],
                         [("pass", "PASS"), ("hang", "INTERRUPTED"), ("fail", "NOT RUN")])
        self.assertIn("--- stopped early by preflight", self.read(folder, "tests", "hang.log"))
        self.assertIn(f"{os.path.basename(folder)}/tests/hang.log", names)
        pids = [int(p) for p in self.read(self.pids).split()]
        deadline = time.monotonic() + 10
        while any(alive(p) for p in pids) and time.monotonic() < deadline:
            time.sleep(0.2)
        self.assertFalse([p for p in pids if alive(p)], "the running test's processes were killed")

    def test_killed_midway(self):
        """Ended without its finally block (the window closed, the PC restarted): the results folder
        still has the summary of the steps that finished."""
        self.manifest["tests"][2]["timeout"] = 120
        self.write_manifest()
        environment = dict(os.environ, HOME=self.home, TMPDIR=self.scratch, COMPUTERNAME="FAKE-PC",
                           PATH=self.bin + os.pathsep + os.environ.get("PATH", ""))
        process = subprocess.Popen([PWSH, "-NoProfile", "-NonInteractive", "-File",
                                    os.path.join(self.preflight, "preflight.ps1"), "-OutDir", self.out,
                                    "-Only", "pass,fail,hang"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=environment)
        pids = []
        try:
            deadline = time.monotonic() + 90
            while len(pids) < 2 and time.monotonic() < deadline and process.poll() is None:
                time.sleep(0.2)
                if os.path.exists(self.pids):
                    pids = [int(p) for p in self.read(self.pids).split()]
            self.assertEqual(len(pids), 2, "the hang test did not start")
        finally:
            process.kill()
            process.wait(timeout=30)
            # Nothing ends the hung test's processes on Linux once pwsh is gone.
            for pid in pids:
                try:
                    os.kill(pid, 9)
                except ProcessLookupError:
                    pass
        folders = [p for p in glob.glob(os.path.join(self.out, "kyty-preflight-FAKE-PC-*")) if os.path.isdir(p)]
        self.assertEqual(len(folders), 1)
        self.assertFalse(os.path.exists(folders[0] + ".zip"))
        with open(os.path.join(folders[0], "summary.json"), encoding="utf-8") as f:
            summary = json.load(f)
        self.assertEqual((summary["result"], summary["exit_code"]), ("RUNNING", None))
        self.assertEqual([(s["name"], s["status"]) for s in summary["steps"]],
                         [("pass", "PASS"), ("fail", "FAIL"), ("hang", "NOT RUN")])
        self.assertIn("result: RUNNING (preflight had not finished", self.read(folders[0], "summary.txt"))
        self.assertIn("[2/3] fail ... FAIL (", self.read(folders[0], "console.txt"))

    def test_runs_what_package_gpu_tests_writes(self):
        """The packager's tests.json, from a fake build whose test executables are shell scripts."""
        build = os.path.join(self.tmp, "build")
        os.makedirs(build)
        self.executable(os.path.join(build, "shader_recompiler_compute_tests"),
                        '#!/bin/sh\n[ "$1" = --vram-limit-only ] && { echo "VramHeapLimit failed" >&2; exit 2; }\n'
                        'echo "ran $*"\n')
        self.executable(os.path.join(build, "kernel_file_system_tests"), "#!/bin/sh\necho ok\n")
        self.executable(os.path.join(build, "common_tests"), "#!/bin/sh\nexit 0\n")

        def test(name, exe, *args):
            return {"name": name, "command": [os.path.join(build, exe), *args],
                    "properties": [{"name": "WORKING_DIRECTORY", "value": build}]}
        info = {"tests": [test("common", "common_tests"), test("kernel_file_system", "kernel_file_system_tests"),
                          test("shader_recompiler_compute", "shader_recompiler_compute_tests"),
                          test("device_creation", "shader_recompiler_compute_tests", "--device-creation-only"),
                          test("shader_recompiler_gta6_alu", "shader_recompiler_compute_tests", "--gta6-alu-only"),
                          test("vram_heap_limit", "shader_recompiler_compute_tests", "--vram-limit-only"),
                          test("shader_recompiler_shader_mem", "shader_recompiler_compute_tests", "--shader-mem-only")]}
        ctest_json = os.path.join(self.tmp, "ctest.json")
        with open(ctest_json, "w", encoding="utf-8") as f:
            json.dump(info, f)
        shutil.rmtree(self.package)
        packager = os.path.join(REPO, ".github", "scripts", "package_gpu_tests.py")
        r = subprocess.run([sys.executable, packager, build, self.package, "--ctest-json", ctest_json],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = self.run_preflight("-Quick")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        _, summary, _ = self.results()
        self.assertEqual([(s["name"], s["status"], s["timeout_seconds"]) for s in summary["steps"]],
                         [("device_creation", "PASS", 300), ("shader_recompiler_gta6_alu", "PASS", 300),
                          ("vram_heap_limit", "FAIL", 300), ("shader_recompiler_shader_mem", "PASS", 300)])
        self.assertIn("VramHeapLimit failed", r.stdout)
        self.assertIn("[1/4] device_creation ... PASS (", r.stdout)

    def test_game_with_tests(self):
        r = self.run_preflight("-Game", self.game, "-BootSeconds", "2", "-WithTests", "-Only", "pass")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _, summary, _ = self.results()
        self.assertEqual([s["name"] for s in summary["steps"]], ["imports", "pass", "boot", "logs"])

    def test_cold_cache(self):
        """-ColdCache boots as preflight did before it kept a cache: no --shader-cache, no link, and
        the shader cache of earlier boots left alone. Without LOCALAPPDATA there is no cache either."""
        r = self.run_preflight("-Game", self.game, "-BootSeconds", "2", "-ColdCache")
        output = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, output)
        folder, summary, _ = self.results()
        calls = self.read(folder, "emulator", "argv.txt").splitlines()
        self.assertEqual(calls[1], f"--game {self.game} --preset rtx3050 --session-log {folder}/session.log")
        self.assertTrue(summary["options"]["cold_cache"])
        cache = summary["cache"]
        self.assertEqual((cache["mode"], cache["reason"], cache["shader_cache"]), ("cold", "-ColdCache", False))
        self.assertEqual(cache["before"], {"files": 0, "bytes": 0})
        self.assertEqual(cache["after"], {"files": 1, "bytes": len("driver pipeline cache")})
        self.assertIn("Shader cache: none (-ColdCache). This boot compiles every shader.", output)
        self.assertNotIn("No shader cache from earlier boots", output)
        self.assertIn("shader cache: cold by request (-ColdCache): no files before the boot, 1 file (21 bytes) after",
                      self.read(folder, "summary.txt"))
        self.assertFalse(os.path.exists(os.path.join(self.localappdata, "KytyPS5")), "-ColdCache makes no cache folder")
        shutil.rmtree(self.out)

        r = self.run_preflight("-Game", self.game, "-BootSeconds", "2", LOCALAPPDATA=None)
        output = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, output)
        folder, summary, _ = self.results()
        self.assertIn("--shader-cache", self.read(folder, "emulator", "argv.txt").splitlines()[1])
        self.assertEqual((summary["cache"]["mode"], summary["cache"]["reason"]), ("fallback", "LOCALAPPDATA is not set"))
        self.assertIn("No shader cache from earlier boots (LOCALAPPDATA is not set). This boot compiles every shader.",
                      output)

    def test_zip_leaves_out_links(self):
        """The zip goes into no junction or symbolic link, at the top of the results folder or below
        (Windows PowerShell 5.1's Get-ChildItem -Recurse follows both). The stand-in emulator links
        two folders of the results folder to the shader cache."""
        os.makedirs(self.cache)
        with open(os.path.join(self.cache, "PPSA00000.shaders"), "wb") as f:
            f.write(b"\0" * 4096)
        r = self.run_preflight("-Game", self.game, "-BootSeconds", "2", KYTY_FAKE_LINK=self.cache)
        output = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, output)
        folder, _, names = self.results()
        base = os.path.basename(folder)
        self.assertTrue(os.path.islink(os.path.join(folder, "linked")))
        self.assertTrue(os.path.islink(os.path.join(folder, "extra", "inner", "linked")))
        self.assertIn(f"{base}/extra/inner/note.txt", names)
        self.assertFalse([n for n in names if "linked" in n or "PPSA00000" in n], names)
        self.assertIn("left out of the zip, a junction or symbolic link: linked", output)
        self.assertIn("left out of the zip, a junction or symbolic link: extra/inner/linked", output)
        self.assertEqual(os.listdir(self.cache), ["PPSA00000.shaders"])

    def test_killed_during_boot(self):
        """Ended during the boot without its finally block (the PC restarted): the summary already
        has the shader cache the boot started with, and console.txt shows that the boot started."""
        pids_file = os.path.join(self.tmp, "boot.pids")
        process = subprocess.Popen([PWSH, "-NoProfile", "-NonInteractive", "-File",
                                    os.path.join(self.preflight, "preflight.ps1"), "-OutDir", self.out,
                                    "-Game", self.game, "-BootSeconds", "120"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   env=self.environment(KYTY_FAKE_PIDS=pids_file))
        pids = []
        try:
            deadline = time.monotonic() + 90
            while len(pids) < 2 and time.monotonic() < deadline and process.poll() is None:
                time.sleep(0.2)
                if os.path.exists(pids_file):
                    pids = [int(p) for p in self.read(pids_file).split()]
            self.assertEqual(len(pids), 2, "the boot did not start")
        finally:
            process.kill()
            process.wait(timeout=30)
            # Nothing ends the stand-in emulator on Linux once pwsh is gone.
            for pid in pids:
                try:
                    os.kill(pid, 9)
                except ProcessLookupError:
                    pass
        folders = [p for p in glob.glob(os.path.join(self.out, "kyty-preflight-FAKE-PC-*")) if os.path.isdir(p)]
        self.assertEqual(len(folders), 1)
        self.assertFalse(os.path.exists(folders[0] + ".zip"))
        with open(os.path.join(folders[0], "summary.json"), encoding="utf-8") as f:
            summary = json.load(f)
        self.assertEqual([(s["name"], s["status"]) for s in summary["steps"]], [("imports", "PASS"), ("boot", "NOT RUN")])
        cache = summary["cache"]
        self.assertEqual((cache["mode"], cache["before"], cache["after"]), ("fallback", {"files": 0, "bytes": 0}, None))
        self.assertIn("shader cache: cold, fallback (", self.read(folders[0], "summary.txt"))
        self.assertTrue(self.read(folders[0], "console.txt").endswith("Boot ... "))


if __name__ == "__main__":
    unittest.main()
