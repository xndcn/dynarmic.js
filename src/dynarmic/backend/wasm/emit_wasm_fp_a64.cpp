// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Floating-point opcodes only the A64 frontend emits: f64x2 vector
// arithmetic, FMULX, fused multiply-add on doubles, and the operations that
// go through the reference FP:: implementation (fp_ref thunk): estimates,
// Newton-Raphson steps, FRECPX and every half-precision arithmetic op.
//
// IR FP values are bit patterns (i32 / i64 locals, v128 vectors); they are
// reinterpreted at the wasm float ops.

#include <array>
#include <functional>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_fp_util.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"
#include "dynarmic/common/fp/rounding_mode.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// --- fp_ref thunk ----------------------------------------------------------

// Calls fp_ref with up to three operands; `push_operand(i)` pushes operand i
// as an i64 bit pattern. Leaves the i64 result on the stack.
template<typename F>
void EmitFpRefCall(EmitContext& ctx, FpRefOp op, u32 esize, u32 imm, size_t num_operands, F&& push_operand) {
    FuncBody& code = ctx.code;
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I64, ValType::I64, ValType::I64}, {ValType::I64}});
    const u32 fn = ctx.Import(kImportFpRef, ty);
    EmitPushFpsrAddr(ctx);
    code.LocalGet(kParamEctx);
    code.I32Const(static_cast<s32>(static_cast<u32>(op) | (esize << 8)));
    code.I32Const(static_cast<s32>(imm));
    code.I32Const(static_cast<s32>(ctx.fpcr));
    for (size_t i = 0; i < 3; i++) {
        if (i < num_operands) {
            push_operand(i);
        } else {
            code.I64Const(0);
        }
    }
    code.Call(fn);
}

// Scalar op through fp_ref: operands args[0..num_operands).
void EmitScalarFpRef(EmitContext& ctx, IR::Inst* inst, FpRefOp op, u32 esize, size_t num_operands, u32 imm = 0) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitFpRefCall(ctx, op, esize, imm, num_operands, [&](size_t i) {
        EmitPush(ctx.code, ctx.reg_alloc, args[i]);
        if (esize != 64) {
            ctx.code.U8(Op::I64ExtendI32U);
        }
    });
    if (esize != 64) {
        ctx.code.U8(Op::I32WrapI64);
    }
    ctx.code.LocalSet(dst);
}

// Vector op through fp_ref, one call per lane: operands args[0..num_operands).
void EmitVectorFpRef(EmitContext& ctx, IR::Inst* inst, FpRefOp op, u32 esize, size_t num_operands, u32 imm = 0) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    std::array<u32, 3> operand{};
    for (size_t i = 0; i < num_operands; i++) {
        operand[i] = ctx.Temp(ValType::V128);
        EmitPush(code, ctx.reg_alloc, args[i]);
        code.LocalSet(operand[i]);
    }
    const u32 lanes = 128 / esize;
    VZero(ctx);
    for (u32 lane = 0; lane < lanes; lane++) {
        EmitFpRefCall(ctx, op, esize, imm, num_operands, [&](size_t i) {
            code.LocalGet(operand[i]);
            switch (esize) {
            case 16:
                code.Simd(Op::I16x8ExtractLaneU);
                code.U8(static_cast<u8>(lane));
                code.U8(Op::I64ExtendI32U);
                break;
            case 32:
                code.Simd(Op::I32x4ExtractLane);
                code.U8(static_cast<u8>(lane));
                code.U8(Op::I64ExtendI32U);
                break;
            default:
                code.Simd(Op::I64x2ExtractLane);
                code.U8(static_cast<u8>(lane));
                break;
            }
        });
        switch (esize) {
        case 16:
            code.U8(Op::I32WrapI64);
            code.Simd(Op::I16x8ReplaceLane);
            break;
        case 32:
            code.U8(Op::I32WrapI64);
            code.Simd(Op::I32x4ReplaceLane);
            break;
        default:
            code.Simd(Op::I64x2ReplaceLane);
            break;
        }
        code.U8(static_cast<u8>(lane));
    }
    code.LocalSet(dst);
}

