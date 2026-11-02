// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

// Scalar floating point (VFP) for the WASM backend.
//
// IR FP values are bit patterns in integer locals (U32 = f32 bits, U64 = f64
// bits), so every real float op is reinterpret -> float op -> reinterpret.
// Wasm f32/f64 ops are IEEE-754 round-to-nearest-even with no exception
// flags, matching what the x64 backend gets from SSE with default MXCSR.

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_fp_util.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/common/fp/rounding_mode.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// Push args[0] as an f32/f64 value (reinterpret from its bit pattern).
void EmitPushF32(EmitContext& ctx, const RegAlloc::Argument& arg) {
    EmitPush(ctx.code, ctx.reg_alloc, arg);
    ctx.code.U8(Op::F32ReinterpretI32);
}

void EmitPushF64(EmitContext& ctx, const RegAlloc::Argument& arg) {
    EmitPush(ctx.code, ctx.reg_alloc, arg);
    ctx.code.U8(Op::F64ReinterpretI64);
}

// Emit a binary f32/f64 op; the float result is left on the stack.
void EmitFBinOp(EmitContext& ctx, IR::Inst* inst, u8 op32, u8 op64) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (inst->GetType() == IR::Type::U32) {
        EmitPushF32(ctx, args[0]);
        EmitPushF32(ctx, args[1]);
        ctx.code.U8(op32);
    } else {
        EmitPushF64(ctx, args[0]);
        EmitPushF64(ctx, args[1]);
        ctx.code.U8(op64);
    }
}

// Reinterpret the float on the stack back to the IR bit-pattern type and
// store it in inst's local.
void EmitStoreFloatResult(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.U8(inst->GetType() == IR::Type::U32 ? Op::I32ReinterpretF32 : Op::I64ReinterpretF64);
    ctx.code.LocalSet(dst);
}

void EmitFBin(EmitContext& ctx, IR::Inst* inst, u8 op32, u8 op64) {
    EmitFBinOp(ctx, inst, op32, op64);
    EmitStoreFloatResult(ctx, inst);
}

// Apply the rounding mode (an immediate, see FP::RoundingMode) to the float
// on top of the stack.
void EmitRoundImm(EmitContext& ctx, FP::RoundingMode rounding, bool is_f64) {
    FuncBody& code = ctx.code;
    const u8 op_nearest = is_f64 ? Op::F64Nearest : Op::F32Nearest;
    const u8 op_ceil = is_f64 ? Op::F64Ceil : Op::F32Ceil;
    const u8 op_floor = is_f64 ? Op::F64Floor : Op::F32Floor;
    const u8 op_trunc = is_f64 ? Op::F64Trunc : Op::F32Trunc;
    switch (rounding) {
    case FP::RoundingMode::ToNearest_TieEven:
        code.U8(op_nearest);
        return;
    case FP::RoundingMode::TowardsPlusInfinity:
        code.U8(op_ceil);
        return;
    case FP::RoundingMode::TowardsMinusInfinity:
        code.U8(op_floor);
        return;
    case FP::RoundingMode::TowardsZero:
        code.U8(op_trunc);
        return;
    case FP::RoundingMode::ToNearest_TieAwayFromZero: {
        // r = trunc(x); if |x - r| >= 0.5 then r + copysign(1, x) else r
        const ValType ft = is_f64 ? ValType::F64 : ValType::F32;
        const u32 x = ctx.Temp(ft);
        const u32 r = ctx.Temp(ft);
        code.LocalSet(x);
        code.LocalGet(x);
        code.U8(op_trunc);
        code.LocalSet(r);
        // |x - r| >= 0.5
        code.LocalGet(x);
        code.LocalGet(r);
        code.U8(is_f64 ? Op::F64Sub : Op::F32Sub);
        code.U8(is_f64 ? Op::F64Abs : Op::F32Abs);
        if (is_f64) {
            code.F64Const(0x3FE0000000000000ULL);  // 0.5
        } else {
            code.F32Const(0x3F000000);  // 0.5f
        }
        code.U8(is_f64 ? Op::F64Ge : Op::F32Ge);
        code.If({{ft}});
        code.LocalGet(r);
        if (is_f64) {
            code.F64Const(0x3FF0000000000000ULL);  // 1.0
        } else {
            code.F32Const(0x3F800000);  // 1.0f
        }
        code.LocalGet(x);
        code.U8(is_f64 ? Op::F64Copysign : Op::F32Copysign);
        code.U8(is_f64 ? Op::F64Add : Op::F32Add);
        code.Else();
        code.LocalGet(r);
        code.End();
        return;
    }
    case FP::RoundingMode::ToOdd:
        // Only used by A64; not worth the bit games until needed.
        UNIMPLEMENTED();
    }
    UNREACHABLE();
}

