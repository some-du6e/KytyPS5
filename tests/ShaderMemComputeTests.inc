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

// MUBUF _x2 atomics on one lane: unsigned 64-bit max (a per-DWORD max would mix halves),
// cmpswap_x2 (VDATA[1:0] source, VDATA[3:2] comparison), wrapping inc/dec_x2 with carries,
// signed 64-bit min, borrowing sub_x2, xor_x2; GLC returns the old QWORD, GLC=0 does not and
// EXEC=0 changes nothing.
TestCase ShaderMemBufferAtomics64() {
  using O = ShaderOpcode;
  TestCase test;
  test.name = "ShaderMemBufferAtomics64";
  auto &code = test.code;
  constexpr u32 results = 16; // first result DWORD
  u32 next_result = results;
  std::vector<uint64_t> memory = {0x0000000180000000ull, 0x1111111122222222ull,
                                  0x00000000ffffffffull, 0ull,
                                  1ull,                  0x0000000100000000ull,
                                  0xf0f0f0f00f0f0f0full, 0x0123456789abcdefull};
  for (const auto value : memory) {
    test.initial.push_back(static_cast<u32>(value));
    test.initial.push_back(static_cast<u32>(value >> 32u));
  }
  test.initial.resize(results, 0u);
  test.expected = test.initial;
  const auto pair = [&](u32 reg, uint64_t value) {
    AppendVMovLiteral(&code, reg, static_cast<u32>(value));
    AppendVMovLiteral(&code, reg + 1u, static_cast<u32>(value >> 32u));
  };
  const auto stored = [&](u32 reg, uint64_t value) {
    for (u32 half = 0; half < 2; half++) {
      AppendStoreVgpr(&code, reg + half, next_result);
      test.expected.resize(next_result + 1u, 0u);
      test.expected[next_result++] = static_cast<u32>(value >> (32u * half));
    }
  };
  // Runs the atomic on QWORD `slot` with DATA in v[2:5] and checks the returned/kept VDATA.
  const auto atomic = [&](u32 opcode, u32 slot, uint64_t data, bool glc, uint64_t compare = 0) {
    AppendVMovU32(&code, 20, slot * 8u);
    pair(2, data);
    pair(4, compare);
    AppendBufferStoreOpcode(&code, opcode, 2, 20, glc);
    const auto old = memory[slot];
    uint64_t updated = old;
    switch (opcode) {
    case 0x58: updated = std::max(old, data); break;
    case 0x51: updated = old == compare ? data : old; break;
    case 0x5c: updated = old >= data ? 0 : old + 1; break;
    case 0x5d: updated = (old == 0 || old > data) ? data : old - 1; break;
    case 0x55:
      updated = static_cast<uint64_t>(
          std::min(static_cast<int64_t>(old), static_cast<int64_t>(data)));
      break;
    case 0x53: updated = old - data; break;
    case 0x5b: updated = old ^ data; break;
    default: std::abort();
    }
    memory[slot] = updated;
    stored(2, glc ? old : data);
  };
  atomic(0x58, 0, 0x00000000ffffffffull, true);  // umax_x2: high halves decide
  atomic(0x58, 0, 0x0000000200000000ull, true);
  atomic(0x51, 1, 0xaaaaaaaabbbbbbbbull, true, 0x1111111122222222ull); // cmpswap_x2 hit
  atomic(0x51, 1, 0x5555555566666666ull, true, 0x1111111122222222ull); // miss
  atomic(0x5c, 2, 0x0000000100000000ull, true);  // inc_x2: carry into the high DWORD
  atomic(0x5c, 2, 0x0000000100000000ull, true);  // old >= DATA wraps to 0
  atomic(0x5d, 3, 0x0000000500000000ull, true);  // dec_x2: 0 wraps to DATA
  atomic(0x5d, 3, 0x0000000500000000ull, true);  // borrow out of the low DWORD
  atomic(0x5d, 3, 0x0000000300000000ull, false); // old > DATA wraps to DATA, GLC=0
  atomic(0x55, 4, 0xffffffffffffffffull, true);  // smin_x2: -1 < 1
  atomic(0x53, 5, 1ull, true);                   // sub_x2 borrows
  atomic(0x5b, 6, 0xffffffff00000000ull, true);  // xor_x2
  // EXEC = 0: no update, VDATA kept.
  code.push_back(EncodeSop1(0x04, 10, 126));
  AppendVMovU32(&code, 20, 7u * 8u);
  pair(2, 0x1234567812345678ull);
  code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
  AppendBufferStoreOpcode(&code, 0x5c, 2, 20, true);
  code.push_back(EncodeSop1(0x04, 126, 10));
  stored(2, 0x1234567812345678ull);
  AppendEnd(&code);
  for (u32 slot = 0; slot < memory.size(); slot++) {
    test.expected[slot * 2u] = static_cast<u32>(memory[slot]);
    test.expected[slot * 2u + 1u] = static_cast<u32>(memory[slot] >> 32u);
  }
  test.opcodes = {O::V_MOV_B32,
                  O::S_MOV_B64,
                  O::BUFFER_ATOMIC_UMAX_X2,
                  O::BUFFER_ATOMIC_CMPSWAP_X2,
                  O::BUFFER_ATOMIC_INC_X2,
                  O::BUFFER_ATOMIC_DEC_X2,
                  O::BUFFER_ATOMIC_SMIN_X2,
                  O::BUFFER_ATOMIC_SUB_X2,
                  O::BUFFER_ATOMIC_XOR_X2,
                  O::BUFFER_STORE_DWORD,
                  O::S_ENDPGM};
  test.required_spirv = {"OpAtomicUMax %ulong", "OpAtomicCompareExchange %ulong", "Int64Atomics"};
  return ShaderMemSingleThread(std::move(test));
}

