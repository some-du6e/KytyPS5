# kyty-triage

A first look at a KytyPS5 log from a failed run. Give it the logs a tester attached and it
prints who ran what on which GPU, where the run died, which imports and shader instructions were
missing, the GPU errors and `[HACK]` notices, how far the game got, and a ranked list of places
in this repo to look at. It was written for GTA 6 launch day (2026-11-19), when the reports will
be pasted DbgReports, cruz-style session logs and multi-GB printf logs. GTA V PS5 logs from
upstream issues are the test data.

Python 3.8+ standard library only. It only reads the logs and never runs anything from them.

```
python3 -I tools/triage/kyty_triage.py _Logs/PPSA04263_2026-11-10_18-12-48.log _kyty.txt
python3 -I tools/triage/kyty_triage.py Kyty-log.zip              # every text member, read in memory
python3 -I tools/triage/kyty_triage.py _kyty.txt.gz --json > run.json
python3 -I tools/triage/kyty_triage.py D:/KytyPS5                 # newest _Logs/*.log + _kyty.txt
pbpaste | python3 -I tools/triage/kyty_triage.py -               # pasted issue text
python3 -I tools/triage/kyty_triage.py --agent-prompt crash.log  # brief for a coding agent
python3 -I tools/triage/kyty_triage.py --compare old.json new.log # what changed between two runs
```

Options:

| option | what it does |
|---|---|
| `--json` | the full report as JSON (schema 1; `--compare` takes these files back) |
| `--agent-prompt` | a plain-text brief with the evidence lines, ranked fix areas and the repo's rules for changes |
| `--compare A B` | diff two runs (logs or saved `--json` reports): new and gone fatal signatures, imports, opcodes, resource-tracking hashes, GPU errors, VUIDs, hacks, soft-fails |
| `--quick` | plain files over 32 MiB: read the last 16 MiB first, then the first 16 MiB. Ignored for .gz/.zip, which can't seek |
| `--redact` | replace user names, game folders and e-mail addresses in the output with `<redacted>` / `<game>` |
| `--exit-code N` | the emulator's exit code if the tester knows it (`0xC0000409` works) |
| `--repo DIR` | checkout used to map library names to `src/libs` files (default: the tree the tool sits in) |
| `--rules FILE` | another rules file |
| `--top N`, `-o FILE` | fix areas shown, output file |

## What it reads

One pass over each input, line by line. A line is dropped by its first byte unless a built-in
extractor or a rule has a literal prefix starting with that byte, so most of a printf log
(tab-indented parameter dumps) costs one dictionary lookup. Only matching lines get decoded,
with `errors=replace`. CRLF, a cut-off last line and bad UTF-8 are fine. Measured on this
machine: the 852 MB gzip'd printf log from #888 takes 25 s and 24 MB of RAM, a 108 MB one takes 2.6 s.

Inputs under 8 MiB and stdin count as possibly pasted text. Then quote markers, backticks and
indentation are stripped, the GitHub issue form (`### GPU` and so on) fills in the identity, and a
fatal message pasted without its `--- Build ---` header still counts as the crash.

Formats it knows (the emitters are listed in the survey catalogue and `notes/log-contract.md`):

- Fatal reports: the DbgReport block (`--- Build ---`, label, `--- Error ---`/`--- Fatal Error ---`,
  message, ` in file:line`, from `src/common/assert.cpp`), the pre-September `--- Stack Trace ---`
  form, `PageManager fatal:`, `terminate: uncaught exception`, `Guest abort diagnostics:`, the
  guest stack trace, and `while compiling shader:` after a report. The first report is the
  primary one. Later reports (another thread noticing the same device loss) are listed after it.
  The same report in a session log and a printf log of one run is counted once.
- `--- Guest fault context ---` with `thread:`, `pc:`, `guest pc:`, registers, stack hits and
  `pipeline being created by the driver:`. The pc of `Unhandled host exception` is resolved to
  `module+offset` from the `guest pc:` line, else the `Loading:`/`base_vaddr`/`mapped_size` module
  map of the printf log, else the host module, else the GTA V-style eboot base `0x900000000`.
  The fault is then classed as guest code, a jump to a bad address, the GPU driver, the
  emulator, or an unknown host address.
- Identity: session header and build label, `Build`/`Host` block, param.sfo lines, `Title ID:`,
  `Vulkan device:`/`Select device:`, and the contract lines `GPU:`, `Host:`, `Title profile:`,
  `Preset:`, `VRAM:`.
- Imports: all three forms of `Unresolved import stub called` (console, printf, contract with
  `policy=`), `Unresolved object import:`, `Unresolved imports: ... by library:`, `Unresolved
  import name:`, `Unresolved import called:` (`--strict-imports`), `Stubbed:`/`Relocate: unresolved`
  load lists, `Rebind:`, `PS5 NID fallback:`. Symbols are grouped by library. The library is
  mapped to the `src/libs` file that registers it (`LIB_NAME`/`LIB_VERSION`).
- Shaders: `[HACK] skip unsupported shader instruction` and the fatal decoder/CFG errors, both
  split into family, opcode, raw words and reason. The stage comes from `ShaderRecompiler <stage> ... hash=`
  lines, `while compiling shader:` or the driver pipeline line. Also `shader resource tracking:`
  failures with hash, stage and pc, checked against the known GTA V hashes.
- GPU: `[Vulkan][E|W|...]` messages grouped by VUID, `<call> failed: Error...`, `Device fault:`
  / `Device lost:`, VRAM spill and pressure lines, image and VMA failures, descriptor pools,
  pipeline creation.