// --- FMULX -----------------------------------------------------------------

// FMULX = FMUL, except (+-0) * (+-inf) = +-2.0 (sign = sign(a) ^ sign(b)).
template<bool is_f64>
void EmitMulXScalar(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const ValType it = is_f64 ? ValType::I64 : ValType::I32;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 a = ctx.Temp(it);
    const u32 b = ctx.Temp(it);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(a);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.LocalSet(b);

    auto push_abs = [&](u32 v) {
        code.LocalGet(v);
        if (is_f64) {
            code.I64Const(0x7FFF'FFFF'FFFF'FFFF);
            code.U8(Op::I64And);
        } else {
            code.I32Const(0x7FFF'FFFF);
            code.U8(Op::I32And);
        }
    };
    auto push_is_zero = [&](u32 v) {
        push_abs(v);
        code.U8(is_f64 ? Op::I64Eqz : Op::I32Eqz);
    };
    auto push_is_inf = [&](u32 v) {
        push_abs(v);
        if (is_f64) {
            code.I64Const(0x7FF0'0000'0000'0000);
            code.U8(Op::I64Eq);
        } else {
            code.I32Const(0x7F80'0000);
            code.U8(Op::I32Eq);
        }
    };

    // select(two, product, special)
    code.LocalGet(a);
    code.LocalGet(b);
    if (is_f64) {
        code.U8(Op::I64Xor);
        code.I64Const(static_cast<s64>(0x8000'0000'0000'0000));
        code.U8(Op::I64And);
        code.I64Const(0x4000'0000'0000'0000);
        code.U8(Op::I64Or);
    } else {
        code.U8(Op::I32Xor);
        code.I32Const(static_cast<s32>(0x8000'0000));
        code.U8(Op::I32And);
        code.I32Const(0x4000'0000);
        code.U8(Op::I32Or);
    }
    code.LocalGet(a);
    code.U8(is_f64 ? Op::F64ReinterpretI64 : Op::F32ReinterpretI32);
    code.LocalGet(b);
    code.U8(is_f64 ? Op::F64ReinterpretI64 : Op::F32ReinterpretI32);
    code.U8(is_f64 ? Op::F64Mul : Op::F32Mul);
    code.U8(is_f64 ? Op::I64ReinterpretF64 : Op::I32ReinterpretF32);
    push_is_zero(a);
    push_is_inf(b);
    code.U8(Op::I32And);
    push_is_inf(a);
    push_is_zero(b);
    code.U8(Op::I32And);
    code.U8(Op::I32Or);
    code.U8(Op::Select);
    code.LocalSet(dst);
}

template<u32 esize>
void EmitMulXVector(EmitContext& ctx, IR::Inst* inst) {
    static_assert(esize == 32 || esize == 64);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 a = ctx.Temp(ValType::V128);
    const u32 b = ctx.Temp(ValType::V128);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(a);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.LocalSet(b);

    const u64 abs_mask = esize == 64 ? 0x7FFF'FFFF'FFFF'FFFF : 0x7FFF'FFFF;
    const u64 inf = esize == 64 ? 0x7FF0'0000'0000'0000 : 0x7F80'0000;
    const u64 sign = esize == 64 ? 0x8000'0000'0000'0000 : 0x8000'0000;
    const u64 two = esize == 64 ? 0x4000'0000'0000'0000 : 0x4000'0000;
    const u32 eq = esize == 64 ? Op::I64x2Eq : Op::I32x4Eq;
    auto push_cmp_abs = [&](u32 v, u64 value) {
        code.LocalGet(v);
        VLaneConst(ctx, esize, abs_mask);
        code.Simd(Op::V128And);
        VLaneConst(ctx, esize, value);
        code.Simd(eq);
    };

    // two = ((a ^ b) & sign) | 2.0
    code.LocalGet(a);
    code.LocalGet(b);
    code.Simd(Op::V128Xor);
    VLaneConst(ctx, esize, sign);
    code.Simd(Op::V128And);
    VLaneConst(ctx, esize, two);
    code.Simd(Op::V128Or);
    // product
    code.LocalGet(a);
    code.LocalGet(b);
    code.Simd(esize == 64 ? Op::F64x2Mul : Op::F32x4Mul);
    // special = (a == 0 & b == inf) | (a == inf & b == 0)
    push_cmp_abs(a, 0);
    push_cmp_abs(b, inf);
    code.Simd(Op::V128And);
    push_cmp_abs(a, inf);
    push_cmp_abs(b, 0);
    code.Simd(Op::V128And);
    code.Simd(Op::V128Or);
    code.Simd(Op::V128Bitselect);
    code.LocalSet(dst);
}

// --- f64x2 helpers -----------------------------------------------------------

void EmitV64Binary(EmitContext& ctx, IR::Inst* inst, u32 subop) {
    EmitVBinOp(ctx, inst, subop);
}

void EmitV64Unary(EmitContext& ctx, IR::Inst* inst, u32 subop) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(subop);
    ctx.code.LocalSet(dst);
}

// dst = args[0] op mask (bit manipulation of the sign bits)
void EmitVSignOp(EmitContext& ctx, IR::Inst* inst, u32 esize, u32 subop, u64 mask) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    VLaneConst(ctx, esize, mask);
    ctx.code.Simd(subop);
    ctx.code.LocalSet(dst);
}

// Rounds the f64x2 on top of the stack to integral values.
void EmitRoundImmV64(EmitContext& ctx, FP::RoundingMode rounding) {
    FuncBody& code = ctx.code;
    switch (rounding) {
    case FP::RoundingMode::ToNearest_TieEven:
        code.Simd(Op::F64x2Nearest);
        return;
    case FP::RoundingMode::TowardsPlusInfinity:
        code.Simd(Op::F64x2Ceil);
        return;
    case FP::RoundingMode::TowardsMinusInfinity:
        code.Simd(Op::F64x2Floor);
        return;
    case FP::RoundingMode::TowardsZero:
        code.Simd(Op::F64x2Trunc);
        return;
    case FP::RoundingMode::ToNearest_TieAwayFromZero: {
        // r = trunc(x); |x - r| >= 0.5 ? r + copysign(1, x) : r
        const u32 x = ctx.Temp(ValType::V128);
        const u32 r = ctx.Temp(ValType::V128);
        code.LocalSet(x);
        code.LocalGet(x);
        code.Simd(Op::F64x2Trunc);
        code.LocalTee(r);
        code.LocalGet(x);
        VLaneConst(ctx, 64, 0x8000'0000'0000'0000);
        code.Simd(Op::V128And);
        VLaneConst(ctx, 64, 0x3FF0'0000'0000'0000);
        code.Simd(Op::V128Or);
        code.Simd(Op::F64x2Add);
        code.LocalGet(r);
        code.LocalGet(x);
        code.LocalGet(r);
        code.Simd(Op::F64x2Sub);
        code.Simd(Op::F64x2Abs);
        VLaneConst(ctx, 64, 0x3FE0'0000'0000'0000);
        code.Simd(Op::F64x2Ge);
        code.Simd(Op::V128Bitselect);
        return;
    }
    case FP::RoundingMode::ToOdd:
        UNIMPLEMENTED();
    }
    UNREACHABLE();
}

u64 Pow2F64(u32 exponent_offset, bool negative) {
    const s32 e = 1023 + (negative ? -static_cast<s32>(exponent_offset) : static_cast<s32>(exponent_offset));
    return static_cast<u64>(e) << 52;
}

// Calls fp_fma64(op1, op2, addend), with op1 negated for the subtracting form.
void EmitFma64Call(EmitContext& ctx, const std::function<void()>& push_addend, const std::function<void()>& push_op1,
                   const std::function<void()>& push_op2, bool negate_product) {
    FuncBody& code = ctx.code;
    const u32 fn = ctx.Import(kImportFpFma64, ctx.Type({{ValType::F64, ValType::F64, ValType::F64}, {ValType::F64}}));
    push_op1();
    if (negate_product) {
        code.U8(Op::F64Neg);
    }
    push_op2();
    push_addend();
    code.Call(fn);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Scalar
// ---------------------------------------------------------------------------

void EmitFPMulAdd64Fused(EmitContext& ctx, IR::Inst* inst, bool is_sub) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    auto push = [&](size_t i) {
        return [&ctx, &args, i] {
            EmitPush(ctx.code, ctx.reg_alloc, args[i]);
            ctx.code.U8(Op::F64ReinterpretI64);
        };
    };
    EmitFma64Call(ctx, push(0), push(1), push(2), is_sub);
    ctx.code.U8(Op::I64ReinterpretF64);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPMulAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitScalarFpRef(ctx, inst, FpRefOp::MulAdd, 16, 3);
}

template<>
void EmitIR<IR::Opcode::FPMulSub16>(EmitContext& ctx, IR::Inst* inst) {
    EmitScalarFpRef(ctx, inst, FpRefOp::MulSub, 16, 3);
}

template<>
void EmitIR<IR::Opcode::FPMulX32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMulXScalar<false>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPMulX64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMulXScalar<true>(ctx, inst);
}

#define FP_REF_SCALAR(name, op, esize, n)                                \
    template<>                                                           \
    void EmitIR<IR::Opcode::name>(EmitContext & ctx, IR::Inst * inst) {  \
        EmitScalarFpRef(ctx, inst, FpRefOp::op, esize, n);               \
    }

FP_REF_SCALAR(FPRecipEstimate16, RecipEstimate, 16, 1)
FP_REF_SCALAR(FPRecipEstimate32, RecipEstimate, 32, 1)
FP_REF_SCALAR(FPRecipEstimate64, RecipEstimate, 64, 1)
FP_REF_SCALAR(FPRSqrtEstimate16, RSqrtEstimate, 16, 1)
FP_REF_SCALAR(FPRSqrtEstimate32, RSqrtEstimate, 32, 1)
FP_REF_SCALAR(FPRSqrtEstimate64, RSqrtEstimate, 64, 1)
FP_REF_SCALAR(FPRecipExponent16, RecipExponent, 16, 1)
FP_REF_SCALAR(FPRecipExponent32, RecipExponent, 32, 1)
FP_REF_SCALAR(FPRecipExponent64, RecipExponent, 64, 1)
FP_REF_SCALAR(FPRecipStepFused16, RecipStepFused, 16, 2)
FP_REF_SCALAR(FPRecipStepFused32, RecipStepFused, 32, 2)
FP_REF_SCALAR(FPRecipStepFused64, RecipStepFused, 64, 2)
FP_REF_SCALAR(FPRSqrtStepFused16, RSqrtStepFused, 16, 2)
FP_REF_SCALAR(FPRSqrtStepFused32, RSqrtStepFused, 32, 2)
FP_REF_SCALAR(FPRSqrtStepFused64, RSqrtStepFused, 64, 2)

#undef FP_REF_SCALAR

template<>
void EmitIR<IR::Opcode::FPRoundInt16>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate() && args[2].IsImmediate());
    const u32 imm = args[1].GetImmediateU8() | (u32{args[2].GetImmediateU1()} << 8);
    EmitScalarFpRef(ctx, inst, FpRefOp::RoundInt, 16, 1, imm);
}

// ---------------------------------------------------------------------------
// Vector, 16-bit lanes (all through fp_ref except sign manipulation)
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPVectorAbs16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVSignOp(ctx, inst, 16, Op::V128And, 0x7FFF);
}

template<>
void EmitIR<IR::Opcode::FPVectorNeg16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVSignOp(ctx, inst, 16, Op::V128Xor, 0x8000);
}

template<>
void EmitIR<IR::Opcode::FPVectorEqual16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::CompareEqual, 16, 2);
}

template<>
void EmitIR<IR::Opcode::FPVectorMulAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::MulAdd, 16, 3);
}

template<>
void EmitIR<IR::Opcode::FPVectorRecipEstimate16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RecipEstimate, 16, 1);
}

