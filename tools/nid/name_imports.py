#!/usr/bin/env python3
"""Name the NIDs of unresolved imports in a --dump-imports report or in logs, offline.

A --dump-imports report (format 1) comes back in its own format with the '?' of each unresolved
import replaced by the symbol name: the report the emulator writes when it runs with --nid-names
and the same names file. Logs (session logs, printf _kyty.txt logs, console captures, pasted
text; plain, .gz or .zip) give a list of the unresolved imports they mention, grouped by library
like the report, with names.

Names come from a names file as build_names.py writes it, read with the emulator's rules
(nid.py). Standard library only; run it with `python3 -I tools/nid/name_imports.py`.
"""

import argparse
import collections
import gzip
import importlib.util
import io
import json
import os
import re
import sys
import zipfile

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
TOOL = "name_imports.py"


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOL_DIR, filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


nid = _load("kyty_nid", "nid.py")

# src/loader/importDiagnostics.cpp, FormatImportReport.
REPORT_MAGIC = "# Kyty import report, format "
R_REPORT_UNRESOLVED = re.compile(r"^  unresolved (\S+) (\S+) (\S+) (.*?)(\r?)$")

# SymbolDatabase::GenerateName: "<nid>[<Lib>_v<n>][<Mod>_v<a>.<b>][<Type>]".
R_GENERATED = re.compile(r"(?P<nid>[A-Za-z0-9+\-]{11})\[(?P<lib>[^\]\[\s]+)\]\[(?P<mod>[^\]\[\s]+)\]\[(?P<type>\w+)\]")
R_LIB_VERSION = re.compile(r"_v\d+$")

# The unresolved-import lines of src/loader/runtimeLinker.cpp and their older forms
# (tools/triage/README.md lists them):
#   Unresolved import stub called: <gen> (program=<file>, first call, returns=0x<hex>, policy=<p>)
#   Unresolved import stub called [<n>]: ... symbol=<gen> type=<t> bind=<b> program=<path>
#   Unresolved import stub called: <gen>
#   Unresolved object import: <gen> (program=<file>) -> invalid page 0x<hex>
#   Unresolved import name: <gen> = <symbol>
#   Unresolved import called: <gen> from <file>                    (--strict-imports)
#   Stubbed: [...] <- ..., <gen>, <type>, <bind>                    (older printf logs, load time)
#   Relocate: unresolved PLT import patched to stub [...] [...] <- ..., <gen>, <type>, <bind>, <path>
#                                                                   (printf logs, load time)
R_PROGRAM_PAREN = re.compile(r"\(program=([^,)]*)")
R_PROGRAM_FIELD = re.compile(r"\bprogram=(.+?)\s*$")
R_STRICT = re.compile(r"Unresolved import called: \S+ from (.+?)\s*$")
R_NAME_LINE = re.compile(r"Unresolved import name: \S+ = (\S+)")
R_LOAD_TAIL = re.compile(r"^, \w+, \w+, (.+?)\s*$")  # after <gen>: ", <type>, <bind>[, <program>]"

KIND_ORDER = {"called": 0, "object": 1, "load": 2}


def library_of(segment):
    """ImportDiagnostics::LibraryFromGeneratedName for the "<Lib>_v<n>" segment."""
    lib = R_LIB_VERSION.sub("", segment)
    return lib[len("libSce"):] if lib.startswith("libSce") else lib


def basename(path):
    return re.split(r"[\\/]", path.strip())[-1] if path else path


# ---------------------------------------------------------------------------
# Inputs


def open_inputs(path, stdin=None):
    """(display name, binary file with peek()) for each text in the input: a plain file, a .gz,
    every member of a .zip (read in memory, never extracted), or '-' for stdin."""
    if path == "-":
        data = (stdin or sys.stdin.buffer).read()
        yield "<stdin>", io.BufferedReader(io.BytesIO(data))
        return
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic[:2] == b"\x1f\x8b":
        with gzip.open(path, "rb") as f:
            yield os.path.basename(path), f
    elif magic == b"PK\x03\x04":
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if not info.is_dir():
                    with z.open(info) as f:
                        yield "%s:%s" % (os.path.basename(path), info.filename), f
    else:
        with open(path, "rb") as f:
            yield os.path.basename(path), f


def is_report(head):
    """True when the text starts with the --dump-imports report header."""
    for line in head.decode("utf-8", "replace").lstrip("﻿").split("\n"):
        if line.strip():
            return line.startswith(REPORT_MAGIC)
    return False


# ---------------------------------------------------------------------------
# Reports


def name_report(text, table):
    """The report with a name on each unresolved line that has '?' and a known NID.
    Returns (text, Counter of unresolved / named / named_before / differs)."""
    out = []
    stats = collections.Counter()
    for line in text.split("\n"):
        m = R_REPORT_UNRESOLVED.match(line)
        if not m:
            out.append(line)
            continue
        nid_text, _type, _importer, target, cr = m.groups()
        stats["unresolved"] += 1
        name = table.find(nid_text)
        if target != "?":
            stats["named_before"] += 1
            if name is not None and name != target:
                stats["differs"] += 1
            out.append(line)
        elif name is None:
            out.append(line)
        else:
            stats["named"] += 1
            out.append(line[:m.start(4)] + name + cr)
    return "\n".join(out), stats


