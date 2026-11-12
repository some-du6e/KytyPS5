// gta6-prep: shader-alu. Tests of the shader recompiler that need no GPU: the RDNA2 ALU and scalar
// encodings decode to their opcodes (each checked against llvm-mc -mcpu=gfx1030 -show-encoding),
// programs using them translate and emit valid SPIR-V, and a program with an unsupported
// instruction is reported for skipping instead of closing the emulator.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "spirv-tools/libspirv.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {
namespace {

namespace Decoder = ShaderRecompiler::Decoder;
using Decoder::Opcode;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderAluDecodeTests: failed: %s\n", text);
		std::abort();
	}
}

void Check(bool value, const std::string& text) {
	Check(value, text.c_str());
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// EXIT ends the process with status 321 (see Common::DbgExitHandler).
template <typename Function>
void ExpectFatal(Function function, const char* text) {
	const pid_t pid = ::fork();
	Check(pid >= 0, "fork failed while starting a fatal shader test");
	if (pid == 0) {
		function();
		::_exit(0);
	}
	int   status = 0;
	pid_t waited = -1;
	do {
		waited = ::waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	Check(waited == pid, "waitpid failed while collecting a fatal shader test");
	Check(WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff), text);
}
#endif

void EnsureConfigInitialized() {
	static bool initialized = false;
	if (initialized) {
		return;
	}
	static Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	ShaderInit();
	initialized = true;
}

constexpr uint32_t EndPgm = 0xbf810000u;

constexpr uint32_t InlineU32(uint32_t value) {
	return 128u + value;
}

constexpr uint32_t Vgpr(uint32_t reg) {
	return 256u + reg;
}

constexpr uint32_t EncodeSop1(uint32_t opcode, uint32_t dst, uint32_t src) {
	return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) | ((opcode & 0xffu) << 8u) |
	       (src & 0xffu);
}

constexpr uint32_t EncodeSopk(uint32_t opcode, uint32_t dst, uint32_t imm) {
	return 0x80000000u | (((opcode + 0x60u) & 0x7fu) << 23u) | ((dst & 0x7fu) << 16u) |
	       (imm & 0xffffu);
}

constexpr uint32_t EncodeSopp(uint32_t opcode, uint32_t simm = 0) {
	return 0x80000000u | (0x7fu << 23u) | ((opcode & 0x7fu) << 16u) | (simm & 0xffffu);
}

constexpr uint32_t EncodeVop1(uint32_t opcode, uint32_t dst, uint32_t src0) {
	return (0x3fu << 25u) | ((dst & 0xffu) << 17u) | ((opcode & 0xffu) << 9u) | (src0 & 0x1ffu);
}

constexpr uint32_t EncodeVop3Word0(uint32_t opcode, uint32_t dst, uint32_t abs = 0,
                                   uint32_t op_sel = 0, bool clamp = false) {
	return (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | ((abs & 0x7u) << 8u) |
	       ((op_sel & 0xfu) << 11u) | (clamp ? (1u << 15u) : 0u) | (dst & 0xffu);
}

constexpr uint32_t EncodeVop3Word1(uint32_t src0, uint32_t src1, uint32_t src2 = 0,
                                   uint32_t omod = 0, uint32_t neg = 0) {
	return (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9u) | ((src2 & 0x1ffu) << 18u) |
	       ((omod & 0x3u) << 27u) | ((neg & 0x7u) << 29u);
}

// buffer_store_dword v<value>, v<address>, s[0:3] offen
constexpr uint32_t EncodeStore0() {
	return (0x38u << 26u) | (0x1cu << 18u) | (1u << 12u);
}

constexpr uint32_t EncodeStore1(uint32_t value, uint32_t address) {
	return (128u << 24u) | ((value & 0xffu) << 8u) | (address & 0xffu);
}

ShaderRecompiler::CompileOptions ComputeOptions(bool skip_unsupported) {
	static const ShaderComputeInputInfo compute = [] {
		ShaderComputeInputInfo info {};
		info.lds_size_dwords = 1024;
		return info;
	}();
	static const auto user_data = [] {
		std::array<uint32_t, 64> data {};
		data[2] = 0x1000u;      // num_records
		data[3] = 3u << 28u;    // raw offset bounds
		return data;
	}();
	ShaderRecompiler::CompileOptions options;
	options.stage                         = ShaderType::Compute;
	options.user_data                     = user_data;
	options.input_info.compute            = &compute;
	options.dump_ir                       = false;
	options.skip_unsupported_instructions = skip_unsupported;
	return options;
}

bool ReadNoMemory(void*, uint64_t, std::span<uint32_t>) {
	return false;
}

void CheckSpirvValidates(const std::vector<uint32_t>& binary, const char* name) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		char buffer[1024] = {};
		std::snprintf(buffer, sizeof(buffer), "%zu:%zu: %s\n", position.line, position.column,
		              message);
		messages += buffer;
	});
	if (!tools.Validate(binary)) {
		std::fprintf(stderr, "ShaderAluDecodeTests: %s: SPIR-V validation failed:\n%s\n", name,
		             messages.c_str());
		std::abort();
	}
}

// Translates and compiles a compute program; it must not be skipped.
void CompileToValidSpirv(std::span<const uint32_t> code, const char* name, uint32_t wave_size = 64,
                         const ShaderRecompiler::Spirv::Profile* profile = nullptr) {
	auto options                 = ComputeOptions(true);
	if (profile != nullptr) {
		options.profile = profile;
	}
	ShaderComputeInputInfo compute = *options.input_info.compute;
	compute.wave_size            = wave_size;
	options.input_info.compute   = &compute;
	options.wave_size            = wave_size;
	auto source       = ShaderRecompiler::PrepareShaderSource(code, options);
	Check(source.unsupported_pc == UINT32_MAX,
	      std::string(name) + ": decoded an unsupported instruction: " + source.unsupported_text);
	auto translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
	Check(translated.failure_pc == UINT32_MAX,
	      std::string(name) + ": has no translation: " + translated.failure);
	auto                                         plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
	ShaderRecompiler::IR::ResourceSnapshot       resources;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	const ShaderRecompiler::IR::SrtRuntime       runtime {
	          .user_data                  = options.user_data,
	          .read_specialization_memory = ReadNoMemory,
    };
	Check(ShaderRecompiler::IR::MaterializeResources(plan, runtime, resources, specialization),
	      std::string(name) + ": resources did not materialize");
	auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
	CheckSpirvValidates(compiled.spirv, name);
}

Decoder::Instruction DecodeOne(std::span<const uint32_t> words) {
	Decoder::Instruction inst;
	Decoder::DecodeInstruction(words, 0, inst);
	return inst;
}

// An encoding as llvm-mc -arch=amdgcn -mcpu=gfx1030 -show-encoding assembles `text`.
struct Encoding {
	const char*           text;
	std::vector<uint32_t> words;
	Opcode                opcode;
};

// Every encoding CheckEncodings() was given: each is an opcode, or a form of an older opcode, the
// decoder learned in gta6-prep: shader-alu (see TestLegacyDecodeKeepsCodeTableKeys()).
std::vector<Encoding>& CheckedEncodings() {
	static std::vector<Encoding> encodings;
	return encodings;
}

void CheckEncodings(std::initializer_list<Encoding> encodings) {
	for (const auto& encoding: encodings) {
		CheckedEncodings().push_back(encoding);
		const auto inst = DecodeOne(encoding.words);
		Check(inst.opcode == encoding.opcode &&
		          inst.word_count == static_cast<uint32_t>(encoding.words.size()),
		      std::string("did not decode to its opcode and length: ") + encoding.text + " -> " +
		          Decoder::InstructionToString(inst));
	}
}

// ---- Skipping programs with unsupported instructions ----

void TestUnsupportedEncodingsDecodeWithoutExit() {
	// Bits [31:26] = 0x3b name no RDNA2 encoding family.
	const uint32_t unknown_family[] = {0xec000000u, EndPgm};
	auto           inst             = DecodeOne(unknown_family);
	Check(inst.opcode == Opcode::UNSUPPORTED && inst.family == Decoder::Family::Unknown &&
	          inst.word_count == 1u && inst.raw[0] == unknown_family[0] &&
	          inst.unsupported_reason.find("family is unknown") != std::string_view::npos,
	      "an unknown encoding family did not decode to an unsupported instruction");

	// s_mov_b32 s0, ttmp0 (source code 108) and s_mov_b32 ttmp0, s0 (destination code 108).
	const uint32_t bad_source[] = {EncodeSop1(0x03, 0, 108), EndPgm};
	inst                        = DecodeOne(bad_source);
	Check(inst.opcode == Opcode::UNSUPPORTED && inst.family == Decoder::Family::SOP1 &&
	          inst.word_count == 1u &&
	          inst.unsupported_reason.find("scalar source operand") != std::string_view::npos,
	      "an unknown scalar source operand did not decode to an unsupported instruction");
	const uint32_t bad_destination[] = {EncodeSop1(0x03, 108, 0), EndPgm};
	inst                             = DecodeOne(bad_destination);
	Check(inst.opcode == Opcode::UNSUPPORTED &&
	          inst.unsupported_reason.find("scalar destination operand") != std::string_view::npos,
	      "an unknown scalar destination operand did not decode to an unsupported instruction");
	// The pending operand failure does not leak into the next instruction.
	const uint32_t good[] = {EncodeSop1(0x03, 1, 2)};
	inst                  = DecodeOne(good);
	Check(inst.opcode == Opcode::S_MOV_B32, "an operand failure leaked into the next decode");

	// Words after an unsupported instruction are not instructions: neither a missing S_ENDPGM
	// nor a branch past the code closes the emulator.
	const uint32_t garbage[] = {EncodeVop3Word0(0x3ffu, 0), EncodeVop3Word1(Vgpr(0), Vgpr(1)),
	                            EncodeSopp(0x02, 0x7fffu), 0x12345678u};
	Decoder::Program program;
	Decoder::DecodeProgram(garbage, program);
	const auto* first = Decoder::FindUnsupportedInstruction(program);
	Check(first != nullptr && first->pc == 0u && first->family == Decoder::Family::VOP3,
	      "the first unsupported instruction of a program was not found");
	const auto text = Decoder::InstructionToString(*first);
	Check(text.find("unsupported family=VOP3 opcode=0x3ff") != std::string::npos,
	      "the unsupported instruction text lost its family or opcode: " + text);

	// A last word that claims more words than remain is dropped, not read past the code.
	const uint32_t truncated[] = {0xec000000u, EncodeVop3Word0(0x141u, 0)};
	Decoder::DecodeProgram(truncated, program);
	Check(program.instructions.size() == 1u &&
	          program.instructions[0].opcode == Opcode::UNSUPPORTED,
	      "a truncated instruction after an unsupported one was decoded");
	// A branch decoded from the padded tail keeps its program-relative target.
	const uint32_t tail_branch[] = {0xec000000u, EncodeSopp(0x02, 0xfffeu)};
	Decoder::DecodeProgram(tail_branch, program);
	Check(program.instructions.size() == 2u && program.instructions[1].pc == 4u &&
	          program.instructions[1].branch_target == 0u,
	      "a branch decoded after an unsupported instruction lost its target");

	const uint32_t supported[] = {EncodeSop1(0x03, 1, 2), EndPgm};
	Decoder::DecodeProgram(supported, program);
	Check(Decoder::FindUnsupportedInstruction(program) == nullptr,
	      "a supported program reported an unsupported instruction");
}

