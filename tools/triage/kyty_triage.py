#!/usr/bin/env python3
"""kyty-triage: first look at a KytyPS5 log from a failed run.

Reads session logs (_Logs/*.log), printf logs (_kyty.txt), console captures and
pasted issue text, plain or .gz or inside a .zip (read in memory, never
extracted), or stdin. Prints identity, crash site, missing imports, shader
failures, GPU errors, [HACK] notices, how far the run got and a ranked list of
places in the repo to look at.

Standard library only; run it with `python3 -I tools/triage/kyty_triage.py`.
The rules (patterns, categories, fix areas, known issues) live in rules.json
next to this file. See README.md.
"""

import argparse
import collections
import gzip
import io
import json
import math
import os
import re
import sys
import zipfile

TOOL = "kyty-triage"
SCHEMA = 1
TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_RULES = os.path.join(TOOL_DIR, "rules.json")

# Sizes. Printf logs reach several GB (#199/#200), so everything streams.
QUICK_HEAD_BYTES = 16 << 20   # --quick: the first 16 MiB (identity, module map) ...
QUICK_TAIL_BYTES = 16 << 20   # ... and the last 16 MiB, read first (the crash is at the end)
LENIENT_MAX_BYTES = 8 << 20   # inputs below this are treated as possibly pasted text
MAX_MESSAGE_LINES = 64        # lines of one fatal message before giving up on its " in file:line"
MAX_KEYS_PER_RULE = 256       # distinct finding keys kept per rule
MAX_LINE_CHARS = 2000         # longer lines are cut before matching
EXAMPLES_PER_KEY = 3

# ---------------------------------------------------------------------------
# Small helpers

HEX_RE = re.compile(r"0x[0-9a-fA-F]+|\b[0-9a-fA-F]{8,}\b")
NUM_RE = re.compile(r"\b\d+\b")


def normalize(text):
    """Signature form of a message: hex and decimal numbers replaced by '#'."""
    text = HEX_RE.sub("#", text)
    text = NUM_RE.sub("#", text)
    return re.sub(r"\s+", " ", text).strip()


def basename(path):
    if not path:
        return path
    return re.split(r"[\\/]", path)[-1]


def src_relative(path):
    """'D:\\a\\KytyPS5\\KytyPS5\\src\\graphics\\x.cpp' -> 'src/graphics/x.cpp' when it can."""
    if not path:
        return None
    p = path.replace("\\", "/")
    i = p.rfind("/src/")
    if i >= 0:
        return p[i + 1:]
    if p.startswith("src/"):
        return p
    return None


def build_origin(path):
    """Which CI layout a source path in a fatal report comes from."""
    if not path:
        return None
    p = path.replace("\\", "/")
    m = re.match(r"^(?:[A-Za-z]:/a|/home/runner/work|/Users/runner/work)/([^/]+)/", p)
    if m:
        osname = "Windows" if p[1:2] == ":" else ("Linux" if p.startswith("/home/") else "macOS")
        return "GitHub Actions, repo %s (%s)" % (m.group(1), osname)
    if p.startswith("src/"):
        return "relative (pasted or local build)"
    return "local build"


# ---------------------------------------------------------------------------
# Redaction (used by --redact and by make_fixture.py)

