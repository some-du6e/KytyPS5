#!/usr/bin/env python3
"""Trim a raw KytyPS5 log into a small, redacted test fixture.

Keeps the first --head lines, every line a rule or built-in extractor of
kyty_triage.py recognizes (at most --per-key lines per kind), the lines of every
fatal report / fault context, and the last --tail lines. User names and game
install folders are redacted. The original bytes of kept lines are preserved
otherwise (CRLF, invalid UTF-8, a missing final newline).

Prints the INDEX.json entry; with --index it is also written into that file.

    python3 -I tools/triage/make_fixture.py RAW --case NAME --issue URL \\
        [--member NAME] [--index tools/triage/tests/fixtures/INDEX.json]

The raw file is only read; nothing in it is executed or extracted to disk.
"""

import argparse
import collections
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kyty_triage as kt  # noqa: E402

CONTEXT_PREFIXES = (b"--- Build ---", b"--- Error ---", b"--- Fatal Error ---", b"--- Stack Trace ---",
                    b"--- Guest fault context ---", b"Guest abort diagnostics:", b"Stack trace [thread = ",
                    b"Could not find suitable device:", b"--- Session ", b"--- Relaunched ", b"Build\n", b"Build\r\n",
                    b"Host\n", b"Host\r\n")
CONTEXT_LINES = 48
GAP = b"[... %d lines omitted by make_fixture.py ...]\n"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def select(source, scanner, head, tail, per_key):
    keep = {}                     # lineno -> raw
    per = collections.Counter()
    tail_q = collections.deque(maxlen=tail)
    ctx = 0
    total = 0
    last_raw = b""
    f = source.open()
    try:
        for lineno, raw in enumerate(f, 1):
            total = lineno
            last_raw = raw
            if lineno <= head:
                keep[lineno] = raw
            tail_q.append((lineno, raw))
            if ctx > 0:
                keep[lineno] = raw
                ctx -= 1
            st = raw.lstrip(b" \t>`")
            if st.startswith(CONTEXT_PREFIXES):
                keep[lineno] = raw
                ctx = CONTEXT_LINES
                continue
            if scanner.interesting(st) or scanner.interesting(raw):
                key = kt.normalize(st[:48].decode("utf-8", "replace"))[:24]
                per[key] += 1
                if per[key] <= per_key:
                    keep[lineno] = raw
    finally:
        f.close()
    for lineno, raw in tail_q:
        keep[lineno] = raw
    return keep, total, last_raw.endswith(b"\n")


def render(keep, total, ends_with_newline, roots, max_line):
    out = []
    prev = 0
    for lineno in sorted(keep):
        if lineno > prev + 1:
            out.append(GAP % (lineno - prev - 1))
        raw = keep[lineno]
        nl = b"\r\n" if raw.endswith(b"\r\n") else (b"\n" if raw.endswith(b"\n") else b"")
        body = raw[:len(raw) - len(nl)]
        if len(body) > max_line:
            body = body[:max_line]
        text = body.decode("utf-8", "surrogateescape")
        text = kt.redact(text, roots)
        out.append(text.encode("utf-8", "surrogateescape") + nl)
        prev = lineno
    if prev < total:
        out.append(GAP % (total - prev))
    data = b"".join(out)
    if not ends_with_newline and data.endswith(b"\n") and prev == total:
        data = data[:-1]
    return data


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("raw")
    ap.add_argument("--member", help="zip member to use (default: the first text member)")
    ap.add_argument("--case", required=True, help="fixture name (tests/fixtures/<case>.txt)")
    ap.add_argument("--issue", default="", help="issue URL the raw log came from")
    ap.add_argument("--source-url", default="", help="download URL of the raw file")
    ap.add_argument("--original-name", default=None, help="original file name (default: basename of RAW)")
    ap.add_argument("--raw-sha256", default=None, help="sha256 of the uncompressed log if RAW is a .gz of it")
    ap.add_argument("--note", default="")
    ap.add_argument("--head", type=int, default=200)
    ap.add_argument("--tail", type=int, default=300)
    ap.add_argument("--per-key", type=int, default=40)
    ap.add_argument("--max-bytes", type=int, default=190000)
    ap.add_argument("--max-line", type=int, default=1200)
    ap.add_argument("--out-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures"))
    ap.add_argument("--index", default=None, help="INDEX.json to add the entry to")
    args = ap.parse_args(argv)

    sources = kt.expand_inputs([args.raw])
    if args.member:
        sources = [s for s in sources if s.name.endswith(":" + args.member)]
    if not sources:
        ap.error("no readable log in %s" % args.raw)
    source = sources[0]
    scanner = kt.Scanner(kt.RuleSet(), kt.Results())

    head, tail, per_key = args.head, args.tail, args.per_key
    while True:
        keep, total, ends_nl = select(source, scanner, head, tail, per_key)
        roots = set()
        for raw in keep.values():
            roots |= kt.find_game_roots(raw.decode("utf-8", "replace"))
        data = render(keep, total, ends_nl, roots, args.max_line)
        if len(data) <= args.max_bytes or (head <= 20 and tail <= 40 and per_key <= 4):
            break
        head, tail, per_key = max(20, head * 2 // 3), max(40, tail * 2 // 3), max(4, per_key * 2 // 3)

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, args.case + ".txt")
    with open(out_path, "wb") as f:
        f.write(data)

    entry = collections.OrderedDict()
    entry["case"] = args.case
    entry["file"] = args.case + ".txt"
    entry["synthetic"] = False
    entry["issue"] = args.issue
    if args.source_url:
        entry["source_url"] = args.source_url
    entry["original_filename"] = args.original_name or os.path.basename(args.raw)
    if args.member or source.name != args.raw:
        entry["zip_member"] = source.name.split(":", 1)[1] if ":" in source.name else args.member
    entry["sha256"] = sha256_file(args.raw)
    if args.raw_sha256:
        entry["raw_sha256"] = args.raw_sha256
    entry["raw_lines"] = total
    entry["kept_lines"] = len(keep)
    entry["bytes"] = len(data)
    entry["trim"] = {"head": head, "tail": tail, "per_key": per_key, "context": CONTEXT_LINES}
    entry["redacted"] = True
    if args.note:
        entry["note"] = args.note
    print(json.dumps(entry, indent=1))
    if args.index:
        index = []
        if os.path.exists(args.index):
            with open(args.index, "r", encoding="utf-8") as f:
                index = json.load(f)
        index = [e for e in index if e.get("case") != args.case] + [entry]
        index.sort(key=lambda e: e["case"])
        with open(args.index, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=1)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