void TestPrepareShaderSourceRecordsUnsupported() {
	const uint32_t code[] = {EncodeSop1(0x03, 1, 2), EncodeVop3Word0(0x3ffu, 0),
	                         EncodeVop3Word1(Vgpr(0), Vgpr(1)), EndPgm};
	for (const bool skip: {false, true}) {
		const auto source = ShaderRecompiler::PrepareShaderSource(code, ComputeOptions(skip));
		Check(source.unsupported_pc == 4u &&
		          source.unsupported_text.find("0x00000004: unsupported family=VOP3 opcode=0x3ff") !=
		              std::string::npos,
		      "PrepareShaderSource did not record the first unsupported instruction");
	}
	// A program with calls is not prepared further when it is skipped: call target queries
	// would translate the unsupported instruction.
	const uint32_t call[] = {EncodeSop1(0x21, 14, 14), EncodeVop3Word0(0x3ffu, 0),
	                         EncodeVop3Word1(Vgpr(0), Vgpr(1)), EndPgm};
	const auto     source = ShaderRecompiler::PrepareShaderSource(call, ComputeOptions(true));
	Check(source.unsupported_pc == 4u && source.calls.empty() && !source.expand_calls,
	      "a skipped program with calls was prepared further");

	// The front of a merged stage stops at an unsupported instruction.
	auto local       = ComputeOptions(true);
	local.stage      = ShaderType::Local;
	static const ShaderVertexInputInfo vertex {};
	local.input_info = {};
	local.input_info.vertex = &vertex;
	const uint32_t front[] = {0xec000000u, 0x01020304u};
	const auto     local_source = ShaderRecompiler::PrepareShaderSource(front, local);
	Check(local_source.unsupported_pc == 0u,
	      "the front of a merged stage did not report its unsupported instruction");
}

void TestTranslateProgramReportsFailures() {
	// The decoder's unsupported instruction, without the CFG build failing.
	const uint32_t unsupported[] = {EncodeVop3Word0(0x3ffu, 0), EncodeVop3Word1(Vgpr(0), Vgpr(1)),
	                                EndPgm};
	Decoder::Program decoded;
	Decoder::DecodeProgram(unsupported, decoded);
	const auto failed = ShaderRecompiler::TranslateProgram(decoded, ComputeOptions(true));
	Check(failed.failure_pc == 0u &&
	          failed.failure.find("unsupported family=VOP3") != std::string::npos,
	      "TranslateProgram did not report the unsupported instruction");

	// A decoded instruction the translator rejects: S_GETREG_B32 of HW_REG_MODE.
	const uint32_t getreg[] = {EncodeSop1(0x03, 1, 2), EncodeSopk(0x12, 0, 0x1801u), EndPgm};
	Decoder::DecodeProgram(getreg, decoded);
	Check(decoded.instructions[1].opcode == Opcode::S_GETREG_B32,
	      "S_GETREG_B32 did not decode for the translation failure test");
	const auto rejected = ShaderRecompiler::TranslateProgram(decoded, ComputeOptions(true));
	Check(rejected.failure_pc == 4u && rejected.failure.find("S_GETREG_B32") != std::string::npos,
	      "a translation failure was not reported with its instruction");

	// The frontend records the first failure and stops there.
	Decoder::Program direct;
	auto&            inst  = direct.instructions.emplace_back();
	inst.family            = Decoder::Family::MUBUF;
	inst.opcode            = Opcode::UNSUPPORTED;
	inst.unsupported_reason = "focused unsupported diagnostic";
	ShaderRecompiler::CFG::Graph      cfg;
	ShaderRecompiler::CFG::BasicBlock block;
	block.inst_end = 1u;
	cfg.blocks.push_back(block);
	cfg.entry_block = 0u;
	ShaderComputeInputInfo                         compute {};
	ShaderRecompiler::Frontend::TranslationFailure failure;
	ShaderRecompiler::Frontend::TranslateOptions   options {};
	options.stage              = ShaderType::Compute;
	options.wave_size          = 64u;
	options.input_info.compute = &compute;
	options.failure            = &failure;
	(void)ShaderRecompiler::Frontend::TranslateProgram(direct, cfg, options);
	Check(failure.pc == 0u && failure.text.find("focused unsupported diagnostic") != std::string::npos,
	      "the frontend did not record its translation failure");

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	// --strict-shaders keeps both fatal.
	ExpectFatal([&] { (void)ShaderRecompiler::TranslateProgram(unsupported, ComputeOptions(false)); },
	            "a strict unsupported instruction did not close the emulator");
	ExpectFatal([&] { (void)ShaderRecompiler::TranslateProgram(getreg, ComputeOptions(false)); },
	            "a strict translation failure did not close the emulator");
	options.failure = nullptr;
	ExpectFatal([&] { (void)ShaderRecompiler::Frontend::TranslateProgram(direct, cfg, options); },
	            "a frontend without a failure sink did not close the emulator");
#endif
}

// The configuration default skips: --strict-shaders is off.
void TestStrictShadersDefault() {
	Check(!Config::StrictShadersEnabled(), "--strict-shaders is on by default");
	Config::ConfigOptions options;
	Check(!options.strict_shaders, "ConfigOptions::strict_shaders defaults to on");
}

// ---- Scalar conditional moves, S_BFE_I64, V_PERM_B32, V_SWAP_B32 ----

void TestMovesPermSwap() {
	CheckEncodings({
	    {"s_cmov_b32 s0, s1", {0xbe800501u}, Opcode::S_CMOV_B32},
	    {"s_cmovk_i32 s0, 0x1234", {0xb1001234u}, Opcode::S_CMOVK_I32},
	    {"s_bfe_i64 s[0:1], s[2:3], s4", {0x95000402u}, Opcode::S_BFE_I64},
	    {"s_bfe_i64 s[0:1], s[2:3], 0x80004", {0x9500ff02u, 0x00080004u}, Opcode::S_BFE_I64},
	    {"v_perm_b32 v0, v1, v2, v3", {0xd7440000u, 0x040e0501u}, Opcode::V_PERM_B32},
	    {"v_perm_b32 v0, v1, v2, 0x0c0c0105", {0xd7440000u, 0x03fe0501u, 0x0c0c0105u},
	     Opcode::V_PERM_B32},
	    {"v_swap_b32 v0, v1", {0x7e00cb01u}, Opcode::V_SWAP_B32},
	});
	const uint32_t cmovk[] = {0xb108fffdu}; // s_cmovk_i32 s8, -3
	auto           inst    = DecodeOne(cmovk);
	Check(inst.dst.kind == Decoder::OperandKind::Sgpr && inst.dst.reg == 8u &&
	          inst.src0.signed_val == -3 && inst.src_count == 1u,
	      "S_CMOVK_I32 lost its destination or sign-extended immediate");
	const uint32_t bfe[] = {0x950a0e0cu}; // s_bfe_i64 s[10:11], s[12:13], s14
	inst                 = DecodeOne(bfe);
	Check(inst.data_dwords == 2u && inst.dst.reg == 10u && inst.src0.reg == 12u && inst.src1.reg == 14u,
	      "S_BFE_I64 lost its 64-bit destination or sources");
	const uint32_t swap[] = {0x7e06cb04u}; // v_swap_b32 v3, v4
	inst                  = DecodeOne(swap);
	Check(inst.dst.kind == Decoder::OperandKind::Vgpr && inst.dst.reg == 3u &&
	          inst.src0.kind == Decoder::OperandKind::Vgpr && inst.src0.reg == 4u,
	      "V_SWAP_B32 lost its VGPR operands");
	// V_SWAP_B32 writes its source: a scalar or constant source, SDWA, DPP and VOP3 are rejected.
	for (const uint32_t source: {1u, InlineU32(1), 249u, 250u}) {
		const uint32_t invalid[] = {EncodeVop1(0x65u, 3, source), 0x00060604u};
		Check(DecodeOne(invalid).opcode == Opcode::UNSUPPORTED,
		      "V_SWAP_B32 accepted a source that is not a VGPR");
	}
	const uint32_t vop3_swap[] = {EncodeVop3Word0(0x1e5u, 3), EncodeVop3Word1(Vgpr(4), 0)};
	Check(DecodeOne(vop3_swap).opcode == Opcode::UNSUPPORTED, "V_SWAP_B32 accepted a VOP3 form");

	const uint32_t code[] = {
	    0x7e0202ffu, 0x11223344u,              // v_mov_b32 v1, 0x11223344
	    0x7e0402ffu, 0x8566f788u,              // v_mov_b32 v2, 0x8566f788
	    0xd7440003u, 0x04020501u,              // v_perm_b32 v3, v1, v2, v0
	    0xd7440004u, 0x03fe0501u, 0x0c0b0d05u, // v_perm_b32 v4, v1, v2, 0x0c0b0d05
	    0x7e06cb04u,                           // v_swap_b32 v3, v4
	    0xbf060504u,                           // s_cmp_eq_u32 s4, s5
	    0xbe860507u,                           // s_cmov_b32 s6, s7
	    0xb108fffdu,                           // s_cmovk_i32 s8, -3
	    0x950a0e0cu,                           // s_bfe_i64 s[10:11], s[12:13], s14
	    0x9510ff0cu, 0x00200005u,              // s_bfe_i64 s[16:17], s[12:13], 0x200005
	    0x7e3e0280u,                           // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000031fu,              // buffer_store_dword v3, v31, s[0:3], 0 offen
	    0xe0701004u, 0x8000041fu,              // buffer_store_dword v4, v31, s[0:3], 0 offen offset:4
	    0x7e0a0206u,                           // v_mov_b32 v5, s6
	    0xe0701008u, 0x8000051fu,              // buffer_store_dword v5, v31, s[0:3], 0 offen offset:8
	    0x7e0a0208u,                           // v_mov_b32 v5, s8
	    0xe070100cu, 0x8000051fu,              // buffer_store_dword v5, v31, s[0:3], 0 offen offset:12
	    0x7e0a020au,                           // v_mov_b32 v5, s10
	    0x7e0c0211u,                           // v_mov_b32 v6, s17
	    0xe0701010u, 0x8000051fu,              // buffer_store_dword v5, v31, s[0:3], 0 offen offset:16
	    0xe0701014u, 0x8000061fu,              // buffer_store_dword v6, v31, s[0:3], 0 offen offset:20
	    0xbf810000u,                           // s_endpgm
	};
	CompileToValidSpirv(code, "moves, perm and swap");
}