_REDACT_PATTERNS = [
    # C:\Users\<name>\ and C:/Users/<name>/
    (re.compile(r"(?i)([A-Za-z]:[\\/]+Users[\\/]+)(?!<redacted>)[^\\/\r\n\"']+"), r"\1<redacted>"),
    # /home/<name>/ (CI runner kept)
    (re.compile(r"(/home/)(?!runner/|<redacted>)[^/\s\"']+"), r"\1<redacted>"),
    # /Users/<name>/ on macOS (CI runner and Shared kept)
    (re.compile(r"(?<![A-Za-z]:)(?<![\\/A-Za-z])(/Users/)(?!runner/|Shared/|<redacted>)[^/\s\"']+"),
     r"\1<redacted>"),
    # D:\Games\<name>, E:/Emulation/<name>, ... : the folder under a games directory
    (re.compile(r"(?i)([A-Za-z]:[\\/]+(?:[^\\/\r\n]+[\\/]+)*?(?:games?|emulation|emulators?|roms?|ps5)[\\/]+)"
                r"(?!<redacted>)[^\\/\r\n\"']+"), r"\1<redacted>"),
    # release-group folder names such as "[SuperPSX]-Grand.Theft.Auto.V-..." or "[DLPSGAME.COM] - ..."
    (re.compile(r"\[(?:SuperPSX|DLPSGAME[^\]]*|[A-Za-z0-9.]+\.(?:com|net|org))\][^\\/\r\n\"']*", re.I),
     "<redacted>"),
    # e-mail addresses
    (re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b"), "<redacted-email>"),
]

GAME_ROOT_RE = re.compile(
    r"(?:Loading: |program=|--- Relocate program: |from: )(?P<root>(?:[A-Za-z]:)?[\\/][^\r\n]*?)"
    r"[\\/](?:eboot\.bin|sce_module[\\/])")


def find_game_roots(text):
    roots = set()
    for m in GAME_ROOT_RE.finditer(text):
        root = m.group("root")
        if len(root) > 3:
            roots.add(root)
    return roots


def redact_obj(obj, roots=()):
    """redact() applied to every string in a report."""
    if isinstance(obj, str):
        return redact(obj, roots)
    if isinstance(obj, list):
        return [redact_obj(x, roots) for x in obj]
    if isinstance(obj, tuple):
        return tuple(redact_obj(x, roots) for x in obj)
    if isinstance(obj, dict):
        return obj.__class__((k, redact_obj(v, roots)) for k, v in obj.items())
    return obj


def redact(text, roots=()):
    """Removes user names, game install folders and e-mail addresses from log text."""
    for root in sorted(roots, key=len, reverse=True):
        if root:
            text = text.replace(root, "<game>")
    for pattern, repl in _REDACT_PATTERNS:
        text = pattern.sub(repl, text)
    return text


# ---------------------------------------------------------------------------
# Rules

class Rule:
    def __init__(self, data):
        self.data = data
        self.id = data["id"]
        self.category = data["category"]
        self.title = data.get("title", self.id)
        self.scope = data.get("scope", "line")
        self.severity = int(data.get("severity", 50))
        flags = re.I if data.get("ignore_case") else 0
        self.regex = re.compile(data["regex"], flags | re.M) if data.get("regex") else None
        self.file_re = re.compile(data["file"], re.I) if data.get("file") else None
        self.fix_area = list(data.get("fix_area", []))
        self.next_step = data.get("next_step", "")
        self.known = list(data.get("known", []))
        self.key_fields = list(data.get("key", []))
        self.vendor = data.get("vendor")
        self.group = data.get("group")
        self.prefix = None
        self.needle = None
        if self.scope in ("line", "any"):
            needle = data.get("contains")
            if needle:
                self.needle = needle.encode("utf-8")
            else:
                prefix = data.get("prefix") or literal_prefix(data.get("regex", ""))
                if not prefix:
                    raise ValueError("rule %s: line rules need a regex with a literal ^prefix, "
                                     "or a 'prefix'/'contains' field" % self.id)
                self.prefix = prefix.encode("utf-8")

    def fatal_match(self, message, file):
        if self.scope not in ("fatal", "any"):
            return None
        if self.file_re is not None and not (file and self.file_re.search(basename(file))):
            return None
        if self.regex is None:
            return {} if self.file_re is not None else None
        m = self.regex.search(message)
        if not m:
            return None
        return {k: v for k, v in m.groupdict().items() if v is not None}


_META = set(".^$*+?{}[]|()")


def literal_prefix(regex):
    """The literal text a '^...' regex must start with ('' if none)."""
    if not regex.startswith("^"):
        return ""
    out = []
    i = 1
    while i < len(regex):
        c = regex[i]
        if c == "\\":
            if i + 1 < len(regex) and not regex[i + 1].isalnum():
                out.append(regex[i + 1])
                i += 2
                continue
            break
        if c in _META:
            if c in "?*{" and out:
                out.pop()
            break
        out.append(c)
        i += 1
    return "".join(out)


class RuleSet:
    def __init__(self, path=DEFAULT_RULES):
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.version = data.get("version", 1)
        self.rules = [Rule(r) for r in data["rules"]]
        self.by_id = {r.id: r for r in self.rules}
        if len(self.by_id) != len(self.rules):
            raise ValueError("duplicate rule id in %s" % path)
        self.known_signatures = []
        for k in data.get("known_signatures", []):
            k = dict(k)
            k["_re"] = re.compile(k["match"], re.I)
            self.known_signatures.append(k)
        self.library_hints = data.get("library_hints", {})
        self.harmless_symbols = set(data.get("harmless_symbols", []))
        self.line_rules = [r for r in self.rules if r.scope in ("line", "any")]
        self.fatal_rules = [r for r in self.rules if r.scope in ("fatal", "any")]

    def rule(self, rule_id):
        return self.by_id.get(rule_id)

    def known_for(self, field, value):
        if value is None:
            return []
        out = []
        for k in self.known_signatures:
            if k["field"] == field and k["_re"].search(str(value)):
                out.append({key: v for key, v in k.items() if not key.startswith("_")})
        return out


# ---------------------------------------------------------------------------
# Inputs

class Source:
    """One log stream: a plain file, a .gz, one member of a .zip, or stdin."""

    def __init__(self, name, opener, size=None, path=None, compressed=False):
        self.name = name
        self.opener = opener
        self.size = size
        self.path = path            # set for plain seekable files only
        self.compressed = compressed

    def open(self):
        return self.opener()


def _sniff(path):
    with open(path, "rb") as f:
        return f.read(4)


def _looks_binary(head):
    if not head:
        return False
    if b"\x00" in head[:4096]:
        return True
    return False


def _zip_sources(path, display):
    out = []
    zf = zipfile.ZipFile(path)
    for info in zf.infolist():
        if info.is_dir():
            continue
        lower = info.filename.lower()
        if lower.endswith((".zip", ".spv", ".bin", ".png", ".jpg", ".mp4", ".dmp", ".exe", ".dll")):
            continue
        with zf.open(info) as probe:
            head = probe.read(4096)
        member = info.filename

        if head[:2] == b"\x1f\x8b":
            def opener(m=member):
                return gzip.GzipFile(fileobj=zf.open(m))
            out.append(Source("%s:%s" % (display, member), opener, None, None, True))
            continue
        if _looks_binary(head):
            continue

        def opener(m=member):
            return io.BufferedReader(zf.open(m), 1 << 20)
        out.append(Source("%s:%s" % (display, member), opener, info.file_size, None, True))
    return out


def _directory_sources(path):
    """A Kyty directory: newest _Logs/*.log plus _kyty.txt."""
    out = []
    logs = os.path.join(path, "_Logs")
    if os.path.isdir(logs):
        cands = [os.path.join(logs, n) for n in os.listdir(logs) if n.lower().endswith(".log")]
        if cands:
            out.append(max(cands, key=os.path.getmtime))
    for n in ("_kyty.txt",):
        p = os.path.join(path, n)
        if os.path.isfile(p):
            out.append(p)
    return out


def expand_inputs(paths, stdin=None):
    sources = []
    for p in paths:
        if p == "-":
            stream = stdin if stdin is not None else sys.stdin.buffer
            sources.append(Source("<stdin>", lambda s=stream: s, None, None, False))
            continue
        if os.path.isdir(p):
            found = _directory_sources(p)
            if not found:
                raise FileNotFoundError("no _Logs/*.log or _kyty.txt in %s" % p)
            sources.extend(expand_inputs(found))
            continue
        magic = _sniff(p)
        size = os.path.getsize(p)
        display = p
        if magic[:2] == b"\x1f\x8b":
            sources.append(Source(display, lambda q=p: gzip.open(q, "rb"), None, None, True))
        elif magic[:4] in (b"PK\x03\x04", b"PK\x05\x06"):
            sources.extend(_zip_sources(p, display))
        else:
            sources.append(Source(display, lambda q=p: open(q, "rb", buffering=1 << 20), size, p, False))
    return sources


# ---------------------------------------------------------------------------
# Regexes for the built-in extractors (formats: notes/log-contract.md and the
# survey catalogue A-H; the source file:line of each emitter is in README.md)

R_LABEL = re.compile(r"^(?:(?P<official>Official build (?P<tag>KytyPS5-\d{4}-\d\d-\d\d-[0-9a-f]{7,}))"
                     r"|(?P<fork>Fork build (?P<repo>\S+) (?P<fhash>[0-9a-f]{7,}))"
                     r"|(?P<source>Source build (?P<shash>[0-9a-f]{7,})))\s*$")
R_SESSION = re.compile(r"^--- (?P<kind>Session|Relaunched) (?P<ts>\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) ---$")
R_TITLE = re.compile(r"^--- (?P<kind>Error|Fatal Error) ---\s*$")
R_WHERE_ONLY = re.compile(r"^\s*in (?P<file>(?:[A-Za-z]:)?[^\s:][^:\r\n]*?\.(?:cpp|cc|c|h|hpp|inc|mm)):(?P<line>\d+)"
                          r"[\s\"'`.)]*$")
R_WHERE_TAIL = re.compile(r"^(?P<msg>.*\S) in (?P<file>(?:[A-Za-z]:)?[^\s:][^:\r\n]*?\.(?:cpp|cc|c|h|hpp|inc|mm))"
                          r":(?P<line>\d+)[\s\"'`.)]*$")
R_STACK_WORD = re.compile(r"^\[(\d+)\] ([0-9a-f]{16})\s*$")
R_HOST_EXC = re.compile(r"Unhandled host exception: type=(?P<type>\d+) code=(?P<code>\d+)"
                        r"(?: pc=0x(?P<pc>[0-9a-fA-F]+))? access=(?P<access>\d+) address=0x(?P<address>[0-9a-fA-F]+)")
R_SHADER_CTX = re.compile(r"^while compiling shader: stage=(?P<stage>\S+) hash=0x(?P<hash>[0-9a-f]{16})")
R_FAULT_PC = re.compile(r"^pc: (?P<pc>[0-9a-f]{16}) \((?P<mod>[^+()]+)\+0x(?P<off>[0-9a-f]+)\)")
R_GUEST_PC = re.compile(r"^guest pc: (?P<mod>[^+\s]+)\+0x(?P<off>[0-9a-f]+)")
# gta6/diag also prints the faulting data address when it is inside a guest module, and the
# guest frame chain: "guest stack trace:" then " #<n> <16 hex> (<module>+0x<off> or ?)".
R_GUEST_ADDR = re.compile(r"^guest address: (?P<mod>[^+\s]+)\+0x(?P<off>[0-9a-f]+)")
R_GUEST_FRAME = re.compile(r"^#(?P<n>\d+) (?P<v>[0-9a-f]{16}) \((?:(?P<mod>[^+()]+)\+0x(?P<off>[0-9a-f]+)|\?)\)\s*$")
R_RSP = re.compile(r"^\s*\[rsp\+0x(?P<o>[0-9a-f]+)\] (?P<v>[0-9a-f]{16}) \((?P<mod>[^+()]+)\+0x(?P<off>[0-9a-f]+)\)")
R_REG = re.compile(r"\b(r[a-z0-9]{1,2})\s*=([0-9a-f]{16})")
R_DRIVER_PIPE = re.compile(r"^pipeline being created by the driver:(?P<stages>(?: \w+ 0x[0-9a-f]{16})+)")
R_SYMBOL = re.compile(r"(?P<nid>[A-Za-z0-9+\-]{11})\[(?P<lib>[^\]\[]+?)_v(?P<lv>\d+)\]\[(?P<mod>[^\]\[]+?)_v"
                      r"(?P<maj>\d+)\.(?P<min>\d+)\]\[(?P<type>\w+)\]")
R_STUB_CONTRACT = re.compile(r"^Unresolved import stub called: (?P<sym>\S+) \(program=(?P<prog>.*?), first call, "
                             r"returns=0x(?P<ret>[0-9a-fA-F]+), policy=(?P<policy>[\w-]+)\)")
R_STUB_PRINTF = re.compile(r"^Unresolved import stub called \[(?P<n>\d+)\]: .*?symbol=(?P<sym>\S+)"
                           r"(?: type=(?P<type>\w+))?(?: bind=\w+)?(?: program=(?P<prog>.+))?$")
R_STUB_CONSOLE = re.compile(r"^Unresolved import stub called: (?P<sym>\S+)\s*$")
R_OBJ_IMPORT = re.compile(r"^Unresolved object import: (?P<sym>\S+) \(program=(?P<prog>.*?)\) -> invalid page 0x(?P<page>[0-9a-f]+)")
R_IMPORT_SUMMARY = re.compile(r"^Unresolved imports: (?P<f>\d+) functions, (?P<o>\d+) objects; by library: ?(?P<libs>.*)$")
R_IMPORT_NAME = re.compile(r"^Unresolved import name: (?P<sym>\S+) = (?P<name>\S+)")
R_STRICT_IMPORT = re.compile(r"Unresolved import called: (?P<sym>\S+) from (?P<prog>.+?)\s*$")
R_STUBBED = re.compile(r"^Stubbed: \[[0-9a-f]+\] <- [0-9a-f]+, (?P<sym>[^,]+), (?P<type>\w+), (?P<bind>\w+)")
R_RELOC_UNRES = re.compile(r"^Relocate: unresolved (?P<plt>PLT|non-PLT) import patched to stub \[\d+\] \[[0-9a-f]+\] <- "
                           r"[0-9a-f]+, (?P<sym>[^,]+), (?P<type>\w+), (?P<bind>\w+)")
R_REBIND = re.compile(r"^Rebind: (?P<sym>\S+) -> (?P<to>\S+)")
R_NID_FALLBACK = re.compile(r"^PS5 NID fallback: (?P<from>\S+) -> (?P<to>\S+)")
R_SOFT_FAIL = re.compile(r"^Soft-fail: (?P<api>[^:]+): (?P<msg>.*)$")
R_SYSCALL = re.compile(r"^Guest syscall: (?P<n>\d+) raw syscall instruction\(s\) in (?P<mod>\S+) \(first at \+0x(?P<off>[0-9a-f]+)\)")
R_LOADING = re.compile(r"^Loading: (?P<path>.+?)\s*$")
R_BASE = re.compile(r"^base_vaddr\s*= 0x(?P<v>[0-9a-f]+)")
R_MSIZE = re.compile(r"^(?:mapped_size|base_size_aligned)\s*= 0x(?P<v>[0-9a-f]+)")
R_HACK_FIRST = re.compile(r"^\[HACK\] (?P<hack>.+?): first at (?P<site>.+?), shader=(?P<sh>0x[0-9a-f]{16}|unknown), "
                          r"where=0x(?P<w>[0-9a-f]+)(?:, (?P<detail>.*))?$")
R_HACK_SUMMARY = re.compile(r"^\[HACK\] summary of the last 10 s: (?P<n>\d+) applications in (?P<s>\d+) sources")
R_HACK_SUMLINE = re.compile(r"^\[HACK\]   (?P<hack>.+) at (?P<site>.+) shader=(?P<sh>\S+) where=0x(?P<w>[0-9a-f]+): "
                            r"\+(?P<d>\d+) \(total (?P<t>\d+)\)")
R_HACK_FPS = re.compile(r"^\[HACK\] .*?frame time: the game runs at (?P<fps>[\d.]+) fps")
R_DECODER = re.compile(r"(?:0x(?P<pc>[0-9a-f]{8}): )?unsupported family=(?P<family>\w+) opcode=0x(?P<opcode>[0-9a-f]+)"
                       r" raw=\[(?P<raw>[^\]]*)\] reason=(?P<reason>.+?)\s*$")
R_SRT = re.compile(r"shader resource tracking: hash=0x(?P<hash>[0-9a-f]{16}) stage=(?P<stage>\w+) pc=0x(?P<pc>[0-9a-f]+)"
                   r"(?: (?P<why>.*?))?\.?\s*$")
R_STAGE_HASH = re.compile(r"^ShaderRecompiler (?P<stage>[A-Z]{2,3}) (?:phase \w+: (?:stage=\w+ )?)?hash=0x(?P<hash>[0-9a-f]{16})\b")
R_SPIRV_FAIL = re.compile(r"^(?:SPIR-V (?:validation|emission) failed: hash=0x(?P<hash>[0-9a-f]{16}) stage=(?P<stage>\w+)"
                          r"|ShaderRecompiler (?P<stage2>\w+) SPIR-V validation failed hash=0x(?P<hash2>[0-9a-f]{16}))")
R_VK = re.compile(r"^\[Vulkan\]\[(?P<sev>[VIWE?])\]\[(?P<types>\d+)\]: (?P<msg>.*)$")
R_VUID = re.compile(r"VUID-[A-Za-z0-9_\-]+")
R_GPU = re.compile(r"^GPU: (?P<name>.+?) vendor=0x(?P<vendor>[0-9a-fA-F]{1,4}) device=0x(?P<device>[0-9a-fA-F]{1,4})"
                   r" driver=(?P<driver>\S+) api=(?P<api>\S+) driverID=(?P<driverid>\S+) vram=(?P<vram>\d+) MiB"
                   r"(?: bar=(?P<bar>\d+) MiB)?")
R_HOST = re.compile(r"^Host: cpu=(?P<cpu>.+?) threads=(?P<threads>\d+) ram=(?P<ram>\d+) MiB os=(?P<os>.*)$")
R_TITLE_PROFILE = re.compile(r"^Title profile: (?P<key>\S+) \((?P<display>.*?)\) id=(?P<id>\S*) version=(?P<ver>\S*) "
                             r"name=\"(?P<name>.*)\" quirks=(?P<quirks>\S*) preset=(?P<preset>\S+)\s*$")
R_PRESET = re.compile(r"^Preset: (?P<name>\S+) -> (?P<tokens>.*)$")
R_VRAM = re.compile(r"^VRAM: budget=(?P<budget>\d+) MiB limit=(?P<limit>\S+?)(?: MiB)? profile=(?P<profile>\S+) "
                    r"gc=(?P<gc>\S+)")
R_SHADERS = re.compile(r"^Shaders: VS (?P<VS>\d+) \| PS (?P<PS>\d+) \| CS (?P<CS>\d+) \| GS (?P<GS>\d+) \| "
                       r"LS (?P<LS>\d+) \| HS (?P<HS>\d+) \| TES (?P<TES>\d+)")
R_TRACE = re.compile(r"^\[(\d+)\]\[(\d\d:\d\d:\d\d\.\d{3})\] (\w+)::(\w+)::(\w+)\(\)\s*$")
R_DEVICE_REJECT = re.compile(r"^\s+(?P<gpu>[^:]+): (?P<reasons>.+?)(?: in (?:[A-Za-z]:)?[^\s:][^:]*\.(?:cpp|h):\d+)?\s*$")
R_EXIT_CODE = re.compile(r"(?i)\b(?:exit(?:ed)?(?: with)?(?: exit)? code|exit status|return code|process exited with code)"
                         r"[:= ]+(?P<code>-?\d+|0x[0-9a-f]+)\b")
R_BULLET = re.compile(r"^(?P<k>Build|Game|CPU|GPU|RAM|OS|Hardware)\**\s*:\**\s*(?P<v>\S.*)$", re.I)
R_LABEL_SEARCH = re.compile(r"Official build KytyPS5-\d{4}-\d\d-\d\d-[0-9a-f]{7,}|Fork build \S+ [0-9a-f]{7,}"
                            r"|Source build [0-9a-f]{7,}")
R_ISSUE_HEADER = re.compile(r"^#{2,4} (?P<field>Game ID / serial|KytyPS5 version|OS|CPU|GPU|RAM / VRAM|Game title)\s*$")

STAGE_NAMES = {"CS": "compute", "VS": "vertex", "PS": "pixel", "GS": "geometry", "ES": "export", "LS": "local",
               "HS": "hull", "DS": "domain", "MS": "mesh", "TES": "domain"}

VENDORS = {"10de": "NVIDIA", "1002": "AMD", "8086": "Intel", "106b": "Apple", "13b5": "ARM", "5143": "Qualcomm",
           "10005": "Mesa (lavapipe)"}

EXC_TYPE = {0: "Unknown", 1: "AccessViolation", 2: "IllegalInstruction"}
EXC_ACCESS = {0: "unknown", 1: "read", 2: "write", 3: "execute"}
EXC_CODE = {3221225477: "0xC0000005 STATUS_ACCESS_VIOLATION", 2147483649: "0x80000001 STATUS_GUARD_PAGE_VIOLATION",
            3221225501: "0xC000001D STATUS_ILLEGAL_INSTRUCTION", 3221226505: "0xC0000409 STATUS_STACK_BUFFER_OVERRUN",
            11: "SIGSEGV", 10: "SIGBUS", 4: "SIGILL", 7: "SIGBUS (Linux)", 1: "1 (macOS/other: see #1068)"}
EXIT_CODES = {321: "DbgReport fatal error (assert.cpp _Exit(321))", 322: "PageManager fatal (pageManager.cpp _Exit(322))",
              0: "clean exit", 3: "abort() on Windows (terminate handler)", 134: "SIGABRT (Linux shell 128+6)",
              139: "SIGSEGV (Linux shell 128+11)", 3221225477: "0xC0000005 access violation outside the handler",
              3221226505: "0xC0000409 fast-fail / abort (std::terminate on Windows)",
              3221225725: "0xC00000FD stack overflow", -1073741819: "0xC0000005 access violation (signed)",
              -1073740791: "0xC0000409 fast-fail (signed)"}

DRIVER_MODULE_RE = re.compile(r"(?i)^(?:nv\w*\.dll|nvgpucomp\w*\.dll|nvoglv\w*\.dll|nvwgf\w*\.dll|libnvidia-[\w.\-]+"
                              r"|amdvlk\w*\.dll|amdxc\w*\.dll|amd_\w+\.dll|atig\w*\.dll|igvk\w*\.dll|igc\w*\.dll"
                              r"|igd\w*\.dll|libvulkan_(?:radeon|intel|lvp)\w*\.so[\w.]*|libLLVM[\w.\-]*|libMoltenVK\w*\.dylib"
                              r"|vulkan-1\.dll|libvulkan\.so[\w.]*)$")
EMULATOR_MODULE_RE = re.compile(r"(?i)^(?:kyty\w*(?:\.exe)?|launcher\w*(?:\.exe)?|SDL\w*\.dll|libSDL\w*|ucrtbase\.dll"
                                r"|ntdll\.dll|kernel(?:base|32)\.dll|msvcp\w*\.dll|vcruntime\w*\.dll|libc\.so[\w.]*"
                                r"|libstdc\+\+\.so[\w.]*)$")

# ---------------------------------------------------------------------------
# Accumulators

class Evidence:
    __slots__ = ("source", "lineno", "text")

    def __init__(self, source, lineno, text):
        self.source = source
        self.lineno = lineno
        self.text = text[:MAX_LINE_CHARS]

    def as_dict(self):
        return {"source": self.source, "line": self.lineno, "text": self.text}


class Agg:
    """Counted group of lines: count, first examples, last line number."""

    __slots__ = ("count", "examples", "last", "fields")

    def __init__(self, fields=None):
        self.count = 0
        self.examples = []
        self.last = None
        self.fields = fields or {}

    def add(self, ev, fields=None):
        self.count += 1
        if len(self.examples) < EXAMPLES_PER_KEY:
            self.examples.append(ev)
        self.last = ev
        if fields:
            for k, v in fields.items():
                if v is not None and k not in self.fields:
                    self.fields[k] = v


class FatalBlock:
    def __init__(self, kind, source, lineno):
        self.kind = kind            # dbgreport | legacy | inline | pagemanager | terminate | depth
        self.source = source
        self.lineno = lineno
        self.label = None
        self.title = None
        self.lines = []
        self.file = None
        self.line = None
        self.stack = []
        self.shader = None
        self.extra = []             # "Device fault:" / "Device lost:" lines after the report
        self.end_lineno = lineno
        self.closed = False

    @property
    def message(self):
        return "\n".join(self.lines).strip()

    def key(self):
        # Exact text: the same report in a session log and a printf log of one run is identical.
        return (self.title, self.message, basename(self.file), self.line)


class Results:
    """Everything collected from all sources of one run."""

    def __init__(self):
        self.sources = []
        self.identity = {}
        self.gpu = {}
        self.host = {}
        self.title_profile = None
        self.presets = []
        self.vram_config = None
        self.device_rejects = []
        self.fatal = []
        self.inline_fatal = []
        self.fault_contexts = []
        self.terminate = []
        self.guest_abort = []
        self.guest_stacks = []
        self.modules = {}            # name -> (base, size, path)
        self.imports_called = {}     # symbol -> {"calls": {source: n}, ...}
        self.import_names = {}
        self.imports_load = collections.OrderedDict()   # symbol -> type
        self.import_objects = {}
        self.import_summary = None
        self.strict_import = None
        self.rebinds = 0
        self.nid_fallbacks = 0
        self.soft_fails = collections.OrderedDict()
        self.syscalls = []
        self.hacks = collections.OrderedDict()   # hack -> {"keys": {key: info}, "lines": n}
        self.hack_summaries = 0
        self.hack_other = collections.OrderedDict()
        self.shader_unsupported = collections.OrderedDict()
        self.srt = collections.OrderedDict()
        self.stage_by_hash = {}
        self.vk_messages = collections.OrderedDict()
        self.findings = collections.OrderedDict()   # rule id -> {key: Agg}
        self.finding_overflow = collections.Counter()
        self.progress = {"initialized": [], "shaders": None, "shaders_lines": 0, "flips": 0, "flip_done": 0,
                         "execute_main": False, "start_modules": [], "pipeline": None, "shader_cache": None,
                         "clean_exit": [], "fps": None, "last_trace": None, "bda": False, "relaunched": 0,
                         "markers": collections.deque(maxlen=12)}
        self.exit_code_lines = []
        self.issue_fields = {}
        self.game_roots = set()


# ---------------------------------------------------------------------------
# Scanner

class Scanner:
    def __init__(self, rules, results):
        self.rules = rules
        self.r = results
        self._build_dispatch()

    # Dispatch: first byte -> [(prefix bytes, handler or rule)]. Most printf-log
    # lines are tab-indented parameter dumps and are dropped on the first byte.
    def _build_dispatch(self):
        b = [
            (b"--- Build ---", self.h_build),
            (b"--- Error ---", self.h_title),
            (b"--- Fatal Error ---", self.h_title),
            (b"--- Stack Trace ---", self.h_legacy_stack),
            (b"--- Session ", self.h_session),
            (b"--- Relaunched ", self.h_session),
            (b"--- Guest fault context ---", self.h_fault_context),
            (b"--- Execute: Main", self.h_execute),
            (b"--- Start module: ", self.h_start_module),
            (b"Official build ", self.h_label),
            (b"Fork build ", self.h_label),
            (b"Source build ", self.h_label),
            (b"  version: ", self.h_printf_build),
            (b"  os:", self.h_printf_host),
            (b"  cpu:", self.h_printf_host),
            (b"  threads:", self.h_printf_host),
            (b"Title ID: ", self.h_title_id),
            (b"TITLE_ID, string = ", self.h_sfo),
            (b"APP_VER, string = ", self.h_sfo),
            (b"TITLE, string = ", self.h_sfo),
            (b"CONTENT_ID, string = ", self.h_sfo),
            (b"Vulkan device: ", self.h_vk_device),
            (b"Select device: ", self.h_vk_device),
            (b"GPU: ", self.h_gpu),
            (b"Host: cpu=", self.h_host),
            (b"Title profile: ", self.h_title_profile),
            (b"Preset: ", self.h_preset),
            (b"VRAM: budget=", self.h_vram),
            (b"terminate", self.h_terminate),
            (b"PageManager fatal: ", self.h_side_fatal),
            (b"Guest abort diagnostics:", self.h_guest_abort),
            (b"Stack trace [thread = ", self.h_guest_stack),
            (b"while compiling shader: ", self.h_shader_ctx),
            (b"Device fault: ", self.h_device_detail),
            (b"Device lost: ", self.h_device_detail),
            (b"Unresolved import stub called", self.h_stub),
            (b"Unresolved object import: ", self.h_obj_import),
            (b"Unresolved imports: ", self.h_import_summary),
            (b"Unresolved import name: ", self.h_import_name),
            (b"Unresolved import called: ", self.h_strict_import),
            (b"Stubbed: ", self.h_load_unresolved),
            (b"Relocate: unresolved ", self.h_load_unresolved),
            (b"Rebind: ", self.h_rebind),
            (b"PS5 NID fallback: ", self.h_nid_fallback),
            (b"Soft-fail: ", self.h_soft_fail),
            (b"Guest syscall: ", self.h_syscall),
            (b"Loading: ", self.h_loading),
            (b"base_vaddr", self.h_base),
            (b"mapped_size", self.h_msize),
            (b"[HACK] ", self.h_hack),
            (b"shader resource tracking: ", self.h_srt),
            (b"unsupported decoded instruction: ", self.h_decoder_line),
            (b"ShaderRecompiler ", self.h_stage_hash),
            (b"SPIR-V ", self.h_stage_hash),
            (b"pipeline being created by the driver:", self.h_driver_pipeline),
            (b"[Vulkan][", self.h_vk_message),
            (b"Initialized: ", self.h_initialized),
            (b"Shaders: VS ", self.h_shaders),
            (b"Vulkan pipeline cache: ", self.h_pipeline),
            (b"Pipeline manifest: ", self.h_pipeline),
            (b"Shader cache: ", self.h_shader_cache),
            (b"Window ", self.h_clean_exit),
            (b"Event: quit", self.h_clean_exit),
            (b"exit!!!", self.h_clean_exit),
            (b"done!", self.h_clean_exit),
            (b"Flip done: ", self.h_flip_done),
            (b"CommandProcessor::Flip()", self.h_flip),
            (b"\t flip_mode ", self.h_flip),
        ]
        self.dispatch = collections.defaultdict(list)
        self.prefixes = set()
        for prefix, handler in b:
            self.dispatch[prefix[0]].append((prefix, handler, None))
            self.prefixes.add(prefix)
        for rule in self.rules.line_rules:
            if rule.prefix:
                self.dispatch[rule.prefix[0]].append((rule.prefix, None, rule))
                self.prefixes.add(rule.prefix)
        self.needle_rules = [r for r in self.rules.line_rules if r.needle]
        self.needles = tuple(sorted({r.needle for r in self.needle_rules}))
        # Indented lines only matter for these prefixes (outside blocks).
        self.indented = tuple(p for p in self.prefixes if p[:1] in (b" ", b"\t"))
        self.dispatch = dict(self.dispatch)

    def interesting(self, raw):
        """True when a raw line matches any built-in prefix or rule (make_fixture.py)."""
        if not raw:
            return False
        cands = self.dispatch.get(raw[0])
        if cands:
            for prefix, _h, _r in cands:
                if raw.startswith(prefix):
                    return True
        for n in self.needles:
            if n in raw:
                return True
        return False

    # -- per source ------------------------------------------------------
    def scan(self, source, quick=False):
        info = {"name": source.name, "lines": 0, "bytes": 0, "truncated_last_line": False, "scanned": "full",
                "kind": None, "lenient": False}
        self.r.sources.append(info)
        self.src = source.name
        size = source.size
        self.lenient = size is None and source.path is None and not source.compressed or (
            size is not None and size < LENIENT_MAX_BYTES)
        if source.name == "<stdin>":
            self.lenient = True
        info["lenient"] = self.lenient
        self.block = None
        self.block_handler = None
        self.pending_module = None
        self.issue_field = None
        self.kind_votes = collections.Counter()
        self.last_fatal = None
        self.lineno = 0
        segments = None
        if quick and source.path and size and size > QUICK_HEAD_BYTES + QUICK_TAIL_BYTES:
            segments = [(0, QUICK_HEAD_BYTES), (size - QUICK_TAIL_BYTES, size)]
            info["scanned"] = "quick (first %d MiB + last %d MiB)" % (QUICK_HEAD_BYTES >> 20, QUICK_TAIL_BYTES >> 20)
        f = source.open()
        try:
            if segments is None:
                self._scan_stream(f, info, 0)
            else:
                # Tail first: the crash is at the end. Then the head for identity.
                tail_start = segments[1][0]
                f.seek(tail_start)
                f.readline()  # partial line
                self._scan_stream(f, info, None, label="tail")
                self.block = None
                self.block_handler = None
                f.seek(0)
                self._scan_stream(f, info, 0, limit=segments[0][1], label="head")
        finally:
            if f is not sys.stdin.buffer and source.name != "<stdin>":
                f.close()
        self._close_block()
        if info["kind"] is None:
            info["kind"] = self._guess_kind(info)
        return info

    def _guess_kind(self, info):
        v = self.kind_votes
        if v["session"]:
            return "session-log"
        if v["printf"]:
            return "printf-log"
        if v["markdown"]:
            return "pasted-excerpt"
        if v["console"]:
            return "console-capture"
        return "unknown"

    def _scan_stream(self, f, info, start_lineno, limit=None, label=None):
        lineno = start_lineno if start_lineno is not None else 0
        unknown_numbers = start_lineno is None
        read = 0
        dispatch = self.dispatch
        indented = self.indented
        needles = self.needles
        lenient = self.lenient
        last_raw = b""
        last_trace = None
        for raw in f:
            lineno += 1
            read += len(raw)
            last_raw = raw
            if limit is not None and read > limit:
                break
            first = raw[:1]
            if self.block_handler is not None:
                line = self._decode(raw)
                self.lineno = lineno if not unknown_numbers else -lineno
                if self.block_handler(line):
                    continue
            if first in (b"\t", b" "):
                if lenient:
                    raw = raw.lstrip(b" \t")
                    first = raw[:1]
                elif not raw.startswith(indented):
                    continue
            elif first == b"[" and raw[1:2].isdigit():
                last_trace = (lineno, raw)
                if b"Flip()" in raw:
                    self.r.progress["flips"] += 1
                continue
            if lenient and first in (b">", b"`", b"*", b"-") and not raw.startswith(b"--- "):
                raw = raw.lstrip(b"> `*-").lstrip()
                first = raw[:1]
            if not first:
                continue
            cands = dispatch.get(raw[0])
            hit = False
            line = None
            if cands:
                for prefix, handler, rule in cands:
                    if raw.startswith(prefix):
                        if line is None:
                            line = self._decode(raw)
                            if lenient:
                                line = line.strip().strip("`")
                            self.lineno = lineno if not unknown_numbers else -lineno
                        hit = True
                        if handler is not None:
                            handler(line)
                        else:
                            self._rule_line(rule, line)
            for n in needles:
                if n in raw:
                    if line is None:
                        line = self._decode(raw)
                        if lenient:
                            line = line.strip().strip("`")
                        self.lineno = lineno if not unknown_numbers else -lineno
                    hit = True
                    for rule in self.needle_rules:
                        if rule.needle == n:
                            self._rule_line(rule, line)
            if lenient and not hit:
                if line is None:
                    line = self._decode(raw).strip().strip("`")
                    self.lineno = lineno if not unknown_numbers else -lineno
                self._lenient_line(line)
        if not unknown_numbers:
            info["lines"] = max(info["lines"], lineno)
        info["bytes"] += read
        if label != "head" and last_raw and not last_raw.endswith(b"\n"):
            info["truncated_last_line"] = True
        if last_trace is not None:
            text = self._decode(last_trace[1])
            if R_TRACE.match(text):
                self.r.progress["last_trace"] = Evidence(self.src, last_trace[0], text)

    @staticmethod
    def _decode(raw):
        if len(raw) > MAX_LINE_CHARS:
            raw = raw[:MAX_LINE_CHARS]
        return raw.decode("utf-8", "replace").rstrip("\r\n")

    def ev(self, text):
        return Evidence(self.src, self.lineno, text)

    def marker(self, text):
        self.r.progress["markers"].append(self.ev(text))

    # -- rules -------------------------------------------------------------
    def _rule_line(self, rule, line):
        m = rule.regex.search(line) if rule.regex is not None else None
        if rule.regex is not None and m is None:
            return
        fields = {k: v for k, v in m.groupdict().items() if v is not None} if m else {}
        if rule.key_fields:
            key = "|".join(str(fields.get(k, "")) for k in rule.key_fields)
        else:
            key = normalize(line)[:200]
        per = self.r.findings.setdefault(rule.id, collections.OrderedDict())
        agg = per.get(key)
        if agg is None:
            if len(per) >= MAX_KEYS_PER_RULE:
                self.r.finding_overflow[rule.id] += 1
                return
            agg = per[key] = Agg(dict(fields))
        agg.add(self.ev(line), fields)

    # -- lenient (pasted text) ----------------------------------------------
    def _lenient_line(self, line):
        if not line:
            return
        m = R_BULLET.match(line)
        if m:
            k, v = m.group("k").upper(), m.group("v").strip().strip("*`").strip()
            lm = R_LABEL_SEARCH.search(v)
            if k == "BUILD" and lm:
                self._set_label(lm.group(0))
            elif k in ("GPU", "CPU", "OS", "RAM", "GAME", "HARDWARE") and v:
                self.r.issue_fields.setdefault({"GAME": "Game title"}.get(k, k if k in ("GPU", "CPU", "OS") else
                                                                        k.title()), v)
        m = R_ISSUE_HEADER.match(line)
        if m:
            self.issue_field = m.group("field")
            self.kind_votes["markdown"] += 1
            return
        if self.issue_field:
            if line.startswith("_No response_"):
                self.issue_field = None
            else:
                self.r.issue_fields.setdefault(self.issue_field, line)
                self.issue_field = None
            return
        m = R_EXIT_CODE.search(line)
        if m:
            code = m.group("code")
            self.r.exit_code_lines.append((int(code, 16) if code.lower().startswith("0x") else int(code), self.ev(line)))
        # A fatal message pasted without its "--- Build ---" header.
        if "Unhandled host exception:" in line or "Not implemented (" in line or "Error: condition (" in line \
                or R_WHERE_TAIL.match(line):
            m = R_WHERE_TAIL.match(line)
            if m and not re.search(r"\.(?:cpp|cc|c|h|hpp|inc|mm)$", m.group("file")):
                m = None
            if m or "Unhandled host exception:" in line:
                b = FatalBlock("inline", self.src, self.lineno)
                b.title = "--- Error ---"
                text = m.group("msg") if m else line
                text = re.sub(r"^(?:[\w ]{0,30}: )?(?=(?:Error: condition|Not implemented|Unhandled host exception))",
                              "", text)
                b.lines = [text.strip().strip("`").strip()]
                if m:
                    b.file, b.line = m.group("file"), int(m.group("line"))
                b.closed = True
                self.r.inline_fatal.append(b)

    # -- fatal report state machine -------------------------------------------
    def _close_block(self):
        if self.block is not None and isinstance(self.block, FatalBlock) and not self.block.closed:
            self.block.closed = True
            if self.block.lines or self.block.title:
                self.r.fatal.append(self.block)
                self.last_fatal = self.block
        self.block = None
        self.block_handler = None

    def h_build(self, line):
        self._close_block()
        b = FatalBlock("dbgreport", self.src, self.lineno)
        self.block = b
        self.block_state = "label"
        self.block_handler = self.b_fatal
        self.kind_votes["console"] += 0

    def h_legacy_stack(self, line):
        self._close_block()
        b = FatalBlock("legacy", self.src, self.lineno)
        self.block = b
        self.block_state = "stack"
        self.block_handler = self.b_fatal

    def h_title(self, line):
        # "--- Error ---" without "--- Build ---" (pasted, or the legacy format).
        self._close_block()
        b = FatalBlock("dbgreport", self.src, self.lineno)
        b.title = line.strip()
        self.block = b
        self.block_state = "message"
        self.block_handler = self.b_fatal

    def b_fatal(self, line):
        b = self.block
        s = self.block_state
        text = line.strip() if self.lenient else line
        if self.lenient:
            text = text.lstrip("> ").strip("`")
        if s == "stack":
            m = R_STACK_WORD.match(text)
            if m:
                b.stack.append(m.group(2))
                return True
            if R_TITLE.match(text):
                b.title = text.strip()
                self.block_state = "message"
                return True
            if text.startswith("--- Build ---"):
                self.block_state = "label"
                return True
            self._close_block()
            return False
        if s == "label":
            if not text.strip() and self.lenient:
                return True
            m = R_LABEL.match(text.strip())
            if m:
                b.label = text.strip()
                self._set_label(b.label)
                self.block_state = "title"
                return True
            if R_TITLE.match(text):
                b.title = text.strip()
                self.block_state = "message"
                return True
            # an unknown label (local build without git): keep it
            if not text.startswith("---"):
                b.label = text.strip()
                self.block_state = "title"
                return True
            self._close_block()
            return False
        if s == "title":
            if not text.strip() and self.lenient:
                return True
            if text.startswith("--- Stack Trace ---"):
                if b.kind == "dbgreport":
                    b.kind = "legacy"
                self.block_state = "stack"
                return True
            if R_TITLE.match(text):
                b.title = text.strip()
                self.block_state = "message"
                return True
            self.block_state = "message"
            b.title = None
            # fall through: treat as message
        # message lines until " in file:line"
        if text.startswith("--- Build ---") and b.lines:
            self._close_block()
            return False
        if text.startswith("```") and self.lenient:
            if b.lines:
                self._close_block()
            return True
        m = R_WHERE_ONLY.match(text)
        if m and b.lines:
            b.file, b.line = m.group("file"), int(m.group("line"))
            b.end_lineno = self.lineno
            self._close_block()
            return True
        m = R_WHERE_TAIL.match(text)
        if m:
            b.lines.append(m.group("msg"))
            b.file, b.line = m.group("file"), int(m.group("line"))
            b.end_lineno = self.lineno
            self._close_block()
            return True
        if not text.strip() and not b.lines:
            return True
        b.lines.append(text.rstrip())
        b.end_lineno = self.lineno
        if len(b.lines) >= MAX_MESSAGE_LINES:
            self._close_block()
        return True

    def h_shader_ctx(self, line):
        m = R_SHADER_CTX.match(line)
        if m and self.last_fatal is not None and self.last_fatal.source == self.src:
            self.last_fatal.shader = {"stage": m.group("stage"), "hash": "0x" + m.group("hash")}
            self.r.stage_by_hash.setdefault("0x" + m.group("hash"), m.group("stage"))

    def h_device_detail(self, line):
        if self.last_fatal is not None and self.last_fatal.source == self.src and \
                0 <= self.lineno - self.last_fatal.end_lineno <= 16:
            self.last_fatal.extra.append(line)

    def h_side_fatal(self, line):
        # PageManager reports and exits (322) without a DbgReport block.
        b = FatalBlock("pagemanager", self.src, self.lineno)
        b.title = "PageManager fatal"
        b.lines = [line]
        b.closed = True
        self.r.fatal.append(b)
        self.last_fatal = b

    def h_terminate(self, line):
        if line.startswith("terminate: uncaught exception") or line.startswith("terminate called"):
            b = FatalBlock("terminate", self.src, self.lineno)
            b.title = "terminate"
            b.lines = [line]
            b.closed = True
            self.r.terminate.append(b)

    # -- guest fault context -------------------------------------------------
    def h_fault_context(self, line):
        self._close_block()
        ctx = {"source": self.src, "line": self.lineno, "end": self.lineno, "thread": None, "host_pc": None, "guest_pc": None,
               "guest_address": None, "driver_pipeline": None, "regs": {}, "stack_hits": [], "guest_frames": [],
               "lines": [line]}
        self.r.fault_contexts.append(ctx)
        self.fault = ctx
        self.fault_state = None
        self.block = ctx
        self.block_handler = self.b_fault

    def b_fault(self, line):
        ctx = self.fault
        text = line.strip("`") if self.lenient else line
        st = text.strip()
        if st.startswith("thread: "):
            ctx["thread"] = st[8:]
        elif R_FAULT_PC.match(st):
            m = R_FAULT_PC.match(st)
            ctx["host_pc"] = {"pc": "0x" + m.group("pc"), "module": m.group("mod"), "offset": "0x" + m.group("off")}
        elif R_GUEST_PC.match(st):
            m = R_GUEST_PC.match(st)
            ctx["guest_pc"] = {"module": m.group("mod"), "offset": "0x" + m.group("off")}
        elif R_GUEST_ADDR.match(st):
            m = R_GUEST_ADDR.match(st)
            ctx["guest_address"] = {"module": m.group("mod"), "offset": "0x" + m.group("off")}
        elif st == "guest stack trace:":
            self.fault_state = "gstk"
        elif self.fault_state == "gstk" and R_GUEST_FRAME.match(st):
            m = R_GUEST_FRAME.match(st)
            if len(ctx["guest_frames"]) < 32:
                ctx["guest_frames"].append({"value": "0x" + m.group("v"), "module": m.group("mod"),
                                            "offset": "0x" + m.group("off") if m.group("off") else None})
        elif R_DRIVER_PIPE.match(st):
            self.h_driver_pipeline(st)
            ctx["driver_pipeline"] = st.split(":", 1)[1].strip()
        elif R_REG.search(st) and st[:3] in ("rax", "rsi", "r8 ", "r12") or st.startswith("r8 ="):
            for k, v in R_REG.findall(st):
                ctx["regs"][k] = "0x" + v
        elif st.startswith("code (pc-48") or st == "stack:" or st == "...":
            self.fault_state = st[:4]
        elif R_RSP.match(text):
            m = R_RSP.match(text)
            ctx["stack_hits"].append({"rsp": "0x" + m.group("o"), "value": "0x" + m.group("v"),
                                      "module": m.group("mod"), "offset": "0x" + m.group("off")})
        elif self.fault_state and re.match(r"^\s*(?:[0-9a-f]{2}(?: [0-9a-f]{2})*|[0-9a-f]{16}(?: [0-9a-f]{16})*)\s*$",
                                           text):
            pass
        else:
            self.block = None
            self.block_handler = None
            return False
        if len(ctx["lines"]) < 64:
            ctx["lines"].append(line)
        ctx["end"] = self.lineno
        return True

    # -- guest abort / guest stack trace ------------------------------------------
    def h_guest_abort(self, line):
        self._close_block()
        self.abort = {"source": self.src, "line": self.lineno, "words": []}
        self.r.guest_abort.append(self.abort)
        self.block = self.abort
        self.block_handler = self.b_abort

    def b_abort(self, line):
        m = re.match(r"^\s*\[(\d+)\] 0x([0-9a-f]+) (\S+)\+0x([0-9a-f]+)", line)
        if m:
            if len(self.abort["words"]) < 32:
                self.abort["words"].append({"value": "0x" + m.group(2), "module": m.group(3),
                                            "offset": "0x" + m.group(4)})
            return True
        if line.strip() == "":
            return True
        self.block = None
        self.block_handler = None
        return False

    def h_guest_stack(self, line):
        self._close_block()
        self.gstack = {"source": self.src, "line": self.lineno, "header": line, "frames": []}
        self.r.guest_stacks.append(self.gstack)
        self.block = self.gstack
        self.block_handler = self.b_gstack

    def b_gstack(self, line):
        m = re.match(r"^\s*\[(\d+)\] ([0-9a-f]{16}), off=([0-9a-f]{16}), (\S+)", line)
        if m:
            if len(self.gstack["frames"]) < 32:
                self.gstack["frames"].append({"vaddr": "0x" + m.group(2), "offset": "0x" + m.group(3).lstrip("0"),
                                              "module": m.group(4)})
            return True
        self.block = None
        self.block_handler = None
        return False

    # -- identity ---------------------------------------------------------------
    def _set_label(self, label):
        ident = self.r.identity
        ident.setdefault("build_label", label)
        m = R_LABEL.match(label)
        if m:
            if m.group("official"):
                ident.setdefault("build_kind", "official")
                ident.setdefault("build_id", m.group("tag"))
            elif m.group("fork"):
                ident.setdefault("build_kind", "fork")
                ident.setdefault("build_id", "%s %s" % (m.group("repo"), m.group("fhash")))
            else:
                ident.setdefault("build_kind", "source")
                ident.setdefault("build_id", m.group("shash"))

    def h_session(self, line):
        m = R_SESSION.match(line.strip())
        if not m:
            return
        self.kind_votes["session"] += 1
        if m.group("kind") == "Session":
            self.r.identity.setdefault("session_start", m.group("ts"))
        else:
            self.r.progress["relaunched"] += 1
        self.marker(line)

    def h_label(self, line):
        if R_LABEL.match(line.strip()):
            self._set_label(line.strip())

    def h_printf_build(self, line):
        label = line.split(":", 1)[1].strip()
        self.kind_votes["printf"] += 1
        if R_LABEL.match(label):
            self._set_label(label)

    def h_printf_host(self, line):
        key, _, value = line.strip().partition(":")
        value = value.strip()
        if key in ("os", "cpu", "threads") and value:
            self.r.host.setdefault(key, value)

    def h_title_id(self, line):
        tid = line.split(":", 1)[1].strip()
        if tid:
            self.r.identity.setdefault("title_id", tid)
            self.marker(line)

    def h_sfo(self, line):
        key, _, value = line.partition(", string = ")
        self.kind_votes["printf"] += 1
        names = {"TITLE_ID": "title_id", "APP_VER": "app_ver", "TITLE": "title", "CONTENT_ID": "content_id"}
        if key in names and value.strip():
            self.r.identity.setdefault(names[key], value.strip())

    def h_vk_device(self, line):
        name = line.split(":", 1)[1].strip()
        self.kind_votes["printf"] += 1
        if line.startswith("Select device"):
            self.r.gpu["name"] = name
            self.r.gpu.setdefault("source", "Select device")
        else:
            self.r.gpu.setdefault("name", name)
            self.r.gpu.setdefault("source", "Vulkan device")

    def h_gpu(self, line):
        m = R_GPU.match(line)
        if m:
            g = self.r.gpu
            g["name"] = m.group("name")
            g["vendor_id"] = "0x" + m.group("vendor").lower().zfill(4)
            g["device_id"] = "0x" + m.group("device").lower().zfill(4)
            g["vendor"] = VENDORS.get(m.group("vendor").lower().zfill(4), "unknown")
            g["driver"] = m.group("driver")
            g["api"] = m.group("api")
            g["driver_id"] = m.group("driverid")
            g["vram_mib"] = int(m.group("vram"))
            if m.group("bar"):
                g["bar_mib"] = int(m.group("bar"))
            g["source"] = "GPU line"
            self.kind_votes["session"] += 0
            return
        if line.startswith("GPU: using buffer device address"):
            self.r.progress["bda"] = True
            self.marker(line)
        elif self.lenient and not line.startswith("GPU: indirect"):
            self.r.issue_fields.setdefault("GPU", line[5:].strip().strip("*").strip())

    def h_host(self, line):
        m = R_HOST.match(line)
        if m:
            self.r.host.update({"cpu": m.group("cpu"), "threads": m.group("threads"),
                                "ram_mib": int(m.group("ram")), "os": m.group("os")})

    def h_title_profile(self, line):
        m = R_TITLE_PROFILE.match(line)
        if m:
            d = m.groupdict()
            d["quirks"] = [] if d["quirks"] in ("none", "") else d["quirks"].split(",")
            self.r.title_profile = d
            ident = self.r.identity
            if d["id"]:
                ident.setdefault("title_id", d["id"])
            if d["ver"]:
                ident.setdefault("app_ver", d["ver"])
            if d["name"]:
                ident.setdefault("title", d["name"])
            self.marker(line)

    def h_preset(self, line):
        m = R_PRESET.match(line)
        if m:
            self.r.presets.append({"name": m.group("name"), "tokens": m.group("tokens")})
            self.marker(line)

    def h_vram(self, line):
        m = R_VRAM.match(line)
        if m:
            d = m.groupdict()
            d["line"] = line
            self.r.vram_config = d

    # -- imports --------------------------------------------------------------
    def _called(self, sym, line, **extra):
        rec = self.r.imports_called.get(sym)
        if rec is None:
            rec = self.r.imports_called[sym] = {"calls": collections.Counter(), "first": self.ev(line)}
        rec["calls"][self.src] += 1
        rec["last"] = self.ev(line)
        for k, v in extra.items():
            if v is not None:
                rec.setdefault(k, v)

    def h_stub(self, line):
        m = R_STUB_CONTRACT.match(line)
        if m:
            self.kind_votes["session"] += 0
            self._called(m.group("sym"), line, program=m.group("prog"), returns="0x" + m.group("ret"),
                         policy=m.group("policy"))
            return
        m = R_STUB_PRINTF.match(line)
        if m:
            self._called(m.group("sym"), line, program=(m.group("prog") or None))
            return
        m = R_STUB_CONSOLE.match(line)
        if m:
            self.kind_votes["console"] += 1
            self._called(m.group("sym"), line)

    def h_obj_import(self, line):
        m = R_OBJ_IMPORT.match(line)
        if m:
            self.r.import_objects.setdefault(m.group("sym"), {"program": m.group("prog"), "page": "0x" + m.group("page"),
                                                              "first": self.ev(line)})

    def h_import_summary(self, line):
        m = R_IMPORT_SUMMARY.match(line)
        if m:
            libs = collections.OrderedDict()
            for part in m.group("libs").split(","):
                k, _, v = part.strip().partition("=")
                if k and v.strip().isdigit():
                    libs[k.strip()] = int(v)
            self.r.import_summary = {"functions": int(m.group("f")), "objects": int(m.group("o")),
                                     "by_library": libs, "evidence": self.ev(line)}

    def h_import_name(self, line):
        m = R_IMPORT_NAME.match(line)
        if m:
            self.r.import_names[m.group("sym")] = m.group("name")

    def h_strict_import(self, line):
        m = R_STRICT_IMPORT.search(line)
        if m:
            self.r.strict_import = {"symbol": m.group("sym"), "program": m.group("prog"), "evidence": self.ev(line)}

    def h_load_unresolved(self, line):
        m = R_STUBBED.match(line) or R_RELOC_UNRES.match(line)
        if m:
            self.kind_votes["printf"] += 1
            self.r.imports_load.setdefault(m.group("sym").strip(), m.group("type"))

    def h_rebind(self, line):
        if R_REBIND.match(line):
            self.r.rebinds += 1

    def h_nid_fallback(self, line):
        if R_NID_FALLBACK.match(line):
            self.r.nid_fallbacks += 1

    def h_soft_fail(self, line):
        m = R_SOFT_FAIL.match(line)
        if m:
            key = (m.group("api").strip(), normalize(m.group("msg"))[:160])
            agg = self.r.soft_fails.get(key)
            if agg is None:
                agg = self.r.soft_fails[key] = Agg({"api": key[0], "message": m.group("msg")})
            agg.add(self.ev(line))

    def h_syscall(self, line):
        m = R_SYSCALL.match(line)
        if m:
            self.r.syscalls.append({"count": int(m.group("n")), "module": m.group("mod"), "offset": "0x" + m.group("off"),
                                    "evidence": self.ev(line)})

    def h_loading(self, line):
        m = R_LOADING.match(line)
        if m:
            self.pending_module = {"path": m.group("path")}
            self.kind_votes["printf"] += 1
            for root in find_game_roots(line):
                self.r.game_roots.add(root)

    def h_base(self, line):
        m = R_BASE.match(line)
        if m and self.pending_module is not None and "base" not in self.pending_module:
            self.pending_module["base"] = int(m.group("v"), 16)

    def h_msize(self, line):
        m = R_MSIZE.match(line)
        pm = self.pending_module
        if m and pm is not None and "base" in pm:
            name = basename(pm["path"])
            self.r.modules[name] = (pm["base"], int(m.group("v"), 16), pm["path"])
            self.pending_module = None

    # -- [HACK] -------------------------------------------------------------------
    def h_hack(self, line):
        m = R_HACK_FIRST.match(line)
        if m:
            hack = m.group("hack")
            rec = self.r.hacks.setdefault(hack, {"keys": collections.OrderedDict(), "first": self.ev(line)})
            key = (m.group("site"), m.group("sh"), m.group("w"))
            if key not in rec["keys"] and len(rec["keys"]) < 512:
                rec["keys"][key] = {"site": m.group("site"), "shader": m.group("sh"), "where": "0x" + m.group("w"),
                                    "detail": m.group("detail"), "total": 1, "evidence": self.ev(line)}
            if hack == "skip unsupported shader instruction":
                self._shader_unsupported(m.group("detail") or "", self.ev(line), shader=m.group("sh"),
                                         where=m.group("w"), skipped=True)
            return
        m = R_HACK_SUMLINE.match(line)
        if m:
            hack = m.group("hack")
            rec = self.r.hacks.setdefault(hack, {"keys": collections.OrderedDict(), "first": self.ev(line)})
            key = (m.group("site"), m.group("sh"), m.group("w"))
            info = rec["keys"].get(key)
            if info is None and len(rec["keys"]) < 512:
                info = rec["keys"][key] = {"site": m.group("site"), "shader": m.group("sh"),
                                           "where": "0x" + m.group("w"), "detail": None, "total": 0,
                                           "evidence": self.ev(line)}
            if info is not None:
                info["total"] = max(info["total"], int(m.group("t")))
            return
        if R_HACK_SUMMARY.match(line):
            self.r.hack_summaries += 1
            return
        m = R_HACK_FPS.match(line)
        if m:
            self.r.progress["fps"] = {"fps": float(m.group("fps")), "evidence": self.ev(line)}
            self.marker(line)
        if line.startswith("[HACK]   ("):
            return
        key = normalize(line)[:160]
        agg = self.r.hack_other.get(key)
        if agg is None:
            if len(self.r.hack_other) >= 256:
                return
            agg = self.r.hack_other[key] = Agg({"text": line})
        agg.add(self.ev(line))

    # -- shaders ---------------------------------------------------------------------
    def _shader_unsupported(self, text, ev, shader=None, where=None, skipped=False, fatal=False, stage=None):
        m = R_DECODER.search(text)
        if m:
            fam, op = m.group("family"), "0x" + m.group("opcode")
            key = "%s:%s" % (fam, op)
            fields = {"family": fam, "opcode": op, "encoding": m.group("raw"), "reason": m.group("reason")}
            if m.group("pc"):
                fields["pc"] = "0x" + m.group("pc")
        else:
            reason = text.strip()
            key = "other:" + normalize(reason)[:120]
            fields = {"family": None, "opcode": None, "encoding": None, "reason": reason}
            if where:
                fields["pc"] = "0x" + where
        rec = self.r.shader_unsupported.get(key)
        if rec is None:
            rec = self.r.shader_unsupported[key] = {"fields": fields, "shaders": collections.OrderedDict(),
                                                    "count": 0, "skipped": 0, "fatal": 0, "first": ev}
        rec["count"] += 1
        if skipped:
            rec["skipped"] += 1
        if fatal:
            rec["fatal"] += 1
        if shader and shader != "unknown" and len(rec["shaders"]) < 64:
            rec["shaders"].setdefault(shader, {"pc": ("0x" + where) if where else fields.get("pc"), "stage": stage})

    def h_decoder_line(self, line):
        self._shader_unsupported(line, self.ev(line))

    def h_srt(self, line):
        m = R_SRT.search(line)
        if not m:
            return
        why = (m.group("why") or "").strip()
        why = R_WHERE_TAIL.sub(lambda mm: mm.group("msg"), why) if R_WHERE_TAIL.match(why) else why
        self._srt(m.group("hash"), m.group("stage"), m.group("pc"), why, self.ev(line), fatal=False)

    def _srt(self, h, stage, pc, why, ev, fatal):
        key = "0x%s|0x%s|%s" % (h, pc.lstrip("0") or "0", normalize(why)[:80])
        rec = self.r.srt.get(key)
        if rec is None:
            rec = self.r.srt[key] = {"hash": "0x" + h, "stage": stage, "pc": "0x" + (pc.lstrip("0") or "0"),
                                     "why": why, "count": 0, "fatal": False, "first": ev}
        rec["count"] += 1
        rec["fatal"] = rec["fatal"] or fatal
        self.r.stage_by_hash.setdefault("0x" + h, stage)

    def h_stage_hash(self, line):
        m = R_STAGE_HASH.match(line)
        if m:
            self.r.stage_by_hash.setdefault("0x" + m.group("hash"), m.group("stage"))
            return
        m = R_SPIRV_FAIL.match(line)
        if m:
            h = m.group("hash") or m.group("hash2")
            st = m.group("stage") or m.group("stage2")
            self.r.stage_by_hash.setdefault("0x" + h, st)

    def h_driver_pipeline(self, line):
        m = R_DRIVER_PIPE.match(line.strip())
        if m:
            parts = m.group("stages").split()
            for st, h in zip(parts[0::2], parts[1::2]):
                self.r.stage_by_hash.setdefault(h, st.upper())

    # -- Vulkan ----------------------------------------------------------------------
    def h_vk_message(self, line):
        m = R_VK.match(line)
        if not m:
            return
        vuids = R_VUID.findall(m.group("msg"))
        key = (m.group("sev"), vuids[0] if vuids else normalize(m.group("msg"))[:120])
        agg = self.r.vk_messages.get(key)
        if agg is None:
            if len(self.r.vk_messages) >= 256:
                return
            agg = self.r.vk_messages[key] = Agg({"severity": m.group("sev"), "vuid": vuids[0] if vuids else None,
                                                 "types": int(m.group("types"))})
        agg.add(self.ev(line))

    # -- progress -----------------------------------------------------------------------
    def h_execute(self, line):
        self.r.progress["execute_main"] = True
        self.marker(line)

    def h_start_module(self, line):
        mods = self.r.progress["start_modules"]
        mods.append(line.split(":", 1)[1].strip().rstrip("-").strip())
        del mods[:-8]

    def h_initialized(self, line):
        self.kind_votes["console"] += 1
        inits = self.r.progress["initialized"]
        name = line.split(":", 1)[1].strip()
        if name not in inits:
            inits.append(name)

    def h_shaders(self, line):
        m = R_SHADERS.match(line)
        if m:
            self.r.progress["shaders"] = {k: int(v) for k, v in m.groupdict().items()}
            self.r.progress["shaders_ev"] = self.ev(line)
            self.r.progress["shaders_lines"] += 1

    def h_pipeline(self, line):
        self.r.progress["pipeline"] = self.ev(line)

    def h_shader_cache(self, line):
        self.r.progress["shader_cache"] = self.ev(line)
        self.marker(line)

    def h_clean_exit(self, line):
        st = line.strip()
        if re.match(r"^(?:Window \d+ closed|Event: quit|exit!!!|done!)$", st):
            self.r.progress["clean_exit"].append(self.ev(st))
            self.marker(st)

    def h_flip_done(self, line):
        self.r.progress["flip_done"] += 1

    def h_flip(self, line):
        self.r.progress["flips"] += 1


# ---------------------------------------------------------------------------
# Analysis

def library_files(repo):
    """LIB_NAME/LIB_VERSION library name -> repo-relative source files."""
    out = collections.defaultdict(set)
    if not repo:
        return out
    src = os.path.join(repo, "src")
    if not os.path.isdir(src):
        return out
    pat = re.compile(r'LIB_(?:NAME|VERSION)\(\s*"([^"]+)"')
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ("3rdparty",)]
        for n in files:
            if not n.endswith(".cpp"):
                continue
            p = os.path.join(root, n)
            try:
                with open(p, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            if "LIB_" not in text:
                continue
            for lib in pat.findall(text):
                out[lib.split(".")[0]].add(os.path.relpath(p, repo).replace("\\", "/"))
    return out


def default_repo():
    cand = os.path.normpath(os.path.join(TOOL_DIR, "..", ".."))
    if os.path.isdir(os.path.join(cand, "src", "libs")):
        return cand
    return None


def parse_symbol(sym):
    m = R_SYMBOL.search(sym)
    if not m:
        return {"symbol": sym, "nid": None, "library": "?", "module": "?", "type": None}
    return {"symbol": sym, "nid": m.group("nid"), "library": m.group("lib"), "library_version": int(m.group("lv")),
            "module": m.group("mod"), "module_version": "%s.%s" % (m.group("maj"), m.group("min")),
            "type": m.group("type")}


def resolve_address(addr, modules):
    for name, (base, size, _path) in modules.items():
        if base <= addr < base + size:
            return name, addr - base
    return None, None


def classify_host_exception(m, results, block):
    d = {"type": int(m.group("type")), "code": int(m.group("code")), "access": int(m.group("access")),
         "address": "0x%016x" % int(m.group("address"), 16)}
    d["type_name"] = EXC_TYPE.get(d["type"], "?")
    d["access_name"] = EXC_ACCESS.get(d["access"], "?")
    d["code_name"] = EXC_CODE.get(d["code"], str(d["code"]))
    pc = int(m.group("pc"), 16) if m.group("pc") else None
    addr = int(m.group("address"), 16)
    d["pc"] = "0x%016x" % pc if pc is not None else None
    d["address_kind"] = "near-null" if addr < 0x10000 else "other"
    ctx = None
    for c in results.fault_contexts:
        # The context is printed right before the EXIT (other threads may write in between).
        if c["source"] == block.source and 0 <= (block.lineno - c["end"]) <= 40:
            ctx = c
    mod = off = None
    basis = None
    if ctx and ctx.get("guest_pc"):
        mod, off, basis = ctx["guest_pc"]["module"], int(ctx["guest_pc"]["offset"], 16), "guest pc line"
    elif pc is not None:
        mod, off = resolve_address(pc, results.modules)
        if mod:
            basis = "module map (Loading:/base_vaddr)"
    if mod is None and ctx and ctx.get("host_pc"):
        mod, off, basis = ctx["host_pc"]["module"], int(ctx["host_pc"]["offset"], 16), "host module (pc: line)"
    if mod is None and pc is not None and 0x900000000 <= pc < 0x900000000 + 0x40000000:
        mod, off, basis = "eboot.bin", pc - 0x900000000, "assumed (eboot.bin base 0x900000000)"
    d["pc_module"] = mod
    d["pc_offset"] = "0x%x" % off if off is not None else None
    d["pc_basis"] = basis
    d["fault_context"] = bool(ctx)
    if ctx:
        d["thread"] = ctx.get("thread")
        d["driver_pipeline"] = ctx.get("driver_pipeline")
        d["stack_modules"] = ["%s+%s" % (h["module"], h["offset"]) for h in ctx["stack_hits"][:12]]
        ga = ctx.get("guest_address")
        d["guest_address"] = "%s+%s" % (ga["module"], ga["offset"]) if ga else None
        d["guest_frames"] = ["%s+%s" % (g["module"], g["offset"]) if g["module"] else g["value"]
                             for g in ctx.get("guest_frames", [])[:16]]
    # class
    if pc is not None and (pc < 0x10000 or (d["access"] == 3 and pc == addr)):
        cls = "host-fault-bad-jump"
    elif mod and DRIVER_MODULE_RE.match(mod):
        cls = "host-fault-driver"
    elif mod and EMULATOR_MODULE_RE.match(mod):
        cls = "host-fault-emulator"
    elif mod and (mod.endswith((".bin", ".prx", ".sprx", ".elf", ".self")) or basis and "guest" in basis):
        cls = "host-fault-guest"
    elif mod:
        cls = "host-fault-host"
    elif pc is not None and pc >= 0x7f0000000000:
        cls = "host-fault-host"
    elif pc is not None and results.modules:
        cls = "host-fault-host"
    elif pc is None:
        cls = "host-fault-host"
    else:
        cls = "host-fault-guest"
    d["class"] = cls
    return d


def classify_fatal(block, rules, results):
    msg = block.message
    out = {"kind": block.kind, "title": block.title, "label": block.label, "message": msg,
           "file": block.file, "file_basename": basename(block.file), "line": block.line,
           "src_path": src_relative(block.file), "build_origin": build_origin(block.file),
           "source": block.source, "lineno": block.lineno, "shader": block.shader, "extra": block.extra[:8],
           "stack": block.stack[:16]}
    fields = {}
    rule = None
    m = R_HOST_EXC.search(msg)
    if m:
        exc = classify_host_exception(m, results, block)
        out["host_exception"] = exc
        rule = rules.rule(exc["class"])
        fields.update({"pc_module": exc["pc_module"], "pc_offset": exc["pc_offset"]})
    if rule is None:
        for r in rules.fatal_rules:
            f = r.fatal_match(msg, block.file)
            if f is not None:
                rule = r
                fields.update(f)
                break
    if rule is None:
        rule = rules.rule("fatal-other")
    out["rule_id"] = rule.id if rule else None
    out["category"] = rule.category if rule else "FATAL_OTHER"
    out["fields"] = fields
    # shader details in the message
    dm = R_DECODER.search(msg)
    if dm:
        out["shader_instruction"] = {"family": dm.group("family"), "opcode": "0x" + dm.group("opcode"),
                                     "encoding": dm.group("raw"), "reason": dm.group("reason")}
    sm = R_SRT.search(msg.split("\n")[0])
    if sm:
        out["resource_tracking"] = {"hash": "0x" + sm.group("hash"), "stage": sm.group("stage"),
                                    "pc": "0x" + (sm.group("pc").lstrip("0") or "0"), "why": (sm.group("why") or "")}
    faults = [ln.strip() for ln in msg.split("\n") if ln.strip().startswith(("Device fault: ", "Device lost: "))]
    if faults:
        out["device_fault"] = faults[:12]
    if msg.startswith("Could not find suitable device:"):
        rejects = []
        for ln in msg.split("\n")[1:]:
            rm = R_DEVICE_REJECT.match(ln if ln.startswith(" ") else "  " + ln)
            if rm:
                rejects.append({"gpu": rm.group("gpu").strip(), "reasons": [x.strip() for x in rm.group("reasons").split(";")]})
        out["device_rejects"] = rejects
    sig_parts = [out["category"], out["file_basename"] or "", normalize(msg.split("\n")[0])[:160]]
    if out.get("resource_tracking"):
        sig_parts.append(out["resource_tracking"]["hash"])
    if out.get("host_exception") and out["host_exception"].get("pc_module"):
        sig_parts.append("%s+%s" % (out["host_exception"]["pc_module"], out["host_exception"]["pc_offset"]))
    out["signature"] = " | ".join(p for p in sig_parts if p)
    known = []
    if out.get("resource_tracking"):
        known += rules.known_for("hash", out["resource_tracking"]["hash"])
    if out.get("host_exception") and out["host_exception"].get("pc_module"):
        known += rules.known_for("guest_pc", "%s+%s" % (out["host_exception"]["pc_module"],
                                                        out["host_exception"]["pc_offset"]))
    known += rules.known_for("message", msg)
    out["known"] = known
    return out


def analyze(results, rules, repo=None, exit_code=None):
    rep = collections.OrderedDict()
    rep["tool"] = TOOL
    rep["schema"] = SCHEMA
    rep["sources"] = results.sources

    # ---- identity
    ident = collections.OrderedDict()
    for k in ("build_label", "build_kind", "build_id", "session_start", "title_id", "app_ver", "title", "content_id"):
        if results.identity.get(k) is not None:
            ident[k] = results.identity[k]
    issue = results.issue_fields
    if issue:
        ident["issue_template"] = dict(issue)
        if "title_id" not in ident and issue.get("Game ID / serial"):
            ident["title_id"] = issue["Game ID / serial"].split()[0]
    gpu = dict(results.gpu)
    if not gpu.get("name") and issue.get("GPU"):
        gpu["name"] = issue["GPU"]
        gpu["source"] = "issue template"
    if gpu.get("name") and not gpu.get("vendor"):
        n = gpu["name"].lower()
        gpu["vendor"] = ("NVIDIA" if ("nvidia" in n or "geforce" in n or "rtx" in n or "gtx" in n) else
                         "AMD" if ("amd" in n or "radeon" in n) else
                         "Intel" if ("intel" in n or " arc" in n) else
                         "Apple" if ("apple" in n) else "unknown")
    ident["gpu"] = gpu
    host = dict(results.host)
    if not host.get("os") and issue.get("OS"):
        host["os"] = issue["OS"]
    if not host.get("cpu") and issue.get("CPU"):
        host["cpu"] = issue["CPU"]
    ident["host"] = host
    if results.title_profile:
        ident["title_profile"] = results.title_profile
    if results.presets:
        ident["presets"] = results.presets
    if results.vram_config:
        ident["vram_config"] = results.vram_config
    rep["identity"] = ident
    vendor = (gpu.get("vendor") or "").lower()

    # ---- fatal blocks (dedupe across sources)
    seen = set()
    blocks = []
    for b in results.fatal:
        k = b.key()
        if k in seen:
            continue
        seen.add(k)
        blocks.append(b)
    inline = []
    if not blocks:
        for b in results.inline_fatal:
            k = b.key()
            if k in seen:
                continue
            seen.add(k)
            inline.append(b)
    fatal = [classify_fatal(b, rules, results) for b in blocks + inline]
    for t in results.terminate:
        if t.kind == "terminate":
            fatal.append(classify_fatal(t, rules, results))
    rep["fatal_blocks"] = fatal
    primary = fatal[0] if fatal else None

    # fault contexts and abort info
    rep["fault_contexts"] = [{k: v for k, v in c.items() if k not in ("lines", "end")} for c in results.fault_contexts[:4]]
    rep["guest_abort"] = results.guest_abort[:2]
    rep["guest_stacks"] = results.guest_stacks[:2]
    rep["modules"] = {n: {"base": "0x%x" % b, "size": "0x%x" % s} for n, (b, s, _p) in list(results.modules.items())[:32]}

    # ---- imports
    lib_files = library_files(repo)
    called = []
    for sym, rec in results.imports_called.items():
        info = parse_symbol(sym)
        info["calls"] = max(rec["calls"].values())
        for k in ("program", "returns", "policy"):
            if rec.get(k) is not None:
                info[k] = rec[k]
        if sym in results.import_names:
            info["name"] = results.import_names[sym]
        info["harmless"] = sym in rules.harmless_symbols or any(
            k.get("harmless") for k in rules.known_for("symbol", sym))
        info["known"] = rules.known_for("symbol", sym)
        info["first"] = rec["first"].as_dict()
        info["last"] = rec["last"].as_dict()
        called.append(info)
    called.sort(key=lambda x: (x["harmless"], -x["calls"], x["symbol"]))
    by_lib = collections.OrderedDict()
    for c in called:
        e = by_lib.setdefault(c["library"], {"library": c["library"], "module": c["module"], "symbols": 0, "calls": 0,
                                             "harmless": True})
        e["symbols"] += 1
        e["calls"] += c["calls"]
        e["harmless"] = e["harmless"] and c["harmless"]
    for e in by_lib.values():
        e["files"] = sorted(lib_files.get(e["library"], set()) | lib_files.get(e["module"], set()))
        e["hint"] = rules.library_hints.get(e["library"]) or rules.library_hints.get(e["module"])
    load = collections.Counter()
    for sym in results.imports_load:
        load[parse_symbol(sym)["library"]] += 1
    imports = collections.OrderedDict()
    imports["called"] = called[:200]
    imports["called_total"] = len(called)
    imports["by_library"] = sorted(by_lib.values(), key=lambda e: (e["harmless"], -e["symbols"], -e["calls"]))
    imports["load_time_unresolved"] = {"total": len(results.imports_load), "by_library": dict(load.most_common())}
    if results.import_summary:
        s = dict(results.import_summary)
        s["evidence"] = s["evidence"].as_dict()
        imports["summary"] = s
    imports["objects"] = [dict(parse_symbol(k), program=v["program"], page=v["page"], first=v["first"].as_dict())
                          for k, v in results.import_objects.items()][:100]
    imports["rebinds"] = results.rebinds
    imports["nid_fallbacks"] = results.nid_fallbacks
    if results.strict_import:
        si = dict(results.strict_import)
        si["evidence"] = si["evidence"].as_dict()
        imports["strict_exit"] = si
    rep["imports"] = imports

    # ---- shaders
    su = []
    for key, rec in results.shader_unsupported.items():
        e = dict(rec["fields"])
        e["key"] = key
        e["count"] = rec["count"]
        e["skipped"] = rec["skipped"]
        e["fatal"] = rec["fatal"]
        shaders = []
        for h, s in rec["shaders"].items():
            shaders.append({"hash": h, "pc": s.get("pc"), "stage": s.get("stage") or results.stage_by_hash.get(h)})
        e["shaders"] = shaders
        e["first"] = rec["first"].as_dict()
        su.append(e)
    for f in fatal:
        si = f.get("shader_instruction")
        if si or (f.get("rule_id") or "").startswith("shader-opcode"):
            key = "%s:%s" % (si["family"], si["opcode"]) if si else "other:" + normalize(f["message"])[:120]
            ex = next((e for e in su if e["key"] == key), None)
            if ex is None:
                ex = dict(si or {"family": None, "opcode": None, "encoding": None, "reason": f["message"]})
                ex.update({"key": key, "count": 0, "skipped": 0, "fatal": 0, "shaders": [],
                           "first": {"source": f["source"], "line": f["lineno"], "text": f["message"][:300]}})
                su.append(ex)
            ex["fatal"] += 1
            ex["count"] += 1
            if f.get("shader"):
                ex["shaders"].append({"hash": f["shader"]["hash"], "pc": None, "stage": f["shader"]["stage"]})
    rep["shader_unsupported"] = su
    srt = []
    for rec in results.srt.values():
        e = {k: rec[k] for k in ("hash", "stage", "pc", "why", "count", "fatal")}
        e["known"] = rules.known_for("hash", rec["hash"])
        e["first"] = rec["first"].as_dict()
        srt.append(e)
    for f in fatal:
        rt = f.get("resource_tracking")
        if rt:
            ex = next((e for e in srt if e["hash"] == rt["hash"] and e["pc"] == rt["pc"]), None)
            if ex is None:
                ex = dict(rt)
                ex.update({"count": 0, "known": rules.known_for("hash", rt["hash"]),
                           "first": {"source": f["source"], "line": f["lineno"], "text": f["message"][:300]}})
                srt.append(ex)
            ex["fatal"] = True
            ex["count"] = max(ex["count"], 1)
    rep["shader_resource_tracking"] = srt

    # ---- findings from line rules
    findings = []
    for rule_id, per in results.findings.items():
        rule = rules.rule(rule_id)
        for key, agg in per.items():
            findings.append({"rule_id": rule_id, "category": rule.category, "title": rule.title, "key": key,
                             "count": agg.count, "fields": agg.fields, "severity": rule.severity,
                             "examples": [e.as_dict() for e in agg.examples],
                             "last": agg.last.as_dict() if agg.last else None,
                             "known": [k for f, v in agg.fields.items() for k in rules.known_for(f, v)]})
    for rule_id, n in results.finding_overflow.items():
        rule = rules.rule(rule_id)
        findings.append({"rule_id": rule_id, "category": rule.category, "title": rule.title + " (more keys)",
                         "key": "(overflow)", "count": n, "fields": {}, "severity": rule.severity, "examples": [],
                         "last": None, "known": []})
    rep["findings"] = findings
    gpu_cats = {"DEVICE_LOST", "VALIDATION", "VRAM_OOM", "VRAM_SPILL", "VRAM_PRESSURE", "PIPELINE_CREATION",
                "DESCRIPTOR_POOL", "SPIRV", "DEVICE_SELECTION", "GPU_PAGE_FAULT", "SHADER_COMPILER_DRIVER"}
    vk = []
    for (sev, _k), agg in results.vk_messages.items():
        vk.append({"severity": sev, "vuid": agg.fields.get("vuid"), "count": agg.count,
                   "known": rules.known_for("vuid", agg.fields.get("vuid")),
                   "examples": [e.as_dict() for e in agg.examples]})
    rep["gpu_errors"] = {"findings": [f for f in findings if f["category"] in gpu_cats],
                         "fatal": [f for f in fatal if f["category"] in gpu_cats],
                         "vulkan_messages": vk}

    # ---- workarounds
    hacks = []
    for name, rec in results.hacks.items():
        total = sum(max(1, k["total"]) for k in rec["keys"].values())
        sites = collections.Counter(k["site"] for k in rec["keys"].values())
        hacks.append({"hack": name, "keys": len(rec["keys"]), "applications": total,
                      "sites": [s for s, _ in sites.most_common(6)],
                      "examples": [k["evidence"].as_dict() for k in list(rec["keys"].values())[:3]]})
    hacks.sort(key=lambda h: (-h["applications"], h["hack"]))
    other = [{"text": a.fields["text"], "count": a.count, "first": a.examples[0].as_dict()}
             for a in results.hack_other.values()]
    other.sort(key=lambda h: -h["count"])
    rep["workarounds"] = {"hacks": hacks, "other": other[:40], "summaries": results.hack_summaries}
    rep["soft_fails"] = [{"api": a.fields["api"], "message": a.fields["message"], "count": a.count,
                          "first": a.examples[0].as_dict()} for a in results.soft_fails.values()]
    rep["guest_syscalls"] = [dict(s, evidence=s["evidence"].as_dict()) for s in results.syscalls]

    # ---- progress / outcome
    p = results.progress
    prog = collections.OrderedDict()
    prog["initialized"] = p["initialized"]
    prog["last_initialized"] = p["initialized"][-1] if p["initialized"] else None
    prog["title_id_seen"] = "title_id" in ident
    prog["execute_main"] = p["execute_main"]
    prog["start_modules"] = p["start_modules"]
    prog["shaders"] = p["shaders"]
    prog["flips"] = max(p["flips"], p["flip_done"])
    prog["bda"] = p["bda"]
    prog["relaunched"] = p["relaunched"]
    prog["fps"] = p["fps"]["fps"] if p["fps"] else None
    prog["pipeline"] = p["pipeline"].as_dict() if p["pipeline"] else None
    prog["shader_cache"] = p["shader_cache"].as_dict() if p["shader_cache"] else None
    prog["last_trace"] = p["last_trace"].as_dict() if p["last_trace"] else None
    prog["clean_exit_markers"] = [e.text for e in p["clean_exit"][-4:]]
    prog["markers"] = [e.as_dict() for e in p["markers"]]
    if results.title_profile:
        prog["title_profile"] = "%s (%s)" % (results.title_profile["key"], results.title_profile["display"])
    if results.presets:
        prog["presets"] = [x["name"] for x in results.presets]
    rep["progress"] = prog

    truncated = any(s["truncated_last_line"] for s in results.sources)
    if primary:
        outcome = "crash"
    elif p["clean_exit"]:
        outcome = "clean-exit"
    elif truncated:
        outcome = "truncated"
    else:
        outcome = "no-fatal"
    rep["outcome"] = outcome
    ec = None
    if exit_code is not None:
        ec = {"value": exit_code, "basis": "--exit-code"}
    elif results.exit_code_lines:
        v, e = results.exit_code_lines[-1]
        ec = {"value": v, "basis": "log line", "evidence": e.as_dict()}
    elif primary:
        if primary["kind"] == "pagemanager":
            ec = {"value": 322, "basis": "inferred (PageManager fatal)"}
        elif primary["kind"] == "terminate":
            ec = {"value": None, "basis": "inferred: std::terminate -> abort (3 or 0xC0000409 on Windows, 134 on Linux)"}
        else:
            ec = {"value": 321, "basis": "inferred (DbgReport)"}
    elif outcome == "clean-exit":
        ec = {"value": 0, "basis": "inferred (clean exit markers)"}
    if ec and ec.get("value") is not None:
        ec["meaning"] = EXIT_CODES.get(ec["value"], "unknown")
    rep["exit_code"] = ec

    if primary:
        rep["primary"] = {k: primary.get(k) for k in ("category", "rule_id", "title", "message", "file_basename",
                                                      "line", "src_path", "signature", "source", "lineno")}
        rep["primary"]["crash_site"] = ("%s:%s" % (primary["file_basename"], primary["line"])
                                        if primary.get("file_basename") else None)
        rr = rules.rule(primary["rule_id"]) if primary.get("rule_id") else None
        rep["primary"]["rule_title"] = rr.title if rr else None
        if primary.get("host_exception"):
            rep["primary"]["host_exception"] = primary["host_exception"]
    else:
        rep["primary"] = {"category": {"clean-exit": "CLEAN_EXIT", "truncated": "NO_FATAL_TRUNCATED"}.get(
            outcome, "NO_FATAL"), "rule_id": None, "crash_site": None, "message": None, "signature": outcome}

    rep["fix_areas"] = rank_fix_areas(rep, rules, vendor)
    rep["rules_version"] = rules.version
    return rep


def _score_count(n):
    return min(30.0, 6.0 * math.log2(1 + n))


def rank_fix_areas(rep, rules, vendor):
    areas = collections.OrderedDict()

    def add(rule_id, score, evidence, why, extra_paths=(), extra_known=(), next_step=None, append_paths=()):
        rule = rules.rule(rule_id)
        if rule is None:
            return
        if rule.vendor and vendor and rule.vendor.lower() != vendor:
            score -= 15
        key = rule.group or rule_id
        a = areas.get(key)
        if a is None:
            a = areas[key] = {"rule_id": rule_id, "category": rule.category, "title": rule.title,
                              "paths": list(rule.fix_area), "next_step": next_step or rule.next_step,
                              "known": list(rule.known), "score": score, "why": [], "evidence": []}
        else:
            if score > a["score"]:
                a["rule_id"], a["title"], a["category"] = rule_id, rule.title, rule.category
            a["score"] = max(a["score"], score) + 1
            for p in rule.fix_area:
                if p not in a["paths"]:
                    a["paths"].append(p)
            for k in rule.known:
                if k not in a["known"]:
                    a["known"].append(k)
        for p in reversed(list(extra_paths)):
            if p in a["paths"]:
                a["paths"].remove(p)
            a["paths"].insert(0, p)
        for p in append_paths:
            if p and p not in a["paths"]:
                a["paths"].append(p)
        for k in extra_known:
            if k not in a["known"]:
                a["known"].insert(0, k)
        if why and why not in a["why"] and len(a["why"]) < 6:
            a["why"].append(why)
        if evidence and len(a["evidence"]) < 6:
            a["evidence"].append(evidence)

    def known_text(k):
        refs = k.get("refs") or []
        return "%s: %s" % (", ".join(refs), k.get("note", "")) if refs else k.get("note", "")

    fatal = rep["fatal_blocks"]
    primary_cat = fatal[0]["category"] if fatal else None
    for i, f in enumerate(fatal):
        base = 1000 if i == 0 else 400 - i
        ev = {"source": f["source"], "line": f["lineno"], "text": f["message"].split("\n")[0][:300]}
        why = ("primary fatal: " if i == 0 else "later fatal: ") + "%s%s" % (
            f["message"].split("\n")[0][:120], " (%s:%s)" % (f["file_basename"], f["line"]) if f["file_basename"] else "")
        rule = rules.rule(f["rule_id"])
        own = [f["src_path"]] if f.get("src_path") else []
        generic = rule is None or not rule.fix_area or all(p.endswith("/") for p in rule.fix_area)
        add(f["rule_id"], base + (rule.severity if rule else 0), ev, why,
            extra_paths=own if generic else (), append_paths=() if generic else own,
            extra_known=[known_text(k) for k in f.get("known", [])])

    imp = rep["imports"]
    real = [c for c in imp["called"] if not c["harmless"]]
    if real or imp.get("strict_exit") or imp["objects"]:
        n = len(real) + len(imp["objects"])
        score = rules.rule("unresolved-import-called").severity * 4 + _score_count(n)
        guest_crash = primary_cat in ("HOST_FAULT_GUEST", "HOST_FAULT_BAD_JUMP", "GUEST_ABORT", "UNCAUGHT_EXCEPTION")
        why = "%d unresolved import(s) called at run time: %s" % (
            n, ", ".join("%s (%s)" % (c["library"], c["symbol"][:11]) for c in real[:5]))
        # A guest crash soon after a stub returned is the usual tell (unless the crash is a known one).
        lines = {s["name"]: s["lines"] for s in rep["sources"]}
        recent = False
        for c in real:
            last = c.get("last") or {}
            total = lines.get(last.get("source")) or 0
            ln = last.get("line") or 0
            if ln < 0 or (total and (total - ln < 5000 or ln >= 0.9 * total)):
                recent = True
        if fatal and fatal[0].get("known"):
            recent = False
        if guest_crash and real and recent:
            score = 1000 + rules.rule(fatal[0]["rule_id"]).severity + 1
            why += "; boosted: the guest crashed after these stubs returned"
        paths = []
        missing = []
        for e in imp["by_library"]:
            if e["harmless"]:
                continue
            for p in e["files"][:2]:
                if p not in paths:
                    paths.append(p)
            if not e["files"]:
                missing.append(e["library"])
        if missing:
            why += "; no src/libs file registers %s yet (new LIB_VERSION block)" % ", ".join(missing[:6])
        ev = real[0]["first"] if real else (imp["objects"][0]["first"] if imp["objects"] else None)
        add("unresolved-import-called", score, ev, why, extra_paths=paths[:8])
    if imp.get("summary") and not real:
        s = imp["summary"]
        if s["functions"] + s["objects"]:
            add("unresolved-import-summary", rules.rule("unresolved-import-summary").severity * 4, s["evidence"],
                "%d functions and %d objects unresolved at load" % (s["functions"], s["objects"]))

    for e in rep["shader_unsupported"]:
        rid = "shader-opcode-skip" if e["skipped"] else "shader-opcode"
        rule = rules.rule(rid)
        score = rule.severity * 4 + _score_count(len(e["shaders"]) or e["count"])
        what = "%s %s" % (e["family"], e["opcode"]) if e.get("family") else (e.get("reason") or "")[:80]
        add(rid, score, e["first"], "unsupported shader instruction %s in %d shader(s)" % (what, len(e["shaders"]) or 1),
            extra_paths=family_paths(e.get("family")))
    for e in rep["shader_resource_tracking"]:
        if e.get("fatal"):
            continue
        rule = rules.rule("shader-resource-tracking")
        add("shader-resource-tracking", rule.severity * 4 + _score_count(e["count"]), e["first"],
            "resource tracking failed for %s %s pc %s" % (e["stage"], e["hash"], e["pc"]),
            extra_known=[known_text(k) for k in e.get("known", [])])
    for f in rep["findings"]:
        if f["rule_id"].startswith("progress-") or f["severity"] <= 0:
            continue
        ex = f["examples"][0] if f["examples"] else None
        add(f["rule_id"], f["severity"] * 4 + _score_count(f["count"]), ex,
            "%s x%d" % (f["title"], f["count"]), extra_known=[known_text(k) for k in f.get("known", [])])
    for v in rep["gpu_errors"]["vulkan_messages"]:
        if v["severity"] in ("E", "W"):
            rid = "vulkan-validation-error" if v["severity"] == "E" else "vulkan-validation-warning"
            rule = rules.rule(rid)
            add(rid, rule.severity * 4 + _score_count(v["count"]), v["examples"][0] if v["examples"] else None,
                "%s %s x%d" % (v["severity"], v["vuid"] or "(no VUID)", v["count"]),
                extra_known=[known_text(k) for k in v.get("known", [])])
    for h in rep["workarounds"]["hacks"]:
        if h["hack"] == "skip unsupported shader instruction":
            continue
        add("hack-notice", rules.rule("hack-notice").severity * 4 + _score_count(h["applications"]),
            h["examples"][0] if h["examples"] else None, "[HACK] %s (%d sources)" % (h["hack"], h["keys"]))
    for s in rep["soft_fails"]:
        add("soft-fail", rules.rule("soft-fail").severity * 4 + _score_count(s["count"]), s["first"],
            "Soft-fail in %s" % s["api"])
    for s in rep["guest_syscalls"]:
        add("guest-syscall", rules.rule("guest-syscall").severity * 4, s["evidence"],
            "%d raw syscalls in %s" % (s["count"], s["module"]))

    if not fatal:
        prog = rep["progress"]
        outcome = rep["outcome"]
        if outcome in ("no-fatal", "truncated"):
            sc = prog.get("shader_cache")
            last = prog["markers"][-1]["text"] if prog["markers"] else ""
            if sc and vendor == "nvidia" and last.startswith("Shader cache: compiling"):
                add("nvidia-shader-cache-warmup", 900, sc, "the log ends during the shader cache warm-up on NVIDIA")
            add("no-fatal-" + outcome, rules.rule("no-fatal-" + outcome).severity * 4,
                prog["markers"][-1] if prog["markers"] else None,
                "no fatal report; the log %s" % ("ends mid-line" if outcome == "truncated" else "just stops"))
    out = sorted(areas.values(), key=lambda a: -a["score"])
    for i, a in enumerate(out, 1):
        a["rank"] = i
        a["score"] = round(a["score"], 1)
    return out


def family_paths(family):
    if not family:
        return []
    d = "src/graphics/shader/recompiler/frontend/decode/"
    fam = family.upper()
    if fam.startswith(("SOP", "SMEM", "SMRD")):
        return [d + "ScalarAluOps.cpp"]
    if fam.startswith(("VOP", "VINTRP")):
        return [d + "VectorAluOps.cpp"]
    if fam.startswith(("MIMG",)):
        return [d + "ImageOps.cpp"]
    if fam.startswith(("DS", "MUBUF", "MTBUF", "FLAT", "GLOBAL", "SCRATCH")):
        return [d + "MemoryOps.cpp"]
    if fam.startswith("EXP"):
        return [d + "ExportOps.cpp"]
    return []


# ---------------------------------------------------------------------------
# Running

def triage(paths, rules=None, quick=False, repo=None, exit_code=None, stdin=None):
    rules = rules or RuleSet()
    results = Results()
    scanner = Scanner(rules, results)
    for src in expand_inputs(paths, stdin=stdin):
        scanner.scan(src, quick=quick)
    if repo is None:
        repo = default_repo()
    rep = analyze(results, rules, repo=repo, exit_code=exit_code)
    rep["_game_roots"] = sorted(results.game_roots)
    return rep


def load_report(path, rules=None, quick=False, repo=None):
    """A saved --json report or a log to triage."""
    if path.endswith(".json"):
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict) and data.get("tool") == TOOL:
            return data
    return triage([path], rules=rules, quick=quick, repo=repo)


