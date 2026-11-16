# Preflight: GPU tests and a first boot on your PC

GitHub's CI machines have no GPU, so the GPU half of gta6-prep has never run anywhere. Your RTX 3050
is the first place it can. Preflight runs it there and zips what happened into one file for you to
send to Claude.

There are two jobs. Before 2026-11-18, run the GPU tests. On launch day, run the import report and a
first boot of the GTA 6 dump.

## Get the build

1. Open the [gta6-prep CI runs](https://github.com/some-du6e/KytyPS5/actions/workflows/gta6-ci.yml?query=branch%3Agta6-prep)
   while signed in to GitHub, and click the newest run with a green check.
2. Scroll down to **Artifacts** and click **KytyPS5-gta6-prep-Windows-x64**. You don't need the
   others: `ctest-linux`, `ctest-windows` and `preflight-smoke-windows` are CI logs, and
   `KytyPS5-gta6-prep-Linux-x86_64` is the Linux build.
3. Optional, and it saves a warning later: right-click the downloaded zip, choose Properties, tick
   **Unblock** at the bottom, click OK.
4. Right-click the zip, choose **Extract All**, click Extract. Run nothing from inside the zip itself.

The extracted folder has `kyty_emulator.exe` and a `preflight` folder next to it.

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
2. **Boot.** It starts the game with `--preset rtx3050` and a session log, lets it run for 5 minutes,
   then closes it. Leave the preflight window open. To play longer, add `-BootSeconds 0`: the game
   then runs until you close the game window. A boot that didn't crash shows as `RAN`, not `PASS`:
   a game stuck on a black screen also keeps running, so only the logs tell how far it got.
3. **Logs.** It collects the emulator's logs and zips everything.

Send the zip it prints, especially if the game crashed: the zip is how the crash gets fixed.

## Options

| option | what it does |
|---|---|
| `-Quick` | only the tests marked quick in `gpu-tests\tests.json` |
| `-Only a,b` | only the tests named `a` and `b` |
| `-Game <folder>` | the import report and a boot of the game, instead of the tests |
| `-BootSeconds <n>` | how long the boot runs, default 300; 0 runs until you close the game |
| `-ExtraArgs "<options>"` | more emulator options for the import report and the boot; paths in full |
| `-WithTests` | with `-Game`: run the GPU tests too, before the boot (`-Quick` and `-Only` apply) |
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

The emulator's working folder, `emulator` in the results folder, holds the game's save data and
temp files from the boot. It stays out of the zip. Once the zip is sent, the results folder can go.

Preflight writes your profile path as `%USERPROFILE%` in the files it writes itself, and drops the
GPU's UUIDs from vulkaninfo's output. The tests' and the emulator's own logs are copied as they are,
so they can contain your Windows user name and the game folder's path. Nothing gets uploaded, and
nothing needs administrator rights.

## If something goes wrong

- A warning that `VCRUNTIME140.dll` or another runtime DLL is missing, or that `msvcp140.dll` is
  older than 14.40: install the
  [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe), then
  run preflight again. The emulator needs it too.
- "tests.json is missing": preflight ran from inside the zip. Extract the zip first.
- The window closes at once: run `preflight.cmd` from a command prompt instead, to see the message.

## For developers

CI builds the package in the Windows job of `.github/workflows/gta6-ci.yml`:
`.github/scripts/package_gpu_tests.py` copies the GPU test executables (the group `run_tests.py`
calls `gpu`) and the DLLs they import from the build folder into `preflight/gpu-tests`, and writes
`tests.json` with each test's arguments, time limit and quick flag. The same job smoke-tests
`preflight.ps1` under Windows PowerShell 5.1. `tests/test_preflight.py` runs it under pwsh against a
fake package.
