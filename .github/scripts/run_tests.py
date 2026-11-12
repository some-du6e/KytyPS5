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
    selected = names[group]
    if skipped:
        print(f"skipped, executable not built on this platform: {' '.join(skipped)}")
    print(f"{group} tests ({len(selected)}): {' '.join(selected)}", flush=True)
    if not selected:
        return 0
    regex = "^(" + "|".join(selected) + ")$"
    log = os.path.join(build, f"ctest-{group}.log")
    with open(log, "w", encoding="utf-8") as out:
        proc = subprocess.Popen(["ctest", "--test-dir", build, "--output-on-failure", "--timeout", "900",
                                 "-R", regex], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace")
        for line in proc.stdout:
            sys.stdout.write(line)
            out.write(line)
        rc = proc.wait()
    if rc != 0 and group == "gpu" and not required:
        print(f"::warning::{group} tests failed (rc={rc}); no usable Vulkan device on this runner is the usual cause")
        return 0
    return rc


if __name__ == "__main__":
    sys.exit(main())
