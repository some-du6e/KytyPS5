# Preflight: GPU tests and a first boot on your PC

GitHub's CI machines have no GPU, so the GPU half of gta6-prep has never run anywhere. Your RTX 3050
is the first place it can. Preflight runs it there and zips what happened into one file for you to
send to Claude.

There are two jobs. Before 2026-11-18, run the GPU tests. On launch day, run the import report and a
first boot of the GTA 6 dump.

## Get the build

1. Open the [gta6-prep CI runs](https://github.com/some-du6e/KytyPS5/actions/workflows/gta6-ci.yml?query=branch%3Agta6-prep)
   while signed in to GitHub. Pick the newest run whose page lists
   **KytyPS5-gta6-prep-Windows-x64** under **Artifacts** at the bottom. Go by that list, not by the
   check mark: a bisect run can be green with no artifacts, and a run can be red only because a
   preflight smoke test after the upload failed. If the run you picked has a red X, ask Claude
   whether to use it.
2. Click **KytyPS5-gta6-prep-Windows-x64**. You don't need the others: `ctest-linux`,
   `ctest-windows` and `preflight-smoke-windows` are CI logs, and `KytyPS5-gta6-prep-Linux-x86_64`
   is the Linux build.
3. Optional, and it saves a warning later: right-click the downloaded zip, choose Properties, tick
   **Unblock** at the bottom, click OK.
4. Right-click the zip, choose **Extract All**, click Extract. Run nothing from inside the zip itself.

The extracted folder has `kyty_emulator.exe` and a `preflight` folder next to it, with
`gpu-tests\tests.json` inside. No `preflight` folder means packaging failed in that run. Tell
Claude, and take an older run whose download has the folder.

## Before the 18th: the GPU tests

1. Close games and anything else that uses the GPU.
2. Open the `preflight` folder and double-click `preflight.cmd`.
3. If Windows shows "Windows protected your PC", click **More info**, then **Run anyway**. If it asks
   "Do you want to run this file?", click **Run**. You get neither if you unblocked the zip.
4. Wait. Each test prints one line, like `[3/42] device_creation ... PASS (1.2 s)`. A dot appears
   every 30 seconds while a long test runs.
5. At the end the window shows `Results: ...\kyty-preflight-<your PC>-<date>.zip` and
   `Send this file to Claude`. That zip sits in the `preflight` folder, next to `preflight.cmd`.
   Press a key to close the window, then send the zip.

Nobody has timed the 42 tests on a real GPU yet. Expect something like 10 to 30 minutes. A test that
hangs gets stopped after 5 minutes, and `shader_recompiler_compute`, which runs every case, after 30.
Failed tests are as useful as passing ones: the zip says which ones failed and why.

Save your work in other programs first. If the screen goes black for a moment during a test,
Windows reset the GPU because the test hung it. Preflight then waits a minute before the next
test, since Windows restarts the PC after five such resets within a minute.

For a short check first, open a command prompt in the `preflight` folder: click the folder's address
bar in Explorer, type `cmd` and press Enter. Then run:

```
preflight.cmd -Quick
```

That runs only the four quick tests: device creation and the GPU tests of this branch's own changes.
A couple of minutes.

To stop early, press **Ctrl+C** in the preflight window. The tests that already ran still get
zipped. Don't close the window with the X: that stops the running test too, but no zip gets
written. If that happened, or the PC restarted, zip the `kyty-preflight-<your PC>-<date>` folder
yourself (right-click it, **Send to**, **Compressed (zipped) folder**) and send that.

## Launch day: the GTA 6 dump

Open a command prompt in the `preflight` folder as above, and run this with your dump's folder, the
one that holds `eboot.bin`:

```
preflight.cmd -Game "D:\PS5\GTA6" -ExtraArgs "--title-profile gta6"
```

Don't end the folder path with a backslash: `"D:\PS5\GTA6\"` confuses the command prompt.

More emulator options go inside the `-ExtraArgs` quotes. Give a file with its full path, for
example `-ExtraArgs "--title-profile gta6 --nid-names D:\PS5\names.txt"`. The emulator runs in the
results folder, so a bare `names.txt` is not found.

Preflight then does three things:

1. **Import report.** The emulator loads `eboot.bin` and the `sce_module` PRX files, without the GPU
   and without running game code, and writes `imports.txt`: which functions the game needs that the
   emulator lacks. Seconds to a minute.
2. **Boot.** It starts the game with `--preset rtx3050`, the shader cache and a session log, lets it
   run for 5 minutes, then closes it. Leave the preflight window open. To play longer, add
   `-BootSeconds 0`: the game then runs until you close the game window. A boot that didn't crash
   shows as `RAN`, not `PASS`: a game stuck on a black screen also keeps running, so only the logs
   tell how far it got.
3. **Logs.** It collects the emulator's logs and zips everything.

Send the zip it prints, especially if the game crashed: the zip is how the crash gets fixed.

If the window got closed or the PC restarted during a boot, no zip gets written. Zip the results
folder yourself, without its `emulator` folder. Open the results folder, select everything except
`emulator`, right-click, and choose **Send to**, **Compressed (zipped) folder**.

## The shader cache

The first boot of a game compiles every shader, which can take minutes. Preflight keeps what a boot
compiled in `%LOCALAPPDATA%\KytyPS5\preflight-cache\_PipelineCache`, and the next boot of the same
build loads it from there. In each results folder, `emulator\_PipelineCache` links to that folder.
Windows calls this kind of link a junction.

- You can delete the cache whenever no game boot is running. The next boot compiles everything
  again.
- A new emulator build ignores the shaders an older build stored. The emulator checks this itself,
  so there is nothing to clear after you download a new build. The driver's pipeline cache in the
  same folder is kept across builds and can still help.
- A GPU driver update starts the driver's pipeline cache over.
- The cache can grow to gigabytes. Delete the `preflight-cache` folder to get the space back.
- Run one game boot at a time. Two at once would write to the same cache.
- `-ColdCache` boots without the cache, the way a first boot does, so you can compare the two. It
  leaves the cache as it is. The NVIDIA driver keeps a shader cache of its own as well, so a
  `-ColdCache` boot can still be faster than the very first one.
- If preflight cannot make the link, it prints one yellow line and the boot compiles everything.
  That happens when the results folder is on a drive that isn't NTFS, such as a USB stick formatted
  exFAT.

The `shader cache:` line of `summary.txt` tells a warm boot from a cold one. It says how many files
the cache held before and after the boot, or why the boot ran without it.

## Options

| option | what it does |
|---|---|
| `-Quick` | only the tests marked quick in `gpu-tests\tests.json` |
| `-Only a,b` | only the tests named `a` and `b` |
| `-Game <folder>` | the import report and a boot of the game, instead of the tests |
| `-BootSeconds <n>` | how long the boot runs, default 300; 0 runs until you close the game |
| `-ExtraArgs "<options>"` | more emulator options for the import report and the boot; paths in full |
| `-WithTests` | with `-Game`: run the GPU tests too, before the boot (`-Quick` and `-Only` apply) |
| `-ColdCache` | with `-Game`: boot without the shader cache, to compare with a first boot; the cache stays as it is |
| `-Emulator <path>` | another `kyty_emulator.exe`; the default is the one in the folder above `preflight` |
| `-OutDir <folder>` | where the results go; the default is the `preflight` folder |

Exit codes, for scripts: 0 when no step failed (every step `PASS`, or `RAN` for a boot), 1 when a
test or emulator step failed, 2 when preflight could not run because of a bad argument or a
missing file.

## What the zip holds

- `summary.txt`: one line per step, with the last lines a failed test printed. `summary.json` is
  the same for programs.
- `system.txt`: Windows version, CPU, RAM, GPU, driver, VRAM, `nvidia-smi` and `vulkaninfo --summary`
  output, and the versions of the Visual C++ runtime DLLs.
- `tests\<name>.log`: each test's command line and output.
- With `-Game`: `imports.txt`, `session.log`, `emulator-imports.log`, `emulator-boot.log` and the
  `kyty-logs-<date>.zip` that `collect_logs.ps1` makes.
- `console.txt`: what the window showed.

The emulator's working folder, `emulator` in the results folder, holds the game's save data, temp
files from the boot and the link to the shader cache. It stays out of the zip, and so does the
cache. Once the zip is sent, the results folder can go. Deleting it in Explorer removes the link
and leaves the cache alone.

Preflight writes your profile path as `%USERPROFILE%` in the files it writes itself, and drops the
GPU's UUIDs from vulkaninfo's output. The tests' and the emulator's own logs are copied as they are,
so they can contain your Windows user name and the game folder's path. Nothing gets uploaded, and
nothing needs administrator rights.

## If something goes wrong

- A warning that `VCRUNTIME140.dll` or another runtime DLL is missing, or that `msvcp140.dll` is
  older than this build needs: install the latest
  [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vc14/vc_redist.x64.exe), then run
  preflight again. The emulator needs it too. The runtime must be at least as new as the Visual C++
  version that built the emulator, which `gpu-tests\tests.json` records as `msvc_toolset`.
- "tests.json is missing": preflight ran from inside the zip. Extract the zip first.
- The window closes at once: run `preflight.cmd` from a command prompt instead, to see the message.

## For developers

CI builds the package in the Windows job of `.github/workflows/gta6-ci.yml`:
`.github/scripts/package_gpu_tests.py` copies the GPU test executables (the group `run_tests.py`
calls `gpu`) and the DLLs they import from the build folder into `preflight/gpu-tests`, and writes
`tests.json` with each test's arguments, time limit and quick flag. The same job smoke-tests
`preflight.ps1` under Windows PowerShell 5.1. `tests/test_preflight.py` runs it under pwsh against a
fake package.

A game boot's `emulator\_PipelineCache` is a junction to the shader cache, and the boot gets
`--shader-cache`. Windows PowerShell 5.1's `Get-ChildItem -Recurse` and `Remove-Item -Recurse` go
through junctions. So preflight walks folders itself for the zip and never enters a junction or a
symbolic link. Don't remove a results folder with `Remove-Item -Recurse` in 5.1, because it empties
the cache through the link. pwsh on Linux makes no junction, so the tests there only see the
fallback. The Windows smoke test checks the junction.
