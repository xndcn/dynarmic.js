// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

// NEON floating-point vector ops (FPVector*32) for the WASM backend.
//
// U128 IR values live in v128 locals; f32 lane ops map directly onto the wasm
// fixed-width SIMD instruction set. Wasm f32x4 arithmetic is IEEE-754
// round-to-nearest-even with no exception flags, matching what the x64
// backend gets from SSE with default MXCSR.

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_fp_util.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/backend/wasm/wasm_binary.h"
#include "dynarmic/common/fp/rounding_mode.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {
namespace {

// ARMv7 NEON arithmetic runs under the "standard FPSCR" (IR fpcr_controlled
// argument false): flush-to-zero, default NaN, round to nearest. A64 / VFP
// follow FPCR.FZ. The fpcr_controlled flag is the last IR argument of the
// FPVector ops that have one.
bool FpcrControlled(IR::Inst* inst) {
    const IR::Value last = inst->GetArg(inst->NumArgs() - 1);
    return !(last.IsImmediate() && last.GetType() == IR::Type::U1 && !last.GetU1());
}

bool FlushToZero32(EmitContext& ctx, IR::Inst* inst) {
    return !FpcrControlled(inst) || (ctx.fpcr & (1u << 24)) != 0;
}

// FPCR value the reference thunks run under for this op.
s32 EffectiveFpcr(EmitContext& ctx, IR::Inst* inst) {
    if (FpcrControlled(inst)) {
        return static_cast<s32>(ctx.fpcr);
    }
    return static_cast<s32>((ctx.fpcr & 0x04000000) | 0x03000000);  // AHP kept, DN | FZ, RMode = nearest
}

// Flushes the f32 lanes of the v128 on the stack whose exponent is zero
// (denormals) to signed zeros.
void EmitFZ32(EmitContext& ctx) {
    FuncBody& code = ctx.code;
    const u32 t = ctx.Temp(ValType::V128);
    code.LocalTee(t);
    VLaneConst(ctx, 32, 0x80000000);
    code.Simd(Op::V128And);
    code.LocalGet(t);
    code.LocalGet(t);
    VLaneConst(ctx, 32, 0x7F800000);
    code.Simd(Op::V128And);
    VZero(ctx);
    code.Simd(Op::I32x4Eq);
    code.Simd(Op::V128Bitselect);
}

// Pushes a vector operand, flushed when `fz`.
void PushV(EmitContext& ctx, const RegAlloc::Argument& arg, bool fz) {
    EmitPush(ctx.code, ctx.reg_alloc, arg);
    if (fz) {
        EmitFZ32(ctx);
    }
}

// Lane-wise binary FP op; `fp_result` = the result is FP (flushed too).
void EmitVFpBinOp(EmitContext& ctx, IR::Inst* inst, u32 subop, bool fp_result = true) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool fz = FlushToZero32(ctx, inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    PushV(ctx, args[0], fz);
    PushV(ctx, args[1], fz);
    ctx.code.Simd(subop);
    if (fz && fp_result) {
        EmitFZ32(ctx);
    }
    ctx.code.LocalSet(dst);
}

// Unary vector op: dst = subop(args[0]).
void EmitVUnOp(EmitContext& ctx, IR::Inst* inst, u32 subop) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(subop);
    ctx.code.LocalSet(dst);
}

// 2^fbits as f32 raw bits (fbits <= 64); splatted per lane.
void EmitPow2Splat32(EmitContext& ctx, u8 fbits) {
    VLaneConst(ctx, 32, (u32(127) + fbits) << 23);
}

