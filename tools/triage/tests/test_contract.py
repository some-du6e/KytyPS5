"""Every line of notes/log-contract.md is parsed into the right report field (synthetic fixtures)."""

import io
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage_test_util as u  # noqa: E402


class ContractSessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rep = u.report("synthetic-contract-session.txt")

    def test_identity_lines(self):
        ident = self.rep["identity"]
        gpu = ident["gpu"]
        self.assertEqual(gpu["name"], "NVIDIA GeForce RTX 3050")
        self.assertEqual(gpu["vendor"], "NVIDIA")
        self.assertEqual(gpu["vendor_id"], "0x10de")
        self.assertEqual(gpu["device_id"], "0x2507")
        self.assertEqual(gpu["driver"], "581.42.0.0")
        self.assertEqual(gpu["api"], "1.4.312")
        self.assertEqual(gpu["driver_id"], "NvidiaProprietary")
        self.assertEqual(gpu["vram_mib"], 8192)
        self.assertEqual(gpu["bar_mib"], 256)
        host = ident["host"]
        self.assertEqual(host["cpu"], "AMD Ryzen 5 5600 6-Core Processor")
        self.assertEqual(host["threads"], "12")
        self.assertEqual(host["ram_mib"], 16304)
        self.assertEqual(host["os"], "Windows 11 (10.0.26100)")
        tp = ident["title_profile"]
        self.assertEqual((tp["key"], tp["id"], tp["ver"], tp["name"], tp["quirks"], tp["preset"]),
                         ("none", "PPSA00001", "01.000.000", "SYNTHETIC TITLE", [], "none"))
        self.assertEqual([p["name"] for p in ident["presets"]], ["low-vram", "safe"])
        self.assertEqual(ident["presets"][0]["tokens"],
                         "--no-native-raytracing --no-trinity --no-readback-tiled-images "
                         "--no-readback-storage-images --no-keep-small-layer-targets "
                         "--vram-profile low --vram-gc proportional --oom-recovery")
        vram = ident["vram_config"]
        self.assertEqual((vram["budget"], vram["limit"], vram["profile"], vram["gc"]), ("7168", "off", "low", "proportional"))
        self.assertEqual(ident["session_start"], "2026-11-19 20:05:11")
        self.assertEqual(ident["build_label"], "Fork build some-du6e/KytyPS5 1234abc")

    def test_import_lines(self):
        imp = self.rep["imports"]
        called = {c["symbol"]: c for c in imp["called"]}
        np = called["AAAAAAAAAAA[NpManager_v1][NpManager_v1.1][Func]"]
        self.assertEqual((np["policy"], np["returns"], np["program"], np["library"], np["nid"]),
                         ("np-offline", "0x80550003", "eboot.bin", "NpManager", "AAAAAAAAAAA"))
        self.assertEqual(np["name"], "sceNpCheckPlus")
        self.assertEqual(called["BBBBBBBBBBB[Http2_v1][Http2_v1.1][Func]"]["policy"], "http-down")
        self.assertEqual(called["DDDDDDDDDDD[Net_v1][Net_v1.1][Func]"]["policy"], "net-down")
        self.assertEqual(called["EEEEEEEEEEE[Ajm_v1][Ajm_v1.1][Func]"]["policy"], "zero")
        self.assertEqual(imp["objects"][0]["library"], "NpUniversalDataSystem")
        self.assertEqual(imp["objects"][0]["page"], "0x0000000ffff00000")
        s = imp["summary"]
        self.assertEqual((s["functions"], s["objects"]), (37, 2))
        self.assertEqual(s["by_library"]["NpManager"], 9)
        self.assertEqual(len(s["by_library"]), 6)
        self.assertEqual(imp["rebinds"], 1)
        self.assertEqual(imp["nid_fallbacks"], 1)
        libs = [e["library"] for e in imp["by_library"]]
        self.assertEqual(set(libs), {"NpManager", "Http2", "Net", "Ajm"})

    def test_hle_lines(self):
        apis = [s["api"] for s in self.rep["soft_fails"]]
        self.assertEqual(apis, ["sceNpSetContentRestriction", "sceAudio3dPortOpen"])
        sc = self.rep["guest_syscalls"][0]
        self.assertEqual((sc["count"], sc["module"], sc["offset"]), (3, "eboot.bin", "0x1a2b3c"))

    def test_shader_skip_line(self):
        su = self.rep["shader_unsupported"]
        self.assertEqual(len(su), 1)
        e = su[0]
        self.assertEqual((e["family"], e["opcode"], e["encoding"], e["skipped"], e["fatal"]),
                         ("VOP3", "0x344", "0xd7440000 0x00020501", 1, 0))
        self.assertEqual(e["reason"], "VOP3 opcode is not implemented")
        self.assertEqual(e["shaders"], [{"hash": "0x1122334455667788", "pc": "0x120", "stage": "CS"}])

    def test_vram_lines(self):
        f = {(x["rule_id"], x["key"]): x for x in self.rep["findings"]}
        spill = f[("vram-spill", "images")]
        self.assertEqual(spill["count"], 2)
        self.assertEqual(spill["fields"]["kind"], "images")
        self.assertIn(("vram-spill", "buffers"), f)
        self.assertTrue(any(k[0] == "vram-pressure" for k in f))

    def test_formatted_buffer_fallback_line(self):
        f = [x for x in self.rep["findings"] if x["rule_id"] == "shader-formatted-raw-fallback"]
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0]["fields"]["dir"], f[0]["fields"]["format"], f[0]["fields"]["hash"]),
                         ("store", "13", "1122334455667788"))

    def test_guest_pc_and_stack(self):
        exc = self.rep["primary"]["host_exception"]
        self.assertEqual((exc["pc_module"], exc["pc_offset"], exc["pc_basis"]), ("eboot.bin", "0x1234567", "guest pc line"))
        self.assertEqual(exc["thread"], "[GAME] Main Thread")
        self.assertEqual(exc["access_name"], "read")
        self.assertEqual(exc["address_kind"], "near-null")
        self.assertEqual(exc["stack_modules"], ["eboot.bin+0x1234600", "KytyPS5.exe+0x12c3d4"])
        ctx = self.rep["fault_contexts"][0]
        self.assertEqual(ctx["regs"]["rcx"], "0x0000000900001000")
        self.assertEqual(ctx["regs"]["r8"], "0x0000000000000000")

    def test_ranking_prefers_imports_called_before_a_guest_fault(self):
        areas = self.rep["fix_areas"]
        self.assertEqual(areas[0]["rule_id"], "unresolved-import-called")
        self.assertEqual(areas[1]["rule_id"], "host-fault-guest")


