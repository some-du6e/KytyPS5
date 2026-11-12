"""Every line of notes/log-contract.md is parsed into the right report field (synthetic fixtures)."""

import os
import sys
import unittest

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
        self.assertEqual([p["name"] for p in ident["presets"]], ["low-vram", "nvidia-safe"])
        self.assertEqual(ident["presets"][0]["tokens"], "--vram-profile low --oom-recovery")
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


if __name__ == "__main__":
    unittest.main()
