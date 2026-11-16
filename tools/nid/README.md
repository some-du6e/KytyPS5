# NID names

PS4 and PS5 programs import functions by NID, an 11-character hash of the symbol name. KytyPS5
logs the imports it cannot resolve by NID, and `--nid-names <file>` adds the symbol name when
the file has it. Nothing in the repository provides that file. This directory builds one from
public lists, and names the NIDs in a report or log after the run.

| file | what it does |
|---|---|
| `nid.py` | `SymbolToNid`, `NameTable` and `FindNameForImport` from `src/loader/nid.cpp`, ported to Python. `python3 -I tools/nid/nid.py sceKernelUsleep` prints `1jfXLRVzisc sceKernelUsleep` |
| `build_names.py` | downloads seven public name lists and writes the names file |
| `name_imports.py` | names the unresolved imports in a `--dump-imports` report or in logs |
| `SOURCES.md` | the lists, their licenses and name counts, and why none is committed |
| `tests/` | `python3 -m unittest discover -s tools/nid/tests -t tools/nid -v`. Offline; the CI tools job runs them |

Python 3.8 or newer (tested with 3.12), standard library only. On Windows use `py -3` for
`python3`.

## Build the names file

```
python3 -I tools/nid/build_names.py
```

The first run downloads 46 MB into a cache outside the repository and writes `nid-names.txt`
there: `%LOCALAPPDATA%\kyty-nid` on Windows, `~/.cache/kyty-nid` on Linux,
`~/Library/Caches/kyty-nid` on macOS. Later runs read the cache. Each list is pinned to a commit
and checked against its SHA-256, so a changed or truncated download stops the build.

The run on 2026-11-16 printed this, with some lines left out and the home directory shortened:

```
source sce_symbols-ps4: 96374 names, 1431 only here (ps4_names.txt, license none, rev 6fd62a1ecd95)
source sce_symbols-ps5: 157900 names, 48067 only here (ps5_names.txt, license none, rev 6fd62a1ecd95)
source fpps4: 78372 names, 15772 only here (ps4libdoc.pas, license LGPL-2.1, rev 04cefd43e6fd)
  pair check: 78372 of 78372 NID/name pairs hash to the listed NID (100.00%)
source ps4_module_loader: 97623 names, 17 only here (aerolib.csv, license GPL-3.0, rev 657f9d380abf)
  pair check: 97623 of 97623 NID/name pairs hash to the listed NID (100.00%)
names file: ~/.cache/kyty-nid/nid-names.txt: 176487 names, 176487 distinct NIDs
HLE check: 1759 of 1776 LIB_FUNC NIDs in src/libs get a name (99.0%)
```

The last line is the sanity check. KytyPS5's own HLE tables hold 1,776 LIB_FUNC NIDs, and a
working hash with good lists names nearly all of them. Below 50% the script exits
with status 3. `-v` lists the ones without a name. On 2026-11-16 those were 14 Psml and Mfsr2
functions, one libc, one libkernel and one pad function that no list knows. The pair checks
hash every name that a list gives with its NID and compare. A list that gives no names, or more
than 1% fewer than its count in `SOURCES`, stops the build like a failed download, because losing
one list would hardly move the HLE share.

The file has one name per line, sorted, without duplicates, after `#` lines that name each
list and its revision. Only names of printable ASCII without spaces are kept: the emulator
trims blanks and takes a line starting with `#` for a comment.

Options: `-o FILE` writes elsewhere. The script refuses a names file or cache inside the
checkout (or inside `--repo`), so neither can be committed by accident (`SOURCES.md` says why).
`--offline` never downloads, `--refresh` downloads again, `--sources a,b` picks lists
(`--list-sources`), `--extra FILE` merges a names file of your own, `--skip-failed` builds from
the lists that could be fetched, and `--cache DIR` or `$KYTY_NID_CACHE` moves the cache.

## Use it with the emulator

```
kyty_emulator --game D:\GTA6 --dump-imports gta6-imports.txt --nid-names "%LOCALAPPDATA%\kyty-nid\nid-names.txt"
kyty_emulator --game D:\GTA6 --session-log auto --nid-names "%LOCALAPPDATA%\kyty-nid\nid-names.txt"
```

Keep the quotes: cmd.exe expands `%LOCALAPPDATA%` before it splits the arguments, so a profile
path with a space would end the value early. In PowerShell write
`"$env:LOCALAPPDATA\kyty-nid\nid-names.txt"`. The emulator logs
`NID names: 176487 names from ...` at start. In the `--dump-imports` report each
`unresolved` line then ends in the symbol name instead of `?`. In the session log, each
`Unresolved import stub called:` and `Unresolved object import:` line is followed by
`Unresolved import name: <generated name> = <symbol>`.