class ContractOtherTests(unittest.TestCase):
    def test_device_lost_report(self):
        rep = u.report("synthetic-contract-device-lost.txt")
        prim = rep["fatal_blocks"][0]
        self.assertEqual(prim["rule_id"], "device-lost")
        self.assertEqual(prim["message"].split("\n")[0], "vkWaitSemaphores failed: ErrorDeviceLost (-4)")
        self.assertEqual(prim["device_fault"][1], "Device fault: address ReadInvalid 0x0000001160400000 precision=0x1000")
        self.assertEqual(len(prim["device_fault"]), 3)
        vk = {v["vuid"]: v for v in rep["gpu_errors"]["vulkan_messages"]}
        self.assertIn("VUID-vkCmdDrawIndexed-None-06479", vk)
        self.assertTrue(vk["VUID-vkCmdDrawIndexed-None-06479"]["known"])
        lost = [f for f in rep["gpu_errors"]["findings"] if f["rule_id"] == "vk-device-lost"]
        self.assertEqual(lost[0]["fields"]["op"], "vkQueueSubmit")
        # the named device-lost report and the vkQueueSubmit line are one fix area
        self.assertEqual(sum(1 for a in rep["fix_areas"] if a["category"] == "DEVICE_LOST"), 1)

    def test_diag_fault_context_with_guest_address_and_guest_stack(self):
        # gta6/diag's report: "guest address:" after "guest pc:", a full stack dump and a
        # "guest stack trace:" block; the EXIT is more than 40 lines after "guest address:".
        rep = u.report("synthetic-diag-fault-context.txt")
        exc = rep["primary"]["host_exception"]
        self.assertTrue(exc["fault_context"])
        self.assertEqual((exc["pc_module"], exc["pc_offset"], exc["pc_basis"]), ("eboot.bin", "0x1c2d3e4", "guest pc line"))
        self.assertEqual(exc["access_name"], "write")
        self.assertEqual(exc["thread"], "[GAME] Render Thread")
        self.assertEqual(exc["guest_address"], "eboot.bin+0x28ab5f0")
        self.assertEqual(len(exc["guest_frames"]), 16)
        self.assertEqual(exc["guest_frames"][0], "eboot.bin+0x1b00000")
        self.assertEqual(exc["guest_frames"][15], "0x00000007eff00000")
        ctx = rep["fault_contexts"][0]
        self.assertEqual(len(ctx["regs"]), 16)
        self.assertEqual(len(ctx["stack_hits"]), 24)
        self.assertEqual(ctx["guest_address"], {"module": "eboot.bin", "offset": "0x28ab5f0"})
        md = u.kt.render_markdown(rep)
        self.assertIn("- address = `eboot.bin+0x28ab5f0` (guest address line)", md)

    def test_gpu_line_with_a_mesa_vendor_id(self):
        # VK_VENDOR_ID_MESA is 0x10005; gta6/diag prints it with {:04x}, a minimum width (CI lavapipe).
        line = (b"GPU: llvmpipe (LLVM 19.1.1, 256 bits) vendor=0x10005 device=0x0000 driver=0.0.1 api=1.4.305 "
                b"driverID=MesaLlvmpipe vram=0 MiB bar=0 MiB\n")
        rep = u.kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(line))
        gpu = rep["identity"]["gpu"]
        self.assertEqual(gpu["name"], "llvmpipe (LLVM 19.1.1, 256 bits)")
        self.assertEqual((gpu["vendor"], gpu["vendor_id"], gpu["device_id"]), ("Mesa (lavapipe)", "0x10005", "0x0000"))
        self.assertEqual((gpu["driver_id"], gpu["vram_mib"]), ("MesaLlvmpipe", 0))

    def test_strict_imports(self):
        rep = u.report("synthetic-contract-strict-imports.txt")
        self.assertEqual(rep["primary"]["rule_id"], "strict-import-exit")
        f = rep["fatal_blocks"][0]["fields"]
        self.assertEqual(f["symbol"], "FFFFFFFFFFF[NpUniversalDataSystem_v1][NpUniversalDataSystem_v1.1][Func]")
        self.assertEqual(f["program"], "eboot.bin")

    def test_strict_shaders(self):
        rep = u.report("synthetic-contract-strict-shaders.txt")
        b = rep["fatal_blocks"][0]
        self.assertEqual(b["rule_id"], "shader-opcode")
        self.assertEqual(b["shader"], {"stage": "CS", "hash": "0xa1b2c3d4e5f60718"})
        self.assertEqual(b["shader_instruction"]["family"], "MIMG")
        e = rep["shader_unsupported"][0]
        self.assertEqual((e["key"], e["fatal"]), ("MIMG:0xe6", 1))
        self.assertEqual(e["shaders"][0]["stage"], "CS")
        self.assertIn("src/graphics/shader/recompiler/frontend/decode/ImageOps.cpp", rep["fix_areas"][0]["paths"])