// 2^fbits as f32/f64 raw bits (fbits <= 64).
u32 Pow2F32Bits(u8 fbits) {
    return (u32(127) + fbits) << 23;
}

u64 Pow2F64Bits(u8 fbits) {
    return (u64(1023) + fbits) << 52;
}

// FP<Half|Single|Double>ToFixed<S|U><16|32|64>
// result = saturate(round(value * 2^fbits))
// A half-precision source is first widened to f32 (exact) through the
// reference FP::FPConvert thunk, which also handles FPCR.AHP and folds the
// exception bits it raises into state->fpsr; the rest is the f32 path.
void EmitFPToFixed(EmitContext& ctx, IR::Inst* inst, bool is_f64, bool is_signed, u32 int_bits,
                   bool is_half = false) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate() && args[2].IsImmediate());
    const u8 fbits = args[1].GetImmediateU8();
    const auto rounding = static_cast<FP::RoundingMode>(args[2].GetImmediateU8());
    FuncBody& code = ctx.code;

    if (is_half) {
        const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32}, {ValType::I32}});
        const u32 fn = ctx.Import(kImportFpHalfToSingle, ty);
        EmitPushFpsrAddr(ctx);
        code.LocalGet(kParamEctx);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.I32Const(static_cast<s32>(ctx.fpcr));
        code.I32Const(static_cast<s32>(FP::RoundingMode::ToNearest_TieEven));
        code.Call(fn);
        code.U8(Op::F32ReinterpretI32);
    } else if (is_f64) {
        EmitPushF64(ctx, args[0]);
    } else {
        EmitPushF32(ctx, args[0]);
    }
    if (fbits != 0) {
        if (is_f64) {
            code.F64Const(Pow2F64Bits(fbits));
        } else {
            code.F32Const(Pow2F32Bits(fbits));
        }
        code.U8(is_f64 ? Op::F64Mul : Op::F32Mul);
    }
    EmitRoundImm(ctx, rounding, is_f64);

    // Saturating truncation (wasm traps nothing: NaN -> 0, inf -> min/max),
    // which matches ARM's FPToFixed saturation behaviour.
    const u32 trunc_op = is_f64 ? (int_bits == 64 ? (is_signed ? Op::I64TruncSatF64S : Op::I64TruncSatF64U)
                                                     : (is_signed ? Op::I32TruncSatF64S : Op::I32TruncSatF64U))
                                : (int_bits == 64 ? (is_signed ? Op::I64TruncSatF32S : Op::I64TruncSatF32U)
                                                     : (is_signed ? Op::I32TruncSatF32S : Op::I32TruncSatF32U));
    code.U8(Op::PrefixMisc);
    code.ULEB(trunc_op);

    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    if (int_bits == 16) {
        // trunc_sat saturates to 32 bits; clamp to the 16-bit range (the
        // unsigned result is a u32, so compare it unsigned).
        const s32 hi = is_signed ? 32767 : 65535;
        const u32 v = ctx.Temp(ValType::I32);
        code.LocalSet(v);
        if (is_signed) {
            // max(v, lo): select(v, lo, v > lo)
            code.LocalGet(v);
            code.I32Const(-32768);
            code.LocalGet(v);
            code.I32Const(-32768);
            code.U8(Op::I32GtS);
            code.U8(Op::Select);
            code.LocalSet(v);
        }
        // min(v, hi): select(v, hi, v < hi)
        code.LocalGet(v);
        code.I32Const(hi);
        code.LocalGet(v);
        code.I32Const(hi);
        code.U8(is_signed ? Op::I32LtS : Op::I32LtU);
        code.U8(Op::Select);
    }
    code.LocalSet(dst);
}