# ---------------------------------------------------------------------------
# Output

def _md_escape(text):
    return (text or "").replace("|", "\\|").replace("\n", " ")


def _fmt_ev(ev):
    if not ev:
        return ""
    return "%s:%s" % (os.path.basename(ev["source"]) if ev.get("source") else "?", ev.get("line"))


def _ident_rows(rep):
    ident = rep["identity"]
    gpu = ident.get("gpu", {})
    host = ident.get("host", {})
    rows = [("Build", ident.get("build_label")),
            ("Title", " ".join(x for x in (ident.get("title_id"), ident.get("app_ver"),
                                            ("\"%s\"" % ident["title"]) if ident.get("title") else None) if x)),
            ("Session", ident.get("session_start"))]
    g = gpu.get("name")
    if g:
        extra = []
        if gpu.get("vendor"):
            extra.append(gpu["vendor"])
        if gpu.get("driver"):
            extra.append("driver %s" % gpu["driver"])
        if gpu.get("driver_id"):
            extra.append(gpu["driver_id"])
        if gpu.get("vram_mib"):
            extra.append("%d MiB VRAM" % gpu["vram_mib"])
        if gpu.get("api"):
            extra.append("Vulkan %s" % gpu["api"])
        rows.append(("GPU", "%s (%s)%s" % (g, ", ".join(extra), "" if gpu.get("source") == "GPU line"
                                           else " [from %s]" % gpu.get("source"))))
    else:
        rows.append(("GPU", "not in the log (no `GPU:` or `Select device:` line)"))
    if host:
        rows.append(("Host", ", ".join("%s=%s" % (k, host[k]) for k in ("os", "cpu", "threads", "ram_mib") if host.get(k))))
    tp = ident.get("title_profile")
    if tp:
        rows.append(("Title profile", "%s (%s) quirks=%s preset=%s" % (tp["key"], tp["display"],
                                                                      ",".join(tp["quirks"]) or "none", tp["preset"])))
    for p in ident.get("presets", []):
        rows.append(("Preset", "%s -> %s" % (p["name"], p["tokens"])))
    if ident.get("vram_config"):
        rows.append(("VRAM config", ident["vram_config"]["line"][6:]))
    return [(k, v) for k, v in rows if v]