// ---- The S_*_SAVEEXEC and S_*_WREXEC family ----

void TestSaveexecFamily() {
	CheckEncodings({
	    {"s_or_saveexec_b64 s[0:1], s[2:3]", {0xbe802502u}, Opcode::S_OR_SAVEEXEC_B64},
	    {"s_xor_saveexec_b64 s[0:1], s[2:3]", {0xbe802602u}, Opcode::S_XOR_SAVEEXEC_B64},
	    {"s_andn2_saveexec_b64 s[0:1], s[2:3]", {0xbe802702u}, Opcode::S_ANDN2_SAVEEXEC_B64},
	    {"s_nand_saveexec_b64 s[0:1], s[2:3]", {0xbe802902u}, Opcode::S_NAND_SAVEEXEC_B64},
	    {"s_nor_saveexec_b64 s[0:1], s[2:3]", {0xbe802a02u}, Opcode::S_NOR_SAVEEXEC_B64},
	    {"s_xnor_saveexec_b64 s[0:1], s[2:3]", {0xbe802b02u}, Opcode::S_XNOR_SAVEEXEC_B64},
	    {"s_orn1_saveexec_b64 s[0:1], s[2:3]", {0xbe803802u}, Opcode::S_ORN1_SAVEEXEC_B64},
	    {"s_andn1_wrexec_b64 s[0:1], s[2:3]", {0xbe803902u}, Opcode::S_ANDN1_WREXEC_B64},
	    {"s_andn2_wrexec_b64 s[0:1], s[2:3]", {0xbe803a02u}, Opcode::S_ANDN2_WREXEC_B64},
	    {"s_or_saveexec_b32 s0, s2", {0xbe803d02u}, Opcode::S_OR_SAVEEXEC_B32},
	    {"s_xor_saveexec_b32 s0, s2", {0xbe803e02u}, Opcode::S_XOR_SAVEEXEC_B32},
	    {"s_andn2_saveexec_b32 s0, s2", {0xbe803f02u}, Opcode::S_ANDN2_SAVEEXEC_B32},
	    {"s_nand_saveexec_b32 s0, s2", {0xbe804102u}, Opcode::S_NAND_SAVEEXEC_B32},
	    {"s_nor_saveexec_b32 s0, s2", {0xbe804202u}, Opcode::S_NOR_SAVEEXEC_B32},
	    {"s_xnor_saveexec_b32 s0, s2", {0xbe804302u}, Opcode::S_XNOR_SAVEEXEC_B32},
	    {"s_orn1_saveexec_b32 s0, s2", {0xbe804502u}, Opcode::S_ORN1_SAVEEXEC_B32},
	    {"s_andn1_wrexec_b32 s0, s2", {0xbe804602u}, Opcode::S_ANDN1_WREXEC_B32},
	    {"s_andn2_wrexec_b32 s0, s2", {0xbe804702u}, Opcode::S_ANDN2_WREXEC_B32},
	    {"s_xor_saveexec_b32 s0, 0x12345", {0xbe803effu, 0x00012345u}, Opcode::S_XOR_SAVEEXEC_B32},
	});
	const uint32_t pair[] = {0xbe982516u}; // s_or_saveexec_b64 s[24:25], s[22:23]
	Check(DecodeOne(pair).data_dwords == 2u, "a 64-bit SAVEEXEC did not write an SGPR pair");

	const uint32_t wave64[] = {
	    0x7e3e0280u,              // v_mov_b32 v31, 0
	    0xbe94047eu,              // s_mov_b64 s[20:21], exec
	    0xd4c40016u, 0x00020087u, // v_cmp_gt_u32 s[22:23], 7, v0
	    0xbe982516u,              // s_or_saveexec_b64 s[24:25], s[22:23]
	    0xbe982616u,              // s_xor_saveexec_b64 s[24:25], s[22:23]
	    0xbe982716u,              // s_andn2_saveexec_b64 s[24:25], s[22:23]
	    0xbe982916u,              // s_nand_saveexec_b64 s[24:25], s[22:23]
	    0xbe982a16u,              // s_nor_saveexec_b64 s[24:25], s[22:23]
	    0xbe982b16u,              // s_xnor_saveexec_b64 s[24:25], s[22:23]
	    0xbe983816u,              // s_orn1_saveexec_b64 s[24:25], s[22:23]
	    0xbe983916u,              // s_andn1_wrexec_b64 s[24:25], s[22:23]
	    0xbe9a3a16u,              // s_andn2_wrexec_b64 s[26:27], s[22:23]
	    0x7e020218u,              // v_mov_b32 v1, s24
	    0xe0701000u, 0x8000011fu, // buffer_store_dword v1, v31, s[0:3], 0 offen
	    0xbefe0414u,              // s_mov_b64 exec, s[20:21]
	    0x7e02021au,              // v_mov_b32 v1, s26
	    0xe0701004u, 0x8000011fu, // buffer_store_dword v1, v31, s[0:3], 0 offen offset:4
	    0xbf810000u,              // s_endpgm
	};
	CompileToValidSpirv(wave64, "64-bit SAVEEXEC and WREXEC", 64);
	const uint32_t wave32[] = {
	    0x7e3e0280u,              // v_mov_b32 v31, 0
	    0xbe94037eu,              // s_mov_b32 s20, exec_lo
	    0xd4c40016u, 0x00020087u, // v_cmp_gt_u32 s22, 7, v0
	    0xbe983d16u,              // s_or_saveexec_b32 s24, s22
	    0xbe983e16u,              // s_xor_saveexec_b32 s24, s22
	    0xbe983f16u,              // s_andn2_saveexec_b32 s24, s22
	    0xbe984116u,              // s_nand_saveexec_b32 s24, s22
	    0xbe984216u,              // s_nor_saveexec_b32 s24, s22
	    0xbe984316u,              // s_xnor_saveexec_b32 s24, s22
	    0xbe984516u,              // s_orn1_saveexec_b32 s24, s22
	    0xbe984616u,              // s_andn1_wrexec_b32 s24, s22
	    0xbe9a4716u,              // s_andn2_wrexec_b32 s26, s22
	    0x7e020218u,              // v_mov_b32 v1, s24
	    0xe0701000u, 0x8000011fu, // buffer_store_dword v1, v31, s[0:3], 0 offen
	    0xbefe0314u,              // s_mov_b32 exec_lo, s20
	    0x7e02021au,              // v_mov_b32 v1, s26
	    0xe0701004u, 0x8000011fu, // buffer_store_dword v1, v31, s[0:3], 0 offen offset:4
	    0xbf810000u,              // s_endpgm
	};
	CompileToValidSpirv(wave32, "32-bit SAVEEXEC and WREXEC", 32);
}

// ---- VOP3 F16 arithmetic with source and result modifiers ----