// DS QWORD atomics on one lane, against a C++ model of the LDS QWORDs.
TestCase ShaderMemDsAtomics64() {
  using O = ShaderOpcode;
  TestCase test;
  test.name = "ShaderMemDsAtomics64";
  auto &code = test.code;
  std::array<uint64_t, 6> lds = {0x00000000ffffffffull, 0ull, 5ull, 0xff00ff00ff00ff00ull,
                                 0x8000000000000000ull, 0x0123456789abcdefull};
  const auto pair = [&](u32 reg, uint64_t value) {
    AppendVMovLiteral(&code, reg, static_cast<u32>(value));
    AppendVMovLiteral(&code, reg + 1u, static_cast<u32>(value >> 32u));
  };
  const auto result = [&](u32 reg, uint64_t value) {
    for (u32 half = 0; half < 2; half++) {
      AppendStoreVgpr(&code, reg + half, static_cast<u32>(test.expected.size()));
      test.expected.push_back(static_cast<u32>(value >> (32u * half)));
    }
  };
  AppendVMovU32(&code, 7, 0);
  for (u32 slot = 0; slot < lds.size(); slot++) {
    pair(2, lds[slot]);
    code.push_back(EncodeDs0(0x4d, slot * 8u)); // ds_write_b64
    code.push_back(EncodeDs1(0, 2, 7));
  }
  // VDST v[6:7], DATA v[2:3], DATA2 v[4:5], ADDR v7.
  const auto ds = [&](u32 opcode, u32 slot, uint64_t data, uint64_t data2, bool rtn,
                      uint64_t updated) {
    pair(2, data);
    pair(4, data2);
    pair(6, 0x5a5a5a5aa5a5a5a5ull);
    code.push_back(EncodeDs0(opcode, slot * 8u));
    code.push_back(EncodeDs1Ex(6, 4, 2, 7));
    result(6, rtn ? lds[slot] : 0x5a5a5a5aa5a5a5a5ull);
    lds[slot] = updated;
  };
  ds(0x63, 0, 0x0000000100000000ull, 0, true, 0x0000000100000000ull); // inc_rtn: carry
  ds(0x63, 0, 0x0000000100000000ull, 0, true, 0);                     // wraps to 0
  ds(0x64, 1, 7, 0, true, 7);                                         // dec_rtn: 0 -> DATA
  ds(0x44, 1, 3, 0, false, 3);                                        // dec: 7 > 3 -> DATA
  ds(0x62, 2, 3, 0, true, 3ull - 5ull);                               // rsub_rtn: DATA - MEM
  ds(0x6c, 3, 0x00000000ffffffffull, 0x0000000012345678ull, true,
     (0xff00ff00ff00ff00ull & ~0x00000000ffffffffull) | 0x12345678ull); // mskor_rtn
  // cmpst_b64: DATA compares, DATA2 replaces.
  ds(0x50, 4, 0x8000000000000000ull, 0x7fffffffffffffffull, false, 0x7fffffffffffffffull);
  ds(0x50, 4, 0x8000000000000000ull, 1ull, false, 0x7fffffffffffffffull);
  ds(0x65, 4, 0xffffffffffffffffull, 0, true, 0xffffffffffffffffull); // min_rtn_i64: -1
  ds(0x48, 5, 0xf000000000000000ull, 0, false, 0xf000000000000000ull); // max_u64
  ds(0x6b, 5, 0x0f0f0f0f0f0f0f0full, 0, true, 0xf000000000000000ull ^ 0x0f0f0f0f0f0f0f0full);
  for (u32 slot = 0; slot < lds.size(); slot++) {
    code.push_back(EncodeDs0(0x76, slot * 8u)); // ds_read_b64
    code.push_back(EncodeDs1(8, 0, 7));
    result(8, lds[slot]);
  }
  AppendEnd(&code);
  test.compute_info.lds_size_dwords = 12;
  test.opcodes = {O::V_MOV_B32,         O::DS_WRITE_B64,     O::DS_READ_B64,
                  O::DS_INC_RTN_U64,    O::DS_DEC_RTN_U64,   O::DS_DEC_U64,
                  O::DS_RSUB_RTN_U64,   O::DS_MSKOR_RTN_B64, O::DS_CMPST_B64,
                  O::DS_MIN_RTN_I64,    O::DS_MAX_U64,       O::DS_XOR_RTN_B64,
                  O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.required_spirv = {"OpAtomicCompareExchange %ulong", "OpAtomicSMin %ulong",
                         "WorkgroupMemoryExplicitLayoutKHR"};
  return ShaderMemSingleThread(std::move(test));
}

void AppendShaderMemCases(std::vector<TestCase> &cases) {
  cases.push_back(ShaderMemBufferAtomics64());
  cases.push_back(ShaderMemDsAtomics64());
  cases.push_back(ShaderMemDsAtomics32());
  for (u32 wave_size : {32u, 64u}) cases.push_back(ShaderMemDsAddF32Contention(wave_size));
}