- Workarounds: `[HACK] ...: first at ...` grouped by hack, with totals from the 10 s summaries,
  plus `Soft-fail:` and `Guest syscall:`.
- Progress: `Initialized:`, `Shaders: VS ...`, `--- Execute: Main`, pipeline cache and manifest
  lines, flips (only visible when a trace of `...Flip()` calls or `Flip done:` is on), the frame
  time `[HACK]` line, the last traced HLE call, and the clean-exit markers. Without a fatal
  report the outcome is `clean-exit`, `truncated` (last line cut off: killed or hung while
  writing) or `no-fatal`.

## Ranking

Each signal maps to a rule, and each rule has fix-area paths, a next step and known issues or
commits. The primary fatal report scores 1000 plus its rule's severity. Everything else scores
`4 * severity` plus a little for how often it happened. Rules that share a `group` merge into one
entry. One correlation is hard-coded: when the guest crashes (bad read, bad jump, abort) and
unresolved imports that are not known to be harmless were called in the last 10% (or last 5000 lines) of the log,
the imports are ranked above the crash. A stub returning 0 right before a guest null
dereference is the usual pattern. Known signatures (the GTA V eboot+0x28ab5xx write fault,
for example) turn that off.

## Adding a rule

Rules live in `rules.json`, so you can add one without touching the code.

```json
{
 "id": "my-new-check",
 "category": "RENDER_STATE",
 "title": "Short name shown in the report",
 "scope": "line",
 "severity": 50,
 "regex": "^Some prefix: (?P<what>\\w+) at 0x(?P<addr>[0-9a-f]+)",
 "key": ["what"],
 "fix_area": ["src/graphics/host_gpu/renderer/renderDraw.cpp"],
 "next_step": "What to change, or what to ask the tester for.",
 "known": ["#1234 short note", "abcdef12 commit that touched it"]
}
```

- `scope`: `fatal` tests the message of fatal reports (rules are tried in file order and the
  first match wins; `file` is a regex on the basename of the report's source file). `line` tests
  every log line. `meta` rules have no regex and are used by the tool's own extractors.
- A `line` rule needs a regex starting with a literal `^prefix` (at least 4 characters) or a
  `contains` substring. That literal is what keeps big logs fast.
- Named groups become fields. `key` says which fields count as "the same finding"; without it the
  line with numbers replaced is the key.
- Optional: `group` (merge with other rules in the ranking), `vendor` (`nvidia`/`amd`/`intel`,
  lowers the score on other GPUs), `ignore_case`.
- `known_signatures` match fields (`hash`, `symbol`, `guest_pc`, `vuid`, `message`) against
  issue references; `"harmless": true` keeps an import out of the ranking.

Then add an example: a line under `expect-line: <id>` in
`tests/fixtures/synthetic-rule-lines.txt`, or a report under `expect: <id>` in
`tests/fixtures/synthetic-fatal-catalogue.txt`. `test_rules.py` fails for a rule that no fixture
triggers, for a missing fix-area path, and for a line rule without an example.

## Fixtures and tests

```
python3 -I -m unittest discover -s tools/triage/tests -t tools/triage -v
```

`tests/fixtures/*.txt` are trimmed, redacted GTA V (and two UFC 6 / Astro Bot) logs from upstream
issues, plus synthetic ones whose first line says SYNTHETIC. `INDEX.json` records where each came
from (issue URL, original file name, sha256 of the downloaded file, and of the uncompressed log
for .gz). `tests/expected/<case>.json` holds the golden fields: primary category and rule, crash
site, top fix area, identity, imports, opcodes, hashes, hacks and VUIDs. Keys ending in
`_includes` list items that must be present.

To add a real log:

```
python3 -I tools/triage/make_fixture.py RAW.txt.gz --case gta6-1234-short-name \
    --issue https://github.com/KytyPS5/KytyPS5/issues/1234 --source-url <attachment URL> \
    --index tools/triage/tests/fixtures/INDEX.json
```

It keeps the first 200 lines, up to 40 lines of each kind the tool recognizes, 48 lines after
every report or fault context, and the last 300 lines. It redacts user and game paths and shrinks
until the file is under 190 KB. Write `tests/expected/<case>.json` by hand from the tool's
`--json` output after checking it, then run the tests. The raw download stays out of the repo.

## Collecting logs from a tester

`collect_logs.ps1` (Windows) and `collect_logs.sh` (Linux/macOS) zip the newest `_Logs/*.log`,
`_kyty.txt` and `vulkaninfo --summary` into `kyty-logs-<date>.zip`. Run them from the emulator
folder or pass it as the first argument. A printf log over 256 MiB is cut to its first 32 MiB and
last 128 MiB. The zip contains the tester's user name and game path. Either attach it privately
or paste `kyty_triage.py --redact` output instead.

```
powershell -ExecutionPolicy Bypass -File collect_logs.ps1 -KytyDir D:\KytyPS5
sh collect_logs.sh ~/KytyPS5
```

## Limits

- The session log of builds before the gta6/diag work has no GPU name, driver or VRAM size, and
  no unresolved-import calls (those went to stdout only). The printf log has them but is off by
  default (`--printf-direction File`).
- Flip counts need a trace that logs `...Flip()` calls. Normal logs don't, so `flips: 0` means
  "unknown", not "no frames".
- The 1024-call cap on printf `Unresolved import stub called` lines in older builds can hide
  later symbols. Contract-format lines (one per symbol) don't have that problem.
- GTA 6 title IDs, module names and thread names are unknown before launch. The rules key on
  emulator-side messages, and the GTA V signatures are hints only.
