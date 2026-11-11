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
void CompileToValidSpirv(std::span<const uint32_t> code, const char* name, uint32_t wave_size = 64) {
	auto options      = ComputeOptions(true);
	options.wave_size = wave_size;
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

} // namespace
} // namespace Libs::Graphics

int main() {
	using namespace Libs::Graphics;
	EnsureConfigInitialized();
	TestStrictShadersDefault();
	TestUnsupportedEncodingsDecodeWithoutExit();
	TestPrepareShaderSourceRecordsUnsupported();
	TestTranslateProgramReportsFailures();
	std::printf("ShaderAluDecodeTests: all tests passed\n");
	return 0;
}