void TestVop3F16Modifiers() {
	CheckEncodings({
	    {"v_max_f16_e64 v0, |v1|, -v2 clamp", {0xd5398100u, 0x40020501u}, Opcode::V_MAX_F16},
	    {"v_min_f16_e64 v0, v1, v2 mul:2", {0xd53a0000u, 0x08020501u}, Opcode::V_MIN_F16},
	    {"v_add_f16_e64 v0, -v1, |v2| div:2", {0xd5320200u, 0x38020501u}, Opcode::V_ADD_F16},
	    {"v_sub_f16_e64 v0, v1, v2 clamp", {0xd5338000u, 0x00020501u}, Opcode::V_SUB_F16},
	    {"v_subrev_f16_e64 v0, -v1, v2 mul:4", {0xd5340000u, 0x30020501u}, Opcode::V_SUBREV_F16},
	    {"v_mul_f16_e64 v0, |v1|, |v2|", {0xd5350300u, 0x00020501u}, Opcode::V_MUL_F16},
	    {"v_fmac_f16_e64 v0, v1, -v2 clamp", {0xd5368000u, 0x40020501u}, Opcode::V_FMAC_F16},
	    {"v_max_f16_e64 v0, 0x3c00, v2", {0xd5390000u, 0x000204f2u}, Opcode::V_MAX_F16},
	});
	const uint32_t max[] = {0xd5398100u, 0x40020501u};
	auto           inst  = DecodeOne(max);
	Check(inst.src0.absolute && !inst.src0.negate && inst.src1.negate && !inst.src1.absolute &&
	          inst.dst.clamp && inst.dst.omod == 0u && inst.src_count == 2u,
	      "VOP3 V_MAX_F16 lost its ABS/NEG or CLAMP");
	const uint32_t add[] = {0xd5320200u, 0x38020501u};
	inst                 = DecodeOne(add);
	Check(inst.src0.negate && inst.src1.absolute && inst.dst.omod == 3u && !inst.dst.clamp,
	      "VOP3 V_ADD_F16 lost its NEG/ABS or OMOD");
	// OP_SEL and a modifier on the third (absent) source stay unsupported.
	const uint32_t op_sel[] = {EncodeVop3Word0(0x139u, 0, 0, 1), EncodeVop3Word1(Vgpr(1), Vgpr(2))};
	Check(DecodeOne(op_sel).opcode == Opcode::UNSUPPORTED, "VOP3 V_MAX_F16 accepted OP_SEL");
	const uint32_t third[] = {EncodeVop3Word0(0x136u, 0, 4), EncodeVop3Word1(Vgpr(1), Vgpr(2))};
	Check(DecodeOne(third).opcode == Opcode::UNSUPPORTED,
	      "VOP3 V_FMAC_F16 accepted ABS on its accumulator");

	const uint32_t code[] = {
	    0x7e0202ffu, 0x3c00bc00u, // v_mov_b32 v1, 0x3c00bc00
	    0x7e0402ffu, 0x4200c400u, // v_mov_b32 v2, 0x4200c400
	    0xd5398103u, 0x40020501u, // v_max_f16_e64 v3, |v1|, -v2 clamp
	    0xd53a0004u, 0x08020501u, // v_min_f16_e64 v4, v1, v2 mul:2
	    0xd5320205u, 0x38020501u, // v_add_f16_e64 v5, -v1, |v2| div:2
	    0xd5338006u, 0x00020501u, // v_sub_f16_e64 v6, v1, v2 clamp
	    0xd5340007u, 0x30020501u, // v_subrev_f16_e64 v7, -v1, v2 mul:4
	    0xd5350308u, 0x00020501u, // v_mul_f16_e64 v8, |v1|, |v2|
	    0xd5368009u, 0x40020501u, // v_fmac_f16_e64 v9, v1, -v2 clamp
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000031fu, // buffer_store_dword v3, v31, s[0:3], 0 offen
	    0xe0701004u, 0x8000041fu, // buffer_store_dword v4, v31, s[0:3], 0 offen offset:4
	    0xe0701008u, 0x8000051fu, // buffer_store_dword v5, v31, s[0:3], 0 offen offset:8
	    0xe070100cu, 0x8000061fu, // buffer_store_dword v6, v31, s[0:3], 0 offen offset:12
	    0xe0701010u, 0x8000071fu, // buffer_store_dword v7, v31, s[0:3], 0 offen offset:16
	    0xe0701014u, 0x8000081fu, // buffer_store_dword v8, v31, s[0:3], 0 offen offset:20
	    0xe0701018u, 0x8000091fu, // buffer_store_dword v9, v31, s[0:3], 0 offen offset:24
	    0xbf810000u, // s_endpgm
	};

	CompileToValidSpirv(code, "VOP3 F16 modifiers");
}

// ---- VOP3P dot products and V_DOT4C_I32_I8 ----

void TestDotProducts() {
	CheckEncodings({
	    {"v_dot2_f32_f16 v0, v1, v2, v3", {0xcc134000u, 0x1c0e0501u}, Opcode::V_DOT2_F32_F16},
	    {"v_dot2_f32_f16 v0, v1, v2, v3 neg_lo:[1,1,1] neg_hi:[1,0,0]", {0xcc134100u, 0xfc0e0501u}, Opcode::V_DOT2_F32_F16},
	    {"v_dot2_f32_f16 v0, v1, v2, v3 clamp", {0xcc13c000u, 0x1c0e0501u}, Opcode::V_DOT2_F32_F16},
	    {"v_dot2_i32_i16 v0, v1, v2, v3", {0xcc144000u, 0x1c0e0501u}, Opcode::V_DOT2_I32_I16},
	    {"v_dot2_i32_i16 v0, v1, v2, v3 op_sel:[1,0,0] op_sel_hi:[0,1,1]", {0xcc144800u, 0x140e0501u}, Opcode::V_DOT2_I32_I16},
	    {"v_dot2_u32_u16 v0, v1, v2, v3", {0xcc154000u, 0x1c0e0501u}, Opcode::V_DOT2_U32_U16},
	    {"v_dot4_i32_i8 v0, v1, v2, v3", {0xcc164000u, 0x1c0e0501u}, Opcode::V_DOT4_I32_I8},
	    {"v_dot4_i32_i8 v0, v1, v2, v3 clamp", {0xcc16c000u, 0x1c0e0501u}, Opcode::V_DOT4_I32_I8},
	    {"v_dot4_u32_u8 v0, v1, v2, v3", {0xcc174000u, 0x1c0e0501u}, Opcode::V_DOT4_U32_U8},
	    {"v_dot8_i32_i4 v0, v1, v2, v3", {0xcc184000u, 0x1c0e0501u}, Opcode::V_DOT8_I32_I4},
	    {"v_dot8_u32_u4 v0, v1, v2, 0x12345", {0xcc194000u, 0x1bfe0501u, 0x00012345u}, Opcode::V_DOT8_U32_U4},
	    {"v_dot4c_i32_i8 v0, v1, v2", {0x1a000501u}, Opcode::V_DOT4C_I32_I8},
	    {"v_dot4c_i32_i8 v0, 0x1020304, v2", {0x1a0004ffu, 0x01020304u}, Opcode::V_DOT4C_I32_I8},
	});
	const uint32_t f16[] = {0xcc13c105u, 0xfc120501u}; // neg_lo:[1,1,1] neg_hi:[1,0,0] clamp
	auto           inst  = DecodeOne(f16);
	Check(inst.src0.negate && inst.src0.negate_hi && inst.src1.negate && !inst.src1.negate_hi &&
	          inst.src2.negate && inst.dst.clamp && inst.src_count == 3u,
	      "V_DOT2_F32_F16 lost its NEG/NEG_HI or CLAMP");
	const uint32_t halves[] = {0xcc144806u, 0x14020701u}; // op_sel:[1,0,0] op_sel_hi:[0,1,1]
	inst                    = DecodeOne(halves);
	Check(inst.src0.op_sel && !inst.src0.op_sel_hi && !inst.src1.op_sel && inst.src1.op_sel_hi,
	      "V_DOT2_I32_I16 lost its OP_SEL / OP_SEL_HI");
	// Unimplemented modifiers stay unsupported: NEG_HI on the F32 accumulator, NEG on integer
	// dots, OP_SEL on the packed-byte dots.
	for (const auto& words: {std::array<uint32_t, 2> {0xcc134400u, 0x1c0e0501u},
	                         std::array<uint32_t, 2> {0xcc144000u, 0x3c0e0501u},
	                         std::array<uint32_t, 2> {0xcc164800u, 0x1c0e0501u},
	                         std::array<uint32_t, 2> {0xcc164000u, 0x3c0e0501u}}) {
		Check(DecodeOne(words).opcode == Opcode::UNSUPPORTED,
		      "a dot product accepted a modifier it does not implement");
	}

	const uint32_t code[] = {
	    0x7e0202ffu, 0x3c00bc00u, // v_mov_b32 v1, 0x3c00bc00
	    0x7e0402ffu, 0x4200c400u, // v_mov_b32 v2, 0x4200c400
	    0x7e0602ffu, 0x80ff017fu, // v_mov_b32 v3, 0x80ff017f
	    0xcc134004u, 0x1bca0501u, // v_dot2_f32_f16 v4, v1, v2, 1.0
	    0xcc13c105u, 0xfc120501u, // v_dot2_f32_f16 v5, v1, v2, v4 neg_lo:[1,1,1] neg_hi:[1,0,0] clamp
	    0xcc144806u, 0x14020701u, // v_dot2_i32_i16 v6, v1, v3, v0 op_sel:[1,0,0] op_sel_hi:[0,1,1]
	    0xcc15c007u, 0x1c020701u, // v_dot2_u32_u16 v7, v1, v3, v0 clamp
	    0xcc16c008u, 0x1c020303u, // v_dot4_i32_i8 v8, v3, v1, v0 clamp
	    0xcc17c009u, 0x1b060303u, // v_dot4_u32_u8 v9, v3, v1, -1 clamp
	    0xcc18400au, 0x1c020503u, // v_dot8_i32_i4 v10, v3, v2, v0
	    0xcc19400bu, 0x1bfe0503u, 0x00012345u, // v_dot8_u32_u4 v11, v3, v2, 0x12345
	    0x1a100503u, // v_dot4c_i32_i8 v8, v3, v2
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000041fu, // buffer_store_dword v4, v31, s[0:3], 0 offen
	    0xe0701004u, 0x8000051fu, // buffer_store_dword v5, v31, s[0:3], 0 offen offset:4
	    0xe0701008u, 0x8000061fu, // buffer_store_dword v6, v31, s[0:3], 0 offen offset:8
	    0xe070100cu, 0x8000071fu, // buffer_store_dword v7, v31, s[0:3], 0 offen offset:12
	    0xe0701010u, 0x8000081fu, // buffer_store_dword v8, v31, s[0:3], 0 offen offset:16
	    0xe0701014u, 0x8000091fu, // buffer_store_dword v9, v31, s[0:3], 0 offen offset:20
	    0xe0701018u, 0x80000a1fu, // buffer_store_dword v10, v31, s[0:3], 0 offen offset:24
	    0xe070101cu, 0x80000b1fu, // buffer_store_dword v11, v31, s[0:3], 0 offen offset:28
	    0xbf810000u, // s_endpgm
	};
	CompileToValidSpirv(code, "dot products");
}