template<>
void EmitIR<IR::Opcode::FPVectorRSqrtEstimate16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RSqrtEstimate, 16, 1);
}

template<>
void EmitIR<IR::Opcode::FPVectorRecipStepFused16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RecipStepFused, 16, 2);
}

template<>
void EmitIR<IR::Opcode::FPVectorRSqrtStepFused16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RSqrtStepFused, 16, 2);
}

template<>
void EmitIR<IR::Opcode::FPVectorRoundInt16>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate() && args[2].IsImmediate());
    const u32 imm = args[1].GetImmediateU8() | (u32{args[2].GetImmediateU1()} << 8);
    EmitVectorFpRef(ctx, inst, FpRefOp::RoundInt, 16, 1, imm);
}

static void EmitVToFixed16(EmitContext& ctx, IR::Inst* inst, bool is_unsigned) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate() && args[2].IsImmediate());
    const u32 imm = args[1].GetImmediateU8() | (u32{args[2].GetImmediateU8()} << 8) | (16u << 16) | (u32{is_unsigned} << 24);
    EmitVectorFpRef(ctx, inst, FpRefOp::ToFixed, 16, 1, imm);
}

template<>
void EmitIR<IR::Opcode::FPVectorToSignedFixed16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVToFixed16(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorToUnsignedFixed16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVToFixed16(ctx, inst, true);
}