// Apply the rounding mode (an immediate, see FP::RoundingMode) lane-wise to
// the f32x4 on top of the stack.
void EmitRoundImmV32(EmitContext& ctx, FP::RoundingMode rounding) {
    FuncBody& code = ctx.code;
    switch (rounding) {
    case FP::RoundingMode::ToNearest_TieEven:
        code.Simd(Op::F32x4Nearest);
        return;
    case FP::RoundingMode::TowardsPlusInfinity:
        code.Simd(Op::F32x4Ceil);
        return;
    case FP::RoundingMode::TowardsMinusInfinity:
        code.Simd(Op::F32x4Floor);
        return;
    case FP::RoundingMode::TowardsZero:
        code.Simd(Op::F32x4Trunc);
        return;
    case FP::RoundingMode::ToNearest_TieAwayFromZero: {
        // r = trunc(x); if |x - r| >= 0.5 then r + copysign(1, x) else r
        const u32 x = ctx.Temp(ValType::V128);
        const u32 r = ctx.Temp(ValType::V128);
        code.LocalSet(x);
        code.LocalGet(x);
        code.Simd(Op::F32x4Trunc);
        code.LocalTee(r);
        // v1 = r + copysign(1.0f, x) = r + (0x3F800000 | (x & 0x80000000))
        code.LocalGet(x);
        VLaneConst(ctx, 32, 0x80000000);
        code.Simd(Op::V128And);
        VLaneConst(ctx, 32, 0x3F800000);
        code.Simd(Op::V128Or);
        code.Simd(Op::F32x4Add);
        // v2 = r
        code.LocalGet(r);
        // mask = |x - r| >= 0.5f
        code.LocalGet(x);
        code.LocalGet(r);
        code.Simd(Op::F32x4Sub);
        code.Simd(Op::F32x4Abs);
        VLaneConst(ctx, 32, 0x3F000000);
        code.Simd(Op::F32x4Ge);
        code.Simd(Op::V128Bitselect);
        return;
    }
    case FP::RoundingMode::ToOdd:
        // Only used by A64; not worth the bit games until needed.
        UNIMPLEMENTED();
    }
    UNREACHABLE();
}