// FPFixed<S|U><16|32|64>To<Single|Double>
// result = float(value) / 2^fbits
void EmitFixedToFP(EmitContext& ctx, IR::Inst* inst, bool to_f64, bool is_signed, u32 int_bits) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate() && args[2].IsImmediate());
    const u8 fbits = args[1].GetImmediateU8();
    FuncBody& code = ctx.code;

    EmitPush(code, ctx.reg_alloc, args[0]);
    if (int_bits == 64) {
        code.U8(to_f64 ? (is_signed ? Op::F64ConvertI64S : Op::F64ConvertI64U)
                       : (is_signed ? Op::F32ConvertI64S : Op::F32ConvertI64U));
    } else {
        if (int_bits == 16) {
            // i32 local holds a raw 16-bit value; normalize the sign first.
            if (is_signed) {
                code.U8(Op::I32Extend16S);
            } else {
                code.I32Const(0xFFFF);
                code.U8(Op::I32And);
            }
        }
        code.U8(to_f64 ? (is_signed ? Op::F64ConvertI32S : Op::F64ConvertI32U)
                       : (is_signed ? Op::F32ConvertI32S : Op::F32ConvertI32U));
    }
    if (fbits != 0) {
        // Exact power-of-two scale down.
        if (to_f64) {
            code.F64Const(Pow2F64Bits(fbits));
            code.U8(Op::F64Div);
        } else {
            code.F32Const(Pow2F32Bits(fbits));
            code.U8(Op::F32Div);
        }
    }
    EmitStoreFloatResult(ctx, inst);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Abs / Neg: pure integer bit ops
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPAbs16>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0x7FFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPAbs32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0x7FFFFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPAbs64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I64Const(0x7FFFFFFFFFFFFFFFLL);
    ctx.code.U8(Op::I64And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPNeg16>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0x8000);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPNeg32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0x80000000));
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPNeg64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I64Const(static_cast<s64>(0x8000000000000000ULL));
    ctx.code.U8(Op::I64Xor);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F32Add, Op::F32Add);
}

template<>
void EmitIR<IR::Opcode::FPAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F64Add, Op::F64Add);
}

template<>
void EmitIR<IR::Opcode::FPSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F32Sub, Op::F32Sub);
}

template<>
void EmitIR<IR::Opcode::FPSub64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F64Sub, Op::F64Sub);
}

template<>
void EmitIR<IR::Opcode::FPMul32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F32Mul, Op::F32Mul);
}

template<>
void EmitIR<IR::Opcode::FPMul64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F64Mul, Op::F64Mul);
}

template<>
void EmitIR<IR::Opcode::FPDiv32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F32Div, Op::F32Div);
}

template<>
void EmitIR<IR::Opcode::FPDiv64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F64Div, Op::F64Div);
}

template<>
void EmitIR<IR::Opcode::FPSqrt32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitPushF32(ctx, args[0]);
    ctx.code.U8(Op::F32Sqrt);
    EmitStoreFloatResult(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPSqrt64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitPushF64(ctx, args[0]);
    ctx.code.U8(Op::F64Sqrt);
    EmitStoreFloatResult(ctx, inst);
}

// ---------------------------------------------------------------------------
// Min / Max (NaN-propagating, like SSE minss/maxss and ARM FPMin/FPMax)
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPMin32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F32Min, Op::F32Min);
}

template<>
void EmitIR<IR::Opcode::FPMin64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F64Min, Op::F64Min);
}

template<>
void EmitIR<IR::Opcode::FPMax32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F32Max, Op::F32Max);
}

template<>
void EmitIR<IR::Opcode::FPMax64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFBin(ctx, inst, Op::F64Max, Op::F64Max);
}