def render_markdown(rep, top=10):
    out = []
    ident = rep["identity"]
    prim = rep["primary"]
    title = ident.get("title_id") or "unknown title"
    out.append("# KytyPS5 triage: %s, %s" % (title, rep["outcome"]))
    out.append("")
    if prim.get("message"):
        out.append("**Primary failure:** `%s` %s%s" % (prim["category"], _md_escape(prim["message"].split("\n")[0][:300]),
                                                      " (`%s`)" % prim["crash_site"] if prim.get("crash_site") else ""))
    else:
        out.append("**Primary failure:** `%s` (no fatal report in the log)" % prim["category"])
    if rep.get("exit_code"):
        ec = rep["exit_code"]
        out.append("")
        out.append("Exit code: %s (%s)%s" % (ec.get("value") if ec.get("value") is not None else "?", ec["basis"],
                                             ", %s" % ec["meaning"] if ec.get("meaning") else ""))
    out.append("")
    out.append("## Identity")
    out.append("")
    out.append("| | |")
    out.append("|---|---|")
    for k, v in _ident_rows(rep):
        out.append("| %s | %s |" % (k, _md_escape(str(v))))
    out.append("")

    out.append("## Crash site")
    out.append("")
    if not rep["fatal_blocks"]:
        out.append("No fatal report. Outcome `%s`: %s" % (rep["outcome"], {
            "clean-exit": "the window was closed / the emulator quit normally.",
            "truncated": "the last line is cut off: the process was killed, hung while writing, or is still running.",
            "no-fatal": "the log stops without a report: hang, kill, or a crash outside the handler "
                        "(check the console and Windows Event Viewer)."}.get(rep["outcome"], "")))
    for i, f in enumerate(rep["fatal_blocks"][:4]):
        tag = "Primary" if i == 0 else "Later report %d" % i
        out.append("**%s** (`%s`, %s, %s line %s):" % (tag, f["category"], f["kind"], os.path.basename(f["source"]),
                                                     f["lineno"]))
        out.append("")
        out.append("```")
        if f.get("label"):
            out.append(f["label"])
        if f.get("title"):
            out.append(f["title"])
        out.append(f["message"][:1500])
        if f.get("file"):
            out.append(" in %s:%s" % (f["file"], f["line"]))
        if f.get("shader"):
            out.append("while compiling shader: stage=%s hash=%s" % (f["shader"]["stage"], f["shader"]["hash"]))
        for x in f.get("extra", []):
            out.append(x)
        out.append("```")
        exc = f.get("host_exception")
        if exc:
            out.append("")
            out.append("- %s %s at %s, address %s (%s), code %s" % (exc["type_name"], exc["access_name"], exc["pc"],
                                                                   exc["address"], exc["address_kind"], exc["code_name"]))
            if exc.get("pc_module"):
                out.append("- pc = `%s+%s` (%s)" % (exc["pc_module"], exc["pc_offset"], exc["pc_basis"]))
            if exc.get("thread"):
                out.append("- thread: %s" % exc["thread"])
            if exc.get("driver_pipeline"):
                out.append("- pipeline being created by the driver: %s" % exc["driver_pipeline"])
            if exc.get("stack_modules"):
                out.append("- stack hits: %s" % ", ".join(exc["stack_modules"][:8]))
            if exc.get("guest_address"):
                out.append("- address = `%s` (guest address line)" % exc["guest_address"])
            if exc.get("guest_frames"):
                out.append("- guest stack trace: %s" % ", ".join(exc["guest_frames"][:8]))
            out.append("- class: `%s`" % exc["class"])
        if f.get("stack"):
            out.append("")
            out.append("- legacy host stack trace: %d frames (pre-2026-09 build)" % len(f["stack"]))
        if f.get("device_rejects"):
            for d in f["device_rejects"]:
                out.append("- device rejected: %s: %s" % (d["gpu"], "; ".join(d["reasons"])))
        if f.get("build_origin"):
            out.append("- built by: %s" % f["build_origin"])
        for k in f.get("known", []):
            out.append("- known: %s %s" % (", ".join(k.get("refs", [])), k.get("note", "")))
        out.append("")
    if len(rep["fatal_blocks"]) > 4:
        out.append("(%d more fatal reports)" % (len(rep["fatal_blocks"]) - 4))
        out.append("")

    out.append("## Suggested fix areas")
    out.append("")
    if not rep["fix_areas"]:
        out.append("Nothing to suggest.")
    for a in rep["fix_areas"][:top]:
        out.append("%d. **%s** (`%s`, score %s)" % (a["rank"], a["title"], a["category"], a["score"]))
        if a["paths"]:
            out.append("   - where: %s" % ", ".join("`%s`" % p for p in a["paths"][:6]))
        if a["why"]:
            out.append("   - why: %s" % "; ".join(a["why"][:3]))
        if a["next_step"]:
            out.append("   - next: %s" % a["next_step"])
        if a["known"]:
            out.append("   - known: %s" % "; ".join(a["known"][:4]))
        if a["evidence"]:
            out.append("   - evidence: `%s` (%s)" % (_md_escape(a["evidence"][0].get("text", ""))[:200],
                                                   _fmt_ev(a["evidence"][0])))
    out.append("")

    imp = rep["imports"]
    out.append("## Missing imports")
    out.append("")
    if imp["by_library"]:
        out.append("Called at run time (%d symbols):" % imp["called_total"])
        out.append("")
        out.append("| library | module | symbols | calls | files | note |")
        out.append("|---|---|---|---|---|---|")
        for e in imp["by_library"][:30]:
            out.append("| %s | %s | %d | %d | %s | %s |" % (e["library"], e["module"], e["symbols"], e["calls"],
                                                          ", ".join(e["files"][:2]) or "(none)",
                                                          _md_escape(("harmless; " if e["harmless"] else "") +
                                                                     (e.get("hint") or ""))))
        out.append("")
        for c in imp["called"][:25]:
            extra = []
            if c.get("name"):
                extra.append(c["name"])
            if c.get("policy"):
                extra.append("policy=%s returns=%s" % (c["policy"], c.get("returns")))
            if c["harmless"]:
                extra.append("known harmless")
            out.append("- `%s` x%d%s" % (c["symbol"], c["calls"], " (%s)" % ", ".join(extra) if extra else ""))
        out.append("")
    else:
        out.append("No unresolved import was called (or the log does not carry those lines).")
        out.append("")
    if imp.get("summary"):
        s = imp["summary"]
        out.append("Load-time summary: %d functions, %d objects; %s" % (
            s["functions"], s["objects"], ", ".join("%s=%d" % kv for kv in list(s["by_library"].items())[:12])))
        out.append("")
    lt = imp["load_time_unresolved"]
    if lt["total"]:
        out.append("Unresolved at load (printf log): %d symbols; %s" % (
            lt["total"], ", ".join("%s=%d" % kv for kv in list(lt["by_library"].items())[:12])))
        out.append("")
    if imp["objects"]:
        out.append("Unresolved object imports: %s" % ", ".join("`%s`" % o["symbol"] for o in imp["objects"][:10]))
        out.append("")
    if imp.get("strict_exit"):
        out.append("`--strict-imports` stopped at `%s` from %s" % (imp["strict_exit"]["symbol"],
                                                                 imp["strict_exit"]["program"]))
        out.append("")
    if imp["nid_fallbacks"] or imp["rebinds"]:
        out.append("NID fallbacks: %d, rebinds: %d" % (imp["nid_fallbacks"], imp["rebinds"]))
        out.append("")

    out.append("## Unsupported shader instructions")
    out.append("")
    if rep["shader_unsupported"]:
        out.append("| family | opcode | encoding | reason | shaders (stage) | count | skipped | fatal |")
        out.append("|---|---|---|---|---|---|---|---|")
        for e in rep["shader_unsupported"][:30]:
            sh = ", ".join("%s%s" % (s["hash"], " (%s)" % s["stage"] if s.get("stage") else "") for s in e["shaders"][:3])
            out.append("| %s | %s | %s | %s | %s | %d | %d | %d |" % (e.get("family") or "-", e.get("opcode") or "-",
                                                                     _md_escape(e.get("encoding") or "-"),
                                                                     _md_escape((e.get("reason") or "")[:100]), sh or "-",
                                                                     e["count"], e["skipped"], e["fatal"]))
    else:
        out.append("None.")
    out.append("")
    out.append("## Shader resource tracking")
    out.append("")
    if rep["shader_resource_tracking"]:
        for e in rep["shader_resource_tracking"][:20]:
            known = "; ".join("%s %s" % (", ".join(k.get("refs", [])), k.get("note", "")) for k in e["known"])
            out.append("- %s %s pc %s: %s x%d%s%s" % (e["stage"], e["hash"], e["pc"], e["why"], e["count"],
                                                     " (fatal)" if e["fatal"] else "", " -- known: " + known if known else ""))
    else:
        out.append("None.")
    out.append("")

    out.append("## GPU errors")
    out.append("")
    ge = rep["gpu_errors"]
    any_gpu = False
    for f in ge["fatal"]:
        any_gpu = True
        out.append("- fatal `%s`: %s" % (f["category"], _md_escape(f["message"].split("\n")[0][:200])))
    for f in ge["findings"]:
        any_gpu = True
        ex = f["examples"][0] if f["examples"] else {}
        out.append("- `%s` %s x%d: `%s` (%s)" % (f["category"], f["title"], f["count"], _md_escape(ex.get("text", ""))[:200],
                                                _fmt_ev(ex)))
    for v in ge["vulkan_messages"][:20]:
        any_gpu = True
        ex = v["examples"][0] if v["examples"] else {}
        out.append("- Vulkan [%s] %s x%d: `%s`" % (v["severity"], v["vuid"] or "", v["count"],
                                                   _md_escape(ex.get("text", ""))[:200]))
    if not any_gpu:
        out.append("None.")
    out.append("")

    out.append("## Workarounds ([HACK])")
    out.append("")
    wa = rep["workarounds"]
    if wa["hacks"] or wa["other"]:
        for h in wa["hacks"][:20]:
            out.append("- %s: %d source(s), %d application(s); at %s" % (h["hack"], h["keys"], h["applications"],
                                                                        ", ".join(h["sites"][:3])))
        for h in wa["other"][:10]:
            out.append("- %s x%d" % (_md_escape(h["text"][:160]), h["count"]))
    else:
        out.append("None.")
    out.append("")
    if rep["soft_fails"] or rep["guest_syscalls"]:
        out.append("## HLE soft-fails")
        out.append("")
        for s in rep["soft_fails"][:20]:
            out.append("- %s: %s x%d" % (s["api"], _md_escape(s["message"][:160]), s["count"]))
        for s in rep["guest_syscalls"]:
            out.append("- %d raw syscall(s) in %s (first at +%s)" % (s["count"], s["module"], s["offset"]))
        out.append("")

    out.append("## How far it got")
    out.append("")
    p = rep["progress"]
    bits = []
    if p.get("last_initialized"):
        bits.append("last subsystem initialized: %s" % p["last_initialized"])
    bits.append("title id seen: %s" % ("yes" if p["title_id_seen"] else "no"))
    if p.get("execute_main"):
        bits.append("guest main executed")
    if p.get("shaders"):
        bits.append("shaders: " + " ".join("%s %d" % (k, v) for k, v in p["shaders"].items() if v))
    bits.append("flips: %d" % p["flips"])
    if p.get("fps") is not None:
        bits.append("fps: %s" % p["fps"])
    if p.get("bda"):
        bits.append("BDA enabled")
    if p.get("title_profile"):
        bits.append("title profile: %s" % p["title_profile"])
    if p.get("presets"):
        bits.append("presets: %s" % ", ".join(p["presets"]))
    if p.get("relaunched"):
        bits.append("relaunched %d time(s)" % p["relaunched"])
    for b in bits:
        out.append("- " + b)
    if p.get("last_trace"):
        out.append("- last HLE call traced: `%s`" % p["last_trace"]["text"])
    if p["markers"]:
        out.append("- last markers:")
        for m in p["markers"][-6:]:
            out.append("  - `%s` (%s)" % (_md_escape(m["text"])[:160], _fmt_ev(m)))
    out.append("")
    out.append("## Sources")
    out.append("")
    for s in rep["sources"]:
        out.append("- %s: %s, %s lines, %s%s%s" % (s["name"], s["kind"], s["lines"] or "?",
                                                  "%.1f MiB" % (s["bytes"] / 1048576.0), ", truncated last line"
                                                  if s["truncated_last_line"] else "",
                                                  ", " + s["scanned"] if s["scanned"] != "full" else ""))
    out.append("")
    return "\n".join(out)