// FMINNM/FMAXNM share one implementation for scalar and vector 32/64-bit
// values. NaN comparisons only produce masks; payload/sign selection and
// signaling classification use the original integer bits, independent of the
// host engine's permitted NaN canonicalization for arithmetic.
void EmitNumericMinMax(EmitContext& ctx, IR::Inst* inst, u32 esize, bool vector, bool is_max) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const bool wide = esize == 64;
    const u32 fpcr = vector ? static_cast<u32>(EffectiveFpcr(ctx, inst)) : ctx.fpcr;
    const bool fz = (fpcr & (1u << 24)) != 0;
    const bool dn = (fpcr & (1u << 25)) != 0;
    const ValType value_type = vector ? ValType::V128 : (wide ? ValType::I64 : ValType::I32);
    const ValType mask_type = vector ? ValType::V128 : ValType::I32;
    const u64 sign = u64{1} << (esize - 1);
    const u64 exponent = wide ? 0x7FF0000000000000 : 0x7F800000;
    const u64 quiet = wide ? u64{1} << 51 : u64{1} << 22;
    const u64 fraction = (quiet << 1) - 1;
    const u32 a = ctx.Temp(value_type), b = ctx.Temp(value_type);
    const u32 an = ctx.Temp(mask_type), bn = ctx.Temp(mask_type);
    const u32 as = ctx.Temp(mask_type), bs = ctx.Temp(mask_type);
    const u32 result = ctx.Temp(value_type);
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    auto constant = [&](u64 bits) {
        if (vector) VLaneConst(ctx, esize, bits);
        else if (wide) code.I64Const(static_cast<s64>(bits));
        else code.I32Const(static_cast<s32>(bits));
    };
    auto bit_and = [&] {
        if (vector) code.Simd(Op::V128And);
        else code.U8(wide ? Op::I64And : Op::I32And);
    };
    auto bit_or = [&] {
        if (vector) code.Simd(Op::V128Or);
        else code.U8(wide ? Op::I64Or : Op::I32Or);
    };
    auto mask_and = [&] { if (vector) code.Simd(Op::V128And); else code.U8(Op::I32And); };
    auto mask_or = [&] { if (vector) code.Simd(Op::V128Or); else code.U8(Op::I32Or); };
    auto mask_not = [&] { if (vector) code.Simd(Op::V128Not); else code.U8(Op::I32Eqz); };
    auto select = [&] { if (vector) code.Simd(Op::V128Bitselect); else code.U8(Op::Select); };
    auto is_zero = [&](u32 value, u64 mask) {
        code.LocalGet(value);
        constant(mask);
        bit_and();
        if (vector) {
            VZero(ctx);
            code.Simd(wide ? Op::I64x2Eq : Op::I32x4Eq);
        } else {
            code.U8(wide ? Op::I64Eqz : Op::I32Eqz);
        }
    };
    // Consume a condition (lane mask for SIMD) and set a cumulative FPSR bit.
    auto raise = [&](u32 flag) {
        if (vector) code.Simd(Op::V128AnyTrue);
        code.If();
        code.LocalGet(kParamState);
        code.LocalGet(kParamState);
        code.Load(Op::I32Load, ctx.state.fpsr, 2);
        code.I32Const(static_cast<s32>(flag));
        code.U8(Op::I32Or);
        code.Store(Op::I32Store, ctx.state.fpsr, 2);
        code.End();
    };
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(a);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.LocalSet(b);
    for (const u32 value : {a, b}) {
        if (!fz) continue;
        // A nonzero subnormal input raises IDC and becomes a signed zero.
        const u32 denormal = ctx.Temp(mask_type);
        is_zero(value, exponent);
        is_zero(value, fraction);
        mask_not();
        mask_and();
        code.LocalTee(denormal);
        raise(1u << 7);
        code.LocalGet(value);
        constant(sign);
        bit_and();
        code.LocalGet(value);
        code.LocalGet(denormal);
        select();
        code.LocalSet(value);
    }
    auto classify = [&](u32 value, u32 nan, u32 signaling) {
        code.LocalGet(value);
        if (!vector) code.U8(wide ? Op::F64ReinterpretI64 : Op::F32ReinterpretI32);
        code.LocalGet(value);
        if (!vector) code.U8(wide ? Op::F64ReinterpretI64 : Op::F32ReinterpretI32);
        if (vector) code.Simd(wide ? Op::F64x2Ne : Op::F32x4Ne);
        else code.U8(wide ? Op::F64Ne : Op::F32Ne);
        code.LocalTee(nan);
        is_zero(value, quiet);
        mask_and();
        code.LocalSet(signaling);
    };
    classify(a, an, as);
    classify(b, bn, bs);
    code.LocalGet(as);
    code.LocalGet(bs);
    mask_or();
    raise(1);

    // Numeric result, with quiet NaNs treated as missing operands. The
    // final NaN selection below overrides both-NaN and signaling-NaN cases.
    code.LocalGet(b);
    code.LocalGet(a);
    code.LocalGet(a);
    if (!vector) code.U8(wide ? Op::F64ReinterpretI64 : Op::F32ReinterpretI32);
    code.LocalGet(b);
    if (!vector) code.U8(wide ? Op::F64ReinterpretI64 : Op::F32ReinterpretI32);
    if (vector) code.Simd(wide ? (is_max ? Op::F64x2Max : Op::F64x2Min)
                              : (is_max ? Op::F32x4Max : Op::F32x4Min));
    else {
        code.U8(wide ? (is_max ? Op::F64Max : Op::F64Min) : (is_max ? Op::F32Max : Op::F32Min));
        code.U8(wide ? Op::I64ReinterpretF64 : Op::I32ReinterpretF32);
    }
    code.LocalGet(bn);
    select();
    code.LocalGet(an);
    select();
    code.LocalSet(result);

    if (dn) {
        constant(exponent | quiet);
    } else {
        // a signaling wins, then b signaling, then the first quiet NaN.
        code.LocalGet(a);
        code.LocalGet(b);
        code.LocalGet(as);
        code.LocalGet(bs);
        mask_not();
        mask_or();
        select();
        constant(quiet);
        bit_or();
    }
    code.LocalGet(result);
    code.LocalGet(as);
    code.LocalGet(bs);
    mask_or();
    code.LocalGet(an);
    code.LocalGet(bn);
    mask_and();
    mask_or();
    select();
    code.LocalSet(dst);
}

