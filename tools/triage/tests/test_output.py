"""Output modes: markdown, --json, --agent-prompt, --compare, --redact (through main())."""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage_test_util as u  # noqa: E402

kt = u.kt


def run_main(*args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = kt.main(list(args))
    return code, out.getvalue()


class OutputTests(unittest.TestCase):
    def test_markdown_sections(self):
        code, text = run_main(u.fixture("synthetic-gta6-boot-failure.txt"))
        self.assertEqual(code, 0)
        for heading in ("# KytyPS5 triage: PPSA99999, crash", "## Identity", "## Crash site", "## Suggested fix areas",
                        "## Missing imports", "## Unsupported shader instructions", "## Shader resource tracking",
                        "## GPU errors", "## Workarounds ([HACK])", "## How far it got", "## Sources"):
            self.assertIn(heading, text)
        self.assertIn("`eboot.bin+0x1c2d3e4`", text)
        self.assertIn("| VOP3P | 0x2a |", text)
        self.assertIn("NVIDIA GeForce RTX 3050 (NVIDIA, driver 581.42.0.0", text)

    def test_json_is_complete_and_parseable(self):
        code, text = run_main("--json", u.fixture("gta5-729-guest-write-rtx3060.txt"))
        rep = json.loads(text)
        self.assertEqual(rep["tool"], "kyty-triage")
        self.assertEqual(rep["schema"], kt.SCHEMA)
        for key in ("sources", "identity", "primary", "outcome", "exit_code", "fatal_blocks", "imports",
                    "shader_unsupported", "shader_resource_tracking", "gpu_errors", "workarounds", "progress",
                    "fix_areas"):
            self.assertIn(key, rep)
        self.assertNotIn("_game_roots", rep)
        self.assertEqual(rep["exit_code"]["value"], 321)
        self.assertEqual(rep["primary"]["host_exception"]["pc_offset"], "0x28abdd4")
        self.assertTrue(any("#729" in k for k in rep["fix_areas"][0]["known"]))

    def test_exit_code_option(self):
        _, text = run_main("--json", "--exit-code", "0xC0000409", u.fixture("synthetic-rule-lines.txt"))
        ec = json.loads(text)["exit_code"]
        self.assertEqual((ec["value"], ec["basis"]), (3221226505, "--exit-code"))
        self.assertIn("0xC0000409", ec["meaning"])

    def test_agent_prompt(self):
        _, text = run_main("--agent-prompt", u.fixture("synthetic-gta6-boot-failure.txt"))
        self.assertIn("KytyPS5 repository", text)
        self.assertIn("Unhandled host exception: type=1 code=3221225477 pc=0x0000000901c2d3e4", text)
        self.assertIn("Suggested fix areas, most likely first:", text)
        self.assertIn("src/libs/libNet.cpp", text)
        self.assertIn("q1Wd8kXcR2s[NpManager_v1][NpManager_v1.1][Func]", text)
        self.assertIn("VOP3P 0x2a", text)
        self.assertIn("synthetic-gta6-boot-failure.txt:19:", text)

    def test_compare_logs_and_saved_json(self):
        tmp = tempfile.mkdtemp(prefix="kyty-triage-test-")
        try:
            saved = os.path.join(tmp, "a.json")
            run_main("--json", "-o", saved, u.fixture("synthetic-gta6-boot-failure.txt"))
            _, text = run_main("--json", "--compare", saved, u.fixture("synthetic-contract-session.txt"))
        finally:
            shutil.rmtree(tmp)
        c = json.loads(text)
        self.assertEqual(c["mode"], "compare")
        d = c["diff"]
        self.assertIn("AAAAAAAAAAA[NpManager_v1][NpManager_v1.1][Func]", d["imports_called"]["new"])
        self.assertIn("q1Wd8kXcR2s[NpManager_v1][NpManager_v1.1][Func]", d["imports_called"]["removed"])
        self.assertIn("VOP3:0x344", d["shader_unsupported"]["new"])
        self.assertIn("MIMG:0xe6", d["shader_unsupported"]["removed"])
        self.assertTrue(c["primary_changed"])
        _, md = run_main("--compare", u.fixture("gta5-729-guest-write-rtx3060.txt"),
                         u.fixture("gta5-888-guest-write-rtx4060-win.txt"))
        self.assertIn("# KytyPS5 triage: compare", md)
        self.assertIn("Primary failure changed.", md)

    def test_redact(self):
        text = ("Loading: C:/Users/Some One/Downloads/[SuperPSX]-Game-PPSA00001/PPSA00001 [ 01.000.000 ]\\eboot.bin\n"
                "base_vaddr = 0x0000000900000000\nmapped_size = 0x0000000005165000\n"
                "Pipeline manifest: 1 modules in C:\\Users\\karim\\KytyPS5\\assets\\x.kpm\n"
                "Vulkan loader: /Users/someone/Downloads/libMoltenVK.dylib\n"
                "Can't open /home/alice/Games/PS5/PPSA00001/sce_sys/playgo-chunk.dat\n"
                "Soft-fail: sceFoo: mail me at someone@example.com\n" +
                "--- Build ---\nOfficial build KytyPS5-2026-11-19-abcdef1\n--- Error ---\nboom\n"
                " in D:\\a\\KytyPS5\\KytyPS5\\src\\x\\y.cpp:1\n")
        tmp = tempfile.mkdtemp(prefix="kyty-triage-test-")
        try:
            p = os.path.join(tmp, "l.txt")
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
            _, md = run_main("--redact", p)
            _, js = run_main("--redact", "--json", p)
        finally:
            shutil.rmtree(tmp)
        for out in (md, js):
            for secret in ("Some One", "karim", "someone", "alice", "SuperPSX", "example.com"):
                self.assertNotIn(secret, out)
        self.assertIn("D:\\a\\KytyPS5\\KytyPS5\\src\\x\\y.cpp", md)
        self.assertIn("src/x/y.cpp", js)
        self.assertEqual(kt.redact("/home/runner/work/KytyPS5/KytyPS5/src/a.cpp"), "/home/runner/work/KytyPS5/KytyPS5/src/a.cpp")

    def test_cli_runs_isolated(self):
        r = subprocess.run([sys.executable, "-I", os.path.join(u.TOOL_DIR, "kyty_triage.py"), "--json",
                            u.fixture("macos-1133-device-selection.txt")], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        rep = json.loads(r.stdout)
        self.assertEqual(rep["primary"]["category"], "DEVICE_SELECTION")
        rejects = rep["fatal_blocks"][0]["device_rejects"]
        self.assertEqual(rejects[0]["gpu"], "Apple M4")
        self.assertIn("shaderCullDistance is not supported", rejects[0]["reasons"])
        with open(u.fixture("gta5-548-buffer-dword0-paste.txt"), "rb") as f:
            pasted = f.read()
        r = subprocess.run([sys.executable, "-I", os.path.join(u.TOOL_DIR, "kyty_triage.py"), "-"],
                           input=pasted, capture_output=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"0x5e3e42fe237d3e16", r.stdout)


if __name__ == "__main__":
    unittest.main()