def render_agent_prompt(rep, top=6):
    ident = rep["identity"]
    prim = rep["primary"]
    gpu = ident.get("gpu", {})
    lines = []
    lines.append("You are working in the KytyPS5 repository (PS5 emulator; C++20, CMake, Vulkan 1.3).")
    lines.append("A tester ran %s%s on %s%s with build \"%s\"." % (
        ident.get("title_id") or "an unknown title", " %s" % ident["app_ver"] if ident.get("app_ver") else "",
        gpu.get("name") or "an unknown GPU", " (%d MiB VRAM)" % gpu["vram_mib"] if gpu.get("vram_mib") else "",
        ident.get("build_label") or "unknown"))
    lines.append("The run ended with outcome `%s`; primary failure category `%s`." % (rep["outcome"], prim["category"]))
    if prim.get("message"):
        lines.append("Primary failure: %s%s" % (prim["message"].split("\n")[0][:400],
                                                " (at %s)" % prim["crash_site"] if prim.get("crash_site") else ""))
    exc = prim.get("host_exception")
    if exc:
        lines.append("Host exception: %s %s, pc %s = %s+%s (%s), address %s, class %s." % (
            exc["type_name"], exc["access_name"], exc["pc"], exc.get("pc_module"), exc.get("pc_offset"),
            exc.get("pc_basis"), exc["address"], exc["class"]))
        if exc.get("guest_address"):
            lines.append("The faulting address is inside a guest module: %s." % exc["guest_address"])
        if exc.get("guest_frames"):
            lines.append("Guest stack trace: %s." % ", ".join(exc["guest_frames"][:8]))
    lines.append("")
    lines.append("Evidence (verbatim log lines, file:line):")
    seen = set()
    count = 0
    for f in rep["fatal_blocks"][:2]:
        for t in ([f["label"]] if f.get("label") else []) + ([f["title"]] if f.get("title") else []) + \
                f["message"].split("\n")[:12] + ([" in %s:%s" % (f["file"], f["line"])] if f.get("file") else []):
            lines.append("    %s" % t)
        lines.append("    (from %s line %s)" % (os.path.basename(f["source"]), f["lineno"]))
    for a in rep["fix_areas"][:top]:
        for e in a["evidence"][:2]:
            k = (e.get("source"), e.get("line"))
            if k in seen or count >= 24:
                continue
            seen.add(k)
            count += 1
            lines.append("    %s:%s: %s" % (os.path.basename(e.get("source") or "?"), e.get("line"), e.get("text", "")[:300]))
    lines.append("")
    lines.append("Suggested fix areas, most likely first:")
    for a in rep["fix_areas"][:top]:
        lines.append("%d. %s [%s]" % (a["rank"], a["title"], a["category"]))
        if a["paths"]:
            lines.append("   files: %s" % ", ".join(a["paths"][:6]))
        if a["why"]:
            lines.append("   why: %s" % "; ".join(a["why"][:3]))
        if a["next_step"]:
            lines.append("   next step: %s" % a["next_step"])
        if a["known"]:
            lines.append("   related: %s" % "; ".join(a["known"][:4]))
    imp = rep["imports"]
    real = [c for c in imp["called"] if not c["harmless"]]
    if real:
        lines.append("")
        lines.append("Unresolved imports called at run time (implement or stub with a plausible result):")
        for c in real[:20]:
            lines.append("  - %s%s x%d" % (c["symbol"], " (%s)" % c["name"] if c.get("name") else "", c["calls"]))
    if rep["shader_unsupported"]:
        lines.append("")
        lines.append("Unsupported shader instructions:")
        for e in rep["shader_unsupported"][:12]:
            lines.append("  - %s %s raw=[%s] reason=%s; shaders: %s" % (
                e.get("family"), e.get("opcode"), e.get("encoding"), (e.get("reason") or "")[:120],
                ", ".join("%s%s" % (s["hash"], "/" + s["stage"] if s.get("stage") else "") for s in e["shaders"][:4])))
    p = rep["progress"]
    lines.append("")
    lines.append("How far it got: last subsystem %s, guest main %s, shaders %s, flips %d." % (
        p.get("last_initialized") or "?", "ran" if p.get("execute_main") else "not seen",
        " ".join("%s %d" % (k, v) for k, v in (p.get("shaders") or {}).items() if v) or "none", p["flips"]))
    lines.append("")
    lines.append("Rules for the change: GTA 6 cannot be tested locally, so keep the defaults unchanged for games that")
    lines.append("already work. Put new behaviour behind a CLI flag (src/main.cpp FLAGS/ParseArgs + Config::ConfigOptions")
    lines.append("+ getter in src/common/emulatorConfig.h/.cpp), except changes that only turn a fatal EXIT into a")
    lines.append("logged soft failure. Add a focused test that needs no GPU where possible. Builds and tests run in CI.")
    lines.append("Task: find the root cause of the primary failure from the evidence above, fix it in the files listed,")
    lines.append("and say what you could not verify.")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Compare