template<>
void EmitIR<IR::Opcode::FPMaxNumeric32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, inst->GetType() == IR::Type::U64 ? 64 : 32, false, true);
}

template<>
void EmitIR<IR::Opcode::FPMaxNumeric64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, inst->GetType() == IR::Type::U64 ? 64 : 32, false, true);
}

template<>
void EmitIR<IR::Opcode::FPMinNumeric32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, inst->GetType() == IR::Type::U64 ? 64 : 32, false, false);
}

template<>
void EmitIR<IR::Opcode::FPMinNumeric64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, inst->GetType() == IR::Type::U64 ? 64 : 32, false, false);
}

// ---------------------------------------------------------------------------
// Compare -> NZCV (N=bit31, Z=bit30, C=bit29, V=bit28)
//   less: 0x8, equal: 0x6, greater: 0x2, unordered: 0x3 (in top nibble)
// ---------------------------------------------------------------------------

static void EmitFPCompare(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const bool is_f64 = inst->GetOpcode() == IR::Opcode::FPCompare64;

    const u32 lt = ctx.Temp(ValType::I32);
    const u32 gt = ctx.Temp(ValType::I32);
    const u32 eq = ctx.Temp(ValType::I32);
    if (is_f64) {
        EmitPushF64(ctx, args[0]);
        EmitPushF64(ctx, args[1]);
    } else {
        EmitPushF32(ctx, args[0]);
        EmitPushF32(ctx, args[1]);
    }
    // stack: fa fb -> need three compares; keep temps
    const u32 fa = ctx.Temp(is_f64 ? ValType::F64 : ValType::F32);
    const u32 fb = ctx.Temp(is_f64 ? ValType::F64 : ValType::F32);
    code.LocalSet(fb);
    code.LocalSet(fa);
    code.LocalGet(fa);
    code.LocalGet(fb);
    code.U8(is_f64 ? Op::F64Lt : Op::F32Lt);
    code.LocalSet(lt);
    code.LocalGet(fa);
    code.LocalGet(fb);
    code.U8(is_f64 ? Op::F64Gt : Op::F32Gt);
    code.LocalSet(gt);
    code.LocalGet(fa);
    code.LocalGet(fb);
    code.U8(is_f64 ? Op::F64Eq : Op::F32Eq);
    code.LocalSet(eq);

    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    // un = 1 - (lt | gt | eq)
    code.I32Const(1);
    code.LocalGet(lt);
    code.LocalGet(gt);
    code.U8(Op::I32Or);
    code.LocalGet(eq);
    code.U8(Op::I32Or);
    code.U8(Op::I32Sub);
    const u32 un = ctx.Temp(ValType::I32);
    code.LocalSet(un);

    code.LocalGet(lt);
    code.I32Const(31);
    code.U8(Op::I32Shl);
    code.LocalGet(eq);
    code.I32Const(30);
    code.U8(Op::I32Shl);
    code.U8(Op::I32Or);
    // C = gt | eq | un
    code.LocalGet(gt);
    code.LocalGet(eq);
    code.U8(Op::I32Or);
    code.LocalGet(un);
    code.U8(Op::I32Or);
    code.I32Const(29);
    code.U8(Op::I32Shl);
    code.U8(Op::I32Or);
    // V = un
    code.LocalGet(un);
    code.I32Const(28);
    code.U8(Op::I32Shl);
    code.U8(Op::I32Or);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPCompare32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPCompare(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPCompare64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPCompare(ctx, inst);
}

// ---------------------------------------------------------------------------
// Fused multiply-add/sub: FPMulAdd(addend, op1, op2) = addend + op1 * op2,
// FPMulSub(minuend, op1, op2) = minuend - op1 * op2.
// 32-bit: op1*op2 is exact in f64, so promote -> f64 mul -> f64 add -> demote
// is fused except for a final double-rounding tie case. 64-bit: wasm has no
// fma instruction; the fp_fma64 thunk (C library fma) is correctly rounded.
// ---------------------------------------------------------------------------

// addend +- op1 * op2, fused: the f32 product is exact in f64, and the f64
// sum is rounded to odd (TwoSum error, then the odd neighbour when inexact),
// which makes the final rounding to f32 correct (53 >= 2 * 24 + 2).
static void EmitFPMulAddSub32(EmitContext& ctx, IR::Inst* inst, bool is_sub) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 c = ctx.Temp(ValType::F64);
    const u32 p = ctx.Temp(ValType::F64);
    const u32 sum = ctx.Temp(ValType::F64);
    const u32 bb = ctx.Temp(ValType::F64);
    const u32 err = ctx.Temp(ValType::F64);
    const u32 bits = ctx.Temp(ValType::I64);
    EmitPushF32(ctx, args[0]);
    code.U8(Op::F64PromoteF32);
    code.LocalSet(c);
    EmitPushF32(ctx, args[1]);
    code.U8(Op::F64PromoteF32);
    EmitPushF32(ctx, args[2]);
    code.U8(Op::F64PromoteF32);
    code.U8(Op::F64Mul);
    if (is_sub) {
        code.U8(Op::F64Neg);
    }
    code.LocalSet(p);

    // TwoSum: sum = c + p, err = exact (c + p) - sum
    code.LocalGet(c);
    code.LocalGet(p);
    code.U8(Op::F64Add);
    code.LocalTee(sum);
    code.LocalGet(c);
    code.U8(Op::F64Sub);
    code.LocalSet(bb);
    code.LocalGet(c);
    code.LocalGet(sum);
    code.LocalGet(bb);
    code.U8(Op::F64Sub);
    code.U8(Op::F64Sub);
    code.LocalGet(p);
    code.LocalGet(bb);
    code.U8(Op::F64Sub);
    code.U8(Op::F64Add);
    code.LocalSet(err);

    // bits(sum) + (inexact && even ? (sign(err) == sign(sum) ? 1 : -1) : 0)
    code.LocalGet(sum);
    code.U8(Op::I64ReinterpretF64);
    code.LocalTee(bits);
    code.I64Const(1);
    code.LocalGet(bits);
    code.LocalGet(err);
    code.U8(Op::I64ReinterpretF64);
    code.U8(Op::I64Xor);
    code.I64Const(63);
    code.U8(Op::I64ShrU);
    code.I64Const(1);
    code.U8(Op::I64Shl);
    code.U8(Op::I64Sub);  // 1 - 2 * (signs differ)
    code.I64Const(0);
    // 0 < |err| < inf (finite inputs; NaN / infinite sums pass through)
    code.LocalGet(err);
    code.U8(Op::F64Abs);
    code.F64Const(0);
    code.U8(Op::F64Gt);
    code.LocalGet(err);
    code.U8(Op::F64Abs);
    code.F64Const(0x7FF0000000000000);
    code.U8(Op::F64Lt);
    code.U8(Op::I32And);
    code.LocalGet(bits);
    code.I64Const(1);
    code.U8(Op::I64And);
    code.U8(Op::I64Eqz);
    code.U8(Op::I32And);
    code.U8(Op::Select);
    code.U8(Op::I64Add);
    code.U8(Op::F64ReinterpretI64);
    code.U8(Op::F32DemoteF64);
    EmitStoreFloatResult(ctx, inst);
}