class ContractRamTests(unittest.TestCase):
    """gta6-prep: ram: the "RAM" lines of src/common/ramOptions.h, as tests/RamPolicyTests.cpp pins them."""

    def test_ram_lines_of_a_session(self):
        rep = u.report("synthetic-ram-lines.txt")
        hm = rep["host_memory"]
        st = hm["startup"]
        self.assertEqual((st["phys_mib"], st["available_mib"], st["commit_mib"], st["commit_limit_mib"],
                          st["headroom_mib"], st["page_file_mib"], st["page_file_estimated"], st["guest_mib"],
                          st["profile"]),
                         (8090, 4712, 18242, 24474, 6232, 16384, True, 12736, "normal"))
        self.assertIsNone(hm["profile"])
        self.assertEqual(hm["warning_count"], 2)
        self.assertTrue(hm["warnings"][0]["text"].startswith("commit headroom 6232 MiB is below 12288 MiB."))
        self.assertEqual((hm["samples"], hm["samples_with_available"], hm["low_available_samples"]), (6, 6, 4))
        self.assertEqual((hm["noaccess_samples"], hm["max_noaccess_mib"], hm["peak_commit_percent"],
                          hm["peak_private_mib"], hm["peak_working_set_mib"]), (5, 312, 95, 9875, 2843))
        self.assertEqual(hm["min_available"]["available_mib"], 187)
        last = hm["last"]
        self.assertEqual((last["t"], last["working_set_mib"], last["private_mib"], last["commit_mib"],
                          last["commit_limit_mib"], last["available_mib"], last["faults_per_s"],
                          last["hard_faults_per_s"], last["vk_local_mib"], last["vk_local_budget_mib"],
                          last["vk_host_mib"], last["vk_host_budget_mib"], last["spilled_mib"], last["noaccess_mib"]),
                         (180, 2843, 9875, 23400, 24474, 187, 3412, None, 5402, 5560, 1873, 3711, 1504, 312))
        self.assertEqual([s["t"] for s in hm["tail"]], [90, 120, 150, 180])
        self.assertEqual((last["one_off_buffers"], last["one_off_mib"]), (15, 400))
        self.assertEqual(hm["one_off"], {"buffers": 15, "mib": 400, "t": 180})
        # Every host memory rule. The log stops low on RAM after NOACCESS samples, so it gets the blue screen hint,
        # under the plain no-fatal one: a log that stops is also a kill, a hang or preflight's time limit.
        self.assertEqual([a["rule_id"] for a in rep["fix_areas"]],
                         ["ram-commit-full", "ram-warning", "ram-available-low", "ram-low-ram-normal-profile",
                          "no-fatal-no-fatal", "ram-noaccess-bluescreen"])
        bluescreen = rep["fix_areas"][-1]
        self.assertIn("0x41202", bluescreen["next_step"])
        self.assertIn("with 187 MiB of RAM available in the last sample", bluescreen["why"][0])
        md = u.kt.render_markdown(rep)
        self.assertIn("## Host memory", md)
        self.assertIn("6 `RAM usage:` sample(s) from t=30 to t=180 s; available RAM down to 187 MiB (under 300 MiB "
                      "in 4 of 6); commit up to 95% of its limit", md)
        self.assertIn("NOACCESS guest pages in 5 sample(s), up to 312 MiB; 15 one-time ring buffer(s) by t=180 s "
                      "(400 MiB)", md)

    def test_low_profile_and_the_exit_line_of_a_fatal_report(self):
        # The low profile's lines, then a fatal report followed by RamLog::WriteExitLine()'s line, which has no
        # renderer fields and "?" for the rates: no blue screen hint (there is a report), no profile hint.
        text = (
            "--- Session 2026-11-19 21:00:00 ---\n"
            "Fork build some-du6e/KytyPS5 1234abc\n"
            "RAM: phys=8090 MiB avail=4712 commit=18242/40858 headroom=22616 pagefile=~32768 guest=12736 profile=low\n"
            "RAM profile: low -> upload_ring=128 download_ring=32 compile_workers=3 async_threads=2 parallel_threads=2 "
            "helpers<=0 cache_save=180s/60s fresh_stack_clear=off recycle_spilled=off oom_recovery=on\n"
            "RAM usage: t=30 ws=2100 private=6400 commit=30000/40858 avail=900 faults=900 hard=? vk_local=1200/5560 "
            "vk_host=300/3711 spilled=0 noaccess=4\n"
            "vmaCreateImage failed: ErrorOutOfDeviceMemory (-2) for image extent=3840x2160x1 format=97 layers=1 "
            "levels=1 samples=1\n"
            "--- Build ---\n"
            "Fork build some-du6e/KytyPS5 1234abc\n"
            "--- Error ---\n"
            "TextureCache: failed to create image: extent=3840x2160x1 in "
            "D:\\a\\KytyPS5\\KytyPS5\\src\\graphics\\host_gpu\\renderer\\cache\\textureCache.cpp:1200\n"
            "RAM usage: t=95 ws=2843 private=9875 commit=38900/40858 avail=150 faults=? hard=?\n")
        rep = u.kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(text.encode("utf-8")))
        self.assertEqual(rep["outcome"], "crash")
        hm = rep["host_memory"]
        self.assertEqual(hm["profile"]["profile"], "low")
        self.assertTrue(hm["profile"]["settings"].startswith("upload_ring=128 download_ring=32 "))
        self.assertEqual(hm["startup"]["profile"], "low")
        last = hm["last"]
        self.assertEqual((last["t"], last["commit_percent"], last["faults_per_s"], last["hard_faults_per_s"]),
                         (95, 95, None, None))
        self.assertNotIn("noaccess_mib", last)
        # A build before the oneoff field: its samples parse, without one.
        self.assertEqual(hm["first"]["noaccess_mib"], 4)
        self.assertNotIn("one_off_buffers", hm["first"])
        self.assertIsNone(hm["one_off"])
        ids = [a["rule_id"] for a in rep["fix_areas"]]
        self.assertEqual(ids[0], "vram-oom")
        self.assertIn("ram-commit-full", ids)
        self.assertNotIn("ram-noaccess-bluescreen", ids)
        self.assertNotIn("ram-low-ram-normal-profile", ids)
        self.assertNotIn("ram-available-low", ids)

    def test_reserve_failure_keeps_its_rule(self):
        # GuestBackingStore's message with the commit advice that follows it (FormatReserveAdvice()).
        text = (
            "--- Build ---\n"
            "Fork build some-du6e/KytyPS5 1234abc\n"
            "--- Error ---\n"
            "could not reserve 12736 MB for guest direct memory: 0x000005af. Windows commits\n"
            "this up front, so the paging file is usually what needs to be larger.\n"
            "Commit: 9000 MiB free of a 24474 MiB limit, 12736 MiB needed. Add about 24576 MiB of page file, fixed "
            "size, on any drive with space (System Properties > Advanced > Performance Settings > Advanced > Virtual "
            "memory > Change, untick Automatically manage paging file size for all drives, pick the drive, Custom "
            "size), then restart Windows.\n"
            " in D:\\a\\KytyPS5\\KytyPS5\\src\\kernel\\memoryAddressSpace.inc:142\n")
        rep = u.kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(text.encode("utf-8")))
        self.assertEqual((rep["primary"]["rule_id"], rep["primary"]["crash_site"]),
                         ("guest-memory", "memoryAddressSpace.inc:142"))
        self.assertIn("Add about 24576 MiB of page file", rep["primary"]["message"])
        self.assertNotIn("host_memory", rep)

    def test_bluescreen_hint_needs_little_ram_at_the_end(self):
        # A healthy session on 32 GB: NOACCESS pages, then the log stops (preflight's time limit, or a window
        # closed: the clean exit lines never reach a session log). No blue screen hint, and nothing above no-fatal.
        text = (
            "--- Session 2026-11-19 21:00:00 ---\n"
            "Fork build some-du6e/KytyPS5 1234abc\n"
            "Title ID: PPSA03420\n"
            "RAM: phys=32690 MiB avail=24000 commit=21000/52000 headroom=31000 pagefile=~19310 guest=12736 "
            "profile=normal\n"
            "RAM usage: t=30 ws=6100 private=16400 commit=26000/52000 avail=19000 faults=900 hard=? vk_local=1200/5560 "
            "vk_host=300/3711 spilled=0 noaccess=2\n"
            "RAM usage: t=60 ws=6420 private=16810 commit=26650/52000 avail=18800 faults=2210 hard=? "
            "vk_local=3300/5560 vk_host=820/3711 spilled=0 noaccess=4\n")
        rep = u.kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(text.encode("utf-8")))
        self.assertEqual(rep["outcome"], "no-fatal")
        self.assertEqual(rep["host_memory"]["noaccess_samples"], 2)
        self.assertEqual([a["rule_id"] for a in rep["fix_areas"]], ["no-fatal-no-fatal"])
        # The same end with little RAM left gets the hint, ranked under no-fatal.
        low = text.replace("avail=18800", "avail=180")
        rep = u.kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(low.encode("utf-8")))
        self.assertEqual([a["rule_id"] for a in rep["fix_areas"]], ["no-fatal-no-fatal", "ram-noaccess-bluescreen"])
        # Little RAM, but no NOACCESS page in the last samples: no hint.
        none = low.replace("noaccess=2", "noaccess=0").replace("noaccess=4", "noaccess=0")
        rep = u.kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(none.encode("utf-8")))
        self.assertNotIn("ram-noaccess-bluescreen", [a["rule_id"] for a in rep["fix_areas"]])

    def test_commit_full_only_where_the_limit_holds(self):
        # Linux overcommits: Committed_AS past CommitLimit is harmless, and the emulator does not warn there.
        head = "--- Session 2026-11-19 21:00:00 ---\nFork build some-du6e/KytyPS5 1234abc\n"
        usage = ("RAM usage: t=30 ws=6100 private=? commit=%s avail=19000 faults=900 hard=3 vk_local=1200/5560 "
                 "vk_host=300/3711 spilled=0 noaccess=0\n")
        cases = [
            # (what tells the platform, the last sample's commit, whether the rule fires)
            ("Host: cpu=AMD Ryzen 5 5600 6-Core Processor threads=12 ram=31900 MiB os=Linux 6.8.0-45-generic\n",
             "36650/20000", False),
            ("Host: cpu=AMD Ryzen 5 5600 6-Core Processor threads=12 ram=31900 MiB os=Linux 6.8.0-45-generic\n",
             "19500/20000", False),
            ("RAM: phys=31900 MiB avail=24000 commit=19000/20000 headroom=1000 pagefile=4096 guest=12736 "
             "profile=normal\n", "19500/20000", False),
            ("RAM: phys=8090 MiB avail=4712 commit=18242/24474 headroom=6232 pagefile=~16384 guest=12736 "
             "profile=low\n", "23400/24474", True),
            ("Host: cpu=AMD Ryzen 5 5600 6-Core Processor threads=12 ram=8090 MiB os=Windows 10.0.26100\n",
             "23400/24474", True),
            # Nothing tells: a charge within the limit counts, one past it cannot be Windows.
            ("", "23400/24474", True),
            ("", "36650/20000", False),
        ]
        for where, commit, fires in cases:
            with self.subTest(where=where.strip(), commit=commit):
                text = head + where + usage % commit
                rep = u.kt.triage(["-"], rules=u.RULES, stdin=io.BytesIO(text.encode("utf-8")))
                self.assertEqual("ram-commit-full" in [a["rule_id"] for a in rep["fix_areas"]], fires)

    def test_ram_lines_count_once_across_sources(self):
        # A preflight zip has each RAM line twice, in session.log and in the console's emulator-boot.log.
        single = u.report("synthetic-ram-lines.txt")["host_memory"]
        folder = tempfile.mkdtemp(prefix="kyty-triage-ram-")
        try:
            path = os.path.join(folder, "run.zip")
            with open(u.fixture("synthetic-ram-lines.txt"), "rb") as f:
                data = f.read()
            with zipfile.ZipFile(path, "w") as z:
                z.writestr("kyty-preflight-X/session.log", data)
                z.writestr("kyty-preflight-X/emulator-boot.log", data)
            rep = u.kt.triage([path], rules=u.RULES)
        finally:
            shutil.rmtree(folder)
        hm = rep["host_memory"]
        for key in ("samples", "warning_count", "samples_with_available", "low_available_samples", "noaccess_samples",
                    "max_noaccess_mib", "peak_commit_percent"):
            self.assertEqual(hm[key], single[key], key)
        self.assertEqual(len(hm["warnings"]), 2)
        self.assertEqual([s["t"] for s in hm["tail"]], [90, 120, 150, 180])

    def test_reports_without_ram_lines_keep_their_shape(self):
        for case in ("synthetic-contract-session.txt", "gta5-829-v22-session-log.txt"):
            with self.subTest(case=case):
                rep = u.report(case)
                self.assertNotIn("host_memory", rep)
                self.assertNotIn("## Host memory", u.kt.render_markdown(rep))


if __name__ == "__main__":
    unittest.main()