def signature_sets(rep):
    s = collections.OrderedDict()
    s["fatal"] = {f["signature"]: f["message"].split("\n")[0][:200] for f in rep.get("fatal_blocks", [])}
    s["imports_called"] = {c["symbol"]: "%s x%d" % (c["library"], c["calls"]) for c in rep["imports"]["called"]}
    s["imports_load_by_library"] = {k: str(v) for k, v in rep["imports"]["load_time_unresolved"]["by_library"].items()}
    s["shader_unsupported"] = {e["key"]: (e.get("reason") or "")[:120] for e in rep.get("shader_unsupported", [])}
    s["shader_resource_tracking"] = {"%s@%s" % (e["hash"], e["pc"]): "%s %s" % (e["stage"], e["why"][:100])
                                     for e in rep.get("shader_resource_tracking", [])}
    s["gpu_errors"] = {"%s|%s" % (f["rule_id"], f["key"][:120]): "%s x%d" % (f["title"], f["count"])
                       for f in rep["gpu_errors"]["findings"]}
    s["vuids"] = {"%s %s" % (v["severity"], v["vuid"] or "-"): "x%d" % v["count"]
                  for v in rep["gpu_errors"]["vulkan_messages"]}
    s["hacks"] = {h["hack"]: "%d sources" % h["keys"] for h in rep["workarounds"]["hacks"]}
    s["soft_fails"] = {"%s: %s" % (x["api"], normalize(x["message"])[:80]): "x%d" % x["count"] for x in rep["soft_fails"]}
    s["findings"] = {"%s|%s" % (f["rule_id"], f["key"][:120]): "x%d" % f["count"] for f in rep["findings"]
                     if f["category"] not in ("PROGRESS",)}
    return s