// ---- Scalar mode, hint, marker and cache no-ops ----

void TestScalarModeAndHintNops() {
	CheckEncodings({
	    {"s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 4), 3", {0xba801801u, 0x00000003u}, Opcode::S_SETREG_IMM32_B32},
	    {"s_version 0x1234", {0xb0801234u}, Opcode::S_VERSION},
	    {"s_round_mode 0x3", {0xbfa40003u}, Opcode::S_ROUND_MODE},
	    {"s_denorm_mode 0xf", {0xbfa5000fu}, Opcode::S_DENORM_MODE},
	    {"s_icache_inv", {0xbf930000u}, Opcode::S_ICACHE_INV},
	    {"s_wakeup", {0xbf830000u}, Opcode::S_WAKEUP},
	    {"s_incperflevel 1", {0xbf940001u}, Opcode::S_INCPERFLEVEL},
	    {"s_decperflevel 1", {0xbf950001u}, Opcode::S_DECPERFLEVEL},
	    {"s_wait_idle", {0xbfa20000u}, Opcode::S_WAIT_IDLE},
	    {"s_ttracedata_imm 0x10", {0xbfa80010u}, Opcode::S_TTRACEDATA_IMM},
	    {"s_code_end", {0xbf9f0000u}, Opcode::S_CODE_END},
	});
	const uint32_t setreg[] = {0xba801801u, 0x00000003u, EndPgm};
	auto           inst     = DecodeOne(setreg);
	Check(inst.src0.kind == Decoder::OperandKind::LiteralConstant && inst.src0.value == 3u &&
	          inst.src1.value == 0x1801u && inst.dst.kind == Decoder::OperandKind::Null,
	      "S_SETREG_IMM32_B32 lost its literal data or register field");
	Decoder::Program program;
	Decoder::DecodeProgram(setreg, program);
	Check(program.instructions.size() == 2u && program.instructions[1].opcode == Opcode::S_ENDPGM,
	      "S_SETREG_IMM32_B32 did not consume its literal");
	const uint32_t round[] = {0xbfa40003u};
	Check(DecodeOne(round).src0.value == 3u, "S_ROUND_MODE lost its mode bits");

	const uint32_t code[] = {
	    0xb0801234u, // s_version 0x1234
	    0xba801801u, 0x00000003u, // s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 4), 3
	    0xbfa40000u, // s_round_mode 0x0
	    0xbfa5000fu, // s_denorm_mode 0xf
	    0x7e025500u, // v_rcp_f32 v1, v0
	    0xbfa5000cu, // s_denorm_mode 0xc
	    0xbf930000u, // s_icache_inv
	    0xbf830000u, // s_wakeup
	    0xbf940001u, // s_incperflevel 1
	    0xbf950001u, // s_decperflevel 1
	    0xbfa20000u, // s_wait_idle
	    0xbfa80010u, // s_ttracedata_imm 0x10
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000011fu, // buffer_store_dword v1, v31, s[0:3], 0 offen
	    0xbf810000u, // s_endpgm
	};
	CompileToValidSpirv(code, "scalar no-ops");
}

// ---- SMEM clock, cache and translation probes ----

void TestScalarMemoryNops() {
	CheckEncodings({
	    {"s_memtime s[0:1]", {0xf4900000u, 0x00000000u}, Opcode::S_MEMREALTIME},
	    {"s_gl1_inv", {0xf47c0000u, 0x00000000u}, Opcode::BUFFER_GL1_INV},
	    {"s_atc_probe 7, s[0:1], 0x0", {0xf49801c0u, 0xfa000000u}, Opcode::S_DCACHE_INV},
	    {"s_atc_probe_buffer 7, s[0:3], 0x0", {0xf49c01c0u, 0xfa000000u}, Opcode::S_DCACHE_INV},
	});
	const uint32_t memtime[] = {0xf4900100u, 0x00000000u}; // s_memtime s[4:5]
	const auto     inst      = DecodeOne(memtime);
	Check(inst.dst.kind == Decoder::OperandKind::Sgpr && inst.dst.reg == 4u && inst.data_dwords == 2u,
	      "S_MEMTIME lost its SGPR pair destination");

	const uint32_t code[] = {
	    0xf4900100u, 0x00000000u, // s_memtime s[4:5]
	    0xf47c0000u, 0x00000000u, // s_gl1_inv
	    0xf49801c0u, 0xfa000000u, // s_atc_probe 7, s[0:1], 0x0
	    0xf49c01c0u, 0xfa000000u, // s_atc_probe_buffer 7, s[0:3], 0x0
	    0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
	    0x7e020204u, // v_mov_b32 v1, s4
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000011fu, // buffer_store_dword v1, v31, s[0:3], 0 offen
	    0xbf810000u, // s_endpgm
	};
	// The clock needs shaderDeviceClock (1 GHz here).
	static constexpr ShaderRecompiler::Spirv::Profile clock {.device_clock_period_ps = 1000u};
	CompileToValidSpirv(code, "scalar memory no-ops", 64, &clock);
}

// ---- VOPC: f16 ordered/false/true, CMPX forms and CLASS_F64 ----

void TestVopcGaps() {
	CheckEncodings({
	    {"v_cmp_f_f16 vcc_lo, v1, v2", {0x7d900501u}, Opcode::V_CMP_F_F16},
	    {"v_cmp_o_f16 vcc_lo, v1, v2", {0x7d9e0501u}, Opcode::V_CMP_O_F16},
	    {"v_cmp_nlg_f16 vcc_lo, v1, v2", {0x7dd40501u}, Opcode::V_CMP_NLG_F16},
	    {"v_cmp_nle_f16 vcc_lo, v1, v2", {0x7dd80501u}, Opcode::V_CMP_NLE_F16},
	    {"v_cmp_tru_f16 vcc_lo, v1, v2", {0x7dde0501u}, Opcode::V_CMP_TRU_F16},
	    {"v_cmpx_f_f16 v1, v2", {0x7db00501u}, Opcode::V_CMPX_F_F16},
	    {"v_cmpx_lg_f16 v1, v2", {0x7dba0501u}, Opcode::V_CMPX_LG_F16},
	    {"v_cmpx_o_f16 v1, v2", {0x7dbe0501u}, Opcode::V_CMPX_O_F16},
	    {"v_cmpx_u_f16 v1, v2", {0x7df00501u}, Opcode::V_CMPX_U_F16},
	    {"v_cmpx_nge_f16 v1, v2", {0x7df20501u}, Opcode::V_CMPX_NGE_F16},
	    {"v_cmpx_nlg_f16 v1, v2", {0x7df40501u}, Opcode::V_CMPX_NLG_F16},
	    {"v_cmpx_tru_f16 v1, v2", {0x7dfe0501u}, Opcode::V_CMPX_TRU_F16},
	    {"v_cmpx_f_f32 v1, v2", {0x7c200501u}, Opcode::V_CMPX_F_F32},
	    {"v_cmpx_tru_f32 v1, v2", {0x7c3e0501u}, Opcode::V_CMPX_TRU_F32},
	    {"v_cmpx_f_i32 v1, v2", {0x7d200501u}, Opcode::V_CMPX_F_I32},
	    {"v_cmpx_t_i32 v1, v2", {0x7d2e0501u}, Opcode::V_CMPX_T_I32},
	    {"v_cmpx_f_u32 v1, v2", {0x7da00501u}, Opcode::V_CMPX_F_U32},
	    {"v_cmpx_t_u32 v1, v2", {0x7dae0501u}, Opcode::V_CMPX_T_U32},
	    {"v_cmpx_le_u16 v1, v2", {0x7d760501u}, Opcode::V_CMPX_LE_U16},
	    {"v_cmpx_ge_u16 v1, v2", {0x7d7c0501u}, Opcode::V_CMPX_GE_U16},
	    {"v_cmp_class_f64 vcc_lo, v[1:2], v3", {0x7d500701u}, Opcode::V_CMP_CLASS_F64},
	    {"v_cmpx_class_f64 v[1:2], v3", {0x7d700701u}, Opcode::V_CMPX_CLASS_F64},
	    {"v_cmp_class_f64_e64 s0, -v[1:2], v3", {0xd4a80000u, 0x20020701u}, Opcode::V_CMP_CLASS_F64},
	    {"v_cmp_o_f16_e64 s0, |v1|, v2", {0xd4cf0100u, 0x00020501u}, Opcode::V_CMP_O_F16},
	    {"v_cmpx_lg_f16_e64 -v1, v2", {0xd4dd007eu, 0x20020501u}, Opcode::V_CMPX_LG_F16},
	});
	const uint32_t cmpx[] = {0x7d760501u}; // v_cmpx_le_u16 v1, v2
	Check(DecodeOne(cmpx).dst.kind == Decoder::OperandKind::ExecLo,
	      "a V_CMPX compare did not write EXEC");
	const uint32_t vop3_class[] = {0xd4a80000u, 0x20020701u}; // v_cmp_class_f64_e64 s0, -v[1:2], v3
	const auto     inst         = DecodeOne(vop3_class);
	Check(inst.src0.negate && inst.dst.kind == Decoder::OperandKind::Sgpr && inst.dst.reg == 0u,
	      "VOP3 V_CMP_CLASS_F64 lost its NEG or scalar destination");
	// 64-bit compares have no SDWA or DPP forms.
	const uint32_t sdwa_class[] = {0x7d5006f9u, 0x86060001u};
	Check(DecodeOne(sdwa_class).opcode == Opcode::UNSUPPORTED, "V_CMP_CLASS_F64 accepted SDWA");

	const uint32_t code[] = {
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xbe94037eu, // s_mov_b32 s20, exec_lo
	    0x7e0202ffu, 0x3c00bc00u, // v_mov_b32 v1, 0x3c00bc00
	    0x7e04a100u, // v_cvt_f16_u16 v2, v0
	    0x7e0602ffu, 0x000003ffu, // v_mov_b32 v3, 0x3ff
	    0x7e0802ffu, 0x7ff80000u, // v_mov_b32 v4, 0x7ff80000
	    0x7e0a0280u, // v_mov_b32 v5, 0
	    0x7d900501u, // v_cmp_f_f16 vcc_lo, v1, v2
	    0xd5010006u, 0x01a90280u, // v_cndmask_b32 v6, 0, 1, vcc_lo
	    0x7d9e0501u, // v_cmp_o_f16 vcc_lo, v1, v2
	    0xd5010007u, 0x01a90280u, // v_cndmask_b32 v7, 0, 1, vcc_lo
	    0x7dd40501u, // v_cmp_nlg_f16 vcc_lo, v1, v2
	    0xd5010008u, 0x01a90280u, // v_cndmask_b32 v8, 0, 1, vcc_lo
	    0x7dd80501u, // v_cmp_nle_f16 vcc_lo, v1, v2
	    0xd5010009u, 0x01a90280u, // v_cndmask_b32 v9, 0, 1, vcc_lo
	    0x7dde0501u, // v_cmp_tru_f16 vcc_lo, v1, v2
	    0xd501000au, 0x01a90280u, // v_cndmask_b32 v10, 0, 1, vcc_lo
	    0x7d500704u, // v_cmp_class_f64 vcc_lo, v[4:5], v3
	    0xd501000bu, 0x01a90280u, // v_cndmask_b32 v11, 0, 1, vcc_lo
	    0xd4a80000u, 0x20020704u, // v_cmp_class_f64_e64 s0, -v[4:5], v3
	    0x7dba0501u, // v_cmpx_lg_f16 v1, v2
	    0x7dbe0501u, // v_cmpx_o_f16 v1, v2
	    0x7df00501u, // v_cmpx_u_f16 v1, v2
	    0x7df20501u, // v_cmpx_nge_f16 v1, v2
	    0x7df40501u, // v_cmpx_nlg_f16 v1, v2
	    0x7dfe0501u, // v_cmpx_tru_f16 v1, v2
	    0x7c3e0501u, // v_cmpx_tru_f32 v1, v2
	    0x7d2e0501u, // v_cmpx_t_i32 v1, v2
	    0x7dae0501u, // v_cmpx_t_u32 v1, v2
	    0x7d760501u, // v_cmpx_le_u16 v1, v2
	    0x7d7c0501u, // v_cmpx_ge_u16 v1, v2
	    0x7d700704u, // v_cmpx_class_f64 v[4:5], v3
	    0x7db00501u, // v_cmpx_f_f16 v1, v2
	    0x7c200501u, // v_cmpx_f_f32 v1, v2
	    0x7d200501u, // v_cmpx_f_i32 v1, v2
	    0x7da00501u, // v_cmpx_f_u32 v1, v2
	    0xbefe0314u, // s_mov_b32 exec_lo, s20
	    0xe0701000u, 0x8000061fu, // buffer_store_dword v6, v31, s[0:3], 0 offen
	    0xe0701004u, 0x8000071fu, // buffer_store_dword v7, v31, s[0:3], 0 offen offset:4
	    0xe0701008u, 0x8000081fu, // buffer_store_dword v8, v31, s[0:3], 0 offen offset:8
	    0xe070100cu, 0x8000091fu, // buffer_store_dword v9, v31, s[0:3], 0 offen offset:12
	    0xe0701010u, 0x80000a1fu, // buffer_store_dword v10, v31, s[0:3], 0 offen offset:16
	    0xe0701014u, 0x80000b1fu, // buffer_store_dword v11, v31, s[0:3], 0 offen offset:20
	    0xbf810000u, // s_endpgm
	};
	CompileToValidSpirv(code, "VOPC gaps", 32);
}

