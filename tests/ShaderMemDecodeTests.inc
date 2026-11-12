// gta6-prep: shader-mem
// Decode and SPIR-V validation tests for the memory, image and LDS opcodes of the gta6-prep block.
// Every encoding comes from llvm-mc 18 (-arch=amdgcn -mcpu=gfx1030 -show-encoding); the
// assembly is next to it. Included into shaderCfgTests.cpp, inside its anonymous namespace.

struct ShaderMemDecodeCase {
  const char *assembly;
  std::vector<uint32_t> words;
  ShaderRecompiler::Decoder::Opcode opcode;
  uint32_t src_count;
  int dst;  // VGPR, or -1 to skip
  int src0; // VGPR, or -1 to skip
  int src1;
  int src2;
  uint32_t offset;
  uint32_t data_dwords;
  const char *ir;    // IR opcode name the program must contain, or nullptr
  const char *spirv; // disassembly text the module must contain, or nullptr
  bool compile = true;
};

void CheckShaderMemDecodeCase(const ShaderMemDecodeCase &test) {
  using namespace ShaderRecompiler::Decoder;
  const auto fail = [&](const char *what) {
    std::fprintf(stderr, "ShaderCfgTests: %s: %s\n", test.assembly, what);
    std::abort();
  };
  Instruction decoded;
  DecodeInstruction(test.words, 0u, decoded);
  if (decoded.opcode != test.opcode) fail("decoded to the wrong opcode");
  if (decoded.word_count != test.words.size()) fail("decoded to the wrong word count");
  if (decoded.src_count != test.src_count) fail("decoded the wrong source count");
  if (test.dst >= 0 && decoded.dst.reg != static_cast<uint32_t>(test.dst)) fail("wrong VDST/VDATA");
  if (test.src0 >= 0 && decoded.src0.reg != static_cast<uint32_t>(test.src0)) fail("wrong src0");
  if (test.src1 >= 0 && decoded.src1.reg != static_cast<uint32_t>(test.src1)) fail("wrong src1");
  if (test.src2 >= 0 && decoded.src2.reg != static_cast<uint32_t>(test.src2)) fail("wrong src2");
  if (decoded.offset != test.offset) fail("wrong offset");
  if (decoded.data_dwords != test.data_dwords) fail("wrong data width");
  const auto text = InstructionToString(decoded);
  if (text.find(magic_enum::enum_name(test.opcode)) == std::string::npos ||
      text.find("unsupported") != std::string::npos) {
    fail("decoder text does not name the opcode");
  }
  if (!test.compile) return;

  std::vector<uint32_t> shader = test.words;
  shader.push_back(0xbf810000u); // s_endpgm
  auto options = MakeCompileOptions(ShaderType::Compute);
  options.dump_ir = true;
  const auto result = RecompileForTest(shader, options);
  if (test.ir != nullptr && result.ir_dump.find(test.ir) == std::string::npos) {
    fail("program does not contain the expected IR opcode");
  }
  CheckSpirvBinaryValidates(result.spirv);
  if (test.spirv != nullptr &&
      DisassembleSpirvBinary(result.spirv).find(test.spirv) == std::string::npos) {
    fail("SPIR-V does not contain the expected operation");
  }
}

void TestShaderMemDsAtomics32() {
  using O = ShaderRecompiler::Decoder::Opcode;
  const ShaderMemDecodeCase cases[] = {
      {"ds_rsub_u32 v1, v2 offset:4", {0xd8080004u, 0x00000201u}, O::DS_RSUB_U32, 2, -1, 1, 2, -1,
       4, 1, "SharedAtomicRSub32", "OpAtomicCompareExchange"},
      {"ds_rsub_rtn_u32 v5, v1, v2 offset:8", {0xd8880008u, 0x05000201u}, O::DS_RSUB_RTN_U32, 2, 5,
       1, 2, -1, 8, 1, "SharedAtomicRSub32", "OpAtomicCompareExchange"},
      // DATA (v2) compares and DATA2 (v3) replaces, the opposite of MUBUF cmpswap.
      {"ds_cmpst_b32 v1, v2, v3 offset:12", {0xd840000cu, 0x00030201u}, O::DS_CMPST_B32, 3, -1, 1,
       2, 3, 12, 1, "SharedAtomicCmpSwap32", "OpAtomicCompareExchange"},
      {"ds_cmpst_f32 v1, v2, v3 offset:16", {0xd8440010u, 0x00030201u}, O::DS_CMPST_F32, 3, -1, 1,
       2, 3, 16, 1, "SharedAtomicCmpSwapF32", "OpFOrdEqual"},
      {"ds_cmpst_rtn_f32 v5, v1, v2, v3 offset:20", {0xd8c40014u, 0x05030201u},
       O::DS_CMPST_RTN_F32, 3, 5, 1, 2, 3, 20, 1, "SharedAtomicCmpSwapF32", "OpFOrdEqual"},
      {"ds_add_f32 v1, v2 offset:24", {0xd8540018u, 0x00000201u}, O::DS_ADD_F32, 2, -1, 1, 2, -1,
       24, 1, "SharedAtomicFAdd32", "OpFAdd"},
      {"ds_add_rtn_f32 v5, v1, v2 offset:28", {0xd954001cu, 0x05000201u}, O::DS_ADD_RTN_F32, 2, 5,
       1, 2, -1, 28, 1, "SharedAtomicFAdd32", "OpFAdd"},
      {"ds_min_rtn_f32 v5, v1, v2 offset:32", {0xd8c80020u, 0x05000201u}, O::DS_MIN_RTN_F32, 2, 5,
       1, 2, -1, 32, 1, "SharedAtomicFMinReturn32", "OpAtomicCompareExchange"},
      {"ds_max_rtn_f32 v5, v1, v2 offset:36", {0xd8cc0024u, 0x05000201u}, O::DS_MAX_RTN_F32, 2, 5,
       1, 2, -1, 36, 1, "SharedAtomicFMaxReturn32", "OpAtomicCompareExchange"},
      {"ds_mskor_rtn_b32 v5, v1, v2, v3 offset:40", {0xd8b00028u, 0x05030201u},
       O::DS_MSKOR_RTN_B32, 3, 5, 1, 2, 3, 40, 1, "SharedAtomicMaskedOrReturn32",
       "OpAtomicCompareExchange"},
  };
  for (const auto &test : cases) CheckShaderMemDecodeCase(test);

  // Compare-stores keep rejecting GDS, like DS_CMPST_RTN_B32.
  ShaderRecompiler::Decoder::Instruction gds;
  const uint32_t gds_words[] = {0xd8440010u | (1u << 17u), 0x00030201u};
  ShaderRecompiler::Decoder::DecodeInstruction(gds_words, 0u, gds);
  Check(gds.opcode == O::UNSUPPORTED, "DS_CMPST_F32 must reject GDS");
}

void RunShaderMemDecodeTests() {
  TestShaderMemDsAtomics32();
}