// FPVectorMulAdd32: fused op1*op2 + addend (IR operand order: addend, op1,
// op2). op1*op2 is exact in f64; the f64 sum is rounded to odd (TwoSum
// error, then the odd neighbour when inexact) so that the demotion to f32
// rounds correctly, as in the scalar FPMulAdd32 path.
void EmitVMulAdd32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool fz = FlushToZero32(ctx, inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    // Move lanes 2,3 into lanes 0,1 for the high half.
    constexpr std::array<u8, 16> hi_to_lo = {8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7};
    const u32 half[2] = {ctx.Temp(ValType::V128), ctx.Temp(ValType::V128)};
    const u32 p = ctx.Temp(ValType::V128);
    const u32 c = ctx.Temp(ValType::V128);
    const u32 sum = ctx.Temp(ValType::V128);
    const u32 bb = ctx.Temp(ValType::V128);
    const u32 err = ctx.Temp(ValType::V128);
    for (u32 h = 0; h < 2; h++) {
        PushV(ctx, args[1], fz);
        if (h != 0) {
            PushV(ctx, args[1], fz);
            code.Simd(Op::I8x16Shuffle);
            code.Bytes(hi_to_lo.data(), hi_to_lo.size());
        }
        code.Simd(Op::F64x2PromoteLowF32x4);
        PushV(ctx, args[2], fz);
        if (h != 0) {
            PushV(ctx, args[2], fz);
            code.Simd(Op::I8x16Shuffle);
            code.Bytes(hi_to_lo.data(), hi_to_lo.size());
        }
        code.Simd(Op::F64x2PromoteLowF32x4);
        code.Simd(Op::F64x2Mul);
        code.LocalSet(p);
        PushV(ctx, args[0], fz);
        if (h != 0) {
            PushV(ctx, args[0], fz);
            code.Simd(Op::I8x16Shuffle);
            code.Bytes(hi_to_lo.data(), hi_to_lo.size());
        }
        code.Simd(Op::F64x2PromoteLowF32x4);
        code.LocalSet(c);

        // TwoSum: sum = c + p, err = exact (c + p) - sum
        code.LocalGet(c);
        code.LocalGet(p);
        code.Simd(Op::F64x2Add);
        code.LocalTee(sum);
        code.LocalGet(c);
        code.Simd(Op::F64x2Sub);
        code.LocalSet(bb);
        code.LocalGet(c);
        code.LocalGet(sum);
        code.LocalGet(bb);
        code.Simd(Op::F64x2Sub);
        code.Simd(Op::F64x2Sub);
        code.LocalGet(p);
        code.LocalGet(bb);
        code.Simd(Op::F64x2Sub);
        code.Simd(Op::F64x2Add);
        code.LocalSet(err);

        // bits(sum) + (inexact && even ? (signs equal ? 1 : -1) : 0), lane-wise
        code.LocalGet(sum);
        VLaneConst(ctx, 64, 1);
        code.LocalGet(sum);
        code.LocalGet(err);
        code.Simd(Op::V128Xor);
        code.I32Const(63);
        code.Simd(Op::I64x2ShrU);
        code.I32Const(1);
        code.Simd(Op::I64x2Shl);
        code.Simd(Op::I64x2Sub);  // 1 - 2 * (signs differ)
        // 0 < |err| < inf (finite inputs; NaN / infinite sums pass through)
        code.LocalGet(err);
        code.Simd(Op::F64x2Abs);
        VZero(ctx);
        code.Simd(Op::F64x2Gt);
        code.Simd(Op::V128And);
        code.LocalGet(err);
        code.Simd(Op::F64x2Abs);
        VLaneConst(ctx, 64, 0x7FF0000000000000);
        code.Simd(Op::F64x2Lt);
        code.Simd(Op::V128And);
        code.LocalGet(sum);
        VLaneConst(ctx, 64, 1);
        code.Simd(Op::V128And);
        VZero(ctx);
        code.Simd(Op::I64x2Eq);
        code.Simd(Op::V128And);
        code.Simd(Op::I64x2Add);
        code.Simd(Op::F32x4DemoteF64x2Zero);  // lanes 0,1 valid; 2,3 zero
        code.LocalSet(half[h]);
    }
    // result = [lo0, lo1, hi0, hi1]
    constexpr std::array<u8, 16> combine = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
    code.LocalGet(half[0]);
    code.LocalGet(half[1]);
    code.Simd(Op::I8x16Shuffle);
    code.Bytes(combine.data(), combine.size());
    if (fz) {
        EmitFZ32(ctx);
    }
    code.LocalSet(dst);
}

