# Symbol name sources

`build_names.py` builds the `--nid-names` file from public lists of PS4 and PS5 symbol names.
This page records where each list comes from, its license, how many names it gave on
2026-11-16, and why none of them is committed here.

KytyPS5 is GPL-2.0-only. A list could be committed only under a license that allows that:
GPL-2.0, GPL-2.0-or-later, LGPL-2.1, or a permissive one. A file without a license grants
nothing, and GPL-3.0 does not combine with GPL-2.0-only. So `build_names.py` downloads every
list at run time into a cache outside the repository and writes the names file there too:
`%LOCALAPPDATA%\kyty-nid` on Windows, `~/.cache/kyty-nid` on Linux, or `--cache DIR` /
`$KYTY_NID_CACHE`. Neither the lists nor the names file belong in git, and `build_names.py`
refuses a cache or names file inside the checkout.

## Lists the build uses

Every file is pinned to a commit and checked against its SHA-256 (`SOURCES` in
`build_names.py`). "Names" counts the distinct names the build keeps from the file, "only here"
the ones no other list has.

| id | file at the pinned revision | license | names | only here |
|---|---|---|---:|---:|
| `sce_symbols-ps4` | [zecoxao/sce_symbols `ps4_names.txt`](https://github.com/zecoxao/sce_symbols/blob/6fd62a1ecd9574ee3394343fff84e3b4568217c1/ps4_names.txt) @ `6fd62a1ecd95` | none | 96,374 | 1,431 |
| `sce_symbols-ps5` | [zecoxao/sce_symbols `ps5_names.txt`](https://github.com/zecoxao/sce_symbols/blob/6fd62a1ecd9574ee3394343fff84e3b4568217c1/ps5_names.txt) @ `6fd62a1ecd95` | none | 157,900 | 48,067 |
| `ps4libdoc` | [idc/ps4libdoc `known_names.txt`](https://github.com/idc/ps4libdoc/blob/8bff4863ac2f788149bbbf13a7eddc1222aec08b/known_names.txt) @ `8bff4863ac2f` (`doc` branch) | none | 42,010 | 0 |
| `shadps4` | [shadps4-emu/shadPS4 `scripts/ps4_names.txt`](https://github.com/shadps4-emu/shadPS4/blob/72f70b7eca5890f387546082c113e7baa2e5965a/scripts/ps4_names.txt) @ `72f70b7eca58` | GPL-2.0-or-later | 94,276 | 2 |
| `fpps4` | [red-prig/fpPS4 `ps4libdoc.pas`](https://github.com/red-prig/fpPS4/blob/04cefd43e6fddd1ab033e7980cd356d14c964905/ps4libdoc.pas) @ `04cefd43e6fd` | LGPL-2.1 | 78,372 | 15,772 |
| `ghidraorbis` | [astrelsky/GhidraOrbis `data/nid_db.xml`](https://github.com/astrelsky/GhidraOrbis/blob/cca221c87b94a64c98edc72a3ba5ef3f7752e435/data/nid_db.xml) @ `cca221c87b94` | GPL-3.0 | 51,105 | 4 |
| `ps4_module_loader` | [SocraticBliss/ps4_module_loader `aerolib.csv`](https://github.com/SocraticBliss/ps4_module_loader/blob/657f9d380abfec9097bc4bddbc63551b3cd5e47b/aerolib.csv) @ `657f9d380abf` | GPL-3.0 | 97,623 | 17 |

Together: 176,487 names, 12 MB, from 46 MB of downloads. Notes on single lists:

- sce_symbols is the largest collection and the only one made for PS5. It has no LICENSE file.
- shadPS4's `REUSE.toml` declares `scripts/ps4_names.txt` GPL-2.0-or-later, but the shadPS4
  script that uses it says the list was derived from sce_symbols. A compatible license on a copy
  of an unlicensed list is not something to lean on, and the list adds 2 names, so it is
  downloaded like the rest.
- fpPS4's `ps4libdoc.pas` has no header of its own; the repository is LGPL-2.1. Going by its name
  and content it was generated from ps4libdoc. It stores each NID as the 64-bit number behind
  the text form.
- `ghidraorbis`, `ps4_module_loader` and `fpps4` list a NID next to each name. The build hashes
  every such name and compares. On 2026-11-16 all 227,101 pairs matched: 51,106, 97,623 and
  78,372. Three outside sources agreeing with `nid.py` on that many names is the check that the
  port is right.

## Lists looked at and not used

| source | license | names | why not |
|---|---|---:|---|
| [zecoxao/sce_symbols `aerolib.csv`](https://github.com/zecoxao/sce_symbols/blob/6fd62a1ecd9574ee3394343fff84e3b4568217c1/aerolib.csv) | none | 160,628 pairs | generated from the two name lists above; no name the build lacks |
| [zecoxao/sce_symbols `unique.txt`](https://github.com/zecoxao/sce_symbols/blob/6fd62a1ecd9574ee3394343fff84e3b4568217c1/unique.txt) | none | 310,421 | names of every SCE platform mixed together (PS3 `cell*`, Vita `ksce*`, PowerPC `._ZN` symbols); its 252,464 extra names name none of the HLE NIDs or GTA V imports the build leaves unnamed |
| sce_symbols `ps3_names.txt`, `vita_names.txt`, `psp_names.txt`, `nids.txt` | none | not counted | other consoles with 32-bit NIDs |
| [shadPS4 `src/core/aerolib/aerolib.inl`](https://github.com/shadps4-emu/shadPS4/blob/72f70b7eca5890f387546082c113e7baa2e5965a/src/core/aerolib/aerolib.inl) | GPL-2.0-or-later | 94,249 pairs | generated from `scripts/ps4_names.txt` by shadPS4's own hash script. The NID/name pairs in `tests/data/confirmed_pairs.tsv` are checked against it |
| fpPS4 `tools/ps4libdoc/known_names.txt` | LGPL-2.1 | 42,010 | byte-identical copy of ps4libdoc's `known_names.txt` |
| fpPS4 `tools/ps4libdoc/ps4_names.txt` | LGPL-2.1 | 51,102 | no name the build lacks |
| [idc/ps4libdoc](https://github.com/idc/ps4libdoc) firmware branches 1.05 to 9.00 | none | not counted | JSON per module and firmware; the `doc` branch's `known_names.txt` has the names |
| [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) | GPL-2.0 | not counted | the names are C++ function definitions spread over 5,500 files of a 260 MB repository, and its own `tools/nid_names.py` names NIDs from sce_symbols' `aerolib.csv` |
| [parl0v/nidhunt](https://github.com/parl0v/nidhunt) | GPL-3.0 | none | a NID brute-force tool with sample inputs, no list |
| [claimore22/ps5rs](https://github.com/claimore22/ps5rs) | GPL-2.0 text, which GitHub reports as NOASSERTION | none | game analysis output and a NID module, no names list |
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) | GPL-3.0 | none | resolves symbols by name at run time, no list |
| [Rufidj/ps5link-sdk](https://github.com/Rufidj/ps5link-sdk) | GPL-3.0 | none | a linker, no list |
| [OpenOrbis/OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) | GPL-3.0 | none | no names list in the repository |
| [Inori/GPCS4](https://github.com/Inori/GPCS4) | GPL-3.0 | not counted | only the HLE tables of the functions it implements |
| [InoriRus/Kyty](https://github.com/InoriRus/Kyty) | MIT | none | its LIB_FUNC tables carry C++ function names, not SCE names |
| [Zer0xFF/ps4_nid_resolver_ida](https://github.com/Zer0xFF/ps4_nid_resolver_ida) | GPL-2.0 | none | IDA plugin that reads ps4libdoc, no list in the repository |
| [aerosoul94/dynlib](https://github.com/aerosoul94/dynlib) | none | none | only a README is left |

## What is committed

Nothing from these lists. `tests/data/names.txt` and `tests/data/confirmed_pairs.tsv` hold 26
names, each a test vector with its source given: a comment next to the LIB_FUNC in `src/libs`,
and shadPS4's `aerolib.inl`, which is GPL-2.0-or-later.

## Moving a list to a newer revision

1. Change `rev` in `SOURCES` and set `sha256` and `size` to `None`. Leave `names` as it is.
2. Run `python3 -I tools/nid/build_names.py`. It downloads the file and prints the new hash and
   size; paste them into `SOURCES`. A list that gives no names, or more than 1% fewer than
   `names`, stops the build: its format has probably changed, and the parser needs a look first.
3. Check the pair checks and the HLE share in the output, then set `names` to the new count and
   update the counts above.
