#!/usr/bin/env python3
"""Packages run_tests.py's GPU group so that it can run on a PC that has no build tree.

Usage: package_gpu_tests.py <build-dir> <out-dir> [--ctest-json <file>]

<out-dir> must be new or empty. It gets each GPU test executable once, the DLLs from the
executable's folder that it imports (libwinpthread-1.dll on clang-cl), and tests.json, which
tools/preflight/preflight.ps1 reads. --ctest-json reads a saved `ctest --show-only=json-v1` instead
of running ctest.

The tests need no data files: their shaders are compiled in, and they write only to the temp
folder. vulkan-1.dll comes with the GPU driver, and the MSVC runtime DLLs with the Visual C++
redistributable; tests.json lists such imports per executable, so preflight.ps1 can check for them
before it runs anything.
"""
import argparse
import json
import math
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ctest_groups import GPU_EXECUTABLES, load_tests, split_tests  # noqa: E402

# The tests preflight.ps1 -Quick runs: device creation, and the GPU tests of gta6-prep's own changes.
QUICK_TESTS = ("device_creation", "shader_recompiler_gta6_alu", "shader_recompiler_shader_mem",
               "vram_heap_limit")

# Seconds a test may run before preflight.ps1 stops it, when ctest sets no TIMEOUT. A test with one
# --*-only flag runs a few groups of cases; shader_recompiler_compute runs all of them.
DEFAULT_TIMEOUT = 300
TIMEOUTS = {"shader_recompiler_compute": 1800}

# ctest properties that change a test's verdict or setup in ways preflight.ps1 does not reproduce.
# A GPU test with one of them stops the packaging, instead of being judged wrongly on the PC.
UNSUPPORTED_PROPERTIES = ("PASS_REGULAR_EXPRESSION", "FAIL_REGULAR_EXPRESSION", "SKIP_REGULAR_EXPRESSION",
                          "SKIP_RETURN_CODE", "ENVIRONMENT_MODIFICATION", "REQUIRED_FILES",
                          "FIXTURES_REQUIRED")


class PackageError(Exception):
    pass