# ---------------------------------------------------------------------------
# Logs


class Import:
    """One generated import name seen in the logs."""

    def __init__(self, symbol, nid_text, library, typ):
        self.symbol = symbol
        self.nid = nid_text
        self.library = library
        self.type = typ
        self.kinds = set()
        self.calls = 0
        self.programs = []
        self.sources = []

    @property
    def kind(self):
        """called (the stub ran), object (an object import), or load (unresolved at load only)."""
        return min(self.kinds, key=KIND_ORDER.get)

    def add_program(self, program):
        program = basename(program)
        if program and program not in self.programs:
            self.programs.append(program)


class LogScan:
    def __init__(self):
        self.imports = collections.OrderedDict()
        self.log_names = {}  # generated name -> symbol, from "Unresolved import name:" lines
        self.summaries = []

    def _get(self, m):
        symbol = m.group(0)
        imp = self.imports.get(symbol)
        if imp is None:
            imp = self.imports[symbol] = Import(symbol, m.group("nid"), library_of(m.group("lib")), m.group("type"))
        return imp

    def line(self, line, source):
        if line.startswith("Unresolved imports: "):
            self.summaries.append(line.strip())
            return
        m = R_GENERATED.search(line)
        if m is None:
            return
        if "Unresolved import stub called" in line:
            imp = self._get(m)
            imp.kinds.add("called")
            imp.calls += 1
            p = R_PROGRAM_PAREN.search(line) or R_PROGRAM_FIELD.search(line)
        elif "Unresolved import called: " in line:
            imp = self._get(m)
            imp.kinds.add("called")
            p = R_STRICT.search(line)
        elif "Unresolved object import: " in line:
            imp = self._get(m)
            imp.kinds.add("object")
            p = R_PROGRAM_PAREN.search(line)
        elif "Unresolved import name: " in line:
            n = R_NAME_LINE.search(line)
            if n is not None:
                self.log_names[m.group(0)] = n.group(1)
            return
        elif line.startswith(("Stubbed: ", "Relocate: unresolved ")):
            imp = self._get(m)
            imp.kinds.add("load")
            p = R_LOAD_TAIL.match(line[m.end():])
        else:
            return
        if p is not None:
            imp.add_program(p.group(1))
        if source not in imp.sources:
            imp.sources.append(source)

    def scan(self, f, source):
        for raw in f:
            if b"nresolved" not in raw and b"Stubbed: " not in raw:
                continue
            text = raw.decode("utf-8", "replace").rstrip("\r\n")
            # Pasted issue text: quote markers and code fences in front of the line.
            self.line(text.lstrip("> `\t"), source)


def named_imports(scan, table):
    """[(Import, name or None, "log" / "names" / None)], sorted by library, kind and NID."""
    rows = []
    for imp in scan.imports.values():
        if imp.symbol in scan.log_names:
            rows.append((imp, scan.log_names[imp.symbol], "log"))
            continue
        name = table.find(imp.nid)
        rows.append((imp, name, "names" if name else None))
    rows.sort(key=lambda r: (r[0].library, KIND_ORDER[r[0].kind], r[0].nid, r[0].symbol))
    return rows


def hle_entries(imp, hle):
    """The LIB_FUNC/LIB_OBJECT registrations that resolve this import now. The linker falls back to
    any HLE symbol with the NID and the import's type, whatever its library; a NoType import binds
    to either only with --notype-import-fallback."""
    if hle is None:
        return []
    want = {"Func": ("FUNC",), "Object": ("OBJECT",)}.get(imp.type, ("FUNC", "OBJECT"))
    return [e for e in hle.get(imp.nid, []) if e[0] in want]


def render_list(rows, inputs, names_path, table, summaries, hle=None):
    kinds = collections.Counter(imp.kind for imp, _n, _f in rows)
    out = [
        "# Kyty unresolved imports from logs, named by tools/nid/name_imports.py",
        "# inputs: %s" % " ".join(inputs),
        "# names: %s (%d names)" % (os.path.basename(names_path), len(table)),
        "# totals: unresolved=%d named=%d called=%d object=%d load=%d" % (
            len(rows), sum(1 for _i, n, _f in rows if n), kinds["called"], kinds["object"], kinds["load"]),
    ]
    if hle is not None:
        out.append("# hle: %d of them registered by a LIB_FUNC/LIB_OBJECT in this checkout now" % (
            sum(1 for imp, _n, _f in rows if hle_entries(imp, hle))))
    for s in summaries[:4]:
        out.append("# log: %s" % s)
    by_lib = collections.OrderedDict()
    for row in rows:
        by_lib.setdefault(row[0].library, []).append(row)
    for lib, lib_rows in by_lib.items():
        out.append("")
        out.append("library %s: unresolved=%d named=%d" % (
            lib or "?", len(lib_rows), sum(1 for _i, n, _f in lib_rows if n)))
        for imp, name, _from in lib_rows:
            line = "  %s %s %s %s %s" % (imp.kind, imp.nid, imp.type, ",".join(imp.programs) or "?", name or "?")
            regs = hle_entries(imp, hle)
            if regs:
                line += " (hle now: %s:%d %s)" % (regs[0][1], regs[0][2], regs[0][3])
            out.append(line)
    return "\n".join(out) + "\n"


