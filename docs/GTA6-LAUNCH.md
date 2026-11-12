# GTA 6 launch prep (gta6-prep)

This branch prepares Karim's fork of KytyPS5 for GTA 6 (out 2026-11-19) on a budget PC with an
RTX 3050 8 GB. Nobody has the game yet, so nothing here has run it. Everything is preparation:
the TheCruZ GTA V fork brought onto current upstream, hardening for a newer-SDK title, an 8 GB
mode, more shader coverage, and tools for reading the first crash logs.

Read the "Verification status" section before trusting any of it. As of this writing, nothing on
this branch has been compiled. GitHub disabled Actions on the fork mid-run, and builds on this
machine are off the table.

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
Python tests. It runs on pushes to `gta6-prep` and `gta6/**`, or on demand. Host tests gate the
job. Tests that open a Vulkan device run in a separate step that only reports, because GitHub
runners have no GPU and lavapipe lacks fragment barycentrics and 64-bit image atomics. Upstream's
own CI never runs those tests either.

## Verification status

Read this before relying on anything above.

| what | status |
|---|---|
| Upstream main `bc934ef0` under gta6-ci | Built on Linux. 46 of 81 tests pass; the 35 failures all need a real GPU (the harness asks for fragment barycentrics and 64-bit LDS/image atomics). `vdecsw_tests` does not compile with clang-cl, and upstream never builds it on Windows. |
| TheCruZ fork, own CI | Green on Linux and Windows at `c00f2f27` (run 43081193192 in TheCruZ/KytyPS5-GTA). |
| Merge `8025b6bd` | Did not compile (missing `c00f2f27` pieces). Fixed in `58cd1f5c`, which has not been built. |
| Everything after `58cd1f5c` | Not built. Not tested. Every changed C++ file (83 of them, launcher excepted) passes `clang++ -fsyntax-only` on Linux. That catches typos and type errors, not link errors and not clang-cl-only problems. |
| Static build review | A separate reviewer checked link inputs of every new test target, platform guards for clang-cl, and syntax-checked the 124 unchanged files that include a changed header. Nothing found. Static only. |
| Python triage tool | 56 tests pass locally, 2 skipped without pwsh. |
| GPU behaviour, NVIDIA behaviour, VRAM use | Unverified on hardware. The machine that did this work has no usable GPU. |

Commits whose correctness no test can show say `Speculative:` in their message body. Commits
without it still say "build + tests pending in gta6-prep CI" until a CI run proves otherwise.

The first job once Actions is back: run gta6-ci on this branch, one run at a time, and fix what
breaks. Expect a handful of compile or link errors. Upstream's Linux CI only runs its host tests,
so treat the GPU group as informational.

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

On Linux, follow "Building on Linux" in README.md. The CI artifact `KytyPS5-gta6-prep-Windows-x64`
is the easier route once Actions runs again.

## Recommended settings for an RTX 3050 8 GB

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
  `VRAM:` and spill numbers change. If they don't, the game ignores it, so turn it back off.
- If the first boot freezes on a loading screen, it is probably compiling pipelines. Keep
  `--pipeline-precompile ask` (the default) and wait. There is no GTA 6 pipeline manifest, so
  the first boot compiles everything cold. Record one with `--record-pipeline-manifest` and copy
  `_PipelineCache/<ID>_<VER>.kpm` into `assets/pipelines/` for later runs.
- Leave `--native-raytracing` off on a 3050. It was built for GTA V's sce::Psr shaders and adds a
  BLAS arena on top of an already full card.
- Use the NVIDIA Studio or Game Ready driver from 2026, and turn Resizable BAR on in the BIOS if
  the board supports it.
- If the game ran on an earlier build and broke on this one, rerun with `--preset safe`. If that
  fixes it, bisect the kill switches one at a time.

## Day-one triage playbook

1. **Before the first boot, dump the imports.** With a decrypted dump:
   `kyty_emulator --game <path> --dump-imports imports.txt`. This needs no GPU. The unresolved
   list per library is the HLE work queue. Add `--nid-names <names.txt>` if a names list is
   available. Diff it against a GTA V dump made the same way: whatever is new in GTA 6 is where
   to start.
2. **Boot with logs on.** Use `--preset rtx3050 --session-log auto`. Testers run
   `tools/triage/collect_logs.ps1` and attach the zip.
3. **Triage.**
   `python3 -I tools/triage/kyty_triage.py <zip or log>`. Read the primary failure and the top
   fix area. `--agent-prompt` turns it into a brief for a coding agent.
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

- **Nothing has been compiled.** That is the biggest risk, and the first thing to fix. See above.
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
- **8 GB.** No eviction policy makes a 12 GB working set fit in 8 GB. Spill to system RAM is the
  likely steady state in dense areas.
- **The estimates below.**

## How likely is "kinda playable" on an RTX 3050?

Short answer: unlikely at launch, possible after weeks of fixes, and capped by hardware even
then. Here is the reasoning, so it can be argued with.

**The hardware gap.** The PS5 GPU is a 36-CU RDNA2 part around 10 TFLOPS with 448 GB/s and 16 GB
shared by CPU and GPU, about 12.5 GB of it for the game. The desktop RTX 3050 8 GB has 224 GB/s,
half the bandwidth. In games it lands around 55 to 65% of a PS5 in rasterization at equal
settings. Reportedly GTA 6 targets 30 fps on the base PS5. Natively ported, the same frame would
run around 17 to 20 fps on a 3050, before any emulation cost.

**The emulation cost.** KytyPS5 translates every shader, emulates wave64 on NVIDIA's 32-wide
hardware by running both halves in one invocation, syncs guest memory with the GPU through the
BDA page table, and runs the PS5 command processor on CPU threads. GTA V shows what that costs.
It is a PS4-era game that TheCruZ's fork runs at 70 to 120 fps in Performance mode on an RX 7900
XT, a card around 3 times a PS5. GTA 6 is a current-gen-only title built for that 30 fps target,
so the margin GTA V has is not there. Expect the emulator to cost somewhere between 1.5x and 3x
on top of the hardware gap. That puts a 3050 around 6 to 13 fps at the game's own resolution.

**The resolution lever.** The emulator cannot lower GTA 6's internal resolution. If the game picks
its resolution from the output mode, `--video-out-resolution 1080p` could make a real
difference, maybe 1.5 to 2x. If it uses dynamic resolution keyed to frame time, it might scale
itself down. Neither is known.

**Memory.** 8 GB minus about 1 GB of fixed emulator overhead leaves around 7 GB for a game
designed around up to 12.5 GB shared. Expect system-RAM spill in dense areas, with stutter.

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

The 3 month row is capped by the hardware math above, not by bugs. On this card, 15 fps in the
open world is the realistic ceiling unless GTA 6 lowers its own resolution. A Ryzen 5 5600 with
an RX 7900 XT, like TheCruZ's GTA V reporter, would be in a different league.

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
| CI | `.github/workflows/gta6-ci.yml`, `.github/scripts/run_tests.py` |
| Tests | `tests/TitleProfileTests.cpp`, `HleSoftFailTests.cpp`, `VramPolicyTests.cpp`, `DiagTests.cpp`, `ShaderRecompilerComputeTests.cpp` |
