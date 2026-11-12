#!/usr/bin/env python3
"""Run the ctest suite in two groups.

host: every test whose executable does not need a Vulkan device. These must pass.
gpu:  tests run by executables that open a Vulkan device (the shader recompiler
      compute harness and the kernel file system tests' test video). GitHub
      runners have no GPU and lavapipe may lack features the harness requires,
      so this group is reported but only fails the job with --gpu-required.

Usage: run_tests.py <build-dir> host|gpu [--gpu-required]
"""
import json
import os
import subprocess
import sys

GPU_EXECUTABLES = ("shader_recompiler_compute_tests", "kernel_file_system_tests")

# Host tests that fail on a platform without any gta6-prep change, and that upstream's CI does not
# run there. They still run, after the gated ones, but only warn.
KNOWN_FAILING = {
    "win32": {
        # Upstream test (1a24e34e): std::filesystem::last_write_time throws "The parameter is
        # incorrect" for the temp file on the windows-2025 runner. upstream build.yml's Windows test
        # list leaves out common.
        "common": "File.GetTimesReportsExactNanoseconds fails on the Windows runner upstream too",
    },
}


def run_ctest(build: str, log: str, names: list) -> int:
    regex = "^(" + "|".join(names) + ")$"
    with open(log, "w", encoding="utf-8") as out:
        proc = subprocess.Popen(["ctest", "--test-dir", build, "--output-on-failure", "--timeout", "900",
                                 "-R", regex], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace")
        for line in proc.stdout:
            sys.stdout.write(line)
            out.write(line)
        return proc.wait()


def main() -> int:
    build, group = sys.argv[1], sys.argv[2]
    required = "--gpu-required" in sys.argv[3:]
    info = json.loads(subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"],
                                     check=True, capture_output=True, text=True).stdout)
    names = {"host": [], "gpu": []}
    skipped = []
    for test in info.get("tests", []):
        command = test.get("command") or [""]
        if command[0] and not os.path.exists(command[0]):
            skipped.append(test["name"])  # target not built on this platform (e.g. vdecsw on clang-cl)
            continue
        exe = os.path.basename(command[0])
        exe = exe[:-4] if exe.endswith(".exe") else exe
        names["gpu" if exe in GPU_EXECUTABLES else "host"].append(test["name"])
    known = KNOWN_FAILING.get(sys.platform, {})
    selected = [name for name in names[group] if name not in known]
    informational = [name for name in names[group] if name in known]
    if skipped:
        print(f"skipped, executable not built on this platform: {' '.join(skipped)}")
    print(f"{group} tests ({len(selected)}): {' '.join(selected)}", flush=True)
    rc = run_ctest(build, os.path.join(build, f"ctest-{group}.log"), selected) if selected else 0
    if informational:
        print(f"known failing on {sys.platform}, not gated: {' '.join(informational)}", flush=True)
        if run_ctest(build, os.path.join(build, f"ctest-{group}-known.log"), informational) != 0:
            for name in informational:
                print(f"::warning::{name}: {known[name]}")
    if rc != 0 and group == "gpu" and not required:
        print(f"::warning::{group} tests failed (rc={rc}); no usable Vulkan device on this runner is the usual cause")
        return 0
    return rc


if __name__ == "__main__":
    sys.exit(main())