// ---- Precise division: V_DIV_SCALE, V_DIV_FMAS, V_DIV_FIXUP ----

void TestPreciseDivision() {
	CheckEncodings({
	    {"v_div_scale_f32 v0, vcc_lo, v1, v2, v3", {0xd56d6a00u, 0x040e0501u}, Opcode::V_DIV_SCALE_F32},
	    {"v_div_scale_f32 v0, s4, -v1, v2, v3", {0xd56d0400u, 0x240e0501u}, Opcode::V_DIV_SCALE_F32},
	    {"v_div_scale_f64 v[0:1], vcc_lo, v[2:3], v[4:5], v[6:7]", {0xd56e6a00u, 0x041a0902u}, Opcode::V_DIV_SCALE_F64},
	    {"v_div_fmas_f32 v0, v1, v2, v3", {0xd56f0000u, 0x040e0501u}, Opcode::V_DIV_FMAS_F32},
	    {"v_div_fmas_f64 v[0:1], v[2:3], v[4:5], v[6:7]", {0xd5700000u, 0x041a0902u}, Opcode::V_DIV_FMAS_F64},
	    {"v_div_fixup_f32 v0, v1, v2, v3", {0xd55f0000u, 0x040e0501u}, Opcode::V_DIV_FIXUP_F32},
	    {"v_div_fixup_f32 v0, v1, |v2|, -v3 clamp", {0xd55f8200u, 0x840e0501u}, Opcode::V_DIV_FIXUP_F32},
	    {"v_div_fixup_f16 v0, v1, v2, v3", {0xd75f0000u, 0x040e0501u}, Opcode::V_DIV_FIXUP_F16},
	    {"v_div_fixup_f16 v0, v1, v2, v3 op_sel:[1,0,0,1]", {0xd75f4800u, 0x040e0501u}, Opcode::V_DIV_FIXUP_F16},
	    {"v_div_fixup_f64 v[0:1], v[2:3], v[4:5], v[6:7]", {0xd5600000u, 0x041a0902u}, Opcode::V_DIV_FIXUP_F64},
	});
	const uint32_t scale[] = {0xd56d0400u, 0x240e0501u}; // v_div_scale_f32 v0, s4, -v1, v2, v3
	auto           inst    = DecodeOne(scale);
	Check(inst.dst2.kind == Decoder::OperandKind::Sgpr && inst.dst2.reg == 4u && inst.src0.negate &&
	          !inst.src1.negate && inst.src_count == 3u,
	      "V_DIV_SCALE_F32 lost its VOP3B scalar destination or NEG");
	const uint32_t fixup[] = {0xd55f8200u, 0x840e0501u}; // v_div_fixup_f32 v0, v1, |v2|, -v3 clamp
	inst                   = DecodeOne(fixup);
	Check(inst.src1.absolute && inst.src2.negate && inst.dst.clamp,
	      "V_DIV_FIXUP_F32 lost its ABS/NEG or CLAMP");
	const uint32_t half[] = {0xd75f4800u, 0x040e0501u}; // op_sel:[1,0,0,1]
	inst                  = DecodeOne(half);
	Check(inst.src0.op_sel && !inst.src1.op_sel && inst.dst.sdwa_sel == 5u,
	      "V_DIV_FIXUP_F16 lost its OP_SEL halves");

	const uint32_t code[] = {
	    0x7e000d00u, // v_cvt_f32_u32 v0, v0
	    0x7e0202ffu, 0x40400000u, // v_mov_b32 v1, 0x40400000
	    0xd56d0402u, 0x04020301u, // v_div_scale_f32 v2, s4, v1, v1, v0
	    0xd56d6a03u, 0x04020300u, // v_div_scale_f32 v3, vcc_lo, v0, v1, v0
	    0x7e085502u, // v_rcp_f32 v4, v2
	    0xbfa5000fu, // s_denorm_mode 15
	    0xd54b0005u, 0x23ca0902u, // v_fma_f32 v5, -v2, v4, 1.0
	    0x56080905u, // v_fmac_f32 v4, v5, v4
	    0x100a0903u, // v_mul_f32 v5, v3, v4
	    0xd54b0006u, 0x240e0b02u, // v_fma_f32 v6, -v2, v5, v3
	    0x560a0906u, // v_fmac_f32 v5, v6, v4
	    0xd54b0002u, 0x240e0b02u, // v_fma_f32 v2, -v2, v5, v3
	    0xbfa5000cu, // s_denorm_mode 12
	    0xd56f0002u, 0x04160902u, // v_div_fmas_f32 v2, v2, v4, v5
	    0xd55f0007u, 0x04020302u, // v_div_fixup_f32 v7, v2, v1, v0
	    0x7e102100u, // v_cvt_f64_f32 v[8:9], v0
	    0x7e142101u, // v_cvt_f64_f32 v[10:11], v1
	    0xd56e6a0cu, 0x04221508u, // v_div_scale_f64 v[12:13], vcc_lo, v[8:9], v[10:11], v[8:9]
	    0xd570000cu, 0x0422150cu, // v_div_fmas_f64 v[12:13], v[12:13], v[10:11], v[8:9]
	    0xd560000cu, 0x0422150cu, // v_div_fixup_f64 v[12:13], v[12:13], v[10:11], v[8:9]
	    0x7e1c1500u, // v_cvt_f16_f32 v14, v0
	    0xd75f480eu, 0x043a1d0eu, // v_div_fixup_f16 v14, v14, v14, v14 op_sel:[1,0,0,1]
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000071fu, // buffer_store_dword v7, v31, s[0:3], 0 offen
	    0xe0701004u, 0x80000c1fu, // buffer_store_dword v12, v31, s[0:3], 0 offen offset:4
	    0xe0701008u, 0x80000d1fu, // buffer_store_dword v13, v31, s[0:3], 0 offen offset:8
	    0xe070100cu, 0x80000e1fu, // buffer_store_dword v14, v31, s[0:3], 0 offen offset:12
	    0xbf810000u, // s_endpgm
	};
	CompileToValidSpirv(code, "precise division", 32);
}

// ---- Integer VOP3 long tail, V_LDEXP_F16 and the F16 norm conversions ----

