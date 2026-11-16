"""How the CI scripts split the ctest suite. run_tests.py and package_gpu_tests.py share it, so the GPU
tests a PC runs from the package are the ones CI calls the GPU group."""
import json
import os
import subprocess

# Executables that open a Vulkan device: the shader recompiler compute harness, and the kernel file
# system tests (they create a hidden Vulkan test window).
GPU_EXECUTABLES = ("shader_recompiler_compute_tests", "kernel_file_system_tests")


def load_tests(build):
    """ctest's json-v1 description of the tests in `build`."""
    return json.loads(subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"],
                                     check=True, capture_output=True, text=True).stdout)


def executable_name(path):
    name = os.path.basename(path)
    return name[:-4] if name.endswith(".exe") else name


def split_tests(info):
    """Returns (groups, skipped). groups maps "host" and "gpu" to their tests, in ctest's order, as
    the dicts of `info`; skipped lists the tests whose executable was not built."""
    groups = {"host": [], "gpu": []}
    skipped = []
    for test in info.get("tests", []):
        # A target not built on this platform (vdecsw on clang-cl): ctest leaves out "command" when
        # it cannot find the executable, or names a path that does not exist.
        command = test.get("command") or []
        if not command or not command[0] or not os.path.exists(command[0]):
            skipped.append(test)
            continue
        groups["gpu" if executable_name(command[0]) in GPU_EXECUTABLES else "host"].append(test)
    return groups, skipped