// ---------------------------------------------------------------------------
// Vector, 32-bit lanes
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPVectorMulX32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMulXVector<32>(ctx, inst);
}

// ---------------------------------------------------------------------------
// Vector, 64-bit lanes
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPVectorAbs64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVSignOp(ctx, inst, 64, Op::V128And, 0x7FFF'FFFF'FFFF'FFFF);
}

template<>
void EmitIR<IR::Opcode::FPVectorNeg64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVSignOp(ctx, inst, 64, Op::V128Xor, 0x8000'0000'0000'0000);
}

template<>
void EmitIR<IR::Opcode::FPVectorAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Add);
}

template<>
void EmitIR<IR::Opcode::FPVectorSub64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Sub);
}

template<>
void EmitIR<IR::Opcode::FPVectorMul64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Mul);
}

template<>
void EmitIR<IR::Opcode::FPVectorDiv64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Div);
}

template<>
void EmitIR<IR::Opcode::FPVectorSqrt64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Unary(ctx, inst, Op::F64x2Sqrt);
}

template<>
void EmitIR<IR::Opcode::FPVectorMax64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Max);
}

template<>
void EmitIR<IR::Opcode::FPVectorMin64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Min);
}

template<>
void EmitIR<IR::Opcode::FPVectorMaxNumeric64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, 64, true, true);
}