static void EmitFPMulAddSub64(EmitContext& ctx, IR::Inst* inst, bool is_sub) {
    EmitFPMulAdd64Fused(ctx, inst, is_sub);
}

template<>
void EmitIR<IR::Opcode::FPMulAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMulAddSub32(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::FPMulAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMulAddSub64(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::FPMulSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMulAddSub32(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::FPMulSub64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMulAddSub64(ctx, inst, true);
}

// ---------------------------------------------------------------------------
// Round to integral, staying in FP format: FPRoundInt(value, rounding, exact)
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPRoundInt32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    EmitPushF32(ctx, args[0]);
    EmitRoundImm(ctx, static_cast<FP::RoundingMode>(args[1].GetImmediateU8()), false);
    EmitStoreFloatResult(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPRoundInt64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    EmitPushF64(ctx, args[0]);
    EmitRoundImm(ctx, static_cast<FP::RoundingMode>(args[1].GetImmediateU8()), true);
    EmitStoreFloatResult(ctx, inst);
}

// ---------------------------------------------------------------------------
// Format conversions
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPSingleToDouble>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitPushF32(ctx, args[0]);
    ctx.code.U8(Op::F64PromoteF32);
    EmitStoreFloatResult(ctx, inst);
}

// f32.demote_f64 rounds to nearest-even. Other rounding modes (FPCR.RMode,
// or round-to-odd for FCVTXN) start from that result r and move its
// magnitude by one ulp when it rounded the wrong way: with back = f64(r),
// |back| > |x| means r rounded up in magnitude, |back| < |x| down. Bit
// arithmetic on the magnitude crosses binades / reaches FLT_MAX or infinity
// correctly; NaNs compare false and keep the demoted quiet NaN.
template<>
void EmitIR<IR::Opcode::FPDoubleToSingle>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const auto rounding = static_cast<FP::RoundingMode>(args[1].GetImmediateU8());
    FuncBody& code = ctx.code;
    if (rounding == FP::RoundingMode::ToNearest_TieEven || rounding == FP::RoundingMode::ToNearest_TieAwayFromZero) {
        // (Ties-away differs from ties-even only on exact ties; not modelled.)
        EmitPushF64(ctx, args[0]);
        code.U8(Op::F32DemoteF64);
        EmitStoreFloatResult(ctx, inst);
        return;
    }

    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 x = ctx.Temp(ValType::F64);
    const u32 bits = ctx.Temp(ValType::I32);
    EmitPushF64(ctx, args[0]);
    code.LocalTee(x);
    code.U8(Op::F32DemoteF64);
    code.U8(Op::I32ReinterpretF32);
    code.LocalSet(bits);

    // Pushes |f64(r)| <op> |x| for the current bits.
    auto magnitude_cmp = [&](u8 op) {
        code.LocalGet(bits);
        code.U8(Op::F32ReinterpretI32);
        code.U8(Op::F64PromoteF32);
        code.U8(Op::F64Abs);
        code.LocalGet(x);
        code.U8(Op::F64Abs);
        code.U8(op);
    };
    auto push_negative = [&] {
        code.LocalGet(bits);
        code.I32Const(31);
        code.U8(Op::I32ShrU);
    };
    // bits += delta when the i32 condition on the stack holds
    auto adjust = [&](s32 delta) {
        code.If();
        code.LocalGet(bits);
        code.I32Const(delta);
        code.U8(Op::I32Add);
        code.LocalSet(bits);
        code.End();
    };

    switch (rounding) {
    case FP::RoundingMode::TowardsZero:
    case FP::RoundingMode::ToOdd:
        magnitude_cmp(Op::F64Gt);
        adjust(-1);
        if (rounding == FP::RoundingMode::ToOdd) {
            // inexact -> force the lowest mantissa bit
            // select(bits | 1, bits, inexact && !isnan(x))
            code.LocalGet(bits);
            code.I32Const(1);
            code.U8(Op::I32Or);
            code.LocalGet(bits);
            code.LocalGet(bits);
            code.U8(Op::F32ReinterpretI32);
            code.U8(Op::F64PromoteF32);
            code.LocalGet(x);
            code.U8(Op::F64Ne);
            code.LocalGet(x);
            code.LocalGet(x);
            code.U8(Op::F64Eq);  // not NaN
            code.U8(Op::I32And);
            code.U8(Op::Select);
            code.LocalSet(bits);
        }
        break;
    case FP::RoundingMode::TowardsPlusInfinity:
    case FP::RoundingMode::TowardsMinusInfinity: {
        // Towards +inf: grow the magnitude of positive results rounded down,
        // shrink negative results rounded up (towards -inf mirrored).
        const bool plus = rounding == FP::RoundingMode::TowardsPlusInfinity;
        magnitude_cmp(Op::F64Lt);
        push_negative();
        if (plus) {
            code.U8(Op::I32Eqz);
        }
        code.U8(Op::I32And);
        adjust(1);
        magnitude_cmp(Op::F64Gt);
        push_negative();
        if (!plus) {
            code.U8(Op::I32Eqz);
        }
        code.U8(Op::I32And);
        adjust(-1);
        break;
    }
    default:
        UNREACHABLE();
    }
    code.LocalGet(bits);
    code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// FP <-> fixed point
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPSingleToFixedS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, true, 16);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, true, 32);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedS64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, true, 64);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, false, 16);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, false, 32);
}