void TestIntegerLongTail() {
	CheckEncodings({
	    {"v_mad_i64_i32 v[0:1], s0, v2, v3, v[4:5]", {0xd5770000u, 0x04120702u}, Opcode::V_MAD_I64_I32},
	    {"v_mad_i64_i32 v[0:1], null, v2, v3, 0x12345", {0xd5777d00u, 0x03fe0702u, 0x00012345u}, Opcode::V_MAD_I64_I32},
	    {"v_mad_u32_u16 v0, v1, v2, v3", {0xd7730000u, 0x040e0501u}, Opcode::V_MAD_U32_U16},
	    {"v_mad_u32_u16 v0, v1, v2, v3 op_sel:[1,1,0,0] clamp", {0xd7739800u, 0x040e0501u}, Opcode::V_MAD_U32_U16},
	    {"v_mad_i32_i16 v0, v1, v2, v3", {0xd7750000u, 0x040e0501u}, Opcode::V_MAD_I32_I16},
	    {"v_add_nc_i32 v0, v1, v2", {0xd77f0000u, 0x00020501u}, Opcode::V_ADD_NC_I32},
	    {"v_add_nc_i32 v0, v1, v2 clamp", {0xd77f8000u, 0x00020501u}, Opcode::V_ADD_NC_I32},
	    {"v_sub_nc_i32 v0, v1, v2 clamp", {0xd7768000u, 0x00020501u}, Opcode::V_SUB_NC_I32},
	    {"v_ldexp_f16 v0, v1, v2", {0x76000501u}, Opcode::V_LDEXP_F16},
	    {"v_ldexp_f16_e64 v0, -v1, v2 clamp", {0xd53b8000u, 0x20020501u}, Opcode::V_LDEXP_F16},
	    {"v_cvt_pknorm_i16_f16 v0, v1, v2", {0xd7120000u, 0x00020501u}, Opcode::V_CVT_PKNORM_I16_F16},
	    {"v_cvt_pknorm_u16_f16 v0, -v1, |v2| op_sel:[1,0,0]", {0xd7130a00u, 0x20020501u}, Opcode::V_CVT_PKNORM_U16_F16},
	    {"v_sat_pk_u8_i16 v0, v1", {0x7e00c501u}, Opcode::V_SAT_PK_U8_I16},
	    {"v_cvt_norm_i16_f16 v0, v1", {0x7e00c701u}, Opcode::V_CVT_NORM_I16_F16},
	    {"v_cvt_norm_u16_f16 v0, v1", {0x7e00c901u}, Opcode::V_CVT_NORM_U16_F16},
	    {"v_cvt_norm_u16_f16_e64 v0, -v1", {0xd5e40000u, 0x20000101u}, Opcode::V_CVT_NORM_U16_F16},
	});
	const uint32_t mad64[] = {0xd5777d00u, 0x03fe0702u, 0x00012345u};
	auto           inst    = DecodeOne(mad64);
	Check(inst.dst2.kind == Decoder::OperandKind::Null && inst.src2.kind == Decoder::OperandKind::LiteralConstant &&
	          inst.src2.value == 0x12345u && inst.src_count == 3u,
	      "V_MAD_I64_I32 lost its null SDST or literal addend");
	const uint32_t mad16[] = {0xd7739800u, 0x040e0501u}; // op_sel:[1,1,0,0] clamp
	inst                   = DecodeOne(mad16);
	Check(inst.src0.op_sel && inst.src1.op_sel && !inst.src2.op_sel && inst.dst.clamp &&
	          inst.dst.sdwa_sel == 6u,
	      "V_MAD_U32_U16 lost its OP_SEL halves or CLAMP, or narrowed its result");
	const uint32_t pknorm[] = {0xd7130a00u, 0x20020501u}; // -v1, |v2| op_sel:[1,0,0]
	inst                    = DecodeOne(pknorm);
	Check(inst.src0.negate && inst.src1.absolute && inst.src0.op_sel && !inst.src1.op_sel,
	      "V_CVT_PKNORM_U16_F16 lost its NEG/ABS or OP_SEL");
	const uint32_t ldexp[] = {0xd53b8000u, 0x20020501u}; // -v1, v2 clamp
	inst                   = DecodeOne(ldexp);
	Check(inst.src0.negate && !inst.src1.negate && inst.dst.clamp,
	      "V_LDEXP_F16 lost its NEG or CLAMP");
	// Unimplemented: NEG on the exponent, OP_SEL on the 32-bit addend, NEG on an integer add.
	const uint32_t bad_ldexp[] = {EncodeVop3Word0(0x13bu, 0), EncodeVop3Word1(Vgpr(1), Vgpr(2), 0, 0, 2)};
	const uint32_t bad_mad[]   = {EncodeVop3Word0(0x373u, 0, 0, 4), EncodeVop3Word1(Vgpr(1), Vgpr(2), Vgpr(3))};
	const uint32_t bad_add[]   = {EncodeVop3Word0(0x37fu, 0), EncodeVop3Word1(Vgpr(1), Vgpr(2), 0, 0, 1)};
	for (const auto words: {std::span<const uint32_t>(bad_ldexp), std::span<const uint32_t>(bad_mad),
	                        std::span<const uint32_t>(bad_add)}) {
		Check(DecodeOne(words).opcode == Opcode::UNSUPPORTED,
		      "an integer or ldexp opcode accepted a modifier it does not implement");
	}

	const uint32_t code[] = {
	    0x7e0202ffu, 0x8001fffeu, // v_mov_b32 v1, 0x8001fffe
	    0x7e0402ffu, 0x7fff0003u, // v_mov_b32 v2, 0x7fff0003
	    0xd5770804u, 0x04020501u, // v_mad_i64_i32 v[4:5], s8, v1, v2, v[0:1]
	    0xd5777d06u, 0x03fe0501u, 0x00012345u, // v_mad_i64_i32 v[6:7], null, v1, v2, 0x12345
	    0xd7739808u, 0x04020501u, // v_mad_u32_u16 v8, v1, v2, v0 op_sel:[1,1,0,0] clamp
	    0xd7750009u, 0x04020501u, // v_mad_i32_i16 v9, v1, v2, v0
	    0xd77f800au, 0x00020501u, // v_add_nc_i32 v10, v1, v2 clamp
	    0xd776800bu, 0x00020501u, // v_sub_nc_i32 v11, v1, v2 clamp
	    0xd77f000cu, 0x00020501u, // v_add_nc_i32 v12, v1, v2
	    0x761a0501u, // v_ldexp_f16 v13, v1, v2
	    0xd53b800du, 0x20020501u, // v_ldexp_f16_e64 v13, -v1, v2 clamp
	    0xd712000eu, 0x00020501u, // v_cvt_pknorm_i16_f16 v14, v1, v2
	    0xd7130a0fu, 0x20020501u, // v_cvt_pknorm_u16_f16 v15, -v1, |v2| op_sel:[1,0,0]
	    0x7e20c501u, // v_sat_pk_u8_i16 v16, v1
	    0x7e22c701u, // v_cvt_norm_i16_f16 v17, v1
	    0xd5e40011u, 0x20000102u, // v_cvt_norm_u16_f16_e64 v17, -v2
	    0x7e240208u, // v_mov_b32 v18, s8
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000041fu, // buffer_store_dword v4, v31, s[0:3], 0 offen
	    0xe0701004u, 0x8000051fu, // buffer_store_dword v5, v31, s[0:3], 0 offen offset:4
	    0xe0701008u, 0x8000061fu, // buffer_store_dword v6, v31, s[0:3], 0 offen offset:8
	    0xe070100cu, 0x8000081fu, // buffer_store_dword v8, v31, s[0:3], 0 offen offset:12
	    0xe0701010u, 0x8000091fu, // buffer_store_dword v9, v31, s[0:3], 0 offen offset:16
	    0xe0701014u, 0x80000a1fu, // buffer_store_dword v10, v31, s[0:3], 0 offen offset:20
	    0xe0701018u, 0x80000b1fu, // buffer_store_dword v11, v31, s[0:3], 0 offen offset:24
	    0xe070101cu, 0x80000c1fu, // buffer_store_dword v12, v31, s[0:3], 0 offen offset:28
	    0xe0701020u, 0x80000d1fu, // buffer_store_dword v13, v31, s[0:3], 0 offen offset:32
	    0xe0701024u, 0x80000e1fu, // buffer_store_dword v14, v31, s[0:3], 0 offen offset:36
	    0xe0701028u, 0x80000f1fu, // buffer_store_dword v15, v31, s[0:3], 0 offen offset:40
	    0xe070102cu, 0x8000101fu, // buffer_store_dword v16, v31, s[0:3], 0 offen offset:44
	    0xe0701030u, 0x8000111fu, // buffer_store_dword v17, v31, s[0:3], 0 offen offset:48
	    0xe0701034u, 0x8000121fu, // buffer_store_dword v18, v31, s[0:3], 0 offen offset:52
	    0xbf810000u, // s_endpgm
	};
	CompileToValidSpirv(code, "integer long tail", 32);
}

// ---- SAD family and VOP3P integer CLAMP ----