template<>
void EmitIR<IR::Opcode::FPVectorMinNumeric64>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, 64, true, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorEqual64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Eq);
}

template<>
void EmitIR<IR::Opcode::FPVectorGreater64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Gt);
}

template<>
void EmitIR<IR::Opcode::FPVectorGreaterEqual64>(EmitContext& ctx, IR::Inst* inst) {
    EmitV64Binary(ctx, inst, Op::F64x2Ge);
}

template<>
void EmitIR<IR::Opcode::FPVectorMulX64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMulXVector<64>(ctx, inst);
}

// FADDP .2d: [a0 + a1, b0 + b1]
template<>
void EmitIR<IR::Opcode::FPVectorPairedAdd64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    constexpr std::array<u8, 16> even = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
    constexpr std::array<u8, 16> odd = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31};
    for (const auto& pattern : {even, odd}) {
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.Simd(Op::I8x16Shuffle);
        code.Bytes(pattern.data(), pattern.size());
    }
    code.Simd(Op::F64x2Add);
    code.LocalSet(dst);
}

// FADDP (scalar, .2d): [a0 + b0, 0]
template<>
void EmitIR<IR::Opcode::FPVectorPairedAddLower64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    EmitPush(code, ctx.reg_alloc, args[0]);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.Simd(Op::F64x2Add);
    VConst(ctx, kLow64Mask);
    code.Simd(Op::V128And);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPVectorMulAdd64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    VZero(ctx);
    for (u8 lane = 0; lane < 2; lane++) {
        auto push = [&ctx, &args, lane](size_t i) {
            return [&ctx, &args, lane, i] {
                EmitPush(ctx.code, ctx.reg_alloc, args[i]);
                ctx.code.Simd(Op::F64x2ExtractLane);
                ctx.code.U8(lane);
            };
        };
        EmitFma64Call(ctx, push(0), push(1), push(2), false);
        code.Simd(Op::F64x2ReplaceLane);
        code.U8(lane);
    }
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPVectorRoundInt64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitRoundImmV64(ctx, static_cast<FP::RoundingMode>(args[1].GetImmediateU8()));
    ctx.code.LocalSet(dst);
}