template<>
void EmitIR<IR::Opcode::FPSingleToFixedU64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, false, 64);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, true, true, 16);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, true, true, 32);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedS64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, true, true, 64);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, true, false, 16);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, true, false, 32);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToFixedU64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, true, false, 64);
}

template<>
void EmitIR<IR::Opcode::FPFixedS16ToSingle>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, false, true, 16);
}

template<>
void EmitIR<IR::Opcode::FPFixedS32ToSingle>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, false, true, 32);
}

template<>
void EmitIR<IR::Opcode::FPFixedS64ToSingle>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, false, true, 64);
}

template<>
void EmitIR<IR::Opcode::FPFixedU16ToSingle>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, false, false, 16);
}

template<>
void EmitIR<IR::Opcode::FPFixedU32ToSingle>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, false, false, 32);
}

template<>
void EmitIR<IR::Opcode::FPFixedU64ToSingle>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, false, false, 64);
}

template<>
void EmitIR<IR::Opcode::FPFixedS16ToDouble>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, true, true, 16);
}

template<>
void EmitIR<IR::Opcode::FPFixedS32ToDouble>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, true, true, 32);
}

template<>
void EmitIR<IR::Opcode::FPFixedS64ToDouble>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, true, true, 64);
}

template<>
void EmitIR<IR::Opcode::FPFixedU16ToDouble>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, true, false, 16);
}