void TestSadAndPackedClamp() {
	CheckEncodings({
	    {"v_sad_u8 v0, v1, v2, v3", {0xd55a0000u, 0x040e0501u}, Opcode::V_SAD_U8},
	    {"v_sad_hi_u8 v0, v1, v2, v3", {0xd55b0000u, 0x040e0501u}, Opcode::V_SAD_HI_U8},
	    {"v_sad_u16 v0, v1, v2, v3", {0xd55c0000u, 0x040e0501u}, Opcode::V_SAD_U16},
	    {"v_msad_u8 v0, v1, v2, v3", {0xd5710000u, 0x040e0501u}, Opcode::V_MSAD_U8},
	    {"v_msad_u8 v0, v1, 0x01020304, v3", {0xd5710000u, 0x040dff01u, 0x01020304u}, Opcode::V_MSAD_U8},
	    {"v_pk_add_u16 v0, v1, v2 clamp", {0xcc0ac000u, 0x18020501u}, Opcode::V_PK_ADD_U16},
	    {"v_pk_sub_u16 v0, v1, v2 clamp", {0xcc0bc000u, 0x18020501u}, Opcode::V_PK_SUB_U16},
	    {"v_pk_add_i16 v0, v1, v2 clamp", {0xcc02c000u, 0x18020501u}, Opcode::V_PK_ADD_I16},
	    {"v_pk_sub_i16 v0, v1, v2 clamp", {0xcc03c000u, 0x18020501u}, Opcode::V_PK_SUB_I16},
	    {"v_pk_mad_i16 v0, v1, v2, v3 clamp", {0xcc00c000u, 0x1c0e0501u}, Opcode::V_PK_MAD_I16},
	    {"v_pk_mad_u16 v0, v1, v2, v3 clamp", {0xcc09c000u, 0x1c0e0501u}, Opcode::V_PK_MAD_U16},
	    {"v_pk_mul_lo_u16 v0, v1, v2 clamp", {0xcc01c000u, 0x18020501u}, Opcode::V_PK_MUL_LO_U16},
	});
	const uint32_t clamp[] = {0xcc0bc000u, 0x18020501u}; // v_pk_sub_u16 v0, v1, v2 clamp
	Check(DecodeOne(clamp).dst.clamp, "V_PK_SUB_U16 lost its CLAMP");
	// The other packed integer opcodes keep rejecting CLAMP.
	const uint32_t max_clamp[] = {0xcc0cc000u, 0x18020501u}; // v_pk_max_u16 v0, v1, v2 clamp
	Check(DecodeOne(max_clamp).opcode == Opcode::UNSUPPORTED, "V_PK_MAX_U16 accepted CLAMP");
	// The SAD opcodes take no modifiers.
	const uint32_t sad_clamp[] = {EncodeVop3Word0(0x15au, 0, 0, 0, true),
	                              EncodeVop3Word1(Vgpr(1), Vgpr(2), Vgpr(3))};
	Check(DecodeOne(sad_clamp).opcode == Opcode::UNSUPPORTED, "V_SAD_U8 accepted CLAMP");

	const uint32_t code[] = {
	    0x7e0202ffu, 0x80ff7f01u, // v_mov_b32 v1, 0x80ff7f01
	    0x7e0402ffu, 0x01fe8080u, // v_mov_b32 v2, 0x01fe8080
	    0xd55a0003u, 0x04020501u, // v_sad_u8 v3, v1, v2, v0
	    0xd55b0004u, 0x04020501u, // v_sad_hi_u8 v4, v1, v2, v0
	    0xd55c0005u, 0x04020501u, // v_sad_u16 v5, v1, v2, v0
	    0xd5710006u, 0x04020501u, // v_msad_u8 v6, v1, v2, v0
	    0xcc0ac007u, 0x18020501u, // v_pk_add_u16 v7, v1, v2 clamp
	    0xcc0bc008u, 0x18020501u, // v_pk_sub_u16 v8, v1, v2 clamp
	    0xcc02c009u, 0x18020501u, // v_pk_add_i16 v9, v1, v2 clamp
	    0xcc03c00au, 0x18020501u, // v_pk_sub_i16 v10, v1, v2 clamp
	    0xcc00c00bu, 0x1c020501u, // v_pk_mad_i16 v11, v1, v2, v0 clamp
	    0xcc09c00cu, 0x1c020501u, // v_pk_mad_u16 v12, v1, v2, v0 clamp
	    0x7e3e0280u, // v_mov_b32 v31, 0
	    0xe0701000u, 0x8000031fu, // buffer_store_dword v3, v31, s[0:3], 0 offen
	    0xe0701004u, 0x8000041fu, // buffer_store_dword v4, v31, s[0:3], 0 offen offset:4
	    0xe0701008u, 0x8000051fu, // buffer_store_dword v5, v31, s[0:3], 0 offen offset:8
	    0xe070100cu, 0x8000061fu, // buffer_store_dword v6, v31, s[0:3], 0 offen offset:12
	    0xe0701010u, 0x8000071fu, // buffer_store_dword v7, v31, s[0:3], 0 offen offset:16
	    0xe0701014u, 0x8000081fu, // buffer_store_dword v8, v31, s[0:3], 0 offen offset:20
	    0xe0701018u, 0x8000091fu, // buffer_store_dword v9, v31, s[0:3], 0 offen offset:24
	    0xe070101cu, 0x80000a1fu, // buffer_store_dword v10, v31, s[0:3], 0 offen offset:28
	    0xe0701020u, 0x80000b1fu, // buffer_store_dword v11, v31, s[0:3], 0 offen offset:32
	    0xe0701024u, 0x80000c1fu, // buffer_store_dword v12, v31, s[0:3], 0 offen offset:36
	    0xbf810000u, // s_endpgm
	};
	CompileToValidSpirv(code, "SAD and packed clamp", 32);
}

// ---- The decoder the Psr code tables were generated with ----

// The opcode field and the length the decoder gave an instruction it did not know, before
// gta6-prep: shader-alu: the base words of its encoding, without literal (SDWA/DPP for VOPC).
struct LegacyUnknown {
	Decoder::Family family;
	uint32_t        opcode_id;
	uint32_t        word_count;
};

LegacyUnknown ExpectedLegacyUnknown(std::span<const uint32_t> words) {
	using Decoder::Family;
	const uint32_t word   = words[0];
	const auto     family = Decoder::GetInstructionFamily(word);
	switch (family) {
		case Family::SOP1: return {family, (word >> 8u) & 0xffu, 1};
		case Family::SOP2: return {family, (word >> 23u) & 0x7fu, 1};
		case Family::SOPK: return {family, (word >> 23u) & 0x1fu, 1};
		case Family::SOPP: return {family, (word >> 16u) & 0x7fu, 1};
		case Family::VOP1: return {family, (word >> 9u) & 0xffu, 1};
		case Family::VOP2: return {family, (word >> 25u) & 0x3fu, 1};
		case Family::VOPC: {
			const uint32_t src0 = word & 0x1ffu;
			return {family, (word >> 17u) & 0xffu, src0 == 249u || src0 == 250u ? 2u : 1u};
		}
		case Family::VOP3: return {family, (word >> 16u) & 0x3ffu, 2};
		case Family::VOP3P: return {family, (word >> 16u) & 0x7fu, 2};
		case Family::SMEM: return {family, (word >> 18u) & 0xffu, 2};
		default: return {family, 0, 0};
	}
}

void TestLegacyDecodeKeepsCodeTableKeys() {
	// Psr::CodeFingerprint() keys the instructions the decoder did not know by their family and
	// opcode field, and steps over their base words only: DecodeLegacyInstruction() must still
	// see every new encoding so, or GTA V's Psr passes stop matching their code tables.
	Check(CheckedEncodings().size() > 100u, "the new encodings were not all collected");
	for (const auto& encoding: CheckedEncodings()) {
		Decoder::Instruction inst;
		Decoder::DecodeLegacyInstruction(encoding.words, 0, inst);
		const auto expected = ExpectedLegacyUnknown(encoding.words);
		Check(inst.opcode == Opcode::UNSUPPORTED && inst.family == expected.family &&
		          inst.opcode_id == expected.opcode_id && inst.word_count == expected.word_count,
		      std::string("the legacy decode knows a new encoding: ") + encoding.text + " -> " +
		          Decoder::InstructionToString(inst));
		Check(!Decoder::IsLegacyDecode(), "the legacy decode stayed on after DecodeLegacyInstruction");
		Check(DecodeOne(encoding.words).opcode == encoding.opcode,
		      std::string("the legacy decode changed the next decode: ") + encoding.text);
	}
	// What the decoder knew before decodes as it did.
	const uint32_t known[][2] = {
	    {0xbe800301u, 0u},          // s_mov_b32 s0, s1
	    {0xd5320000u, 0x00020501u}, // v_add_f16_e64 v0, v1, v2
	    {0xcc0a4000u, 0x18020501u}, // v_pk_add_u16 v0, v1, v2
	    {0xf4940000u, 0x00000000u}, // s_memrealtime s[0:1]
	    {0x7e000301u, 0u},          // v_mov_b32 v0, v1
	};
	for (const auto& words: known) {
		Decoder::Instruction legacy;
		Decoder::DecodeLegacyInstruction(words, 0, legacy);
		const auto current = DecodeOne(words);
		Check(legacy.opcode != Opcode::UNSUPPORTED && legacy.opcode == current.opcode &&
		          legacy.word_count == current.word_count,
		      std::string("the legacy decode rejects an older encoding: ") +
		          Decoder::InstructionToString(current));
	}
}

} // namespace
} // namespace Libs::Graphics

// Names each test before it runs: a fatal error inside one (EXIT ends the process with status
// 321 and no Check message) is then attributed to the right test.
void Run(const char* name, void (*test)()) {
	std::fprintf(stderr, "ShaderAluDecodeTests: running %s\n", name);
	test();
}

int main() {
	using namespace Libs::Graphics;
	EnsureConfigInitialized();
	Run("TestStrictShadersDefault", TestStrictShadersDefault);
	Run("TestUnsupportedEncodingsDecodeWithoutExit", TestUnsupportedEncodingsDecodeWithoutExit);
	Run("TestPrepareShaderSourceRecordsUnsupported", TestPrepareShaderSourceRecordsUnsupported);
	Run("TestTranslateProgramReportsFailures", TestTranslateProgramReportsFailures);
	Run("TestMovesPermSwap", TestMovesPermSwap);
	Run("TestSaveexecFamily", TestSaveexecFamily);
	Run("TestVop3F16Modifiers", TestVop3F16Modifiers);
	Run("TestDotProducts", TestDotProducts);
	Run("TestScalarModeAndHintNops", TestScalarModeAndHintNops);
	Run("TestScalarMemoryNops", TestScalarMemoryNops);
	Run("TestVopcGaps", TestVopcGaps);
	Run("TestPreciseDivision", TestPreciseDivision);
	Run("TestIntegerLongTail", TestIntegerLongTail);
	Run("TestSadAndPackedClamp", TestSadAndPackedClamp);
	Run("TestLegacyDecodeKeepsCodeTableKeys", TestLegacyDecodeKeepsCodeTableKeys);
	std::printf("ShaderAluDecodeTests: all tests passed\n");
	return 0;
}