// FPVectorPairedAdd32 (vpadd.f32 Q): [a0+a1, a2+a3, b0+b1, b2+b3]
// FPVectorPairedAddLower32 (vpadd.f32 D): [a0+a1, b0+b1, 0, 0]
void EmitVPairedAdd32(EmitContext& ctx, IR::Inst* inst, bool lower_only) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool fz = FlushToZero32(ctx, inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const std::array<u8, 16> even = lower_only
        ? std::array<u8, 16>{0, 1, 2, 3, 16, 17, 18, 19, 8, 9, 10, 11, 24, 25, 26, 27}     // [a0, b0, ...]
        : std::array<u8, 16>{0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27};   // [a0, a2, b0, b2]
    const std::array<u8, 16> odd = lower_only
        ? std::array<u8, 16>{4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28, 29, 30, 31}  // [a1, b1, ...]
        : std::array<u8, 16>{4, 5, 6, 7, 12, 13, 14, 15, 20, 21, 22, 23, 28, 29, 30, 31}; // [a1, a3, b1, b3]
    const u32 tx = ctx.Temp(ValType::V128);
    PushV(ctx, args[0], fz);
    PushV(ctx, args[1], fz);
    code.Simd(Op::I8x16Shuffle);
    code.Bytes(even.data(), even.size());
    code.LocalSet(tx);
    PushV(ctx, args[0], fz);
    PushV(ctx, args[1], fz);
    code.Simd(Op::I8x16Shuffle);
    code.Bytes(odd.data(), odd.size());
    code.LocalGet(tx);
    code.Simd(Op::F32x4Add);
    if (lower_only) {
        VConst(ctx, kLow64Mask);
        code.Simd(Op::V128And);
    }
    if (fz) {
        EmitFZ32(ctx);
    }
    code.LocalSet(dst);
}

// FPVectorRoundInt32: round each lane to an integral value, staying in f32.
void EmitVRoundInt32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool fz = FlushToZero32(ctx, inst);
    ASSERT(args[1].IsImmediate());
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    PushV(ctx, args[0], fz);
    EmitRoundImmV32(ctx, static_cast<FP::RoundingMode>(args[1].GetImmediateU8()));
    if (fz) {
        EmitFZ32(ctx);
    }
    ctx.code.LocalSet(dst);
}

// FPVectorToSignedFixed32 / FPVectorToUnsignedFixed32 (vcvt.s32/u32.f32):
// lane = saturate(round(value * 2^fbits)); wasm trunc_sat saturates exactly
// the way ARM's FPToFixed does.
void EmitVToFixed32(EmitContext& ctx, IR::Inst* inst, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool fz = FlushToZero32(ctx, inst);
    ASSERT(args[1].IsImmediate() && args[2].IsImmediate());
    const u8 fbits = args[1].GetImmediateU8();
    const auto rounding = static_cast<FP::RoundingMode>(args[2].GetImmediateU8());
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    PushV(ctx, args[0], fz);
    if (fbits != 0) {
        EmitPow2Splat32(ctx, fbits);
        code.Simd(Op::F32x4Mul);
    }
    EmitRoundImmV32(ctx, rounding);
    code.Simd(is_signed ? Op::I32x4TruncSatF32x4S : Op::I32x4TruncSatF32x4U);
    code.LocalSet(dst);
}

