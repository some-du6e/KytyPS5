#!/usr/bin/env python3
"""Build a names file for kyty_emulator --nid-names from public symbol lists.

Downloads the lists in SOURCES (pinned revisions, checked against their SHA-256) into a cache
outside the repository, merges their names and writes one name per line, sorted and without
duplicates, after '#' lines that name each source and its revision. Then it checks the result:
how many NIDs of KytyPS5's own HLE tables (LIB_FUNC("<nid>", ...) in src/libs) get a name, and
how many NID/name pairs of the lists that carry both agree with the hash. A low share means a
broken hash or bad data.

Most of these lists have no license or one that is not GPL-2.0 compatible, so neither they nor
the names file may be committed: the file goes to the cache unless -o says otherwise. See
SOURCES.md and README.md next to this script.

Standard library only; run it with `python3 -I tools/nid/build_names.py`.
"""

import argparse
import collections
import hashlib
import http.client
import importlib.util
import os
import re
import sys
import urllib.request

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))


def _load_nid():
    spec = importlib.util.spec_from_file_location("kyty_nid", os.path.join(TOOL_DIR, "nid.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


nid = _load_nid()

USER_AGENT = "kyty-nid-build-names/1"
DOWNLOAD_TIMEOUT = 60
MAX_DOWNLOAD_BYTES = 64 << 20
OUTPUT_NAME = "nid-names.txt"


class Source:
    """One public list. `fmt` picks the parser; `license` is the SPDX id of the license the file
    is under in its repository, or "none"."""

    def __init__(self, sid, repo, rev, path, fmt, license, platform, sha256, size):
        self.id = sid
        self.repo = repo
        self.rev = rev
        self.path = path
        self.fmt = fmt
        self.license = license
        self.platform = platform
        self.sha256 = sha256
        self.size = size

    @property
    def url(self):
        return "https://raw.githubusercontent.com/%s/%s/%s" % (self.repo, self.rev, self.path)

    @property
    def page(self):
        return "https://github.com/%s/blob/%s/%s" % (self.repo, self.rev, self.path)

    def cache_path(self, cache):
        return os.path.join(cache, self.id, self.rev[:12], os.path.basename(self.path))


# Revisions and hashes as of 2026-11-16. To move a source to a newer revision, change rev and set
# sha256/size to None; the next run prints the new values to paste here.
SOURCES = [
    Source("sce_symbols-ps4", "zecoxao/sce_symbols", "6fd62a1ecd9574ee3394343fff84e3b4568217c1",
           "ps4_names.txt", "lines", "none", "PS4",
           "cb1854e8ab2b9adf3190ed320f0918ad06ef95e68da2170f96b785c45b33ce32", 7136255),
    Source("sce_symbols-ps5", "zecoxao/sce_symbols", "6fd62a1ecd9574ee3394343fff84e3b4568217c1",
           "ps5_names.txt", "lines", "none", "PS5",
           "0d2489fd3f106b1407294e2de33e3ce6b370e26fe4940fa24bb0cc5f43ecaf17", 10868348),
    Source("ps4libdoc", "idc/ps4libdoc", "8bff4863ac2f788149bbbf13a7eddc1222aec08b",
           "known_names.txt", "lines", "none", "PS4",
           "72eccf3755765be12048b1f9a1ae875687a0ec035c0764df96b24e382b2e8ff0", 1745061),
    Source("shadps4", "shadps4-emu/shadPS4", "72f70b7eca5890f387546082c113e7baa2e5965a",
           "scripts/ps4_names.txt", "lines", "GPL-2.0-or-later", "PS4",
           "1647f6b1c53cf18e5eaadf7f0ce073bd7d9b9aa7bf60adfa620c4123a693a0ce", 7044700),
    Source("fpps4", "red-prig/fpPS4", "04cefd43e6fddd1ab033e7980cd356d14c964905",
           "ps4libdoc.pas", "pascal", "LGPL-2.1", "PS4",
           "fa82ece4f424f19d44e3b8abf83e7a16a2e695658e66ea9ca48be707c981b0d5", 6080243),
    Source("ghidraorbis", "astrelsky/GhidraOrbis", "cca221c87b94a64c98edc72a3ba5ef3f7752e435",
           "data/nid_db.xml", "xml", "GPL-3.0", "PS4",
           "b0d22861c4e8db5b6aadca3d93fabd071b2074e2e92299112dce991b889dd7a4", 3898437),
    Source("ps4_module_loader", "SocraticBliss/ps4_module_loader", "657f9d380abfec9097bc4bddbc63551b3cd5e47b",
           "aerolib.csv", "pairs", "GPL-3.0", "PS4",
           "b80fd57038f66a222548f07fbf78124237e5f3b3abb8645218fd88895e817a95", 8925192),
]
SOURCES_BY_ID = {s.id: s for s in SOURCES}

# ---------------------------------------------------------------------------
# Parsers. Each yields (name bytes, NID text or None, 64-bit NID value or None). The files are
# untrusted downloads: they are only read as text, line by line, never executed or unpacked.

R_PAIR = re.compile(rb"^\s*([A-Za-z0-9+\-]{11})\s+(\S+)")
R_XML_ENTRY = re.compile(rb'<Entry\s+obf="([^"]*)"\s+sym="([^"]*)"')
R_PASCAL = re.compile(rb"\(i:\$([0-9A-Fa-f]{1,16});n:'((?:[^']|'')*)'\)")
R_XML_REF = re.compile(rb"&(#x[0-9A-Fa-f]+|#[0-9]+|amp|lt|gt|quot|apos);")
_XML_NAMED = {b"amp": b"&", b"lt": b"<", b"gt": b">", b"quot": b'"', b"apos": b"'"}


def _xml_unescape(text):
    def ref(m):
        r = m.group(1)
        try:
            if r.startswith(b"#x"):
                return chr(int(r[2:], 16)).encode("utf-8", "surrogatepass")
            if r.startswith(b"#"):
                return chr(int(r[1:])).encode("utf-8", "surrogatepass")
        except (ValueError, OverflowError):  # out of range: leave it, the name filter drops it
            return m.group(0)
        return _XML_NAMED[r]
    return R_XML_REF.sub(ref, text) if b"&" in text else text


def _lines(f):
    first = True
    for line in f:
        if first:
            first = False
            if line.startswith(b"\xef\xbb\xbf"):
                line = line[3:]
        yield line


def parse_lines(f):
    for line in _lines(f):
        yield line.strip(), None, None


def parse_pairs(f):
    for line in _lines(f):
        m = R_PAIR.match(line)
        if m:
            yield m.group(2), m.group(1).decode("ascii"), None


def parse_xml(f):
    for line in _lines(f):
        for m in R_XML_ENTRY.finditer(line):
            yield _xml_unescape(m.group(2)), m.group(1).decode("ascii", "replace"), None


def parse_pascal(f):
    for line in _lines(f):
        for m in R_PASCAL.finditer(line):
            yield m.group(2).replace(b"''", b"'"), None, int(m.group(1), 16)


PARSERS = {"lines": parse_lines, "pairs": parse_pairs, "xml": parse_xml, "pascal": parse_pascal}


def usable_name(name):
    """Names the names file can carry and the emulator can match: printable ASCII without spaces.
    A line starting with '#' would be a comment and surrounding blanks would be trimmed, and real
    PS4/PS5 symbol names have neither, nor spaces or non-ASCII bytes."""
    if not name or name.startswith(b"#"):
        return False
    return all(0x21 <= c <= 0x7E for c in name)


# ---------------------------------------------------------------------------
# Cache and downloads


def default_cache():
    env = os.environ.get("KYTY_NID_CACHE")
    if env:
        return env
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
        return os.path.join(base, "kyty-nid")
    if sys.platform == "darwin":
        return os.path.join(os.path.expanduser("~"), "Library", "Caches", "kyty-nid")
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "kyty-nid")


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class FetchError(Exception):
    pass


def download(url, dest, opener=None):
    """Downloads url to dest through a temporary file next to it. Returns the byte count."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = "%s.download-%d.tmp" % (dest, os.getpid())
    try:
        with open(tmp, "wb") as out:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with (opener or urllib.request.urlopen)(req, timeout=DOWNLOAD_TIMEOUT) as resp:
                total = 0
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > MAX_DOWNLOAD_BYTES:
                        raise FetchError("%s: larger than %d MiB" % (url, MAX_DOWNLOAD_BYTES >> 20))
                    out.write(chunk)
        os.replace(tmp, dest)
        return total
    except (OSError, ValueError, http.client.HTTPException) as e:
        raise FetchError("%s: %s" % (url, e)) from None
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def fetch(source, cache, offline=False, refresh=False, opener=None, log=print):
    """The cached copy of a source, downloaded first when needed and checked against its SHA-256.
    Returns (path, sha256)."""
    path = source.cache_path(cache)
    if refresh and os.path.exists(path) and not offline:
        os.unlink(path)
    if not os.path.exists(path):
        if offline:
            raise FetchError("%s: not in the cache (%s) and --offline was given" % (source.id, path))
        log("downloading %s: %s" % (source.id, source.url))
        download(source.url, path, opener=opener)
    size = os.path.getsize(path)
    if source.size is not None and size != source.size:
        bad = path + ".bad"
        os.replace(path, bad)
        raise FetchError("%s: %d bytes, expected %d (moved to %s)" % (source.id, size, source.size, bad))
    digest = file_sha256(path)
    if source.sha256 is not None and digest != source.sha256:
        bad = path + ".bad"
        os.replace(path, bad)
        raise FetchError("%s: SHA-256 %s, expected %s (moved to %s)" % (source.id, digest, source.sha256, bad))
    if source.sha256 is None:
        log("note: %s has no pinned hash; sha256=%r, size=%d" % (source.id, digest, size))
    return path, digest


# ---------------------------------------------------------------------------
# Merging and checks


class SourceResult:
    def __init__(self, source):
        self.source = source
        self.names = set()
        self.entries = 0
        self.rejected = 0
        self.pairs = 0
        self.pairs_matching = 0
        self.mismatches = []
        self.sha256 = None


def read_source(source, path):
    res = SourceResult(source)
    with open(path, "rb") as f:
        for name, listed_nid, listed_value in PARSERS[source.fmt](f):
            res.entries += 1
            if not usable_name(name):
                res.rejected += 1
                continue
            res.names.add(name)
            if listed_nid is not None or listed_value is not None:
                res.pairs += 1
                value = nid.nid_value(name)
                ok = (nid.encode_nid(value) == listed_nid) if listed_nid is not None else (value == listed_value)
                if ok:
                    res.pairs_matching += 1
                elif len(res.mismatches) < 5:
                    listed = listed_nid if listed_nid is not None else nid.encode_nid(listed_value)
                    res.mismatches.append((listed, name.decode("ascii")))
    return res


def read_extra(path):
    """A local names file in the --nid-names format; its names pass the same filter."""
    names = set()
    rejected = 0
    with open(path, "rb") as f:
        for line in f.read().split(b"\n"):
            line = line.rstrip(b"\r \t").lstrip(b" \t")
            if not line or line.startswith(b"#"):
                continue
            if usable_name(line):
                names.add(line)
            else:
                rejected += 1
    return names, rejected


# clang-format wraps some calls after the NID, so the pattern spans lines.
R_LIB_ENTRY = re.compile(r'\bLIB_(FUNC|OBJECT)\(\s*"([^"]*)"\s*,\s*([^)]*?)\s*\)')


def hle_nids(repo):
    """LIB_FUNC/LIB_OBJECT NIDs registered in <repo>/src/libs: {nid: [(kind, file, line, function)]}."""
    out = collections.OrderedDict()
    src = os.path.join(repo, "src", "libs")
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for name in sorted(files):
            if not name.endswith((".cpp", ".h", ".inl")):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, repo).replace("\\", "/")
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            if "LIB_" not in text:
                continue
            for m in R_LIB_ENTRY.finditer(text):
                lineno = text.count("\n", 0, m.start()) + 1
                func = " ".join(m.group(3).split())
                out.setdefault(m.group(2), []).append((m.group(1), rel, lineno, func))
    return out


def default_repo():
    cand = os.path.normpath(os.path.join(TOOL_DIR, "..", ".."))
    return cand if os.path.isdir(os.path.join(cand, "src", "libs")) else None


def hle_share(table, hle):
    """(named, total) over the LIB_FUNC NIDs, and the unnamed ones."""
    funcs = [n for n, regs in hle.items() if any(kind == "FUNC" for kind, _f, _l, _fn in regs)]
    unnamed = [n for n in funcs if table.find(n) is None]
    return len(funcs) - len(unnamed), len(funcs), unnamed


def header_lines(results, extras, count):
    lines = [
        "# KytyPS5 NID names for kyty_emulator --nid-names, written by tools/nid/build_names.py.",
        "# Built from the public lists below. Most have no license or one that is not GPL-2.0",
        "# compatible: do not commit this file (tools/nid/SOURCES.md).",
    ]
    for r in results:
        s = r.source
        lines.append("# source %s: %s rev %s %s (%s; license %s) names=%d" % (
            s.id, s.repo, s.rev, s.path, s.platform, s.license, len(r.names)))
    for path, names in extras:
        lines.append("# extra: %s names=%d" % (os.path.basename(path), len(names)))
    lines.append("# names: %d" % count)
    return lines


def write_names(path, header, names):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = "%s.%d.tmp" % (path, os.getpid())
    try:
        with open(tmp, "wb") as f:
            for line in header:
                f.write(line.encode("ascii") + b"\n")
            for name in names:
                f.write(name + b"\n")
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def _pct(a, b):
    return 100.0 * a / b if b else 0.0


def main(argv=None, opener=None):
    ap = argparse.ArgumentParser(prog="build_names.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--output", help="names file to write (default: %s in the cache)" % OUTPUT_NAME)
    ap.add_argument("--cache", help="download cache (default: $KYTY_NID_CACHE, else %s)" % default_cache())
    ap.add_argument("--sources", help="comma-separated source ids (default: all; see --list-sources)")
    ap.add_argument("--extra", action="append", default=[], metavar="FILE",
                    help="also merge this local names file (--nid-names format); repeatable")
    ap.add_argument("--offline", action="store_true", help="use only the cache, never download")
    ap.add_argument("--refresh", action="store_true", help="download again even when cached")
    ap.add_argument("--skip-failed", action="store_true",
                    help="build from the sources that could be fetched instead of stopping")
    ap.add_argument("--repo", help="KytyPS5 checkout for the HLE check (default: the one holding this script)")
    ap.add_argument("--min-share", type=float, default=0.5,
                    help="exit 3 when fewer HLE NIDs than this share get a name (default 0.5)")
    ap.add_argument("--list-sources", action="store_true", help="print the sources and exit")
    ap.add_argument("-v", "--verbose", action="store_true", help="also list the HLE NIDs without a name")
    args = ap.parse_args(argv)

    if args.list_sources:
        for s in SOURCES:
            print("%-18s %-4s %-17s %5.1f MB  %s" % (s.id, s.platform, s.license, (s.size or 0) / 1e6, s.page))
        return 0

    cache = args.cache or default_cache()
    selected = SOURCES
    if args.sources:
        ids = [x.strip() for x in args.sources.split(",") if x.strip()]
        unknown = [x for x in ids if x not in SOURCES_BY_ID]
        if unknown:
            ap.error("unknown source(s): %s (see --list-sources)" % ", ".join(unknown))
        selected = [SOURCES_BY_ID[x] for x in ids]

    results = []
    failed = []
    for s in selected:
        try:
            path, digest = fetch(s, cache, offline=args.offline, refresh=args.refresh, opener=opener)
        except FetchError as e:
            print("error: %s" % e, file=sys.stderr)
            failed.append(s.id)
            continue
        r = read_source(s, path)
        r.sha256 = digest
        results.append(r)
    if failed and not args.skip_failed:
        print("error: could not get %s; nothing written (--skip-failed builds without them)" % ", ".join(failed),
              file=sys.stderr)
        return 1

    extras = []
    for path in args.extra:
        try:
            names, rejected = read_extra(path)
        except OSError as e:
            ap.error("--extra: %s" % e)
        extras.append((path, names))
        if rejected:
            print("extra %s: %d lines skipped (not printable ASCII without spaces)" % (path, rejected))

    union = set()
    for r in results:
        union |= r.names
    for _path, names in extras:
        union |= names
    if not union:
        print("error: no names", file=sys.stderr)
        return 1
    ordered = sorted(union)
    output = args.output or os.path.join(cache, OUTPUT_NAME)
    write_names(output, header_lines(results, extras, len(ordered)), ordered)

    # Per source: names, the ones no other source has, pairs that hash to the NID listed with them.
    for r in results:
        others = set()
        for o in results:
            if o is not r:
                others |= o.names
        for _path, names in extras:
            others |= names
        only = len(r.names - others)
        s = r.source
        print("source %s: %d names, %d only here (%s, license %s, rev %s)" % (
            s.id, len(r.names), only, s.path, s.license, s.rev[:12]))
        if r.rejected:
            print("  %d of %d entries skipped (not printable ASCII without spaces, or a '#' line)" % (
                r.rejected, r.entries))
        if r.pairs:
            print("  pair check: %d of %d NID/name pairs hash to the listed NID (%.2f%%)" % (
                r.pairs_matching, r.pairs, _pct(r.pairs_matching, r.pairs)))
            for listed, name in r.mismatches:
                print("    e.g. %s listed for %s, which hashes to %s" % (listed, name, nid.symbol_to_nid(name)))
    if failed:
        print("skipped: %s" % ", ".join(failed))

    # Read the file back the way the emulator does.
    table = nid.NameTable()
    lines_read = table.load_file(output)
    collisions = lines_read - len(table)
    print("names file: %s: %d names, %d distinct NIDs" % (output, lines_read, len(table)))
    if collisions:
        print("  %d names share a NID with another name (the emulator keeps the last)" % collisions)

    status = 0
    repo = args.repo or default_repo()
    if repo and os.path.isdir(os.path.join(repo, "src", "libs")):
        hle = hle_nids(repo)
        named, total, unnamed = hle_share(table, hle)
        share = named / total if total else 0.0
        print("HLE check: %d of %d LIB_FUNC NIDs in src/libs get a name (%.1f%%)" % (named, total, 100.0 * share))
        if args.verbose:
            for n in unnamed:
                kind, rel, lineno, func = hle[n][0]
                print("  no name: %s %s:%d %s" % (n, rel, lineno, func))
        if total and share < args.min_share:
            print("error: only %.1f%% of the HLE NIDs get a name: the hash or the data is broken" % (100.0 * share),
                  file=sys.stderr)
            status = 3
    else:
        print("HLE check: skipped (no KytyPS5 checkout with src/libs; use --repo)")
    return status


if __name__ == "__main__":
    sys.exit(main())