## Name a report or log afterwards

When a report or log was made without `--nid-names`, `name_imports.py` adds the names. It reads
the names file from the cache unless `--names FILE` says otherwise.

```
python3 -I tools/nid/name_imports.py gta6-imports.txt -o gta6-imports-named.txt
```

A `--dump-imports` report comes back in its own format with the `?` replaced: the report the
emulator writes with `--nid-names` and the same names file. Logs give a list instead. Session
logs, printf logs, console captures and pasted text all work, plain, `.gz` or `.zip`, or `-` for
stdin, in UTF-8 or in the UTF-16 that Windows PowerShell 5.1's `>` writes:

```
python3 -I tools/nid/name_imports.py --hle _Logs/PPSA01234_2026-11-19_20-05-11.log _kyty.txt.gz
...
library NpUtility: unresolved=5 named=5
  called BYIZGKm6bO4 Func eboot.bin sceNpBandwidthTestGetStatus
  load mA0zsbqm+kA Func eboot.bin sceNpBandwidthTestInitStartUpload
```

`called` means the stub ran, `object` is an object import, and `load` was listed as unresolved
when the program was relocated at load, with nothing later in the log binding it. The fourth
field is the importing program, `?` when the line does not say.

A load-time list is a snapshot. Since upstream `fdac55a5` (2026-10-04) only `libc.prx` is loaded
at that point. The game loads the other PRXs in its `sce_module` folder itself, and the linker's
next relocation binds the imports they export. Two more kinds cover that, and neither counts as
unresolved:

- `rebound`: a `Rebind:` line says a module loaded later bound the import. gta6-prep writes
  these lines.
- `prx`: for logs of older builds, which bound such imports without a line. The import was only
  unresolved at load, and later in the same log the game loaded the PRX of its library:
  `(libSceNpCppWebApi.prx loaded later)`. This is read from the order of the lines, not logged.

Over several logs an import gets the least resolved state any of them gives it. The
`--dump-imports` report is the complete gap list: it loads `eboot.bin` and every PRX in
`sce_module` before it resolves anything, so its `unresolved` lines are what neither the game's
own modules nor the HLE provide.

`--hle` marks the unresolved imports that a LIB_FUNC or LIB_OBJECT in this checkout registers
now, which helps with a log from an older build: `(hle now: src/libs/libNet.cpp:1684 NpCommerceDialogInitialize)`.
A `NoType` import binds to one only with the emulator's `--notype-import-fallback`, which is off
by default, so it gets `(hle with --notype-import-fallback: ...)` and is not counted as
registered. `--json` prints the same list as JSON.

The triage tool takes the names file too. `python3 -I tools/triage/kyty_triage.py --nid-names <file> <logs>`
puts the names on the called and object imports in its report, JSON and agent brief. Without
the option its output does not change.

## GTA V check

`python3 -I tools/nid/name_imports.py --hle tools/triage/tests/fixtures/gta5-*.txt` over the 19
GTA V logs from upstream issues lists 112 imports. 92 of them are unresolved, and the names file
names all 92; 28 have a LIB_FUNC in gta6-prep now and 64 do not. The other 20 are NpCppWebApi C++
methods that eboot.bin imports from the game's own `sce_module/libSceNpCppWebApi.prx`. They all
come from `gta5-829-v22-printf-rx9070.txt`, a build that loads only libc.prx before the first
relocation. In the full log the fixture was cut from, the game loads that module later, eboot.bin
is relocated again, and no NpCppWebApi stub is ever called. The tool lists them as `prx`, and
they need no HLE.

GTA V called five imports at run time, and gta6-prep implements none of those:

| NID | library | symbol | logs that call it |
|---|---|---|---:|
| `BYIZGKm6bO4` | NpUtility | `sceNpBandwidthTestGetStatus` | 2 |
| `hqzi1IHdQQQ` | NpUtility | `sceNpBandwidthTestInitStartDownload` | 2 |
| `kvdMF48mB3Y` | NpUtility | `sceNpBandwidthTestAbort` | 2 |
| `pLr1fEQS1z8` | NpUtility | `sceNpBandwidthTestShutdown` | 2 |
| `Fc8qxlKINYQ` | VideoRecordingP | `sceVideoRecordingSetInfo` | 11 |

