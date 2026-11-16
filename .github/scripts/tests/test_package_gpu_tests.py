"""package_gpu_tests.py and the selection it shares with run_tests.py, on a fake build folder: no ctest,
no compiler. The executables are minimal PE images whose import tables name DLLs."""

import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, SCRIPTS)
import ctest_groups  # noqa: E402
import package_gpu_tests as pkg  # noqa: E402


def fake_pe(imports, delay_imports=()):
    """A PE32+ image with one section that holds its import and delay-load directories."""
    names = list(imports) + list(delay_imports)
    descriptors = (len(imports) + 1) * 20
    delay_at = descriptors
    delay_size = (len(delay_imports) + 1) * 32 if delay_imports else 0
    strings_at = delay_at + delay_size
    strings = bytearray()
    rvas = []
    for name in names:
        rvas.append(0x1000 + strings_at + len(strings))
        strings += name.encode("ascii") + b"\0"
    section = bytearray()
    for rva in rvas[:len(imports)]:
        section += struct.pack("<5I", 0, 0, 0, rva, 0)
    section += bytes(20)
    if delay_imports:
        for rva in rvas[len(imports):]:
            section += struct.pack("<8I", 1, rva, 0, 0, 0, 0, 0, 0)
        section += bytes(32)
    section += strings
    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x80)
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x22)
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<Q", optional, 24, 0x140000000)
    struct.pack_into("<I", optional, 108, 16)
    struct.pack_into("<II", optional, 112 + 8 * 1, 0x1000, descriptors)
    if delay_imports:
        struct.pack_into("<II", optional, 112 + 8 * 13, 0x1000 + delay_at, delay_size)
    header = struct.pack("<8sIIIIIIHHI", b".idata", len(section), 0x1000, len(section), 0x200, 0, 0, 0, 0,
                         0xC0000040)
    image = bytes(dos) + b"PE\0\0" + coff + bytes(optional) + header
    return image + bytes(0x200 - len(image)) + bytes(section)


def prop(name, value):
    return {"name": name, "value": value}


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kyty-package-test-")
        self.build = os.path.join(self.tmp, "build")
        self.out = os.path.join(self.tmp, "out")
        os.makedirs(os.path.join(self.build, "sub"))
        self.write("shader_recompiler_compute_tests.exe",
                   fake_pe(["libwinpthread-1.dll", "KERNEL32.dll", "VCRUNTIME140.dll", "api-ms-win-crt-heap-l1-1-0.dll"]))
        self.write("kernel_file_system_tests.exe", fake_pe(["kernel32.dll", "LIBWINPTHREAD-1.DLL"], ["helper.dll"]))
        self.write("libwinpthread-1.dll", fake_pe(["KERNEL32.dll", "msvcrt.dll"]))
        self.write("helper.dll", fake_pe(["USER32.dll"]))
        # Next to the executables in a real build, and not needed by them.
        self.write("Qt6Core.dll", fake_pe(["KERNEL32.dll"]))
        self.write("unused.dll", b"not even a PE image")
        self.write("common_tests.exe", fake_pe(["KERNEL32.dll"]))
        self.write("kyty_emulator.exe", fake_pe(["libwinpthread-1.dll"]))
        self.tests = []
        self.add("emulator_preset_cli", "kyty_emulator.exe", "--help", "--preset", "rtx3050")
        self.add("kernel_file_system", "kernel_file_system_tests.exe",
                 props=[prop("ENVIRONMENT", ["KYTY_A=1", "KYTY_B=x=y"]), prop("TIMEOUT", 119.5)])
        self.add("common", "common_tests.exe")
        self.add("shader_recompiler_compute", "shader_recompiler_compute_tests.exe")
        self.add("device_creation", "shader_recompiler_compute_tests.exe", "--device-creation-only")
        self.add("shader_recompiler_gta6_alu", "shader_recompiler_compute_tests.exe", "--gta6-alu-only")
        self.add("vram_heap_limit", "shader_recompiler_compute_tests.exe", "--vram-limit-only",
                 props=[prop("WORKING_DIRECTORY", os.path.join(self.build, "sub"))])
        self.add("shader_recompiler_shader_mem", "shader_recompiler_compute_tests.exe", "--shader-mem-only",
                 props=[prop("WILL_FAIL", True)])
        self.add("pssr", "shader_recompiler_compute_tests.exe", "--pssr-only", props=[prop("DISABLED", True)])
        # Not built: ctest leaves out "command", or names a file that is not there.
        self.tests.append({"name": "vdecsw", "properties": []})
        self.add("gone", "missing_tests.exe")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write(self, name, data):
        with open(os.path.join(self.build, name), "wb") as f:
            f.write(data)

    def add(self, name, exe, *args, props=()):
        # ctest names the folder of the CTestTestfile.cmake when no WORKING_DIRECTORY is set.
        properties = list(props)
        if not any(p["name"] == "WORKING_DIRECTORY" for p in properties):
            properties.append(prop("WORKING_DIRECTORY", self.build))
        self.tests.append({"name": name, "command": [os.path.join(self.build, exe), *args],
                           "properties": properties})

    def run_package(self, *extra):
        path = os.path.join(self.tmp, "ctest.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": self.tests}, f)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = pkg.main([self.build, self.out, "--ctest-json", path, *extra])
        return rc, out.getvalue(), err.getvalue()


