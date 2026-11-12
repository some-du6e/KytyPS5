// gta6-prep: shader-mem
// Execution tests for the memory, image and LDS opcodes of the gta6-prep block, with C++
// reference values. Encodings come from llvm-mc 18 (-mcpu=gfx1030). Included into
// ShaderRecompilerComputeTests.cpp, inside its anonymous namespace, before MakeCases().

TestCase ShaderMemSingleThread(TestCase test) {
  test.compute_info.threads_num[0] = 1;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.has_compute_info = true;
  return test;
}

u32 ShaderMemF32Bits(float value) { return std::bit_cast<u32>(value); }

// ds_add_f32/rtn, ds_cmpst_b32 (DATA compares, DATA2 replaces), ds_cmpst_rtn_f32 (float
// equality), ds_rsub_rtn_u32, ds_max_rtn_f32 and ds_mskor_rtn_b32 on one lane.
TestCase ShaderMemDsAtomics32() {
  using O = ShaderOpcode;
  TestCase test;
  test.name = "ShaderMemDsAtomics32";
  auto &code = test.code;
  const auto result = [&](u32 reg, u32 value) {
    AppendStoreVgpr(&code, reg, static_cast<u32>(test.expected.size()));
    test.expected.push_back(value);
  };
  const auto write_lds = [&](u32 offset, u32 value) {
    AppendVMovLiteral(&code, 3, value);
    code.push_back(EncodeDs0(0x0d, offset));
    code.push_back(EncodeDs1(0, 3, 7));
  };
  const auto read_lds = [&](u32 offset, u32 value) {
    code.push_back(EncodeDs0(0x36, offset));
    code.push_back(EncodeDs1(8, 0, 7));
    result(8, value);
  };
  // DS words: VDST in [31:24], DATA2 in [23:16], DATA in [15:8], ADDR in [7:0].
  const auto ds = [&](u32 opcode, u32 offset, u32 vdst, u32 data1, u32 data0) {
    code.push_back(EncodeDs0(opcode, offset));
    code.push_back(EncodeDs1Ex(vdst, data1, data0, 7));
  };
  AppendVMovU32(&code, 7, 0);
  AppendVMovLiteral(&code, 5, 0x13579bdfu);

  // ds_add_rtn_f32: 1.5 + 2.25 = 3.75, returns 1.5. ds_add_f32 then adds -0.75.
  write_lds(0, ShaderMemF32Bits(1.5f));
  AppendVMovLiteral(&code, 2, ShaderMemF32Bits(2.25f));
  ds(0x55, 0, 5, 0, 2);
  result(5, ShaderMemF32Bits(1.5f));
  AppendVMovLiteral(&code, 2, ShaderMemF32Bits(-0.75f));
  ds(0x15, 0, 0, 0, 2);
  read_lds(0, ShaderMemF32Bits(1.5f + 2.25f - 0.75f));

  // ds_cmpst_b32: MEM == DATA ? DATA2 : MEM. With MUBUF's order (DATA replaces, DATA2 compares)
  // memory would keep 7.
  write_lds(4, 7);
  AppendVMovU32(&code, 2, 7);
  AppendVMovU32(&code, 3, 9);
  ds(0x10, 4, 0, 3, 2);
  read_lds(4, 9);
  AppendVMovU32(&code, 2, 11);
  AppendVMovU32(&code, 3, 9);
  ds(0x10, 4, 0, 3, 2);
  read_lds(4, 9);

  // ds_cmpst_rtn_f32: -0 == +0 as floats, NaN never compares equal (not even to itself).
  write_lds(8, 0x80000000u);
  AppendVMovU32(&code, 2, 0);
  AppendVMovLiteral(&code, 4, ShaderMemF32Bits(2.0f));
  ds(0x31, 8, 5, 4, 2);
  result(5, 0x80000000u);
  read_lds(8, ShaderMemF32Bits(2.0f));
  write_lds(12, 0x7fc00000u);
  AppendVMovLiteral(&code, 2, 0x7fc00000u);
  AppendVMovLiteral(&code, 4, ShaderMemF32Bits(1.0f));
  ds(0x31, 12, 5, 4, 2);
  result(5, 0x7fc00000u);
  read_lds(12, 0x7fc00000u);

  // ds_rsub_rtn_u32: MEM = DATA - MEM = 3 - 5.
  write_lds(16, 5);
  AppendVMovU32(&code, 2, 3);
  ds(0x22, 16, 5, 0, 2);
  result(5, 5);
  read_lds(16, 0xfffffffeu);

  // ds_max_rtn_f32 returns the old value.
  write_lds(20, ShaderMemF32Bits(1.0f));
  AppendVMovLiteral(&code, 2, ShaderMemF32Bits(2.0f));
  ds(0x33, 20, 5, 0, 2);
  result(5, ShaderMemF32Bits(1.0f));
  read_lds(20, ShaderMemF32Bits(2.0f));

  // ds_mskor_rtn_b32: MEM = (MEM & ~DATA) | DATA2.
  write_lds(24, 0xff00ff00u);
  AppendVMovLiteral(&code, 2, 0x0000ffffu);
  AppendVMovLiteral(&code, 4, 0x00001234u);
  ds(0x2c, 24, 5, 4, 2);
  result(5, 0xff00ff00u);
  read_lds(24, (0xff00ff00u & ~0x0000ffffu) | 0x00001234u);

  // EXEC = 0: ds_add_rtn_f32 keeps memory and the destination.
  AppendVMovLiteral(&code, 5, 0x2468ace0u);
  AppendVMovLiteral(&code, 2, ShaderMemF32Bits(100.0f));
  code.push_back(EncodeSop1(0x04, 10, 126)); // s_mov_b64 s[10:11], exec
  code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
  ds(0x55, 0, 5, 0, 2);
  code.push_back(EncodeSop1(0x04, 126, 10));
  result(5, 0x2468ace0u);
  read_lds(0, ShaderMemF32Bits(1.5f + 2.25f - 0.75f));
  AppendEnd(&code);

  test.compute_info.lds_size_dwords = 8;
  test.opcodes = {O::V_MOV_B32,       O::S_MOV_B64,        O::DS_WRITE_B32,
                  O::DS_READ_B32,     O::DS_ADD_F32,       O::DS_ADD_RTN_F32,
                  O::DS_CMPST_B32,    O::DS_CMPST_RTN_F32, O::DS_RSUB_RTN_U32,
                  O::DS_MAX_RTN_F32,  O::DS_MSKOR_RTN_B32, O::BUFFER_STORE_DWORD,
                  O::S_ENDPGM};
  test.required_spirv = {"OpAtomicCompareExchange %uint", "OpFAdd", "OpFOrdEqual"};
  return ShaderMemSingleThread(std::move(test));
}