// FPVectorFromSignedFixed32 / FPVectorFromUnsignedFixed32 (vcvt.f32.s32/u32):
// lane = float(value) / 2^fbits (exact power-of-two scale).
void EmitVFromFixed32(EmitContext& ctx, IR::Inst* inst, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u8 fbits = args[1].GetImmediateU8();
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.Simd(is_signed ? Op::F32x4ConvertI32x4S : Op::F32x4ConvertI32x4U);
    if (fbits != 0) {
        EmitPow2Splat32(ctx, fbits);
        code.Simd(Op::F32x4Div);
    }
    code.LocalSet(dst);
}

// Per-lane thunk call, one vector argument (u32 lanes) + baked-in FPCR:
// sig (fpsr_addr, ectx, value, fpcr) -> u32. Used for FPVectorRecipEstimate32 /
// FPVectorRSqrtEstimate32, which need the reference FP:: LUT implementation.
void EmitVThunk1(EmitContext& ctx, IR::Inst* inst, const char* name) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32}, {ValType::I32}});
    const u32 fn = ctx.Import(name, ty);
    const s32 fpcr = EffectiveFpcr(ctx, inst);
    VZero(ctx);
    for (u32 lane = 0; lane < 4; lane++) {
        EmitPushFpsrAddr(ctx);
        code.LocalGet(kParamEctx);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.Simd(Op::I32x4ExtractLane);
        code.U8(static_cast<u8>(lane));
        code.I32Const(fpcr);
        code.Call(fn);
        code.Simd(Op::I32x4ReplaceLane);
        code.U8(static_cast<u8>(lane));
    }
    code.LocalSet(dst);
}

// Per-lane thunk call, two vector arguments + baked-in FPCR:
// sig (fpsr_addr, ectx, a, b, fpcr) -> u32. Used for FPVectorRecipStepFused32 /
// FPVectorRSqrtStepFused32.
void EmitVThunk2(EmitContext& ctx, IR::Inst* inst, const char* name) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32}, {ValType::I32}});
    const u32 fn = ctx.Import(name, ty);
    const s32 fpcr = EffectiveFpcr(ctx, inst);
    VZero(ctx);
    for (u32 lane = 0; lane < 4; lane++) {
        EmitPushFpsrAddr(ctx);
        code.LocalGet(kParamEctx);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.Simd(Op::I32x4ExtractLane);
        code.U8(static_cast<u8>(lane));
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.Simd(Op::I32x4ExtractLane);
        code.U8(static_cast<u8>(lane));
        code.I32Const(fpcr);
        code.Call(fn);
        code.Simd(Op::I32x4ReplaceLane);
        code.U8(static_cast<u8>(lane));
    }
    code.LocalSet(dst);
}

// FPVectorToHalf32 (vcvt.f16.f32): 4 x f32 lanes -> 4 x f16 in the low 64
// bits, upper 64 zeroed. Per-lane reuse of the scalar fp_s2h thunk (which
// folds raised FPSR bits back into state).
void EmitVToHalf32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32}, {ValType::I32}});
    const u32 fn = ctx.Import(kImportFpSingleToHalf, ty);
    const s32 fpcr = EffectiveFpcr(ctx, inst);
    const s32 rounding = static_cast<s32>(args[1].GetImmediateU8());
    VZero(ctx);
    for (u32 lane = 0; lane < 4; lane++) {
        EmitPushFpsrAddr(ctx);
        code.LocalGet(kParamEctx);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.Simd(Op::I32x4ExtractLane);
        code.U8(static_cast<u8>(lane));
        code.I32Const(fpcr);
        code.I32Const(rounding);
        code.Call(fn);
        code.Simd(Op::I16x8ReplaceLane);
        code.U8(static_cast<u8>(lane));
    }
    code.LocalSet(dst);
}

