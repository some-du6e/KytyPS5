"""rules.json lint and coverage: regexes compile, paths exist, every rule fires on some fixture."""

import glob
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage_test_util as u  # noqa: E402

kt = u.kt


def all_reports():
    return {n: u.report(n) for n in sorted(os.listdir(u.FIXTURES)) if n.endswith(".txt")}


class RuleLintTests(unittest.TestCase):
    def test_load_and_fields(self):
        rules = u.RULES
        self.assertGreater(len(rules.rules), 30)
        for r in rules.rules:
            with self.subTest(rule=r.id):
                self.assertRegex(r.id, r"^[a-z0-9-]+$")
                self.assertRegex(r.category, r"^[A-Z_]+$")
                self.assertIn(r.scope, ("fatal", "line", "any", "meta"))
                self.assertTrue(0 <= r.severity <= 100)
                self.assertTrue(r.next_step)
                if r.scope in ("line", "any"):
                    self.assertTrue(r.prefix or r.needle)
                    self.assertGreaterEqual(len(r.prefix or r.needle), 4)
                if r.scope == "fatal":
                    self.assertTrue(r.regex is not None or r.file_re is not None)

    def test_meta_rules_used_by_the_tool_exist(self):
        with open(os.path.join(u.TOOL_DIR, "kyty_triage.py"), "r", encoding="utf-8") as f:
            src = f.read()
        used = set(re.findall(r'rules\.rule\("([a-z0-9-]+)"\)', src))
        used |= set(re.findall(r'add\("([a-z0-9-]+)"', src))
        used = {x for x in used if not x.endswith("-")}
        used |= {"host-fault-guest", "host-fault-driver", "host-fault-emulator", "host-fault-host",
                 "host-fault-bad-jump", "no-fatal-no-fatal", "no-fatal-truncated", "shader-opcode-skip",
                 "vulkan-validation-error", "vulkan-validation-warning"}
        for rid in sorted(used):
            self.assertIsNotNone(u.RULES.rule(rid), rid)

    def test_fix_area_paths_exist(self):
        if not os.path.isdir(os.path.join(u.REPO, "src")):
            self.skipTest("not inside a KytyPS5 checkout")
        for r in u.RULES.rules:
            for p in r.fix_area:
                with self.subTest(rule=r.id, path=p):
                    full = os.path.join(u.REPO, p)
                    self.assertTrue(os.path.exists(full) or glob.glob(full), p)
        for p in ("src/graphics/shader/recompiler/frontend/decode/ScalarAluOps.cpp",
                  "src/graphics/shader/recompiler/frontend/decode/VectorAluOps.cpp",
                  "src/graphics/shader/recompiler/frontend/decode/ImageOps.cpp",
                  "src/graphics/shader/recompiler/frontend/decode/MemoryOps.cpp",
                  "src/graphics/shader/recompiler/frontend/decode/ExportOps.cpp"):
            self.assertTrue(os.path.exists(os.path.join(u.REPO, p)), p)

    def test_literal_prefix(self):
        self.assertEqual(kt.literal_prefix(r"^VRAM spill: (?P<k>\w+)"), "VRAM spill: ")
        self.assertEqual(kt.literal_prefix(r"^\[HACK\] x"), "[HACK] x")
        self.assertEqual(kt.literal_prefix(r"^abc?d"), "ab")
        self.assertEqual(kt.literal_prefix(r"^Device (?:fault|lost)"), "Device ")
        self.assertEqual(kt.literal_prefix(r"no anchor"), "")

    def test_known_signatures_compile(self):
        for k in u.RULES.known_signatures:
            self.assertIn(k["field"], ("hash", "symbol", "guest_pc", "vuid", "message"))
            self.assertTrue(k.get("refs") or k.get("note"))