class SplitTests(Fixture):
    def test_groups_follow_the_executable(self):
        groups, skipped = ctest_groups.split_tests({"tests": self.tests})
        self.assertEqual([t["name"] for t in groups["host"]], ["emulator_preset_cli", "common"])
        self.assertEqual([t["name"] for t in groups["gpu"]],
                         ["kernel_file_system", "shader_recompiler_compute", "device_creation",
                          "shader_recompiler_gta6_alu", "vram_heap_limit", "shader_recompiler_shader_mem", "pssr"])
        self.assertEqual([t["name"] for t in skipped], ["vdecsw", "gone"])

    def test_executable_name(self):
        self.assertEqual(ctest_groups.executable_name("D:/a/b/shader_recompiler_compute_tests.exe"),
                         "shader_recompiler_compute_tests")
        self.assertEqual(ctest_groups.executable_name("/b/kernel_file_system_tests"), "kernel_file_system_tests")


class PackageTests(Fixture):
    def test_package(self):
        rc, out, err = self.run_package()
        self.assertEqual(rc, 0, err)
        self.assertEqual(sorted(os.listdir(self.out)),
                         ["helper.dll", "kernel_file_system_tests.exe", "libwinpthread-1.dll",
                          "shader_recompiler_compute_tests.exe", "tests.json"])
        with open(os.path.join(self.out, "shader_recompiler_compute_tests.exe"), "rb") as f:
            self.assertEqual(pkg._pe_imports(f.read())[0], "libwinpthread-1.dll")
        with open(os.path.join(self.out, "tests.json"), encoding="utf-8") as f:
            manifest = json.load(f)
        self.assertEqual(manifest["format"], 1)
        tests = {t["name"]: t for t in manifest["tests"]}
        self.assertEqual([t["name"] for t in manifest["tests"]],
                         ["kernel_file_system", "shader_recompiler_compute", "device_creation",
                          "shader_recompiler_gta6_alu", "vram_heap_limit", "shader_recompiler_shader_mem"])
        self.assertEqual(tests["device_creation"],
                         {"name": "device_creation", "executable": "shader_recompiler_compute_tests.exe",
                          "args": ["--device-creation-only"], "timeout": pkg.DEFAULT_TIMEOUT, "quick": True})
        self.assertEqual(tests["kernel_file_system"]["env"], {"KYTY_A": "1", "KYTY_B": "x=y"})
        self.assertEqual(tests["kernel_file_system"]["timeout"], 120)
        self.assertFalse(tests["kernel_file_system"]["quick"])
        self.assertEqual(tests["shader_recompiler_compute"]["timeout"], 1800)
        self.assertEqual(tests["shader_recompiler_compute"]["args"], [])
        self.assertEqual(tests["vram_heap_limit"]["cwd"], "sub")
        self.assertNotIn("cwd", tests["device_creation"])
        self.assertNotIn("env", tests["device_creation"])
        self.assertTrue(tests["shader_recompiler_shader_mem"]["will_fail"])
        self.assertEqual(sorted(t["name"] for t in manifest["tests"] if t["quick"]), sorted(pkg.QUICK_TESTS))
        self.assertEqual(manifest["executables"]["shader_recompiler_compute_tests.exe"],
                         {"dlls": ["libwinpthread-1.dll"],
                          "system_dlls": ["api-ms-win-crt-heap-l1-1-0.dll", "KERNEL32.dll", "msvcrt.dll",
                                          "VCRUNTIME140.dll"]})
        self.assertEqual(manifest["executables"]["kernel_file_system_tests.exe"],
                         {"dlls": ["helper.dll", "libwinpthread-1.dll"],
                          "system_dlls": ["kernel32.dll", "msvcrt.dll", "USER32.dll"]})
        self.assertIn("left out, DISABLED in ctest: pssr", out)
        self.assertIn("not built here: vdecsw gone", out)

    def test_source_comes_from_the_ci_environment(self):
        old = dict(os.environ)
        os.environ.update(GITHUB_SHA="0123abc", GITHUB_RUN_ID="42", GITHUB_REPOSITORY="o/r", GITHUB_REF_NAME="b",
                          VCToolsVersion="14.51.36231")
        try:
            rc, _, err = self.run_package()
        finally:
            os.environ.clear()
            os.environ.update(old)
        self.assertEqual(rc, 0, err)
        with open(os.path.join(self.out, "tests.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["source"], {"repository": "o/r", "commit": "0123abc", "ref": "b", "run_id": "42",
                                                      "msvc_toolset": "14.51.36231"})

    def test_not_a_pe_image(self):
        # A Linux build: the executables are copied, without a DLL list.
        self.write("shader_recompiler_compute_tests.exe", b"\x7fELF")
        self.write("kernel_file_system_tests.exe", b"\x7fELF")
        rc, _, err = self.run_package()
        self.assertEqual(rc, 0, err)
        self.assertEqual(sorted(os.listdir(self.out)),
                         ["kernel_file_system_tests.exe", "shader_recompiler_compute_tests.exe", "tests.json"])
        with open(os.path.join(self.out, "tests.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["executables"]["kernel_file_system_tests.exe"], {})

    def test_refuses_a_folder_that_is_not_empty(self):
        os.makedirs(self.out)
        with open(os.path.join(self.out, "old.txt"), "w") as f:
            f.write("x")
        rc, _, err = self.run_package()
        self.assertEqual(rc, 1)
        self.assertIn("is not empty", err)
        self.assertEqual(os.listdir(self.out), ["old.txt"])

    def test_quick_tests_must_exist(self):
        self.tests = [t for t in self.tests if t["name"] != "vram_heap_limit"]
        rc, _, err = self.run_package()
        self.assertEqual(rc, 1)
        self.assertIn("quick tests not in the GPU group: vram_heap_limit", err)
        self.assertFalse(os.path.exists(self.out))

    def test_unsupported_property(self):
        self.tests[3]["properties"].append(prop("PASS_REGULAR_EXPRESSION", ["ok"]))
        rc, _, err = self.run_package()
        self.assertEqual(rc, 1)
        self.assertIn("shader_recompiler_compute: preflight.ps1 cannot reproduce ctest's PASS_REGULAR_EXPRESSION", err)

    def test_working_directory_outside_the_build(self):
        self.tests[4]["properties"] = [prop("WORKING_DIRECTORY", self.tmp)]
        rc, _, err = self.run_package()
        self.assertEqual(rc, 1)
        self.assertIn("device_creation: WORKING_DIRECTORY", err)

    def test_no_gpu_tests(self):
        self.tests = [t for t in self.tests if t["name"] in ("common", "vdecsw")]
        rc, _, err = self.run_package()
        self.assertEqual(rc, 1)
        self.assertIn("no GPU tests", err)

    def test_pe_imports(self):
        def imports(data):
            self.write("probe.bin", data)
            return pkg.pe_imports(os.path.join(self.build, "probe.bin"))
        self.assertEqual(imports(fake_pe(["a.dll"], ["b.dll", "c.dll"])), ["a.dll", "b.dll", "c.dll"])
        self.assertEqual(imports(fake_pe([])), [])
        self.assertIsNone(imports(b"MZ" + bytes(10)))  # cut short
        self.assertIsNone(imports(fake_pe(["a.dll"])[:0x210]))  # the names are cut off
        self.assertIsNone(imports(b"#!/bin/sh\n"))


class RunTestsSelection(Fixture):
    """run_tests.py, with a stand-in ctest first on PATH, runs the groups the shared module computes."""

    def run_tests(self, group):
        bin_dir = os.path.join(self.tmp, "bin")
        os.makedirs(bin_dir, exist_ok=True)
        info = os.path.join(self.tmp, "ctest.json")
        with open(info, "w", encoding="utf-8") as f:
            json.dump({"tests": self.tests}, f)
        log = os.path.join(self.tmp, "ctest-calls.txt")
        fake = os.path.join(bin_dir, "ctest")
        with open(fake, "w", encoding="utf-8", newline="\n") as f:
            f.write("#!%s\n" % sys.executable +
                    "import sys\n"
                    "if '--show-only=json-v1' in sys.argv:\n"
                    "    sys.stdout.write(open(%r).read())\n"
                    "else:\n"
                    "    open(%r, 'a').write(' '.join(sys.argv[1:]) + '\\n')\n" % (info, log))
        os.chmod(fake, 0o755)
        env = dict(os.environ, PATH=bin_dir + os.pathsep + os.environ.get("PATH", ""))
        r = subprocess.run([sys.executable, os.path.join(SCRIPTS, "run_tests.py"), self.build, group],
                           capture_output=True, text=True, env=env, timeout=60)
        with open(log, encoding="utf-8") as f:
            return r, f.read()

    @unittest.skipIf(os.name == "nt", "the stand-in ctest is a script with a shebang")
    def test_host_and_gpu_groups(self):
        r, calls = self.run_tests("host")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("skipped, executable not built on this platform: vdecsw gone", r.stdout)
        self.assertIn("-R ^(emulator_preset_cli|common)$", calls)
        os.remove(os.path.join(self.tmp, "ctest-calls.txt"))
        r, calls = self.run_tests("gpu")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("-R ^(kernel_file_system|shader_recompiler_compute|device_creation|shader_recompiler_gta6_alu|"
                      "vram_heap_limit|shader_recompiler_shader_mem|pssr)$", calls)


if __name__ == "__main__":
    unittest.main()