def render_json(rows, inputs, names_path, table, summaries, hle=None):
    data = collections.OrderedDict()
    data["tool"] = TOOL
    data["inputs"] = inputs
    data["names_file"] = names_path
    data["names"] = len(table)
    data["summaries"] = summaries
    data["imports"] = [collections.OrderedDict([
        ("symbol", imp.symbol), ("nid", imp.nid), ("library", imp.library), ("type", imp.type),
        ("kind", imp.kind), ("kinds", sorted(imp.kinds, key=KIND_ORDER.get)), ("calls", imp.calls),
        ("programs", imp.programs), ("name", name), ("name_from", name_from), ("sources", imp.sources),
    ] + ([("hle", [{"kind": k, "file": f, "line": ln, "function": fn} for k, f, ln, fn in hle_entries(imp, hle)])]
         if hle is not None else [])) for imp, name, name_from in rows]
    return json.dumps(data, indent=1) + "\n"


# ---------------------------------------------------------------------------


def default_names_file():
    build_names = _load("kyty_build_names", "build_names.py")
    return os.path.join(build_names.default_cache(), build_names.OUTPUT_NAME)


def write_text(path, text):
    data = text.encode("utf-8", "surrogateescape")
    if path:
        with open(path, "wb") as f:
            f.write(data)
    elif hasattr(sys.stdout, "buffer"):
        sys.stdout.flush()
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    else:
        sys.stdout.write(text)


def main(argv=None, stdin=None):
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    ap.add_argument("inputs", nargs="+", help="one --dump-imports report, or logs (.log/.txt/.gz/.zip, - for stdin)")
    ap.add_argument("--names", metavar="FILE", help="names file (default: the one build_names.py writes to its cache)")
    ap.add_argument("--json", action="store_true", help="logs: print the list as JSON")
    ap.add_argument("--hle", action="store_true",
                    help="logs: mark the imports a LIB_FUNC/LIB_OBJECT in the checkout's src/libs registers now")
    ap.add_argument("--repo", metavar="DIR", help="KytyPS5 checkout for --hle (default: the one holding this tool)")
    ap.add_argument("-o", "--output", help="write here instead of stdout")
    args = ap.parse_args(argv)

    names_path = args.names or default_names_file()
    if not os.path.isfile(names_path):
        ap.error("no names file at %s: run tools/nid/build_names.py or pass --names" % names_path)
    table = nid.NameTable()
    table.load_file(names_path)

    scan = LogScan()
    inputs = []
    try:
        for path in args.inputs:
            for display, f in open_inputs(path, stdin=stdin):
                if is_report(f.peek(4096)[:4096]):
                    if len(args.inputs) != 1 or inputs:
                        ap.error("give a --dump-imports report on its own, not with other inputs (%s)" % display)
                    return name_report_file(display, f.read(), table, args.output)
                inputs.append(display)
                scan.scan(f, display)
    except (OSError, zipfile.BadZipFile, EOFError) as e:
        ap.error(str(e))

    hle = None
    if args.hle:
        build_names = _load("kyty_build_names", "build_names.py")
        repo = args.repo or build_names.default_repo()
        if not repo or not os.path.isdir(os.path.join(repo, "src", "libs")):
            ap.error("--hle: no KytyPS5 checkout with src/libs (use --repo)")
        hle = build_names.hle_nids(repo)
    rows = named_imports(scan, table)
    render = render_json if args.json else render_list
    write_text(args.output, render(rows, inputs, names_path, table, scan.summaries, hle=hle))
    return 0


def name_report_file(display, data, table, output):
    text = data.decode("utf-8", "surrogateescape")
    first = text.lstrip("﻿").split("\n", 1)[0].rstrip("\r")
    if first != REPORT_MAGIC + "1":
        print("%s: warning: %s is not report format 1; naming its '  unresolved ...' lines anyway" % (
            TOOL, display), file=sys.stderr)
    named_text, stats = name_report(text, table)
    write_text(output, named_text)
    print("%s: %s: named %d of %d unresolved imports (%d had a name already%s)" % (
        TOOL, display, stats["named"], stats["unresolved"], stats["named_before"],
        ", %d of them differ from the names file" % stats["differs"] if stats["differs"] else ""),
        file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
