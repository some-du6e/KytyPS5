# GTA 6 launch prep (gta6-prep)

This branch prepares Karim's fork of KytyPS5 for GTA 6 (out 2026-11-19) on a budget PC with an
RTX 3050. The work was sized for the 8 GB model; Karim's card is the 6 GB one, which the settings
and estimate sections cover separately. Nobody has the game yet, so nothing here has run it.
Everything is preparation: the TheCruZ GTA V fork brought onto current upstream, hardening for a
newer-SDK title, an 8 GB mode, more shader coverage, tools for reading the first crash logs, a
preflight package that runs the GPU tests and a first boot on Karim's PC, and offline NID naming.

Read the "Verification status" section before trusting any of it. The branch builds on Linux and
Windows and its host tests pass in CI. Its GPU tests have run on an RTX 3050 6 GB, 39 of 42 passing,
but no game has run on it.

## What is on the branch

The branch starts at upstream KytyPS5 main `bc934ef0`.

### 1. TheCruZ's GTA V fork, merged

[TheCruZ/KytyPS5-GTA](https://github.com/TheCruZ/KytyPS5-GTA) at `e9812fc1` (cruz/main~1),
187 commits, merged as-is in `8025b6bd`. It is a merge, not a rebase. Every commit keeps its
author and SHA, and upstream main stays an ancestor, so the next `git merge upstream/main` stays
simple. A trial rebase conflicted at the 4th commit, and 53 of the 187 touch files upstream has
since changed.

- Seven files conflicted with upstream's last 3 commits. The fork had replaced upstream's BDA
  sync queue with an epoch scheme, so upstream's `b510053e` (BDA uploads behind one barrier pair)
  and `0ec2cd5d` (re-queue BDA pages after a DMA shader accessed them) were dropped as
  superseded. The fork already does the first. The second has nothing to attach to, and it adds
  a tag bit to page-table entries that the fork's RT code reads raw. Upstream's `bc934ef0`
  (compute blit helper) was kept. The merge commit has per-file notes.
- `58cd1f5c` takes the emulator half of TheCruZ's "Specific fork edits, not to commit"
  (`c00f2f27`). TheCruZ keeps that commit rebased on top, and later commits use what it defines
  (`Common::SoftExitScope`, manifest `warm_*` state, `--strict-pipeline-manifest`). Without it
  cruz/main~1 does not compile. README, images, the PGO release CI and the launcher's pipeline
  sharing were left out.
- All 187 commits were classified by hand-off reviewers (the full table is in the session notes,
  `notes/understand.json`): 63 general fixes, 66 general perf, 30 native ray tracing, 11
  diagnostics, 7 other-title hacks, 6 build/CI, 3 refactors, 1 fork-only. Native ray tracing is
  already behind `--native-raytracing` (off by default). The other-title hacks (Demon's Souls,
  ASTRO BOT, GT7) already check the title ID or a shader hash, or turn an EXIT into a skipped
  draw. One commit (`27c42613`) is superseded by upstream's version of the same function.
- 33 commits change global behaviour for every game and only had an env var or no switch. They
  stay on by default, since that is the combination tested on GTA V. Each got a kill switch (see
  "Kill switches" below), and `--preset safe` turns them all off for bisecting.

### 2. Title profiles, presets, overrides

- `src/common/titleProfile.*` matches the running title by exact ID or exact normalized name
  (case-folded, ™/®/© stripped, every localized `titleName` from param.json). It never uses a
  prefix match: "Grand Theft Auto V" is a prefix of "Grand Theft Auto VI".
- `gta5`: PPSA03420, PPSA04263, PPSA04264. Its one quirk reports GTA V's three legacy add-on
  entitlement labels, which upstream used to report to every title.
- `gta6`: matched by name only ("grand theft auto vi", "grand theft auto 6"). It has no quirks
  and no default preset, on purpose. Force it with `--title-profile gta6`, or list the real IDs
  in `KYTY_GTA6_TITLE_IDS` once they are known.
- `--preset <name>` expands in place, so later flags override it. Presets: `safe`, `low-vram`,
  `rtx3050`.
- `--quirk <name>=on|off` overrides a profile. Options given on the command line always beat a
  title's defaults. One-way flags got `--no-` forms.
- Every session logs `Title profile: ...` and one `Preset: ...` line per applied preset.

### 3. HLE hardening for a newer SDK

GTA 6 will import functions and pass struct sizes that no tested title has. Before this branch an
unresolved import already returned 0, but it logged only to stdout, capped at 1024 lines, and
missed the session log. Several boot-path APIs still killed the process on an unknown ID or a
grown struct.

- Each unresolved import logs once, on its first call, to the console and the session log.
  Unresolved object imports log at relocation. One summary line groups them by library.
- Imports stubbed at boot are re-bound when `sceKernelLoadStartModule` loads a module that
  exports them (`--no-rebind-imports` turns this off).
- Grown param structs are accepted where the code only reads the known prefix: Np content
  restriction, NetCtl NAT info, Audio3d, Ngs2 system and rack options, libc. A smaller struct
  returns the library's invalid-argument error.
- Unknown IDs on boot-path query APIs return the error firmware would return instead of EXIT:
  SystemService and AppContent params, Audio3d attributes, Ngs2 rack IDs, SaveData, event flags,
  LoadStartModule flags. The same goes for AMPR counter modes. Each logs `Soft-fail: <api>: ...`
  once.
- `--dump-imports <file>` loads eboot.bin and every `sce_module/*.prx` without a GPU and without
  running guest code. It writes, per library, what is imported, what HLE or another module
  resolves, and what stays unresolved. This runs on a decrypted dump on any PC. It is the fastest
  way to see GTA 6's HLE gaps on day one.
- `--nid-names <file>` names NIDs in those logs from a list of symbol names. It uses the hash
  code that already lived in libKernel.
- Raw `syscall` instructions in guest modules are logged at load. They are not emulated.
- Opt-in: `--strict-imports` (stop at the first unresolved call), `--unresolved-offline-returns`
  (unresolved Np/Net/Http/Ssl functions return offline error codes instead of 0) and
  `--notype-import-fallback` (bind untyped imports to functions or objects).

### 4. 8 GB VRAM mode

About 1 GiB of VRAM is gone before the game allocates anything. 768 MiB of that is a fixed BDA
page table covering 1.5 TiB of guest address space. The cache GC thresholds were anchored to
8 GiB, which on a smaller card puts "pressured" eviction at around 40% of the budget. Past the
budget, VMA spills to system RAM over PCIe without telling anyone. The one measured number is
GTA V Fidelity with RT starting at 8.7 GB on TheCruZ's RTX 3090.

- `--vram-profile low` turns on proportional GC thresholds, OOM recovery, a recycled-image pool
  capped at min(256 MiB, budget/32) and trimmed under pressure, 4 parked images instead of 8, an
  RT arena of clamp(heap/8, 512 MiB, 1 GiB), and at most 2 automatic pipeline helpers.
- `--vram-gc legacy|proportional`. Legacy reproduces the old numbers exactly; a test pins them.
- `--oom-recovery`. On an allocation failure it flushes the recycle pool and the destroy thread,
  refreshes the budget, retries, and only then lets the driver oversubscribe. It does not evict
  inside the failing call: images are created under the texture cache lock, so that would
  deadlock.
- `--vram-limit-mb <N>` limits device-local heaps through VMA so a big card behaves like an 8 GB
  one. Use it to test the low profile on any GPU.
- Log lines: `VRAM: ...` once after init (budget, fixed allocations, thresholds), `VRAM spill:
  ...` when images or buffers land outside device-local memory, `VRAM pressure: ...` on
  recovery.
- `--video-out-resolution auto|1080p|4k` changes what `VideoOutGetOutputStatus` tells the game.
  It is the only internal-resolution lever available. The emulator cannot rescale guest render
  targets, because their sizes are visible to the guest. Whether GTA titles size their targets
  from it is unknown, so presets leave it on auto.
- `--skip-unsupported-image-atomics` skips draws that need 64-bit image atomics when the GPU
  lacks them, instead of exiting.
- Merged guest buffers are checked against `maxMemoryAllocationSize` and `maxBufferSize` with a
  clear message.
- Not done: a smaller BDA table. Shaders compute the page index from the raw guest address and
  the layout is baked into compiled SPIR-V and the shader store. The real fix is sparse residency
  or a two-level table. Both are larger than this branch.
- NVIDIA audit, done from knowledge, not vulkaninfo. Every feature the renderer requires should
  be on Ampere with a current driver. The least certain are `VK_EXT_image_view_min_lod` and
  `VK_EXT_depth_clip_control`, both on NVIDIA since roughly R515. NVIDIA has 32-wide subgroups,
  so wave64 stays emulated. Check `vulkaninfo` on the real 3050 before launch.

### 5. Shader recompiler

Before this branch, one unsupported instruction in any shader killed the process with
`shader CFG build failed`. A heavier current-gen renderer is likely to hit at least one.

- An unsupported instruction now skips that program (draw or dispatch) and logs
  `[HACK] skip unsupported shader instruction: ...` once per shader. `--strict-shaders` restores
  the fatal exit. Default on: titles that work never reach this path, since it used to crash.
- 144 opcodes added (86 ALU and scalar, 58 memory and image), each encoding checked against
  llvm-mc for gfx1030:
  - ALU: v_perm_b32, v_swap_b32, the s_*_saveexec and s_*_wrexec families, s_cmov_b32,
    s_cmovk_i32, s_bfe_i64, VOP3 f16 ops with ABS/NEG/CLAMP/OMOD, VOP3P dot2/dot4/dot8 and
    v_dot4c_i32_i8, v_div_scale/fmas/fixup, v_mad_i64_i32, 16-bit mads, v_add/sub_nc_i32 with
    clamp, v_ldexp_f16, f16 norm conversions, the SAD family, the VOPC gaps.
  - Scalar no-ops: s_setreg_imm32_b32, s_round_mode, s_denorm_mode, cache and perf hints,
    s_memtime.
  - Memory: LDS float add and compare-store, ds_rsub, ds_mskor; 64-bit LDS and buffer atomics;
    buffer and image inc/dec/sub/fcmpswap; FLAT/GLOBAL loads with GLC/SLC and byte D16
    loads/stores; image_msaa_load; image_sample *_g16.
  - Formats: 8/16-bit USCALED/SSCALED textures. Unknown vertex-fetch formats return zero with a
    log line instead of EXIT. A formatted buffer access with an unknown format logs when it falls
    back to raw dwords.
- The fork recognizes GTA V's sce::Psr ray tracing shaders by an instruction fingerprint. New
  decodes would have changed those fingerprints and silently turned off native RT. The
  fingerprint pass now decodes in a legacy mode that reproduces the old decoder for both opcode
  blocks.
- Tests: decode tests for every new encoding, and GPU execution tests against C++ references for
  the opcodes with tricky semantics (v_perm_b32, dot4, saveexec, div fixup, inc/dec wrap,
  compare-store operand order, umax_x2, ds_add_f32).

### 6. Diagnostics

- `GPU: <name> vendor=0x10de device=... driver=... vram=... MiB bar=... MiB` and a `Host:` line
  in every session log.
- Failed Vulkan calls print `<vkFunction> failed: <VkResult> (<int>)` instead of `Not implemented
  (result != eSuccess)`. On device lost, `--device-fault-report` adds `VK_EXT_device_fault` data
  where the driver has it.
- The host fault report names the guest module and offset of the pc, the faulting address, the
  stack words and a guest stack trace. It never blocks on the linker lock: it gives up after
  100 ms and says so.
- Fatal reports name the shader being compiled, when there is one.
- `--session-log auto` writes `_Logs/<TITLE_ID>_<timestamp>.log` next to the emulator. The
  launcher can do the same per game. It is off by default; turn it on for testers.

### 7. Triage tool

`tools/triage/kyty_triage.py` reads session logs, `_kyty.txt` printf logs (plain, .gz, or inside
a .zip), or pasted issue text. It prints identity, crash site, unresolved imports by library,
unsupported shader instructions, resource-tracking failures, GPU errors, `[HACK]` notices, how far
the run got, and a ranked list of repo paths to look at. `--agent-prompt` writes a brief for a
coding agent. `--compare A B` diffs two runs. Python standard library only. It streams, so an
852 MB printf log takes about 25 s and 24 MB of RAM.

The tests use 19 trimmed, redacted GTA V logs from upstream issues (#166, #229, #729, #810,
#829, #888, #1123 and others; provenance and hashes are in `tests/fixtures/INDEX.json`) plus
synthetic logs for every new log line. `collect_logs.ps1` and `collect_logs.sh` zip the newest
session log, `_kyty.txt` and a `vulkaninfo --summary` for testers to attach.

### 8. CI

`.github/workflows/gta6-ci.yml` builds every `add_test` target on Linux and Windows and runs the
Python tests. It runs on pushes to `gta6-prep` only, one run at a time across the repo (GitHub
disabled Actions on the fork once after about ten concurrent builds), or on demand. Host tests gate
the job; `run_tests.py` reports host tests that fail upstream on a platform (Windows `common`) as
warnings. `workflow_dispatch` with `bisect_shas` builds only the given targets at each listed
commit, one job after another, to find which commit broke a test. Tests that open a Vulkan device run in a separate step that only reports, because GitHub
runners have no GPU and lavapipe lacks fragment barycentrics and 64-bit image atomics. Upstream's
own CI never runs those tests either. On lavapipe each of those tests took about 85 s to fail, 54
minutes for the group. So `shader_recompiler_compute` now runs first, and when it finds no usable
device, the rest of the group gets 10 s per test. The three tests that open no device still pass
within that limit. The Windows job also packages the GPU tests with preflight
and smoke-tests that package under Windows PowerShell 5.1 (section 9). The tools job runs the
triage, NID, CI-script and preflight tests, the last under pwsh.

### 9. Preflight package

`tools/preflight` lets Karim run the GPU tests and a first boot from the Windows CI artifact,
without building anything. The Windows job runs `.github/scripts/package_gpu_tests.py`, which
copies the GPU test group out of the build tree with only the DLLs each test imports and writes
`gpu-tests/tests.json`. The job puts that in `install/preflight` with `preflight.cmd`,
`preflight.ps1`, the README and `collect_logs.ps1`.

- `preflight.cmd` runs the 42 GPU tests and zips their logs with the PC's GPU, driver, VRAM, CPU,
  Windows build and VC++ runtime versions. Each test gets a time limit (5 minutes, 30 for
  `shader_recompiler_compute`) and runs in a job object, so nothing outlives preflight. After a
  test that lost the GPU device, it waits 60 s before the next one. In the files it writes itself,
  preflight replaces the profile path with `%USERPROFILE%` and drops vulkaninfo's device UUIDs.
  Raw test and emulator logs go into the zip unchanged.
- `preflight.cmd -Game "<dump folder>" -ExtraArgs "--title-profile gta6"` writes the
  `--dump-imports` report, boots the game for 5 minutes with `--preset rtx3050` and a session log,
  then collects the logs. `-BootSeconds 0` keeps the game running until its window closes. A boot
  that didn't crash is `RAN`, never `PASS`, because a game stuck on a black screen keeps running
  too. Paths inside `-ExtraArgs` must be full paths, since the emulator runs in the results folder.
- Game boots share one shader cache, `%LOCALAPPDATA%\KytyPS5\preflight-cache\_PipelineCache`,
  linked into the run folder for the boot only. Boots of the same build reuse its translated
  shaders. A new build ignores them, because the store records the build, but it still reuses the
  driver's pipeline cache. After a boot whose log shows a rejected compute pipeline or a crash
  while loading the cache, preflight renames the store to `.shaders.unusable`, so the next boot
  cannot die on it. Boots with `--shader-validation` or a shader log leave the store out, because
  stored shaders skip both. `-ColdCache` turns the whole thing off for a clean comparison.
- On a PC with less than 14 GiB of RAM the boot also gets `--ram-profile low`, and `memory.csv`
  records the PC's memory every 30 s.
- The Windows build uses the DLL version of the MSVC runtime. Before it runs anything, preflight
  checks every packaged executable's imports. It names the VC++ redistributable when a DLL is
  missing or when `msvcp140.dll` is older than the toolset that built the package (14.51 in CI).
- CI smoke-tests the package under Windows PowerShell 5.1 in three ways. The packaged tests must
  load outside the build tree, `-Game` on an empty folder must still write its zip, and a hung
  test with a child process must get stopped.

### 10. NID names

The emulator logs an unresolved import by its NID, a hash of the symbol name. `tools/nid` turns
NIDs back into names, offline once the names file exists.

- `build_names.py` downloads seven public NID lists at run time and writes a `--nid-names` file
  to `%LOCALAPPDATA%\kyty-nid` on Windows or `~/.cache/kyty-nid` on Linux. None of the lists is
  committed, since most have no license. Each download is checked against a pinned size and
  SHA-256.
- `name_imports.py` names the imports in a `--dump-imports` report, or in logs made without
  names. It follows `Rebind:` lines and the PRXs a game loads later, so imports that the game's
  own modules provide drop out. `kyty_triage.py --nid-names <file>` adds the names to a triage
  report.
- `nid.py` converts names to NIDs and back. Its Python port of `SymbolToNid` agrees with three
  other projects' tables on all 227,101 of their pairs. The names file names 1,759 of the
  emulator's 1,776 HLE NIDs.

The GTA V check over the 19 triage fixtures lists 112 imports. 20 are NpCppWebApi methods that bind
to GTA V's own `sce_module/libSceNpCppWebApi.prx` once it loads, so they are not gaps. Of the other
92, this branch has an HLE function for 28 and lacks 64. GTA V called 5 of the 64: four
`sceNpBandwidthTest*` functions and `sceVideoRecordingSetInfo`, which the triage rules mark
harmless. The other 59 only appear at load: 16 in Agc, 16 in AgcDriver, 5 in ContentExport, 4 in
WebBrowserDialog, 3 in Ampr and 15 across ten other libraries. That is GTA V's list. GTA 6's comes
from its own `--dump-imports` report, which loads `eboot.bin` and every `sce_module` PRX before it
resolves anything.

## Verification status

Read this before relying on anything above.

| what | status |
|---|---|
| Upstream main `bc934ef0` under gta6-ci | Built on Linux. 46 of 81 tests pass; the 35 failures all need a real GPU (the harness asks for fragment barycentrics and 64-bit LDS/image atomics). `vdecsw_tests` does not compile with clang-cl, and upstream never builds it on Windows. |
| TheCruZ fork, own CI | Green on Linux and Windows at `c00f2f27` (run 43081193192 in TheCruZ/KytyPS5-GTA), for the tests its CI runs. It does not run `shader_cfg`, which fails at `c00f2f27` (see below). |
| gta6-prep, full CI (run 43304032901, `30032c06`) | Green. Builds on Linux (clang) and Windows (clang-cl): emulator, launcher and every test target. Linux: 69 of 69 host tests pass. Windows: 67 of 67 gated host tests pass; `vdecsw` is skipped because clang-cl does not build it (as upstream), and upstream's `common` fails on the Windows runner as it does upstream, so it is reported, not gated. |
| GPU test group on CI | 3 of 42 pass on lavapipe; the rest stop because the runner has no device with fragment barycentrics and 64-bit LDS/image atomics, as upstream's do. |
| GPU test group on an RTX 3050 6 GB (Windows 11, driver 621.14, through preflight) | 39 of 42 pass, including this branch's own `shader_recompiler_gta6_alu` and `vram_heap_limit`. `shader_recompiler_compute` passes 1058 checks, then stops at `ShaderMemDsAtomics64`: on NVIDIA every 64-bit LDS atomic returned 0 and changed nothing, while 64-bit LDS reads and writes and 64-bit buffer atomics worked. A fix is in progress. `shader_recompiler_shader_mem` stops at the same case. The run before stopped earlier, at a test bug in `ShaderMemBufferAtomicsWrap` (fixed in `dfdf16a9`). `kernel_file_system` dies with 0xC0000028 (bad stack while unwinding) in its mutex handoff case, as it does on CI's Windows runner; it is being investigated. The 2 GiB buffer case is skipped on PCs with less than 14 GiB of RAM. The first run on that PC got only 4 tests to the GPU, because its page file could not commit the 12.7 GB guest section; the tests now use 6 GiB. |
| Test fixes from the first CI run | `shader_alu_decode`: the VOPC program overwrote its own buffer descriptor (test bug). `resource_tracking`: its "unsupported format" was `k8SScaled`, which shader-mem made supported (bisected to the shader-mem merge). `shader_cfg`: one check (material selection must reject an `s_cmp_le_u32` loop bound) fails in TheCruZ's fork at its own tip; it now prints a "known fork deviation" line instead of aborting, and the ~110 tests after it, which the fork never ran, pass. The recompiler keeps the fork's behaviour. |
| Python triage tool | 56 tests pass in CI, including the two that run `collect_logs.ps1` under pwsh 7. Locally those two skip (no pwsh). |
| gta6-prep with preflight and NID tools (run 43925506782, `05e3db5f`) | Green, with the same host results (Linux 69 of 69, Windows 67 of 67 gated). The Windows artifact is now 47 MB zipped, including 48.7 MB (unzipped) of packaged GPU tests from 2 executables. |
| Preflight package | In CI, 13 CI-script tests and 13 preflight tests pass, the preflight ones under pwsh 7.6.10. All three Windows PowerShell 5.1 smoke steps pass. The packaged tests load outside the build tree: `device_creation` fails at the hidden Vulkan window, as it does under ctest on a runner with no GPU. `-Game` on an empty folder reports FAIL for imports and boot and still writes its zip. A hung test gets stopped at 5 s with its child. On that runner `kernel_file_system` ends with 0xC0000028 (bad stack while unwinding an exception) where Linux fails cleanly, so watch for it on Karim's PC. Nothing has run on a PC with a GPU yet. |
| Read-watch locks (run 44044570383, `709a99b7`, merged as `0aa6b19c`) | Green after one fix: run 44038715401 failed on a Windows-only compile error. Linux passes 74 of 74 host tests and Windows 72 of 72 gated, including `read_watch_lock` and `virtual_memory_allocation`. On the Windows runner, `virtual_memory_allocation` checks with QueryWorkingSetEx that a watch locks the backing pages and the unwatch unlocks them. Whether the lock keeps the blue screen away is unverified: nothing has run on a PC with 8 GB of RAM. |
| Read-watch guard pages (`wf/read-guard`) | Written without a compiler: not built or run yet. CI's Windows runner is the first to check, with VirtualQuery, that the pages past the lock budget become PAGE_GUARD \| PAGE_READONLY, that the locked ones keep PAGE_NOACCESS, that a read faults once and the watch's end leaves no guard, that a page whose guard went stays read-only until its watch ends, that the emulator's exception filter puts no guard back on a served guest page, and that WriteFile() of a guard page takes the guard with no handler run (its log line says whether the call failed). Whether the guard pages keep the blue screen away, and what their stale reads cost, is unverified here: nothing has run on a PC with 8 GB of RAM. |
| `--ram-profile`, RAM lines, preflight and triage RAM support (run 44020349456, `c0e43876`, merged as `2105e147`) | Green. Linux passes 72 of 72 host tests and Windows 70 of 70 gated, including `ram_policy`, `emulator_ram_cli`, `emulator_ram_profile_invalid` and `diag`, which checks the commit figures against `K32GetPerformanceInfo` on Windows. The tools job passes triage 63, NID 68, CI scripts 15 and preflight 23. The Windows PowerShell 5.1 smoke steps check the RAM lines in `system.txt`, `summary.json` and `memory.csv`. Nothing has run on a PC with 8 GB of RAM or on a GPU. |
| Preflight shader cache (run 43966470006, `425a5841`) | Green. 22 preflight tests pass in CI under pwsh. Under Windows PowerShell 5.1, the empty-game steps linked the run folder to the shared cache through a real junction, found the marker file there on both runs, and kept it out of the zip. `-ColdCache` made no link. A junction made with `mklink /J` stayed out of the zip. How much time a warm boot saves is unmeasured, since no real game has booted. |
| GPU group time limit (run 43939026905, `00f3b3b0`) | Green. Once the probe finds no device, the Linux GPU step takes 8.4 minutes instead of 54, and the Linux job 27 minutes instead of 70. The 3 device-free tests still pass on both runners. |
| NID tools | 68 tests pass in CI and locally. The triage tool's output without `--nid-names` is byte-identical to before on all 99 fixture outputs. Not yet run on Windows. |
| GPU behaviour, NVIDIA behaviour, VRAM use | Only the GPU test group above has run on hardware. No game has run on this branch. |

Commits whose correctness no test can show say `Speculative:` in their message body. Commits
that say "build + tests pending in gta6-prep CI" were written before the first CI run; they are
covered by the results above. "Builds and passes host tests" still says nothing about behaviour on
a GPU: the first run on real hardware (an existing game such as GTA V with `--preset rtx3050`) is
the next real test.

## Building

Same as upstream's README. On Windows:

```powershell
git clone -b gta6-prep https://github.com/some-du6e/KytyPS5
cd KytyPS5
git submodule update --init --recursive
cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

On Linux, follow "Building on Linux" in README.md.

The easier route on Windows is no build at all. Every gta6-ci push run on this branch uploads
`KytyPS5-gta6-prep-Windows-x64`, which holds the installed emulator and the `preflight` folder
(section 9), about 47 MB zipped. Pick the newest run
whose Artifacts list shows it, not the newest green check, because a bisect run can be green with
no artifacts. If that run has a red X, ask before using it. Open the run's page on GitHub, scroll
to Artifacts and download it while signed in. GitHub keeps artifacts for 90 days.

The emulator needs the Microsoft Visual C++ 2015-2022 x64 redistributable, 14.51 or newer, from
https://aka.ms/vc14/vc_redist.x64.exe. The older aka.ms/vs/17/release/vc_redist.x64.exe link
installs 14.44, which is older than the toolset CI builds with.

## Recommended settings for an RTX 3050 (8 GB or 6 GB)

Start here:

```
kyty_emulator --game <path> --preset rtx3050 --session-log auto
```

`--preset rtx3050` is native RT off, Pro mode off, tiled and storage image readbacks off, small
layer targets evictable, `--vram-profile low`, proportional GC, OOM recovery, and no GPU-thread
CPU pinning. TheCruZ tuned the built-in pinning for a Ryzen 9 5900X.

Then, depending on what happens:

- If the title profile line says `none` for GTA 6, add `--title-profile gta6`. It changes nothing
  today, but it puts the profile in the logs and gives later quirks a place to go.
- If VRAM spill lines show up early, try `--video-out-resolution 1080p` and watch whether the
  `VRAM:` and spill numbers change. If they don't, the game ignores it, so turn it back off. Don't
  expect much: GTA V kept rendering 3840x2160 targets when AnyPS5 reported a 1080p display
  ([AnyPS5 #6754](https://github.com/boykopovar/AnyPS5/issues/6754#issuecomment-6612522503)), and
  Digital Foundry saw no dynamic resolution in GTA 6 footage (via secondary summaries).
- On the 6 GB model, use the same preset. Its GC thresholds are fractions of the budget the
  driver reports, so they shrink with the card, and the recycled-image pool is budget/32. What
  does not shrink is the roughly 1 GiB the emulator holds before the game allocates anything
  (mostly the 768 MiB BDA page table, whose layout is baked into compiled shaders). That is a
  sixth of a 6 GB card, so expect about 4.5 GB for the game once Windows has its share. Try
  `--video-out-resolution 1080p` as soon as the game renders in-game instead of waiting for spill
  lines, and compare the `VRAM:` and `VRAM spill:` lines with and without it.
- With less than 14 GiB of RAM, add `--ram-profile low` after the preset:
  `kyty_emulator --game <path> --preset rtx3050 --ram-profile low --session-log auto`. Preflight
  adds it by itself on such a PC. It costs speed: fewer compile threads, no pipeline helpers and
  so no precompile pause, and smaller host rings. So leave it off with 16 GB or more. Size the
  page file first, as "A PC with 8 GB of RAM" describes. Without enough commit the emulator may
  not start at all.
- If the first boot freezes on a loading screen, it is probably compiling pipelines. Wait. There
  is no GTA 6 pipeline manifest, so the first boot compiles everything cold, and the
  `--pipeline-precompile ask` pause (the default) only stops the game for a recorded manifest.
  Record one with `--record-pipeline-manifest` and copy `_PipelineCache/<ID>_<VER>.kpm` into
  `assets/pipelines/` for later runs. Under preflight, `_PipelineCache` is
  `%LOCALAPPDATA%\KytyPS5\preflight-cache\_PipelineCache`, and every later preflight boot of every
  build loads a manifest recorded there. The pause needs a pipeline helper, and
  `--ram-profile low` runs none. Add `--pipeline-helpers 1` after it to get the pause back, for
  about 400 MB of RAM.
- Start with `--native-raytracing` off, as the preset has it, then compare once the game is
  in-game: add `--native-raytracing` after the preset and compare fps and the `VRAM:` lines. On
  GTA V it took Fidelity on an RTX 3090 from about 13 fps to 20-30 (capped at 30). GTA 6 has
  ray-traced GI and reflections on the base PS5 (Digital Foundry, via secondary summaries), so
  ray traversal will be a large share of the frame. The catch is that native RT only engages on
  traversal shaders it recognizes, which so far means GTA V's sce::Psr shapes, and its BLAS arena
  takes about 768 MiB of a 6 GB card.
- Once the game gets in-game and you want to play rather than test, run the emulator yourself
  from one fixed folder, for example the extracted artifact:
  `kyty_emulator --game "D:\PS5\GTA6" --preset rtx3050 --title-profile gta6 --shader-cache --session-log auto`.
  Saves (`_SaveData`) and caches (`_PipelineCache`) live in the working folder, so the same
  folder keeps your progress and the compiled shaders. Preflight is different. Each run gets a new
  folder, so saves don't carry over, and only its shader cache is shared between runs. A folder
  you play from has its own cache and starts cold the first time.
- Use the NVIDIA Studio or Game Ready driver from 2026, and turn Resizable BAR on in the BIOS if
  the board supports it.
- If the game ran on an earlier build and broke on this one, rerun with `--preset safe`. If that
  fixes it, bisect the kill switches one at a time.

## A PC with 8 GB of RAM

Nothing here has run on such a PC or with GTA 6. On 8 GB of RAM, whether a game boots depends first
on the page file, then on physical RAM. The GPU comes after both.

**Why.**

- The emulator commits all of its guest memory at startup. It is one pagefile-backed section of
  about 12,736 MiB, whatever the game, created with SEC_COMMIT (`memoryAddressSpace.inc`,
  `memory.cpp`). GTA V splits it into 448 MiB of flexible memory and 12,288 MiB of direct memory.
  Windows backs every committed byte with RAM or the page file, so the commit limit is the RAM plus
  the page files.
- NVIDIA's Windows driver also seems to charge commit for the video memory it allocates. An AnyPS5
  probe measured about 1 GiB of commit per GiB of VRAM; nobody has measured it here. With
  host-visible memory and the emulator's heap on top, a session can want an estimated 26 to 33 GB
  of commit.
- Physical RAM is the harder limit, and no setting fixes it. A 12.4 GiB guest plus 2 to 4 GB for
  the emulator and the driver does not fit in what 8 GB leaves after Windows. Expect constant
  paging and multi-second frames in places.
- A blue screen is possible. The emulator makes GPU-written guest pages PAGE_NOACCESS while it
  watches them for CPU reads. chenxiao07 traced MEMORY_MANAGEMENT 0x1A/0x41202 to Windows
  mishandling such pages of a pagefile-backed section once they are paged out, and reproduced it
  with a standalone program on Windows 11 build 26200
  ([chenxiao07/KytyPS5#5](https://github.com/chenxiao07/KytyPS5/issues/5)). It has not been seen
  on this branch, upstream or TheCruZ's fork.
  - Their fix makes such pages guard pages instead (91a15d6c). Their repro ran twice for 30 minutes
    on guard pages without a blue screen.
  - They keep PAGE_NOACCESS only on pages they have locked in RAM, 32 MiB at most (d8f99391a5).
  - This branch does both under `--ram-profile low` on Windows. It locks the watched pages in RAM, up
    to an eighth of the RAM (about 1 GiB with 8 GB). Before a run of guest pages becomes NOACCESS, it
    locks the same pages of the section's read-write view with VirtualLock, and it unlocks them when
    the watch ends. Pages locked when their watch starts keep PAGE_NOACCESS.
  - The watched pages of the section that the lock does not take become
    PAGE_GUARD | PAGE_READONLY guard pages, as in the other fork. That covers pages past the cap
    and pages Windows refuses to lock, or every watched page with `--lock-read-watched-mb 0`. A
    guard page needs no lock, so a finished watch keeps the budget it frees for the next watches
    instead of offering it to these pages. `--read-watch-guard-pages off` makes them PAGE_NOACCESS
    without a lock again, as before, and a finished watch then offers them the budget it frees.
  - A guard page is one-shot, which is why locked pages keep PAGE_NOACCESS. Windows takes the guard
    off for every thread at the first access. Until the emulator has read back what the GPU wrote
    there, other threads read the page without a fault and see the bytes from before. Their writes
    still fault, since the guard is over read-only. They wait for the readback, and the write
    tracking sees them, as on a NOACCESS page. The other fork put each guard back at once and still
    saw 1 fault in about 90 whose page another thread read first, on a game whose job threads read
    the same GPU results together. Here the window lasts until that first access is served. Stale
    reads can show as flicker, dark or wrong lighting, or physics glitches, and only on guarded
    memory.
  - A Windows API that reads or writes a guest buffer in the kernel, such as a file write or a
    socket send or receive, takes the guard without the emulator seeing a fault. The call fails,
    or uses the bytes from before the GPU's, and the watch is lost. A user-mode access, such as a
    CRT memcpy, faults like any other. Guest file writes and socket sends and receives therefore
    read the guarded pages of their buffer first, which serves their watches. Guest file reads
    already go through a host buffer, and the save-data code copies a save's parameters before it
    writes them.
  - Two kinds of page stay NOACCESS without a lock: watched pages outside the section (private
    memory such as guest stacks; the bug check is a section view's), and pages the game protects
    with PROT_NONE itself. On the `RAM usage:` lines, `guarded` shows how much is guard pages, and
    `unlocked` how much of the section is NOACCESS without a lock (0 with guard pages).
  - Nobody has run the lock or the guard pages in a game or on a PC with 8 GB of RAM, so whether
    they keep the blue screen away, and what the stale reads cost in GTA 6, is unverified.

**Before the first boot.**

1. Add a page file. Keep the existing one, since Windows writes crash dumps through the one on C:.
   Add a fixed-size one on a drive that still has room after the dump: 32,768 MB with 42 GB or
   more free, 16,384 MB with 26 to 42 GB free. With less, make the one on C: bigger instead.
   1. Press Win+R and run `SystemPropertiesPerformance`, then Advanced, Virtual memory, Change.
   2. Untick "Automatically manage paging file size for all drives" first. The drive list and the
      size boxes stay greyed out until you do.
   3. Pick the drive and choose Custom size, with the same initial and maximum size.
   4. Click Set, then OK, then restart Windows.
2. In the next session log, the `RAM:` line should show a headroom of 12,288 MiB or more, with no
   `RAM warning:` line. The headroom counts only the page files' current size. Windows may grow a
   page file it manages past that, but a fixed size avoids the stall while it grows, so set one
   anyway.

**Every session.**

- First close browsers, Discord, game launchers, overlays, OneDrive and Teams. Task Manager's Memory
  page should show about 2.5 GB in use at most.
- Start through preflight or `kyty_emulator` directly, not the launcher, which stays running.
- After a crash, wait until `kyty_emulator.exe` and `WerFault.exe` have left Task Manager. A dying
  emulator still holds its 13 GB of commit.
- Don't use RAM cleaners, don't turn off the page file or memory compression, and don't run with a
  system-managed page file on a nearly full drive.

**`--ram-profile low`** trades speed for memory. `normal`, the default, keeps every built-in value,
and the emulator never picks low by itself. Preflight adds it after `--preset rtx3050` on a PC with
less than 14 GiB of RAM, unless `-ExtraArgs` names a RAM profile.

| setting | normal | low | cost |
|---|---|---|---|
| automatic pipeline helpers | 4 on 12 threads, 2 under `--vram-profile low` (which `--preset rtx3050` sets); more during a precompile pause, bounded by free RAM | none | slower cold compute compiles |
| `--pipeline-precompile` pause | the game stops while a recorded manifest compiles | off, since it needs a helper | stalls in play instead; `--pipeline-helpers 1` brings the pause back for about 400 MB |
| shader compile workers | 6 | 3 | stutter in new areas |
| async pipeline threads | 3 | 2 | slower background compiles |
| shader cache and manifest threads | half the hardware threads | 2 | slower WarmUp |
| host upload / download rings | 512 / 64 MiB | 128 / 32 MiB | bursts take one-time buffers or wait |
| driver cache saves | every 60 s, 20 s after a slow pipeline | 180 s / 60 s | up to 3 minutes of the cache lost in a crash |
| fresh guest stacks | cleared | left as the OS zeroed them | none expected |
| spilled images in the recycle pool | yes | no | more allocations under pressure |
| `--oom-recovery` | off (on under `--preset rtx3050`) | on | slow oversubscription instead of an exit |
| read-watched guest memory locked in RAM (Windows) | none | all of it, up to an eighth of the RAM (`--lock-read-watched-mb`) | up to that much RAM Windows can no longer page out (about 1 GiB with 8 GB), VirtualLock calls that may read pages back from the page file, and about 30 MiB of bookkeeping at 1 GiB locked |
| read-watched guest memory without a lock (Windows) | PAGE_NOACCESS | guard pages (`--read-watch-guard-pages`) | other threads can read such a page stale until its first access is served |

`--pipeline-helpers`, `KYTY_PIPELINE_HELPERS` and `--no-oom-recovery` still win.
`--lock-read-watched-mb <MiB>` sets a smaller lock budget, and `0` turns the lock off.
`--read-watch-guard-pages off` leaves the memory without a lock PAGE_NOACCESS, and `on` makes guard
pages under the normal profile too. Under the recommended `--preset rtx3050` the `--oom-recovery`
row changes nothing, since the preset already turns OOM recovery on. The savings are estimates:
- 416 MiB of host-visible memory from the rings;
- about 400 MB per helper that does not run;
- fewer concurrent shader translations and driver compiles.

The smaller rings can also cost memory, through the one-time buffers that take what they cannot
hold. The `oneoff` field counts those. `ram_policy_tests` pins both profiles, and a `static_assert`
at each use pins the normal value to the literal it replaced.

**What the logs show.**

- `RAM:` appears once at startup, with the guest section committed. It has the RAM and available
  RAM, commit and its limit, the headroom, the page file, the guest section and the profile. On
  Windows the page file is the commit limit less the RAM, marked `~`, and `~0` means there is
  none. Under low, `RAM profile:` lists its settings.
- On Windows, `RAM warning:` appears when the headroom is under 12 GiB. It names the page file to
  add (20 GiB less the headroom, rounded up to 4 GiB), and says that a page file Windows manages
  may grow on its own.
- With a session log, which preflight always opens, a `RAM usage:` line appears every 30 s. On
  Windows the first one at 95% of the commit limit adds a second `RAM warning:`. Without a session
  log neither appears. Its fields:
  - `ws` is the working set and `private` the private bytes. The guest section is a shared section,
    so `private` leaves out its 12.4 GiB; `commit` against its limit shows it.
  - `commit` is the system's commit charge and its limit, and `avail` the available RAM.
  - `faults` is the process's page faults per second, soft ones included. `hard` is `?` on Windows,
    where `memory.csv` has the pages read from disk.
  - `vk_local` and `vk_host` are the VRAM heaps' usage and budget.
  - `spilled` is what went outside VRAM so far. It never goes down, so it shows that spill
    happened, not that it eats host memory now. `vk_host` against its budget shows that.
  - `noaccess` is the guest memory the read watchers hold now: NOACCESS, or guard pages with
    `--read-watch-guard-pages`.
  - `oneoff` counts the one-time buffers that took transfers the host rings could not, so far,
    with their MiB.
  - `locked` is the read-watched memory locked in RAM now, then the budget. It reads `0/0` when the
    lock is off.
  - `unlocked` is the read-watched memory of the guest section that is NOACCESS without a lock now,
    because the budget or Windows refused it. That is the memory the blue screen needs. With guard
    pages it is 0: those pages are guarded instead.
  - `guarded` is the read-watched memory of the guest section that is guard pages now: past the
    budget or refused by Windows, or all of it without a lock budget. A page whose guard went with
    an access still counts until its watch ends.
  - `lockfail` counts the page locks Windows refused so far.
  - `slowlocks` counts the VirtualLock calls that took over 1 ms so far, then gives the longest one
    in ms.
- With a lock budget, a `RAM lock:` line appears at startup. It gives the budget and the working
  set raised to make room for the locks.
  - The room starts at 64 MiB and grows 64 MiB at a time as the locks need it, up to the budget.
  - If Windows refuses the first raise, the emulator asks for half as much, down to 1 MiB, and the
    line names the refusal. If every raise fails, the line says the lock stays off.
  - More `RAM lock:` lines appear in three cases: when the cap cut `--lock-read-watched-mb`, on the
    first VirtualLock or VirtualUnlock that failed, and when Windows refused more locks or more
    room. That last line gives the budget from then on.
- A `RAM usage:` line written with less than 512 MiB available, or with commit at 90% of its limit
  on Windows, is also synced to disk, so that it can survive a blue screen. Other lines reach the
  disk when Windows flushes its cache.
- A fatal report ends with one more `RAM usage:` line, without the VRAM fields.
- If the guest section cannot be created, the "could not reserve <n> MB for guest direct memory"
  report adds the free commit, the limit and the page file that would fit. When the commit had
  room, it names no page file, because the page file was not what failed.
- Preflight writes these to `system.txt`:
  - the commit limit, the page files and the free space per drive;
  - the 10 processes with the most memory (names only);
  - 14 days of events 41, 1001, 2004 and 4101.

  It warns before a boot with less than 25,024 MiB of free commit, and says so when the page file
  can still grow. It suggests `-BootSeconds 0` under low. `memory.csv` gets a row when the boot
  starts and every 30 s after. Its `pages_input_per_s` counts pages read from disk per second, not
  faults.
- `kyty_triage.py` adds a `host_memory` section and five rules: a warning line, commit at 95% or
  more on Windows, under 300 MiB available in most samples, under 14 GiB of RAM with the normal
  profile, and the blue-screen hint.
  - The hint needs a log that stops low on RAM after NOACCESS samples. It quotes `unlocked` from
    the last sample. It ranks under the plain no-fatal rule, because a session log never has a
    clean-exit line and preflight stops its boots.
  - A line that a zip holds twice, in `session.log` and `emulator-boot.log`, counts once.

**What the first log answers.**

- Did the commit limit hold? Look at `headroom`, then at the commit of the last `RAM usage:` line.
- Is physical RAM the wall? Look at `avail` and `faults`, and at `pages_input_per_s` in `memory.csv`.
- Is spill eating host memory? Compare `vk_host` with its budget.
- Did the smaller rings save memory or cost it? Look at `oneoff`.
- Is the blue-screen pattern live? Look at `unlocked` and `guarded`, and for Event 1001 in
  preflight's `system.txt`. `unlocked` above 0 means NOACCESS memory without a lock, so the guard
  pages were off: turn them on. `guarded` above 0 means guard pages took what the lock could not.
- Do the guard pages cost correctness? With `guarded` above 0, look for flicker, dark or wrong
  lighting, or objects in the wrong place. If they go away with `--read-watch-guard-pages off`,
  the stale reads of the guard window are the cause, and NOACCESS brings back the blue-screen
  risk the guards avoid.
- Does the lock stall the game? Look at `slowlocks`.
- Where did memory peak? Compare the `RAM usage:` times with the `Shader cache:` lines.

## Day-one triage playbook

1. **Before the 18th, run the GPU tests.** Extract the artifact, open `preflight`, double-click
   `preflight.cmd` and send the zip it names. That is the first time any of the branch's GPU code
   runs on a GPU.
2. **Import report and first boot.** With the decrypted dump, from a command prompt in
   `preflight`, run `preflight.cmd -Game "D:\PS5\GTA6" -ExtraArgs "--title-profile gta6"`. It
   writes the `--dump-imports` report, which needs no GPU, boots the game for 5 minutes with
   `--preset rtx3050` (plus `--ram-profile low` under 14 GiB of RAM), a session log and the
   shared shader cache, and zips everything. The first
   boot compiles cold. Repeat boots of the same build start warm. Without preflight, run
   `kyty_emulator --game <path> --dump-imports imports.txt`, boot with
   `--preset rtx3050 --session-log auto`, and zip the logs with `tools/triage/collect_logs.ps1`.
3. **Name and triage.** On the machine doing the fixes, build the names file once with
   `python3 -I tools/nid/build_names.py`. `python3 -I tools/nid/name_imports.py --hle imports.txt`
   then lists the HLE work queue by library. Compare it with GTA V's list in `tools/nid/README.md`;
   whatever is new in GTA 6 is where to start. Then run
   `python3 -I tools/triage/kyty_triage.py --nid-names <names file> <zip or log>`, and read the
   primary failure and the top fix area. `--agent-prompt` turns it into a brief for a coding agent.
4. **Fix by category:**
   - Unresolved imports. Implement the function in the `src/libs/<lib>` file the report names, or
     give it a plausible return. If the game misbehaves after an unresolved call returns 0, try
     `--unresolved-offline-returns` (Np/Net/Http) before writing code. `--strict-imports` stops
     at the first call, for a stack.
   - `Soft-fail:` lines. These were EXITs. Each one is an unhandled value from a newer SDK.
     Decide whether it needs real handling.
   - `[HACK] skip unsupported shader instruction`. The decoder text names the family, opcode and
     raw dwords. Add the opcode in `frontend/decode/*Ops.cpp` and its translation, with a decode
     test in `tests/ShaderRecompilerComputeTests.cpp`. Check the encoding with
     `llvm-mc -arch=amdgcn -mcpu=gfx1030 -show-encoding`. Missing geometry or lighting usually
     means one of these.
   - `shader resource tracking:` fatals. These were the main GTA V crash class (see the known
     hashes in `tools/triage/rules.json`). Start in `ir/passes/ResourceTracking.cpp`.
   - Vulkan validation errors and device lost. Rerun with `--device-fault-report`. NVIDIA-only
     failures go behind a vendor check, never globally.
   - `Unhandled host exception` with `guest pc: eboot.bin+0x...`. That is a guest crash, usually
     from a stubbed function's bad return value further up. Read the unresolved imports that ran
     just before it.
   - `VRAM spill` or `VRAM pressure` lines, or a slow game. Compare `--vram-limit-mb` runs on a
     bigger card if one is around.
5. **Re-run and compare.** `kyty_triage.py --compare old.json new.log` shows which failures went
   away and which are new.
6. **Keep fixes per-title** unless they fix a real emulation bug. Add GTA 6 quirks to the `gta6`
   profile in `src/common/titleProfile.cpp`, and its IDs once known.

## Known risks

- **Only the GPU tests have run on a GPU.** On an RTX 3050 6 GB, 39 of 42 pass and one real NVIDIA bug (64-bit LDS atomics) turned up, but no game has run on this branch: the VRAM policy, spill logging and the new opcodes' behaviour in a real frame are untested. Run a
  game that already works before trying GTA 6, so a regression shows up as a regression.
- **The dump.** Running GTA 6 at all needs a decrypted dump from a jailbroken PS5. If the game
  needs firmware newer than any public exploit, dumps may be late or need patching, and none of
  this matters until one exists.
- **Psr fingerprints.** The legacy decode mode is reasoned out, not checked against GTA V's real
  shaders. If GTA V's native RT stops engaging on this branch, look at `psrShaders.cpp` first.
- **Global fork behaviour.** The fork's perf work (GPU stage threads, overlapping dispatches,
  deferred barriers, the large-module SPIR-V optimizer) is on for every game. It was tested on
  GTA V and a few others. `--preset safe` exists for exactly this.
- **Defaults that changed.** These are deliberate, but they are changes. GTA V's legacy
  entitlement labels now go only to GTA V. An unsupported shader instruction skips a draw instead
  of exiting, and several boot-path EXITs now return errors.
- **Silent wrong results.** Returning 0 or an error instead of crashing can push a failure later
  and make it harder to read. The once-per-site `Soft-fail:` and first-call import lines exist to
  keep that visible.
- **8 GB, and worse, 6 GB.** No eviction policy makes a 12 GB working set fit in 8 GB, let alone
  in the 4.5 GB or so a 6 GB card has left. Spill to system RAM over PCIe 4.0 x8 is the likely
  steady state in dense areas, and on the 6 GB card probably everywhere in the open world.
- **Look-alike downloads.** In October 2026, fake AnyPS5 and KytyPS5 repositories shipped an
  infostealer that adds itself to Windows Defender's exclusions. Only use this fork's CI artifact
  or the official projects' release pages, never builds posted in comments, Discord DMs or videos,
  and never add a broad Defender exclusion for an emulator.
- **This branch is behind both of its sources.** Upstream main is about 50 commits past
  `bc934ef0`. Those include nmzik's generic native ray tracing (2026-11-12 to 11-15), which mirrors
  guest BVHs into host acceleration structures and traces recognized traversals with ray queries.
  TheCruZ has since rebased his fork and added CPU-side work such as resolving draws on a second
  thread and draw templates. None of it is on gta6-prep yet.
- **Firmware.** One unverified report says GTA 6 needs PS5 firmware 14.22, which could delay
  usable dumps.
- **The estimates below.**

## How likely is "kinda playable" on an RTX 3050?

Short answer: unlikely at launch, possible after weeks of fixes, and capped by hardware even
then. Here is the reasoning, so it can be argued with.

**The hardware gap.** The PS5 GPU is a 36-CU RDNA2 part around 10 TFLOPS with 448 GB/s and 16 GB
shared by CPU and GPU, about 12.5 GB of it for the game. The desktop RTX 3050 8 GB has 224 GB/s,
half the bandwidth. In games it lands around 55 to 65% of a PS5 in rasterization at equal
settings. Rockstar says GTA 6 targets 30 fps on every console, and only the PS5 Pro adds PSSR and
ray-traced shadows ([Game Informer, 2026-11-10](https://gameinformer.com/2026/11/10/grand-theft-auto-vi-will-run-at-30-fps-on-all-consoles-but-ps5-pro-will-feature-improved)).
Digital Foundry's analysis of base-PS5 footage found a fixed internal resolution of about 1440p
with no dynamic resolution, and ray-traced global illumination and reflections. I only have that
through secondary summaries such as
[GameRant](https://gamerant.com/gta-6-ps5-pro-upgrade-30-fps/). Natively ported, the same frame
would run around 17 to 20 fps on a 3050 8 GB, before any emulation cost.

**The emulation cost.** KytyPS5 translates every shader, emulates wave64 on NVIDIA's 32-wide
hardware by running both halves in one invocation, syncs guest memory with the GPU through the
BDA page table, and runs the PS5 command processor on CPU threads. GTA V shows what that costs.
It is a PS4-era game that TheCruZ's fork runs at 70 to 120 fps in Performance mode on an RX 7900
XT, a card around 3 times a PS5. GTA 6 is a current-gen-only title built for that 30 fps target,
so the margin GTA V has is not there. Expect the emulator to cost somewhere between 1.5x and 3x
on top of the hardware gap. That puts a 3050 around 6 to 13 fps at the game's own resolution.

That range assumes ray tracing costs no more under emulation than anything else, and GTA V's
Fidelity mode says otherwise. On an RTX 3090 that mode runs at about 13 fps with its ray
traversal emulated in shaders, and 20 to 30 with native RT. Against a native port of that mode,
that is an overhead of roughly 4.6 to 6x without native RT, and 2.4 to 3.1x with it. GTA 6
traces rays on the base PS5, so carried over to the 6 GB card, that gives about 1.5 to 5 fps on
day one with the traversal emulated. If native RT learns GTA 6's traversal shaders, it rises to
about 3 to 9. The inputs are weak (a 30 fps-capped range, one AMD profile, no GTA 6 data), so
treat these as an order of magnitude.

**The resolution lever.** Neither this emulator nor AnyPS5 can lower a game's internal
resolution. `--video-out-resolution` only changes the display class the game is told about. GTA V
ignored that class under AnyPS5, and Digital Foundry saw no dynamic resolution in GTA 6. So the
lever probably does nothing. Both projects can load per-title binary patches (`--game-patch`
here), so a community resolution patch could plug in later, but none exists. And if about 30% of
the frame does not scale with resolution, then below about 6 fps no resolution cut reaches 20
fps, because that part alone fills the 50 ms budget.

**Memory.** 8 GB minus about 1 GB of fixed emulator overhead leaves around 7 GB for a game
designed around up to 12.5 GB shared. Expect system-RAM spill in dense areas, with stutter.

**The 6 GB model.** The RTX 3050 6 GB is a cut-down card, not the 8 GB one with less memory:
2304 shader cores at up to 1.47 GHz instead of 2560 at 1.78 GHz, a 96-bit bus with 168 GB/s
instead of 224 GB/s, and 70 W. That is about 25% less compute and bandwidth, and in games it
runs roughly a quarter slower than the 8 GB card. Against a PS5 that is around 40 to 50% in
rasterization, so about 12 to 15 fps for a native port of a 30 fps frame and around 4 to 10 fps
with the emulation cost. After the emulator's fixed 1 GiB and Windows, about 4.5 GB is left for
the game.

**Compatibility.** This comes before any of the above. GTA V needed about two months and 188
commits on top of upstream to get where it is. Some of that carries over, since it is the same
engine family, the same sce::Psr library, the same bindless descriptor style and the same
streamer patterns. A lot won't.

My estimates, for a stock RTX 3050 8 GB with a reasonable 6-core CPU:

| milestone | at launch week | after ~1 month of log-driven fixes | after ~3 months |
|---|---|---|---|
| Boots to an intro or menu | 35% | 65% | 80% |
| Gets in-game | 15% | 45% | 65% |
| "Kinda playable" (in-game, 15+ fps most of the time, game-breaking glitches rare) | under 5% | 10% | 20 to 25% |

For a stock RTX 3050 6 GB, booting depends on compatibility more than on the card, so the first
row barely moves; the others drop:

| milestone | at launch week | after ~1 month of log-driven fixes | after ~3 months |
|---|---|---|---|
| Boots to an intro or menu | 35% | 60% | 75% |
| Gets in-game | 15% | 40% | 60% |
| "Kinda playable" (same definition) | about 1% | 5% | about 10% |

On the 6 GB card, 15 fps needs GTA 6 to lower its own resolution (through
`--video-out-resolution 1080p` or its own dynamic resolution) and the emulation cost to land at
the low end of its range.

The 3 month row is capped by the hardware math above, not by bugs. On this card, 15 fps in the
open world is the realistic ceiling unless GTA 6 lowers its own resolution. A Ryzen 5 5600 with
an RX 7900 XT, like TheCruZ's GTA V reporter, would be in a different league.

20 fps in the open world on the 6 GB card is not in the tables, because nothing I found supports
it through any emulator. A native PC port at lowered settings could get there. A PC version is
rumored for Q1 2027 by anonymous sources
([GamesBeat](https://gamesbeat.com/grand-theft-auto-vis-pc-release-is-slated-for-q1-2027/)), and
Rockstar has not confirmed it.

## AnyPS5 instead?

[AnyPS5](https://github.com/boykopovar/AnyPS5) converts a decrypted PS5 executable into a native
Windows or Linux one and links it against its own versions of the system libraries. The game's
CPU code runs natively, as it does here. The GPU side is still translated to Vulkan. I checked
it on 2026-11-16, as a candidate for GTA 6 on Karim's card. It is not a better bet, for these
reasons.

- **GTA V is far slower there today.** These are all from its GTA V threads:
  - On an RTX 4070 Ti with a 5800X3D, at 4K output on main plus three open PRs, the open world
    ran 6 to 7.5 fps in Performance mode and about 1 fps in Fidelity
    ([#6754](https://github.com/boykopovar/AnyPS5/issues/6754#issuecomment-6921271777)).
  - On an RTX 3080 Ti with a Ryzen 9 5900X (TheCruZ's CPU), the prologue bank ran 12.5 fps on
    main plus one PR with profiling on, 16.3 with three PRs, and 2.3 in Fidelity
    ([#1203](https://github.com/boykopovar/AnyPS5/discussions/1203#discussioncomment-19655022)).
  - TheCruZ's Kyty fork reports 60 to 140 fps in Performance and 20 to 30 in Fidelity on an
    RTX 3090. Scenes and GPUs differ, so roughly: 4 to 11 times faster in Performance and 9 to 13
    in Fidelity.
- **Its limit is per-draw CPU work.** Its maintainers say heavy titles need a new draw model,
  which is being drafted with "no dates promised"
  ([#17533](https://github.com/boykopovar/AnyPS5/issues/17533)). Even a deliberately wrong test
  that skipped most per-draw work stayed under 19 fps in GTA V's open world on a 7800X3D with an
  RX 7900 XTX ([#7895](https://github.com/boykopovar/AnyPS5/issues/7895#issuecomment-6645138156)).
- **6 GB cards have not done well there.**
  - On a 6 GB RTX 4050 Laptop, Astro Bot overran its 5152 MiB budget and spilled to host memory
    at 0.5 to 3.5 fps ([#7210](https://github.com/boykopovar/AnyPS5/issues/7210#issuecomment-6423707965)).
  - On an RTX 3050 6 GB Laptop, Crash Bandicoot 4 ran at about 8.7 fps with black geometry
    ([#9065](https://github.com/boykopovar/AnyPS5/discussions/9065)).
  - GTA V's Fidelity mode ran out of device memory on an 8 GB RTX 4060
    ([#1203](https://github.com/boykopovar/AnyPS5/discussions/1203#discussioncomment-19048719)).
  - Its docs also name a cost specific to NVIDIA on Windows: the driver reads imported guest
    memory slowly, so draw inputs get copied.
- **Kyty's GPU code measured faster in AnyPS5's own threads.** AnyPS5 contributors compared the
  two on the same machines in Ghost of Yotei. On an RTX 3080 Laptop, the KytyPS5-Yotei fork ran 63
  presents per 10 s against 10 for AnyPS5's best branch. Kyty's backend, ported into AnyPS5, ran
  about 7 times faster than AnyPS5's own driver on an RTX 5080, in under 12 GiB of process memory
  against 30 to 35
  ([#675](https://github.com/boykopovar/AnyPS5/discussions/675#discussioncomment-19605082)).
  KytyPS5-Yotei is a Yotei-only fork of upstream KytyPS5, not this branch, so this shows what the
  backend can do, not how fast this branch is.
- **No GTA 6 progress.** Its only GTA VI thread
  ([#20383](https://github.com/boykopovar/AnyPS5/discussions/20383)) is an empty placeholder. No
  maintainer has said anything about GTA 6. It has no render-resolution override either.
- **Safety is fine if you are careful.** GitHub Actions builds and uploads its releases, but they
  are not signed or attested. On 2026-11-12 alone, 820 PRs were merged, and main requires no
  approvals. Main breaks often: it has drawn GTA V upside down since 2026-11-15. Converted games run with full
  user rights, no sandbox and no ASLR.

It moves fast, so look again a few weeks after launch. For launch week, it would add a second
unknown on top of the first.

## Where things live

| area | files |
|---|---|
| Title profiles, presets | `src/common/titleProfile.*`, `src/common/presets.*`, `src/main.cpp` |
| Kill switches | `src/common/emulatorConfig.*` (blocks marked `// gta6-prep: config`) |
| Imports, soft-fails | `src/loader/runtimeLinker.cpp`, `src/loader/nid.*`, `src/libs/*` |
| VRAM | `src/graphics/host_gpu/renderer/cache/memoryPolicy.h`, `src/common/vramOptions.h`, `src/graphics/host_gpu/vma.cpp` |
| Shader skip and opcodes | `src/graphics/shader/recompiler/frontend/decode/*`, `.../translate/*`, `pipelineCache.cpp` |
| Diagnostics | `src/graphics/presentation/window/vulkanWindow.cpp`, `src/loader/runtimeLinker.cpp` (fault report), `src/common/logging/log.cpp` |
| Triage | `tools/triage/` |
| Preflight | `tools/preflight/`, `.github/scripts/package_gpu_tests.py`, `.github/scripts/ctest_groups.py` |
| NID names | `tools/nid/` |
| RAM | `src/common/ramOptions.h`, `src/common/ramLog.*`, `src/kernel/readWatchLocks.h` (the lock bookkeeping), `src/graphics/host_gpu/renderer/cache/streamBuffer.*` (one-time buffer count), `tools/preflight/preflight.ps1`, `tools/triage/` |
| CI | `.github/workflows/gta6-ci.yml`, `.github/scripts/run_tests.py` |
| Tests | `tests/TitleProfileTests.cpp`, `HleSoftFailTests.cpp`, `VramPolicyTests.cpp`, `DiagTests.cpp`, `ShaderRecompilerComputeTests.cpp` |