template<>
void EmitIR<IR::Opcode::FPFixedU32ToDouble>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, true, false, 32);
}

template<>
void EmitIR<IR::Opcode::FPFixedU64ToDouble>(EmitContext& ctx, IR::Inst* inst) {
    EmitFixedToFP(ctx, inst, true, false, 64);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, true, 16, true);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, true, 32, true);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedS64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, true, 64, true);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, false, 16, true);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, false, 32, true);
}

template<>
void EmitIR<IR::Opcode::FPHalfToFixedU64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPToFixed(ctx, inst, false, false, 64, true);
}

// Half-precision conversions (VCVTB/VCVTT f16<->f32/f64) go through the
// reference FP::FPConvert implementation via a thunk; the thunk folds raised
// exception bits back into state->fpsr. The block's FPCR and the rounding
// mode immediate are baked in as constants (same as x64's fallback path).
void EmitFPHalfConvert(EmitContext& ctx, IR::Inst* inst, const char* name, bool wide_arg, bool wide_result) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const ValType arg_type = wide_arg ? ValType::I64 : ValType::I32;
    const ValType result_type = wide_result ? ValType::I64 : ValType::I32;
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, arg_type, ValType::I32, ValType::I32}, {result_type}});
    const u32 fn = ctx.Import(name, ty);
    EmitPushFpsrAddr(ctx);
    ctx.code.LocalGet(kParamEctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(ctx.fpcr));
    ctx.code.I32Const(static_cast<s32>(args[1].GetImmediateU8()));
    ctx.code.Call(fn);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPHalfToSingle>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPHalfConvert(ctx, inst, kImportFpHalfToSingle, false, false);
}

template<>
void EmitIR<IR::Opcode::FPSingleToHalf>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPHalfConvert(ctx, inst, kImportFpSingleToHalf, false, false);
}

template<>
void EmitIR<IR::Opcode::FPHalfToDouble>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPHalfConvert(ctx, inst, kImportFpHalfToDouble, false, true);
}

template<>
void EmitIR<IR::Opcode::FPDoubleToHalf>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPHalfConvert(ctx, inst, kImportFpDoubleToHalf, true, false);
}

}  // namespace Dynarmic::Backend::Wasm