class RuleCoverageTests(unittest.TestCase):
    """Every fatal rule classifies some fixture report and every line rule matches some fixture line."""

    def test_fatal_catalogue_expectations(self):
        rep = u.report("synthetic-fatal-catalogue.txt")
        with open(u.fixture("synthetic-fatal-catalogue.txt"), "r", encoding="utf-8") as f:
            lines = f.read().split("\n")
        expect = {}
        for i, ln in enumerate(lines, 1):
            if ln.startswith("expect: "):
                expect[i + 1] = ln[8:].strip()
        got = {}
        for b in rep["fatal_blocks"]:
            start = b["lineno"]
            # the report starts at "--- Build ---" right after the expect line, or at the line itself
            # for the context-first (fault context) and single-line (PageManager, terminate) cases
            got[start] = b["rule_id"]
        for line, rid in sorted(expect.items()):
            with self.subTest(rule=rid):
                hits = [r for s, r in got.items() if 0 <= s - line <= 8]
                self.assertIn(rid, hits)

    def test_every_rule_fires_somewhere(self):
        reps = all_reports()
        fired = set()
        for rep in reps.values():
            for b in rep["fatal_blocks"]:
                fired.add(b["rule_id"])
            for f in rep["findings"]:
                fired.add(f["rule_id"])
            for a in rep["fix_areas"]:
                fired.add(a["rule_id"])
            for v in rep["gpu_errors"]["vulkan_messages"]:
                fired.add("vulkan-validation-error" if v["severity"] == "E" else "vulkan-validation-warning")
        for r in u.RULES.rules:
            with self.subTest(rule=r.id):
                if r.id in ("nvidia-shader-cache-warmup",):
                    continue  # covered by dedicated tests below
                self.assertIn(r.id, fired)

    def test_rule_lines_expectations(self):
        rep = u.report("synthetic-rule-lines.txt")
        by_line = {}
        for f in rep["findings"]:
            for e in f["examples"]:
                by_line.setdefault(e["line"], set()).add(f["rule_id"])
        with open(u.fixture("synthetic-rule-lines.txt"), "r", encoding="utf-8") as f:
            lines = f.read().split("\n")
        n = 0
        for i, ln in enumerate(lines, 1):
            if ln.startswith("expect-line: "):
                n += 1
                rid = ln[13:].strip()
                with self.subTest(rule=rid):
                    self.assertIn(rid, by_line.get(i + 1, set()))
        line_rules = [r.id for r in u.RULES.line_rules]
        self.assertEqual(n, len(line_rules), "one expect-line per line rule")

    def test_nvidia_shader_cache_hint(self):
        text = ("--- Session 2026-11-19 10:00:00 ---\nOfficial build KytyPS5-2026-11-19-abcdef1\n"
                "GPU: NVIDIA GeForce RTX 3050 vendor=0x10de device=0x2507 driver=581.42.0.0 api=1.4.312 "
                "driverID=NvidiaProprietary vram=8192 MiB bar=256 MiB\n"
                "Shader cache: compiling shaders and pipelines from _ShaderCache/PPSA00001.bin (220 MB) with 12 threads\n")
        rep = kt.triage(["-"], rules=u.RULES, stdin=u_bytes(text))
        self.assertEqual(rep["fix_areas"][0]["rule_id"], "nvidia-shader-cache-warmup")
        self.assertEqual(rep["outcome"], "no-fatal")

    def test_load_summary_without_calls(self):
        text = "Unresolved imports: 3 functions, 1 objects; by library: NpAuth=2, Ssl=1\n"
        rep = kt.triage(["-"], rules=u.RULES, stdin=u_bytes(text))
        self.assertEqual(rep["imports"]["summary"]["by_library"], {"NpAuth": 2, "Ssl": 1})
        self.assertIn("unresolved-import-summary", [a["rule_id"] for a in rep["fix_areas"]])

    def test_validation_warning(self):
        text = "[Vulkan][W][4]: vkCmdPipelineBarrier2(): pipeline barrier with no memory access\n"
        rep = kt.triage(["-"], rules=u.RULES, stdin=u_bytes(text))
        self.assertIn("vulkan-validation-warning", [a["rule_id"] for a in rep["fix_areas"]])


def u_bytes(text):
    import io
    return io.BytesIO(text.encode("utf-8"))


if __name__ == "__main__":
    unittest.main()