The last column counts the logs, of the 19, with a call line for the import. Seven more list the
four NpBandwidthTest imports as unresolved at load without calling them.

The other 87 were unresolved at load and never called in these logs:

| library | NIDs | in gta6-prep now | still unresolved |
|---|---:|---:|---|
| Agc | 21 | 5 | `sceAgcCbCondWrite`, `sceAgcAcbAtomicMem`, `sceAgcAcbWaitUntilSafeForRendering` and 13 more |
| AgcDriver | 16 | 0 | `sceAgcDriverRegisterGdsResource`, `sceAgcDriverIsTraceInProgress`, `sceAgcDriverGetResourceName` and 13 more |
| ContentExport | 5 | 0 | `sceContentExportStart`, `sceContentExportFromFile`, `sceContentExportFromFileWithThumbnail`, `sceContentExportFinish`, `sceContentExportTerm` |
| WebBrowserDialog | 4 | 0 | `sceWebBrowserDialogOpen`, `sceWebBrowserDialogUpdateStatus`, `sceWebBrowserDialogGetResult`, `sceWebBrowserDialogClose` |
| Ampr | 3 | 0 | `sceAmprCommandBufferWaitOnCounter`, `sceAmprMeasureCommandSizeWaitOnCounter`, `sceAmprMeasureCommandSizeWaitOnAddress` |
| NpCommerce | 7 | 5 | `sceNpCommerceShowPsStoreIcon`, `sceNpCommerceHidePsStoreIcon` |
| NpWebApi2 | 4 | 2 | `sceNpWebApi2PushEventCreatePushContext`, `sceNpWebApi2PushEventStartPushContextCallback` |
| NpManager | 2 | 0 | `sceNpNotifyPremiumFeature`, `sceNpUnregisterPremiumEventCallback` |
| ContentDelete | 2 | 0 | `sceContentDeleteByPath`, `sceContentDeleteTerminate` |
| PlayerInvitationDialog | 2 | 0 | `scePlayerInvitationDialogUpdateStatus`, `scePlayerInvitationDialogTerminate` |
| AudioOut, ContentSearch, Coredump, NpUtility, PlayerSelectionDialog | 5 | 0 | `sceAudioOutSetMixLevelPadSpk`, `sceContentSearchSearchContent`, `sceCoredumpWriteUserData`, `sceNpBandwidthTestInitStartUpload`, `scePlayerSelectionDialogTerminate` |
| ImeDialog, NpEntitlementAccess, Net, SystemService, Posix | 16 | 16 | |

That leaves 59 load-time gaps and the five calls.

The fixtures are trimmed copies. The complete logs behind them, from builds of August to
November 2026, list 639 imports. 437 of them are NpCppWebApi methods, all in the November log of
the TheCruZ fork build (v22) that the v22 fixture was cut from, and all `prx` like the 20 above.
The nine logs from builds before 2026-10-04, which load every PRX before they relocate, list no
NpCppWebApi import. That leaves 202 unresolved imports, with the same five calls. The names file
names 201 of them; `iQS6DUtLybE` in VideoRecordingP is in no list.

For GTA 6 the list to work from is its `--dump-imports` report, which loads all of `sce_module`
before it resolves anything. What GTA V needed and gta6-prep still lacks is the first place to
look in it, the five calls first.

## How the port matches the emulator

- `symbol_to_nid()`: SHA-1 of the name followed by a 16-byte salt; the first 8 digest bytes as
  a little-endian number; ten characters of 6 bits from the top, then the low 4 bits times 4,
  from the alphabet `A-Z a-z 0-9 + -`. That equals base64 of the 8 big-endian bytes, which a
  test checks with Python's `base64`.
- `sha1_digest()` is `Sha1Digest` line for line. `symbol_to_nid()` uses `hashlib` because it is
  faster; the tests compare the two at every message length from 0 to 259 bytes and against the
  FIPS 180-2 vectors.
- `NameTable` splits at `\n` only, trims trailing `\r`, space and tab and leading space and tab,
  skips empty lines and lines starting with `#`, keeps a byte order mark as part of the first
  name, and lets a later name replace an earlier one with the same NID, as the C++ does.
- `tests/data/confirmed_pairs.tsv` holds 26 NID/name pairs. Each NID comes from `src/libs` or a
  GTA V log, each name from a comment next to the LIB_FUNC or from shadPS4's NID table. None
  came from running `nid.py`. They cover one, two and three SHA-1 blocks and NIDs with `+` and
  `-`.