def compare(a, b):
    sa, sb = signature_sets(a), signature_sets(b)
    out = collections.OrderedDict()
    out["tool"] = TOOL
    out["schema"] = SCHEMA
    out["mode"] = "compare"
    out["a"] = {"sources": [s["name"] for s in a["sources"]], "outcome": a["outcome"], "primary": a["primary"],
                "build": a["identity"].get("build_label"), "gpu": a["identity"].get("gpu", {}).get("name")}
    out["b"] = {"sources": [s["name"] for s in b["sources"]], "outcome": b["outcome"], "primary": b["primary"],
                "build": b["identity"].get("build_label"), "gpu": b["identity"].get("gpu", {}).get("name")}
    out["primary_changed"] = a["primary"].get("signature") != b["primary"].get("signature")
    diffs = collections.OrderedDict()
    for k in sa:
        ka, kb = set(sa[k]), set(sb[k])
        diffs[k] = {"new": sorted(kb - ka), "removed": sorted(ka - kb), "common": len(ka & kb),
                    "detail": {x: sb[k].get(x) or sa[k].get(x) for x in sorted((ka ^ kb))[:200]}}
    out["diff"] = diffs
    pa, pb = a["progress"], b["progress"]
    out["progress"] = {"flips": [pa["flips"], pb["flips"]], "shaders": [pa.get("shaders"), pb.get("shaders")],
                       "last_initialized": [pa.get("last_initialized"), pb.get("last_initialized")],
                       "execute_main": [pa.get("execute_main"), pb.get("execute_main")]}
    return out


