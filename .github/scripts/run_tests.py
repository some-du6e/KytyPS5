#!/usr/bin/env python3
"""Run the ctest suite in two groups.

host: every test whose executable does not need a Vulkan device, plus the tests
      labelled "host", which run a GPU executable but open no device. These must pass.
gpu:  tests run by executables that open a Vulkan device (the shader recompiler
      compute harness and the kernel file system tests' test video). GitHub
      runners have no GPU and lavapipe may lack features the harness requires,
      so this group is reported but only fails the job with --gpu-required.

Usage: run_tests.py <build-dir> host|gpu [--gpu-required]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ctest_groups import load_tests, split_tests  # noqa: E402  (shared with package_gpu_tests.py)

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

# The GPU group's first test. On a runner without a usable Vulkan device, each harness test takes
# about 85 s to find that out (lavapipe on ubuntu-24.04), so the group took 54 minutes, and only
# the 3 tests that open no device passed. When the probe fails for want of a device, the rest of
# the group runs with a short limit instead: tests that open no device finish in well under a
# second and still count, and the others stop early. KYTY_GPU_TESTS_ALL=1 keeps the full limit.
GPU_PROBE = "shader_recompiler_compute"
NO_DEVICE_TIMEOUT = 10
# The harness's device-selection failures (tests/ShaderRecompilerComputeTests.cpp). A failing
# vkCreateDevice is left out on purpose: on a real GPU that is a bug to see, not a missing device.
NO_DEVICE = tuple("ShaderRecompilerComputeTests: VulkanHarness failed at dispatch: " + reason for reason in (
    "vkCreateInstance returned",
    "vkEnumeratePhysicalDevices returned",
    "no Vulkan graphics+compute device with",
))


def found_no_device(log: str) -> bool:
    with open(log, encoding="utf-8", errors="replace") as f:
        return any(marker in line for line in f for marker in NO_DEVICE)


def run_ctest(build: str, log: str, names: list, timeout: int = 900) -> int:
    regex = "^(" + "|".join(names) + ")$"
    with open(log, "w", encoding="utf-8") as out:
        proc = subprocess.Popen(["ctest", "--test-dir", build, "--output-on-failure", "--timeout", str(timeout),
                                 "-R", regex], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace")
        for line in proc.stdout:
            sys.stdout.write(line)
            out.write(line)
        return proc.wait()


def main() -> int:
    build, group = sys.argv[1], sys.argv[2]
    required = "--gpu-required" in sys.argv[3:]
    groups, not_built = split_tests(load_tests(build))
    names = {key: [test["name"] for test in tests] for key, tests in groups.items()}
    skipped = [test["name"] for test in not_built]
    known = KNOWN_FAILING.get(sys.platform, {})
    selected = [name for name in names[group] if name not in known]
    informational = [name for name in names[group] if name in known]
    if skipped:
        print(f"skipped, executable not built on this platform: {' '.join(skipped)}")
    print(f"{group} tests ({len(selected)}): {' '.join(selected)}", flush=True)
    rc = 0
    timeout = 900
    if group == "gpu" and GPU_PROBE in selected and os.environ.get("KYTY_GPU_TESTS_ALL") != "1":
        probe_log = os.path.join(build, f"ctest-{group}-probe.log")
        rc = run_ctest(build, probe_log, [GPU_PROBE])
        selected = [name for name in selected if name != GPU_PROBE]
        if rc != 0 and found_no_device(probe_log):
            timeout = NO_DEVICE_TIMEOUT
            print(f"::notice::{GPU_PROBE} found no usable Vulkan device, so the other {len(selected)} GPU tests "
                  f"run with a {timeout} s limit. Tests that open no device still pass; the rest stop early "
                  "(KYTY_GPU_TESTS_ALL=1 keeps the full limit).", flush=True)
    if selected:
        rc = run_ctest(build, os.path.join(build, f"ctest-{group}.log"), selected, timeout) or rc
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
