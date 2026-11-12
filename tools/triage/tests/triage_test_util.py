"""Shared helpers for the kyty-triage tests (imported by the test_*.py files)."""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.dirname(HERE)
FIXTURES = os.path.join(HERE, "fixtures")
EXPECTED = os.path.join(HERE, "expected")
REPO = os.path.normpath(os.path.join(TOOL_DIR, "..", ".."))
if TOOL_DIR not in sys.path:
    sys.path.insert(0, TOOL_DIR)

import kyty_triage as kt  # noqa: E402

RULES = kt.RuleSet()
_CACHE = {}


def fixture(name):
    return os.path.join(FIXTURES, name)


def report(name, **kw):
    """Triage of one fixture file (cached per name when no options are given)."""
    if kw:
        return kt.triage([fixture(name)], rules=RULES, **kw)
    if name not in _CACHE:
        _CACHE[name] = kt.triage([fixture(name)], rules=RULES)
    return _CACHE[name]


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def golden_summary(rep):
    """The key fields compared against tests/expected/<case>.json."""
    prim = rep["primary"]
    ident = rep["identity"]
    s = {
        "outcome": rep["outcome"],
        "primary_category": prim.get("category"),
        "primary_rule": prim.get("rule_id"),
        "crash_site": prim.get("crash_site"),
        "top_rule": rep["fix_areas"][0]["rule_id"] if rep["fix_areas"] else None,
        "top_fix_area": rep["fix_areas"][0]["paths"][0] if rep["fix_areas"] and rep["fix_areas"][0]["paths"] else None,
        "fatal_count": len(rep["fatal_blocks"]),
        "title_id": ident.get("title_id"),
        "build_label": ident.get("build_label"),
        "gpu": ident.get("gpu", {}).get("name"),
        "imports_called": sorted(c["symbol"] for c in rep["imports"]["called"]),
        "srt_hashes": sorted({e["hash"] for e in rep["shader_resource_tracking"]}),
        "shader_opcodes": sorted(e["key"] for e in rep["shader_unsupported"]),
        "hacks": sorted(h["hack"] for h in rep["workarounds"]["hacks"]),
        "vuids": sorted({v["vuid"] for v in rep["gpu_errors"]["vulkan_messages"] if v["vuid"]} |
                        {f["fields"].get("vuid") for f in rep["findings"] if f["rule_id"] == "vuid-mention"}),
        "truncated": any(x["truncated_last_line"] for x in rep["sources"]),
    }
    exc = prim.get("host_exception")
    s["guest_pc"] = "%s+%s" % (exc["pc_module"], exc["pc_offset"]) if exc and exc.get("pc_module") else None
    s["fault_class"] = exc["class"] if exc else None
    return s
