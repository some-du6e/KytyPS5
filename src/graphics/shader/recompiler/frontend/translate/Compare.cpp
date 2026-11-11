#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <bit>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::EmitCompareResult(const Decoder::Instruction& inst, IR::U1 value, bool scalar) {
	if (scalar) {
		ir.SetScc(value);
		return;
	}
	WriteVectorMask(inst.dst, value);
}

void Translator::EmitCompareConstant(const Decoder::Instruction& inst, bool value, bool scalar) {
	EmitCompareResult(inst, IR::U1(IR::Value(value)), scalar);
}

void Translator::EmitIntegerCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                    IR::Type type, bool scalar, bool swap_operands) {
	// The equality compares share their IR opcode with the unsigned ones.
	const bool signed_64 = opcode == IR::ValueOpcode::SLessThan64 ||
	                       opcode == IR::ValueOpcode::SLessThanEqual64 ||
	                       inst.opcode == Decoder::Opcode::V_CMP_EQ_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMPX_EQ_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMP_NE_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMPX_NE_I64;
	const auto read = [&](const Decoder::Operand& operand) {
		// RDNA2 expands signed 64-bit integer literals by sign extension.
		if (signed_64 && operand.kind == Decoder::OperandKind::LiteralConstant) {
			return IR::Value(static_cast<uint64_t>(
			    static_cast<int64_t>(std::bit_cast<int32_t>(operand.value))));
		}
		return ReadOperand(operand, type);
	};
	auto lhs = read(inst.src0);
	auto rhs = read(inst.src1);
	if (swap_operands) {
		std::swap(lhs, rhs);
	}
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), scalar);
}

void Translator::EmitInteger16Compare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                      bool signed_value) {
	const auto lhs = ReadU16AsU32(inst.src0, signed_value);
	const auto rhs = ReadU16AsU32(inst.src1, signed_value);
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), false);
}

void Translator::EmitFloatCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                  bool half) {
	const auto type = IR::ArgTypeOf(opcode, 0);
	const auto lhs =
	    half ? IR::Value(ReadF16AsF32(inst.src0)) : ReadOperand(inst.src0, type);
	const auto rhs =
	    half ? IR::Value(ReadF16AsF32(inst.src1)) : ReadOperand(inst.src1, type);
	const IR::FPCompareFlags flags{
	    .flush_input_denorms = !half && type == IR::Type::F32 && flush_f32_inputs};
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs}, flags)), false);
}

void Translator::EmitFloatOrderedCompare(const Decoder::Instruction& inst, bool ordered,
                                         IR::Type type) {
	const auto is_nan_op =
	    type == IR::Type::F64 ? IR::ValueOpcode::FPIsNan64 : IR::ValueOpcode::FPIsNan32;
	const auto is_nan = [&](const Decoder::Operand& operand) {
		const auto value =
		    type == IR::Type::F16 ? IR::Value(ReadF16AsF32(operand)) : ReadOperand(operand, type);
		return IR::U1(ir.Emit(is_nan_op, {value}));
	};
	const auto lhs       = is_nan(inst.src0);
	const auto unordered = ir.LogicalOr(lhs, is_nan(inst.src1));
	EmitCompareResult(inst, ordered ? ir.LogicalNot(unordered) : unordered, false);
}

void Translator::EmitFloatClassCompare(const Decoder::Instruction& inst, bool half) {
	const auto value = half ? IR::Value(Read16LaneBits(inst.src0, false))
	                        : ReadOperand(inst.src0, IR::Type::F32);
	const auto mask = half ? IR::Value(Read16LaneBits(inst.src1, false))
	                      : ReadOperand(inst.src1, IR::Type::U32);
	const auto opcode = half ? IR::ValueOpcode::FPCmpClass16 : IR::ValueOpcode::FPCmpClass32;
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {value, mask})), false);
}

// gta6-prep: shader-alu. D.lane = S1.u[class(S0.f64)]: bit 0 signaling NaN, 1 quiet NaN, 2 -inf,
// 3 -normal, 4 -denormal, 5 -0, 6 +0, 7 +denormal, 8 +normal, 9 +inf. S0 takes ABS / NEG.
void Translator::V_CMP_CLASS_F64(const Decoder::Instruction& inst) {
	const auto u32  = [](uint32_t value) { return IR::U32(IR::Value(value)); };
	auto       pair = ReadU32Pair(PlainOperand(inst.src0));
	if (inst.src0.kind == Decoder::OperandKind::LiteralConstant) {
		// A 64-bit float literal supplies the high word.
		pair = {u32(0u), ReadU32(PlainOperand(inst.src0))};
	}
	auto high = pair[1];
	if (inst.src0.absolute) {
		high = ir.BitwiseAnd(high, u32(0x7fffffffu));
	}
	if (inst.src0.negate) {
		high = ir.BitwiseXor(high, u32(0x80000000u));
	}
	const auto mask     = ReadU32(PlainOperand(inst.src1));
	const auto exponent = IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract,
	                                      {high, IR::Value(20u), IR::Value(11u)}));
	const auto mantissa = ir.LogicalOr(ir.INotEqual(ir.BitwiseAnd(high, u32(0xfffffu)), u32(0u)),
	                                   ir.INotEqual(pair[0], u32(0u)));
	const auto negative = ir.INotEqual(ir.ShiftRightLogical(high, u32(31u)), u32(0u));
	const auto quiet    = ir.INotEqual(ir.BitwiseAnd(high, u32(0x80000u)), u32(0u));
	const auto signed_class = [&](uint32_t negative_bit, uint32_t positive_bit) {
		return ir.Select(negative, u32(negative_bit), u32(positive_bit));
	};
	const auto special = ir.Select(mantissa, ir.Select(quiet, u32(1u), u32(0u)), signed_class(2u, 9u));
	const auto tiny    = ir.Select(mantissa, signed_class(4u, 7u), signed_class(5u, 6u));
	const auto index   = ir.Select(ir.IEqual(exponent, u32(0x7ffu)), special,
	                               ir.Select(ir.IEqual(exponent, u32(0u)), tiny, signed_class(3u, 8u)));
	const auto bit = ir.BitwiseAnd(ir.ShiftRightLogical(mask, index), u32(1u));
	EmitCompareResult(inst, ir.INotEqual(bit, u32(0u)), false);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