// lane = saturate(round(value * 2^fbits)); i64.trunc_sat saturates the way
// ARM's FPToFixed does (NaN -> 0).
static void EmitVToFixed64(EmitContext& ctx, IR::Inst* inst, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate() && args[2].IsImmediate());
    const u8 fbits = args[1].GetImmediateU8();
    const auto rounding = static_cast<FP::RoundingMode>(args[2].GetImmediateU8());
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 v = ctx.Temp(ValType::V128);
    EmitPush(code, ctx.reg_alloc, args[0]);
    if (fbits != 0) {
        VLaneConst(ctx, 64, Pow2F64(fbits, false));
        code.Simd(Op::F64x2Mul);
    }
    EmitRoundImmV64(ctx, rounding);
    code.LocalSet(v);
    VZero(ctx);
    for (u8 lane = 0; lane < 2; lane++) {
        code.LocalGet(v);
        code.Simd(Op::F64x2ExtractLane);
        code.U8(lane);
        code.Misc(is_signed ? Op::I64TruncSatF64S : Op::I64TruncSatF64U);
        code.Simd(Op::I64x2ReplaceLane);
        code.U8(lane);
    }
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPVectorToSignedFixed64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVToFixed64(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::FPVectorToUnsignedFixed64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVToFixed64(ctx, inst, false);
}

// lane = double(value) / 2^fbits (exact power-of-two scale)
static void EmitVFromFixed64(EmitContext& ctx, IR::Inst* inst, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u8 fbits = args[1].GetImmediateU8();
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 v = ctx.Temp(ValType::V128);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(v);
    VZero(ctx);
    for (u8 lane = 0; lane < 2; lane++) {
        code.LocalGet(v);
        code.Simd(Op::I64x2ExtractLane);
        code.U8(lane);
        code.U8(is_signed ? Op::F64ConvertI64S : Op::F64ConvertI64U);
        code.Simd(Op::F64x2ReplaceLane);
        code.U8(lane);
    }
    if (fbits != 0) {
        VLaneConst(ctx, 64, Pow2F64(fbits, true));
        code.Simd(Op::F64x2Mul);
    }
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPVectorFromSignedFixed64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFromFixed64(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::FPVectorFromUnsignedFixed64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFromFixed64(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorRecipEstimate64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RecipEstimate, 64, 1);
}

template<>
void EmitIR<IR::Opcode::FPVectorRSqrtEstimate64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RSqrtEstimate, 64, 1);
}

template<>
void EmitIR<IR::Opcode::FPVectorRecipStepFused64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RecipStepFused, 64, 2);
}

template<>
void EmitIR<IR::Opcode::FPVectorRSqrtStepFused64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVectorFpRef(ctx, inst, FpRefOp::RSqrtStepFused, 64, 2);
}

}  // namespace Dynarmic::Backend::Wasm