// FPVectorFromHalf32 (vcvt.f32.f16): low 4 x f16 lanes -> 4 x f32 lanes.
// Per-lane reuse of the scalar fp_h2s thunk.
void EmitVFromHalf32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32}, {ValType::I32}});
    const u32 fn = ctx.Import(kImportFpHalfToSingle, ty);
    const s32 fpcr = EffectiveFpcr(ctx, inst);
    const s32 rounding = static_cast<s32>(args[1].GetImmediateU8());
    VZero(ctx);
    for (u32 lane = 0; lane < 4; lane++) {
        EmitPushFpsrAddr(ctx);
        code.LocalGet(kParamEctx);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.Simd(Op::I16x8ExtractLaneU);
        code.U8(static_cast<u8>(lane));
        code.I32Const(fpcr);
        code.I32Const(rounding);
        code.Call(fn);
        code.Simd(Op::I32x4ReplaceLane);
        code.U8(static_cast<u8>(lane));
    }
    code.LocalSet(dst);
}

}  // anonymous namespace

void EmitFPMinMaxNumericOp(EmitContext& ctx, IR::Inst* inst, u32 esize, bool vector, bool is_max) {
    EmitNumericMinMax(ctx, inst, esize, vector, is_max);
}

// ---------------------------------------------------------------------------
// Lane-wise direct ops
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPVectorAbs32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::F32x4Abs);
}

template<>
void EmitIR<IR::Opcode::FPVectorNeg32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::F32x4Neg);
}

template<>
void EmitIR<IR::Opcode::FPVectorSqrt32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool fz = FlushToZero32(ctx, inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    PushV(ctx, args[0], fz);
    ctx.code.Simd(Op::F32x4Sqrt);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::FPVectorAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Add);
}

template<>
void EmitIR<IR::Opcode::FPVectorSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Sub);
}

template<>
void EmitIR<IR::Opcode::FPVectorMul32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Mul);
}

template<>
void EmitIR<IR::Opcode::FPVectorDiv32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Div);
}

// Compares produce all-ones lane masks, matching vceq/vcgt/vcge.f32.
template<>
void EmitIR<IR::Opcode::FPVectorEqual32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Eq, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorGreater32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Gt, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorGreaterEqual32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Ge, false);
}

// vmin/vmax.f32: wasm f32x4.min/max match ARM FPMin/FPMax (NaN in -> NaN out,
// min(+0,-0) = -0, max(+0,-0) = +0); the NaN payload may differ from ARM's
// ProcessNaN, same caveat as the x64 backend's vminps/vmaxps.
template<>
void EmitIR<IR::Opcode::FPVectorMin32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Min);
}

template<>
void EmitIR<IR::Opcode::FPVectorMax32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFpBinOp(ctx, inst, Op::F32x4Max);
}

template<>
void EmitIR<IR::Opcode::FPVectorMinNumeric32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, 32, true, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorMaxNumeric32>(EmitContext& ctx, IR::Inst* inst) {
    EmitFPMinMaxNumericOp(ctx, inst, 32, true, true);
}

// ---------------------------------------------------------------------------
// Composite ops
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPVectorPairedAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVPairedAdd32(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorPairedAddLower32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVPairedAdd32(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::FPVectorMulAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVMulAdd32(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPVectorRoundInt32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRoundInt32(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPVectorToSignedFixed32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVToFixed32(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::FPVectorToUnsignedFixed32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVToFixed32(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorFromSignedFixed32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFromFixed32(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::FPVectorFromUnsignedFixed32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFromFixed32(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::FPVectorToHalf32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVToHalf32(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::FPVectorFromHalf32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVFromHalf32(ctx, inst);
}

// ---------------------------------------------------------------------------
// Reference-implementation thunks
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::FPVectorRecipEstimate32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVThunk1(ctx, inst, kImportFpVRecipEstimate32);
}

template<>
void EmitIR<IR::Opcode::FPVectorRSqrtEstimate32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVThunk1(ctx, inst, kImportFpVRSqrtEstimate32);
}

template<>
void EmitIR<IR::Opcode::FPVectorRecipStepFused32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVThunk2(ctx, inst, kImportFpVRecipStep32);
}

template<>
void EmitIR<IR::Opcode::FPVectorRSqrtStepFused32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVThunk2(ctx, inst, kImportFpVRSqrtStep32);
}

}  // namespace Dynarmic::Backend::Wasm