def render_compare_markdown(c):
    out = ["# KytyPS5 triage: compare", ""]
    out.append("| | A | B |")
    out.append("|---|---|---|")
    out.append("| sources | %s | %s |" % (", ".join(c["a"]["sources"]), ", ".join(c["b"]["sources"])))
    out.append("| build | %s | %s |" % (c["a"]["build"], c["b"]["build"]))
    out.append("| GPU | %s | %s |" % (c["a"]["gpu"], c["b"]["gpu"]))
    out.append("| outcome | %s | %s |" % (c["a"]["outcome"], c["b"]["outcome"]))
    out.append("| primary | %s | %s |" % (_md_escape(c["a"]["primary"].get("signature") or ""),
                                         _md_escape(c["b"]["primary"].get("signature") or "")))
    p = c["progress"]
    out.append("| flips | %s | %s |" % tuple(p["flips"]))
    out.append("| last subsystem | %s | %s |" % tuple(p["last_initialized"]))
    out.append("| shaders | %s | %s |" % tuple(" ".join("%s %d" % (k, v) for k, v in (x or {}).items() if v)
                                               for x in p["shaders"]))
    out.append("")
    out.append("Primary failure %s." % ("changed" if c["primary_changed"] else "unchanged"))
    out.append("")
    for k, d in c["diff"].items():
        if not d["new"] and not d["removed"]:
            continue
        out.append("## %s" % k)
        out.append("")
        for x in d["new"][:40]:
            out.append("- new in B: `%s` %s" % (_md_escape(x), _md_escape(d["detail"].get(x) or "")))
        for x in d["removed"][:40]:
            out.append("- gone in B: `%s` %s" % (_md_escape(x), _md_escape(d["detail"].get(x) or "")))
        out.append("")
    return "\n".join(out) + "\n"


def _strip_private(rep):
    return {k: v for k, v in rep.items() if not k.startswith("_")}


def main(argv=None):
    ap = argparse.ArgumentParser(prog="kyty_triage.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("logs", nargs="*", help="log files (.log/.txt/.gz/.zip), a Kyty directory, or - for stdin")
    ap.add_argument("--json", action="store_true", help="print the report as JSON")
    ap.add_argument("--agent-prompt", action="store_true", help="print a brief for a coding agent")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"), help="diff two runs (logs or saved --json reports)")
    ap.add_argument("--quick", action="store_true", help="large plain files: read only the last and first 16 MiB")
    ap.add_argument("--redact", action="store_true", help="remove user names and game paths from the output")
    ap.add_argument("--rules", default=DEFAULT_RULES, help="rules file (default: rules.json next to this tool)")
    ap.add_argument("--repo", default=None, help="KytyPS5 checkout used to map libraries to files (default: auto)")
    ap.add_argument("--exit-code", type=lambda s: int(s, 0), default=None, help="exit code of the emulator, if known")
    ap.add_argument("--top", type=int, default=10, help="fix areas to show (default 10)")
    ap.add_argument("-o", "--output", default=None, help="write to this file instead of stdout")
    args = ap.parse_args(argv)

    rules = RuleSet(args.rules)
    if args.compare:
        a = load_report(args.compare[0], rules=rules, quick=args.quick, repo=args.repo)
        b = load_report(args.compare[1], rules=rules, quick=args.quick, repo=args.repo)
        roots = set(a.get("_game_roots", [])) | set(b.get("_game_roots", []))
        if args.redact:
            a, b = redact_obj(a, roots), redact_obj(b, roots)
        c = compare(a, b)
        text = json.dumps(c, indent=1) + "\n" if args.json else render_compare_markdown(c)
    else:
        logs = args.logs
        if not logs:
            if sys.stdin.isatty():
                ap.error("give a log file, a directory, or - for stdin")
            logs = ["-"]
        rep = triage(logs, rules=rules, quick=args.quick, repo=args.repo, exit_code=args.exit_code)
        roots = set(rep.get("_game_roots", []))
        if args.redact:
            rep = redact_obj(rep, roots)
        if args.json:
            text = json.dumps(_strip_private(rep), indent=1, default=str) + "\n"
        elif args.agent_prompt:
            text = render_agent_prompt(rep, top=min(args.top, 8))
        else:
            text = render_markdown(rep, top=args.top)
    if args.redact:
        text = redact(text, roots)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
