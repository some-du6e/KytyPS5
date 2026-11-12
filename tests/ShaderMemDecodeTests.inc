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

// DS QWORD atomics read DATA (and DATA2) as VGPR pairs; MUBUF _x2 atomics return the old QWORD
// with GLC and CMPSWAP_X2 reads its source and comparison pairs from VDATA[3:0].
void TestShaderMemAtomics64() {
  using O = ShaderRecompiler::Decoder::Opcode;
  const ShaderMemDecodeCase cases[] = {
      {"ds_sub_u64 v1, v[2:3] offset:8", {0xd9040008u, 0x00000201u}, O::DS_SUB_U64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicISub64", "OpAtomicISub %ulong"},
      {"ds_rsub_u64 v1, v[2:3] offset:8", {0xd9080008u, 0x00000201u}, O::DS_RSUB_U64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicRSub64", "OpAtomicCompareExchange %ulong"},
      {"ds_inc_u64 v1, v[2:3] offset:8", {0xd90c0008u, 0x00000201u}, O::DS_INC_U64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicInc64", "OpAtomicCompareExchange %ulong"},
      {"ds_dec_u64 v1, v[2:3] offset:8", {0xd9100008u, 0x00000201u}, O::DS_DEC_U64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicDec64", "OpAtomicCompareExchange %ulong"},
      {"ds_min_i64 v1, v[2:3] offset:8", {0xd9140008u, 0x00000201u}, O::DS_MIN_I64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicSMin64", "OpAtomicSMin %ulong"},
      {"ds_max_i64 v1, v[2:3] offset:8", {0xd9180008u, 0x00000201u}, O::DS_MAX_I64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicSMax64", "OpAtomicSMax %ulong"},
      {"ds_min_u64 v1, v[2:3] offset:8", {0xd91c0008u, 0x00000201u}, O::DS_MIN_U64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicUMin64", "OpAtomicUMin %ulong"},
      {"ds_max_u64 v1, v[2:3] offset:8", {0xd9200008u, 0x00000201u}, O::DS_MAX_U64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicUMax64", "OpAtomicUMax %ulong"},
      {"ds_and_b64 v1, v[2:3] offset:8", {0xd9240008u, 0x00000201u}, O::DS_AND_B64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicAnd64", "OpAtomicAnd %ulong"},
      {"ds_xor_b64 v1, v[2:3] offset:8", {0xd92c0008u, 0x00000201u}, O::DS_XOR_B64, 2, -1, 1, 2, -1, 8, 2, "SharedAtomicXor64", "OpAtomicXor %ulong"},
      {"ds_mskor_b64 v1, v[2:3], v[4:5] offset:8", {0xd9300008u, 0x00040201u}, O::DS_MSKOR_B64, 3, -1, 1, 2, 4, 8, 2, "SharedAtomicMaskedOr64", "OpAtomicCompareExchange %ulong"},
      {"ds_cmpst_b64 v1, v[2:3], v[4:5] offset:8", {0xd9400008u, 0x00040201u}, O::DS_CMPST_B64, 3, -1, 1, 2, 4, 8, 2, "SharedAtomicCmpSwap64", "OpAtomicCompareExchange %ulong"},
      {"ds_add_rtn_u64 v[6:7], v1, v[2:3] offset:8", {0xd9800008u, 0x06000201u}, O::DS_ADD_RTN_U64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicIAddReturn64", "OpAtomicIAdd %ulong"},
      {"ds_sub_rtn_u64 v[6:7], v1, v[2:3] offset:8", {0xd9840008u, 0x06000201u}, O::DS_SUB_RTN_U64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicISub64", "OpAtomicISub %ulong"},
      {"ds_rsub_rtn_u64 v[6:7], v1, v[2:3] offset:8", {0xd9880008u, 0x06000201u}, O::DS_RSUB_RTN_U64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicRSub64", "OpAtomicCompareExchange %ulong"},
      {"ds_inc_rtn_u64 v[6:7], v1, v[2:3] offset:8", {0xd98c0008u, 0x06000201u}, O::DS_INC_RTN_U64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicInc64", "OpAtomicCompareExchange %ulong"},
      {"ds_dec_rtn_u64 v[6:7], v1, v[2:3] offset:8", {0xd9900008u, 0x06000201u}, O::DS_DEC_RTN_U64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicDec64", "OpAtomicCompareExchange %ulong"},
      {"ds_min_rtn_i64 v[6:7], v1, v[2:3] offset:8", {0xd9940008u, 0x06000201u}, O::DS_MIN_RTN_I64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicSMin64", "OpAtomicSMin %ulong"},
      {"ds_max_rtn_i64 v[6:7], v1, v[2:3] offset:8", {0xd9980008u, 0x06000201u}, O::DS_MAX_RTN_I64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicSMax64", "OpAtomicSMax %ulong"},
      {"ds_min_rtn_u64 v[6:7], v1, v[2:3] offset:8", {0xd99c0008u, 0x06000201u}, O::DS_MIN_RTN_U64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicUMin64", "OpAtomicUMin %ulong"},
      {"ds_and_rtn_b64 v[6:7], v1, v[2:3] offset:8", {0xd9a40008u, 0x06000201u}, O::DS_AND_RTN_B64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicAnd64", "OpAtomicAnd %ulong"},
      {"ds_or_rtn_b64 v[6:7], v1, v[2:3] offset:8", {0xd9a80008u, 0x06000201u}, O::DS_OR_RTN_B64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicOrReturn64", "OpAtomicOr %ulong"},
      {"ds_xor_rtn_b64 v[6:7], v1, v[2:3] offset:8", {0xd9ac0008u, 0x06000201u}, O::DS_XOR_RTN_B64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicXor64", "OpAtomicXor %ulong"},
      {"ds_mskor_rtn_b64 v[6:7], v1, v[2:3], v[4:5] offset:8", {0xd9b00008u, 0x06040201u}, O::DS_MSKOR_RTN_B64, 3, 6, 1, 2, 4, 8, 2, "SharedAtomicMaskedOr64", "OpAtomicCompareExchange %ulong"},
      {"ds_wrxchg_rtn_b64 v[6:7], v1, v[2:3] offset:8", {0xd9b40008u, 0x06000201u}, O::DS_WRXCHG_RTN_B64, 2, 6, 1, 2, -1, 8, 2, "SharedAtomicSwap64", "OpAtomicExchange %ulong"},
      {"buffer_atomic_cmpswap_x2 v[4:7], v1, s[0:3], 0 offen glc", {0xe1445000u, 0x80000401u}, O::BUFFER_ATOMIC_CMPSWAP_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicCmpSwap64", "OpAtomicCompareExchange %ulong"},
      {"buffer_atomic_sub_x2 v[4:5], v1, s[0:3], 0 offen glc", {0xe14c5000u, 0x80000401u}, O::BUFFER_ATOMIC_SUB_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicISub64", "OpAtomicISub %ulong"},
      {"buffer_atomic_smin_x2 v[4:5], v1, s[0:3], 0 offen glc", {0xe1545000u, 0x80000401u}, O::BUFFER_ATOMIC_SMIN_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicSMin64", "OpAtomicSMin %ulong"},
      {"buffer_atomic_smax_x2 v[4:5], v1, s[0:3], 0 offen glc", {0xe15c5000u, 0x80000401u}, O::BUFFER_ATOMIC_SMAX_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicSMax64", "OpAtomicSMax %ulong"},
      {"buffer_atomic_umax_x2 v[4:5], v1, s[0:3], 0 offen glc", {0xe1605000u, 0x80000401u}, O::BUFFER_ATOMIC_UMAX_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicUMax64", "OpAtomicUMax %ulong"},
      {"buffer_atomic_xor_x2 v[4:5], v1, s[0:3], 0 offen glc", {0xe16c5000u, 0x80000401u}, O::BUFFER_ATOMIC_XOR_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicXor64", "OpAtomicXor %ulong"},
      {"buffer_atomic_inc_x2 v[4:5], v1, s[0:3], 0 offen glc", {0xe1705000u, 0x80000401u}, O::BUFFER_ATOMIC_INC_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicInc64", "OpAtomicCompareExchange %ulong"},
      {"buffer_atomic_dec_x2 v[4:5], v1, s[0:3], 0 offen glc", {0xe1745000u, 0x80000401u}, O::BUFFER_ATOMIC_DEC_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicDec64", "OpAtomicCompareExchange %ulong"},
      {"buffer_atomic_umax_x2 v[4:5], v1, s[0:3], 0 offen", {0xe1601000u, 0x80000401u}, O::BUFFER_ATOMIC_UMAX_X2, 3, 4, 1, -1, -1, 0, 2, "BufferAtomicUMax64", "OpAtomicUMax %ulong"},
  };
  for (const auto &test : cases) CheckShaderMemDecodeCase(test);

  ShaderRecompiler::Decoder::Instruction gds;
  const uint32_t gds_words[] = {0xd9840008u | (1u << 17u), 0x06000201u};
  ShaderRecompiler::Decoder::DecodeInstruction(gds_words, 0u, gds);
  Check(gds.opcode == O::UNSUPPORTED, "DS 64-bit atomics must reject GDS");
}

// Wrapping inc/dec and float compare-swap for MUBUF; sub/inc/dec/fcmpswap for MIMG (decode only:
// the image forms run in ShaderRecompilerComputeTests with a real storage image).
void TestShaderMemIncDecAtomics() {
  using O = ShaderRecompiler::Decoder::Opcode;
  const ShaderMemDecodeCase cases[] = {
      {"buffer_atomic_inc v4, v1, s[0:3], 0 offen glc", {0xe0f05000u, 0x80000401u},
       O::BUFFER_ATOMIC_INC, 3, 4, 1, -1, -1, 0, 1, "BufferAtomicInc32", "OpAtomicCompareExchange"},
      {"buffer_atomic_dec v4, v1, s[0:3], 0 offen glc", {0xe0f45000u, 0x80000401u},
       O::BUFFER_ATOMIC_DEC, 3, 4, 1, -1, -1, 0, 1, "BufferAtomicDec32", "OpAtomicCompareExchange"},
      {"buffer_atomic_fcmpswap v[4:5], v1, s[0:3], 0 offen glc", {0xe0f85000u, 0x80000401u},
       O::BUFFER_ATOMIC_FCMPSWAP, 3, 4, 1, -1, -1, 0, 1, "BufferAtomicFCmpSwap32", "OpFOrdEqual"},
      {"image_atomic_sub v4, v[0:1], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm glc",
       {0xf0483108u, 0x00020400u}, O::IMAGE_ATOMIC_SUB, 3, 4, 0, -1, -1, 0, 1, nullptr, nullptr,
       false},
      {"image_atomic_inc v4, v[0:1], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm glc",
       {0xf06c3108u, 0x00020400u}, O::IMAGE_ATOMIC_INC, 3, 4, 0, -1, -1, 0, 1, nullptr, nullptr,
       false},
      {"image_atomic_dec v4, v[0:1], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm glc",
       {0xf0703108u, 0x00020400u}, O::IMAGE_ATOMIC_DEC, 3, 4, 0, -1, -1, 0, 1, nullptr, nullptr,
       false},
      {"image_atomic_fcmpswap v[4:5], v[0:1], s[8:15] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc",
       {0xf0743308u, 0x00020400u}, O::IMAGE_ATOMIC_FCMPSWAP, 3, 4, 0, -1, -1, 0, 2, nullptr,
       nullptr, false},
  };
  for (const auto &test : cases) CheckShaderMemDecodeCase(test);

  // Only the compare-swaps take two DWORDs per texel; a 64-bit inc is not implemented.
  ShaderRecompiler::Decoder::Instruction decoded;
  const uint32_t inc64[] = {0xf06c3308u, 0x00020400u};
  ShaderRecompiler::Decoder::DecodeInstruction(inc64, 0u, decoded);
  Check(decoded.opcode == O::UNSUPPORTED, "IMAGE_ATOMIC_INC dmask 0x3 must stay unsupported");
}

void RunShaderMemDecodeTests() {
  TestShaderMemIncDecAtomics();
  TestShaderMemDsAtomics32();
  TestShaderMemAtomics64();
}