def pe_imports(path):
    """The DLL names that the PE image at `path` imports (at load time, then delay-loaded), or None
    when it is no Windows image at all (an ELF or Mach-O build). PackageError for a file that starts
    like one ("MZ") but whose imports cannot be read: copied without its DLLs, every test would fail
    on the PC with "a DLL was not found"."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return None
    try:
        names = _pe_imports(data)
        reason = "no PE header this script reads"
    except (struct.error, ValueError, IndexError, UnicodeDecodeError) as error:
        names, reason = None, str(error) or type(error).__name__
    if names is None:
        raise PackageError(f"cannot read the imports of {path}: {reason}")
    return names


def _pe_imports(data):
    if data[:2] != b"MZ":
        return None
    pe, = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    section_count, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from("<H", data, optional)
    if magic == 0x20B:  # PE32+
        image_base, = struct.unpack_from("<Q", data, optional + 24)
        count_at, directories = optional + 108, optional + 112
    elif magic == 0x10B:  # PE32
        image_base, = struct.unpack_from("<I", data, optional + 28)
        count_at, directories = optional + 92, optional + 96
    else:
        return None
    directory_count, = struct.unpack_from("<I", data, count_at)
    sections = []
    for i in range(section_count):
        header = optional + optional_size + 40 * i
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, header + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset))

    def offset(rva):
        for address, size, raw in sections:
            if address <= rva < address + size:
                return rva - address + raw
        raise ValueError(f"RVA 0x{rva:x} is in no section")

    def name_at(rva):
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii")

    def directory(index):
        return struct.unpack_from("<I", data, directories + 8 * index)[0] if index < directory_count else 0

    names = []
    rva = directory(1)  # IMAGE_DIRECTORY_ENTRY_IMPORT: 20-byte descriptors, the DLL name's RVA at +12
    at = offset(rva) if rva else None
    while at is not None:
        name, = struct.unpack_from("<I", data, at + 12)
        if name == 0:
            break
        names.append(name_at(name))
        at += 20
    rva = directory(13)  # IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT: 32-byte descriptors
    at = offset(rva) if rva else None
    while at is not None:
        attributes, name = struct.unpack_from("<II", data, at)
        if name == 0:
            break
        # Attribute bit 0 clear: an old (VC6) descriptor, which holds a VA instead of an RVA.
        names.append(name_at(name if attributes & 1 else name - image_base))
        at += 32
    return names


def folder_dlls(exe):
    """(dlls, elsewhere) for the executable `exe`: the DLLs in its folder that it loads, directly or
    through another of them, and the imported names that are not in that folder (the system's, or
    the GPU driver's). None when `exe` is no Windows image (a Linux or macOS build)."""
    folder = os.path.dirname(exe)
    present = {name.lower(): name for name in os.listdir(folder) if name.lower().endswith(".dll")}
    dlls, elsewhere = {}, {}
    pending = [exe]
    while pending:
        image = pending.pop()
        imports = pe_imports(image)
        if imports is None:
            if image == exe:
                return None
            raise PackageError(f"{image} is not a PE image, but {os.path.basename(exe)} loads it")
        for name in imports:
            key = name.lower()
            if key in present:
                if key not in dlls:
                    dlls[key] = present[key]
                    pending.append(os.path.join(folder, present[key]))
            else:
                elsewhere.setdefault(key, name)
    return sorted(dlls.values(), key=str.lower), sorted(elsewhere.values(), key=str.lower)


def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def inside(path, root, what):
    """`path` relative to `root`, with forward slashes; PackageError when it is outside `root`."""
    try:
        rel = os.path.relpath(os.path.abspath(path), root)
    except ValueError:  # another drive
        rel = os.pardir
    if rel == os.pardir or rel.startswith(os.pardir + os.sep) or os.path.isabs(rel):
        raise PackageError(f"{what} {path} is outside the build folder {root}")
    return rel.replace(os.sep, "/")


def test_entry(test, build):
    """The tests.json entry of a ctest json-v1 test."""
    name = test["name"]
    command = test["command"]
    props = {p["name"]: p.get("value") for p in test.get("properties", [])}
    unsupported = [p for p in UNSUPPORTED_PROPERTIES if p in props]
    if unsupported:
        raise PackageError(f"{name}: preflight.ps1 cannot reproduce ctest's {', '.join(unsupported)}")
    entry = {"name": name, "executable": inside(command[0], build, f"{name}: the executable"),
             "args": list(command[1:])}
    environment = props.get("ENVIRONMENT")
    if environment:
        if isinstance(environment, str):
            environment = environment.split(";")
        pairs = [item.partition("=") for item in environment]
        bad = [item for item, (_, sep, _) in zip(environment, pairs) if not sep]
        if bad:
            raise PackageError(f"{name}: ENVIRONMENT entry without '=': {bad[0]}")
        entry["env"] = {key: value for key, _, value in pairs}
    # ctest runs a test in the folder of the CTestTestfile.cmake that names it unless
    # WORKING_DIRECTORY says otherwise; json-v1 reports that folder either way.
    working = props.get("WORKING_DIRECTORY")
    if working and not same_path(working, build):
        entry["cwd"] = inside(working, build, f"{name}: WORKING_DIRECTORY")
    timeout = float(props.get("TIMEOUT") or 0)
    entry["timeout"] = int(math.ceil(timeout)) if timeout > 0 else TIMEOUTS.get(name, DEFAULT_TIMEOUT)
    if props.get("WILL_FAIL"):
        entry["will_fail"] = True
    entry["quick"] = name in QUICK_TESTS
    return entry


def describe(gpu_tests, build):
    """(entries, executables, disabled) for the GPU tests: the tests.json entries, the absolute path
    of each distinct executable by its relative one, and the names of disabled tests left out."""
    entries, executables, disabled = [], {}, []
    for test in gpu_tests:
        if any(p["name"] == "DISABLED" and p.get("value") for p in test.get("properties", [])):
            disabled.append(test["name"])
            continue
        entry = test_entry(test, build)
        entries.append(entry)
        executables.setdefault(entry["executable"], os.path.abspath(test["command"][0]))
    if not entries:
        raise PackageError(f"no GPU tests in {build}: are {' and '.join(GPU_EXECUTABLES)} built?")
    missing = [name for name in QUICK_TESTS if name not in {entry["name"] for entry in entries}]
    if missing:
        raise PackageError(f"quick tests not in the GPU group: {', '.join(missing)}")
    return entries, executables, disabled


def megabytes(path):
    return os.path.getsize(path) / (1024 * 1024)


def package(build, out, info):
    groups, not_built = split_tests(info)
    entries, executables, disabled = describe(groups["gpu"], build)
    if os.path.exists(out) and os.listdir(out):
        raise PackageError(f"{out} is not empty")
    # Every import table is read before anything is written: an error leaves no half package.
    imports = {rel: folder_dlls(exe) for rel, exe in executables.items()}
    os.makedirs(out, exist_ok=True)
    files = {}
    total = 0.0
    for rel, exe in executables.items():
        target = os.path.join(out, *rel.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(exe, target)
        total += megabytes(target)
        found = imports[rel]
        record = {}
        line = f"  {rel}  {megabytes(target):.1f} MB"
        if found is not None:
            dlls, elsewhere = found
            for dll in dlls:
                copy = os.path.join(os.path.dirname(target), dll)
                shutil.copy2(os.path.join(os.path.dirname(exe), dll), copy)
                total += megabytes(copy)
                line += f" + {dll} {megabytes(copy):.1f} MB"
            record = {"dlls": dlls, "system_dlls": elsewhere}
            if os.name == "nt":
                system32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
                for name in elsewhere:
                    if not name.lower().startswith(("api-ms-", "ext-ms-")) and \
                            not os.path.exists(os.path.join(system32, name)):
                        print(f"warning: {rel} imports {name}, which is neither next to it nor in {system32}")
        files[rel] = record
        print(line)
    # msvc_toolset: the MSVC build tools' version (msvc-dev-cmd sets VCToolsVersion). The VC++
    # runtime on the PC must be at least as new; preflight.ps1 checks.
    source = {key: os.environ[var] for key, var in (("repository", "GITHUB_REPOSITORY"), ("commit", "GITHUB_SHA"),
                                                    ("ref", "GITHUB_REF_NAME"), ("run_id", "GITHUB_RUN_ID"),
                                                    ("msvc_toolset", "VCToolsVersion"))
              if os.environ.get(var)}
    manifest = {"format": 1, "source": source, "tests": entries, "executables": files}
    with open(os.path.join(out, "tests.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    quick = sum(1 for entry in entries if entry["quick"])
    print(f"GPU tests: {len(entries)} ({quick} quick) from {len(executables)} executables, {total:.1f} MB in {out}")
    if disabled:
        print(f"left out, DISABLED in ctest: {' '.join(disabled)}")
    if not_built:
        print(f"not built here: {' '.join(test['name'] for test in not_built)}")
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description="Package the GPU tests for tools/preflight/preflight.ps1.")
    parser.add_argument("build", help="the CMake build folder")
    parser.add_argument("out", help="the folder to write (new or empty)")
    parser.add_argument("--ctest-json", help="read the tests from this saved `ctest --show-only=json-v1`")
    args = parser.parse_args(argv)
    build = os.path.abspath(args.build)
    if args.ctest_json:
        with open(args.ctest_json, encoding="utf-8") as f:
            info = json.load(f)
    else:
        info = load_tests(build)
    try:
        package(build, os.path.abspath(args.out), info)
    except PackageError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