// Every lane of a 128-thread workgroup adds 1.0 (and 0.5 through the return form) to one LDS
// DWORD; the compare-exchange loop must not lose updates under contention.
TestCase ShaderMemDsAddF32Contention(u32 wave_size) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = wave_size == 32 ? "ShaderMemDsAddF32ContentionWave32"
                              : "ShaderMemDsAddF32ContentionWave64";
  auto &code = test.code;
  AppendVMovU32(&code, 7, 0);
  code.push_back(EncodeSop1(0x04, 10, 126));          // s_mov_b64 s[10:11], exec
  code.push_back(EncodeVopc(0xc2, InlineU32(0), 0));  // v_cmp_eq_u32 vcc, 0, v0
  code.push_back(EncodeSop1(0x04, 126, 106));         // s_mov_b64 exec, vcc
  code.push_back(EncodeDs0(0x0d, 0));
  code.push_back(EncodeDs1(0, 7, 7));
  code.push_back(EncodeSop1(0x04, 126, 10));
  code.push_back(EncodeSopp(0x0a, 0)); // s_barrier
  AppendVMovLiteral(&code, 2, ShaderMemF32Bits(1.0f));
  code.push_back(EncodeDs0(0x15, 0));
  code.push_back(EncodeDs1Ex(0, 0, 2, 7));
  AppendVMovLiteral(&code, 2, ShaderMemF32Bits(0.5f));
  code.push_back(EncodeDs0(0x55, 0));
  code.push_back(EncodeDs1Ex(5, 0, 2, 7));
  code.push_back(EncodeSopp(0x0a, 0));
  code.push_back(EncodeDs0(0x36, 0));
  code.push_back(EncodeDs1(8, 0, 7));
  AppendStoreVgprAtLaneDwordOffset(&code, 8, 0, 0);
  AppendEnd(&code);
  test.expected.assign(128, ShaderMemF32Bits(128.0f * 1.5f));
  test.compute_info.threads_num[0] = 128;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = wave_size;
  test.compute_info.lds_size_dwords = 1;
  test.has_compute_info = true;
  test.opcodes = {O::V_MOV_B32,   O::S_MOV_B64,      O::V_CMP_EQ_U32,
                  O::S_BARRIER,   O::DS_WRITE_B32,   O::DS_ADD_F32,
                  O::DS_ADD_RTN_F32, O::DS_READ_B32, O::V_LSHLREV_B32,
                  O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  return test;
}

void AppendShaderMemCases(std::vector<TestCase> &cases) {
  cases.push_back(ShaderMemDsAtomics32());
  for (u32 wave_size : {32u, 64u}) cases.push_back(ShaderMemDsAddF32Contention(wave_size));
}
