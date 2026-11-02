// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Vector ops tranche 3 for the WASM backend: 32-bit saturating add/sub,
// saturating abs/neg, saturating shifts (VQSHL register/immediate forms,
// VQSHLU), rounding variable shifts (VRSHL), saturating doubling multiplies
// (VQDMULH/VQRDMULH/VQDMULL), polynomial multiply (VMUL.P8/VMULL.P8), and
// reciprocal estimates (VRECPE/VRSQRTE u32, via host LUT thunks).
//
// Lane-wise shift counts in WASM SIMD are scalar, so per-lane variable
// shifts use a barrel shifter: for each stage bit k, shift by k and
// bitselect on (tabs & k). 64-bit lanes lack unsigned compares and min/max,
// so those use GtS + bitselect idioms instead.

#include <array>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

struct LaneOps {
    u32 eq, gt_s, gt_u, min_s, max_s, abs_, neg, add, sub, shl, shr_s, shr_u;
};

LaneOps LaneOpsFor(u32 esize) {
    switch (esize) {
    case 8:
        return {Op::I8x16Eq, Op::I8x16GtS, Op::I8x16GtU, Op::I8x16MinS, Op::I8x16MaxS,
                Op::I8x16Abs, Op::I8x16Neg, Op::I8x16Add, Op::I8x16Sub, Op::I8x16Shl,
                Op::I8x16ShrS, Op::I8x16ShrU};
    case 16:
        return {Op::I16x8Eq, Op::I16x8GtS, Op::I16x8GtU, Op::I16x8MinS, Op::I16x8MaxS,
                Op::I16x8Abs, Op::I16x8Neg, Op::I16x8Add, Op::I16x8Sub, Op::I16x8Shl,
                Op::I16x8ShrS, Op::I16x8ShrU};
    case 32:
        return {Op::I32x4Eq, Op::I32x4GtS, Op::I32x4GtU, Op::I32x4MinS, Op::I32x4MaxS,
                Op::I32x4Abs, Op::I32x4Neg, Op::I32x4Add, Op::I32x4Sub, Op::I32x4Shl,
                Op::I32x4ShrS, Op::I32x4ShrU};
    case 64:
        return {Op::I64x2Eq, Op::I64x2GtS, 0, 0, 0,
                Op::I64x2Abs, Op::I64x2Neg, Op::I64x2Add, Op::I64x2Sub, Op::I64x2Shl,
                Op::I64x2ShrS, Op::I64x2ShrU};
    default:
        UNREACHABLE();
    }
}

// min/max of the two v128 on top of the stack ([a, b] -> min(a, b)).
// 64-bit lanes have no min/max: min = bitselect(a, b, b > a).
void EmitMinS(EmitContext& ctx, const LaneOps& ops, u32 esize) {
    if (esize != 64) {
        ctx.code.Simd(ops.min_s);
        return;
    }
    const u32 a = ctx.Temp(ValType::V128);
    const u32 b = ctx.Temp(ValType::V128);
    ctx.code.LocalSet(b);
    ctx.code.LocalSet(a);
    ctx.code.LocalGet(a);
    ctx.code.LocalGet(b);
    ctx.code.LocalGet(b);
    ctx.code.LocalGet(a);
    ctx.code.Simd(ops.gt_s);
    ctx.code.Simd(Op::V128Bitselect);
}

// [a, b] -> max(a, b); 64-bit: max = bitselect(a, b, a > b).
void EmitMaxS(EmitContext& ctx, const LaneOps& ops, u32 esize) {
    if (esize != 64) {
        ctx.code.Simd(ops.max_s);
        return;
    }
    const u32 a = ctx.Temp(ValType::V128);
    const u32 b = ctx.Temp(ValType::V128);
    ctx.code.LocalSet(b);
    ctx.code.LocalSet(a);
    ctx.code.LocalGet(a);
    ctx.code.LocalGet(b);
    ctx.code.LocalGet(a);
    ctx.code.LocalGet(b);
    ctx.code.Simd(ops.gt_s);
    ctx.code.Simd(Op::V128Bitselect);
}

// Sign-extend the low byte of each lane of the v128 on top of the stack
// (shift-amount semantics use only the sign-extended low byte).
void EmitSignExtendAmounts(EmitContext& ctx, const LaneOps& ops, u32 esize) {
    if (esize == 8) {
        return;
    }
    ctx.code.I32Const(static_cast<s32>(esize - 8));
    ctx.code.Simd(ops.shl);
    ctx.code.I32Const(static_cast<s32>(esize - 8));
    ctx.code.Simd(ops.shr_s);
}

// Barrel-shift each lane of local `value` by the per-lane unsigned magnitude
// in local `tabs` (must be pre-clamped to [0, esize-1]), in place.
void EmitBarrelStages(EmitContext& ctx, const LaneOps& ops, u32 esize, u32 shift_subop, u32 value, u32 tabs) {
    for (u32 k = 1; k < esize; k <<= 1) {
        ctx.code.LocalGet(value);
        ctx.code.I32Const(static_cast<s32>(k));
        ctx.code.Simd(shift_subop);
        ctx.code.LocalGet(value);
        ctx.code.LocalGet(tabs);
        VLaneConst(ctx, esize, k);
        ctx.code.Simd(Op::V128And);
        VLaneConst(ctx, esize, k);
        ctx.code.Simd(ops.eq);
        ctx.code.Simd(Op::V128Bitselect);
        ctx.code.LocalSet(value);
    }
}

// ---------------------------------------------------------------------------
// 32-bit saturating add/sub (8/16-bit lanes have native AddSat/SubSat, done
// in tranche 2; 64-bit lanes are A64-only).
// ---------------------------------------------------------------------------

void EmitSatAddSub32(EmitContext& ctx, IR::Inst* inst, bool is_sub, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps ops = LaneOpsFor(32);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    const u32 ts = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(is_sub ? ops.sub : ops.add);
    ctx.code.LocalSet(ts);

    if (is_signed) {
        // ov = ((a ^ s) & (b ^ s)) >>s 31  (add);  ((a ^ b) & (a ^ s)) >>s 31  (sub)
        if (is_sub) {
            EmitPush(ctx.code, ctx.reg_alloc, args[0]);
            EmitPush(ctx.code, ctx.reg_alloc, args[1]);
            ctx.code.Simd(Op::V128Xor);
            EmitPush(ctx.code, ctx.reg_alloc, args[0]);
            ctx.code.LocalGet(ts);
            ctx.code.Simd(Op::V128Xor);
        } else {
            EmitPush(ctx.code, ctx.reg_alloc, args[0]);
            ctx.code.LocalGet(ts);
            ctx.code.Simd(Op::V128Xor);
            EmitPush(ctx.code, ctx.reg_alloc, args[1]);
            ctx.code.LocalGet(ts);
            ctx.code.Simd(Op::V128Xor);
        }
        ctx.code.Simd(Op::V128And);
        ctx.code.I32Const(31);
        ctx.code.Simd(ops.shr_s);
        // cv = a >= 0 ? MAX : MIN
        const u32 ov = ctx.Temp(ValType::V128);
        ctx.code.LocalSet(ov);
        VLaneConst(ctx, 32, 0x7FFFFFFF);
        VLaneConst(ctx, 32, 0x80000000);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        VLaneConst(ctx, 32, 0xFFFFFFFF);
        ctx.code.Simd(ops.gt_s);  // a > -1
        ctx.code.Simd(Op::V128Bitselect);
        // r = ov ? cv : s
        ctx.code.LocalGet(ts);
        ctx.code.LocalGet(ov);
        ctx.code.Simd(Op::V128Bitselect);
    } else if (is_sub) {
        // borrow = b >u a; r = s & ~borrow
        ctx.code.LocalGet(ts);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.Simd(ops.gt_u);
        ctx.code.Simd(Op::V128AndNot);
    } else {
        // carry = a >u s; r = carry ? UMAX : s
        VLaneConst(ctx, 32, 0xFFFFFFFF);
        ctx.code.LocalGet(ts);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.LocalGet(ts);
        ctx.code.Simd(ops.gt_u);
        ctx.code.Simd(Op::V128Bitselect);
    }
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Saturating abs/neg: r = (a == MIN) ? MAX : abs/neg(a)
// ---------------------------------------------------------------------------

void EmitQAbsNeg(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_neg) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps ops = LaneOpsFor(esize);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u64 min_val = u64{1} << (esize - 1);
    const u64 max_val = (u64{1} << (esize - 1)) - 1;

    VLaneConst(ctx, esize, max_val);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(is_neg ? ops.neg : ops.abs_);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    VLaneConst(ctx, esize, min_val);
    ctx.code.Simd(ops.eq);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// VQSHL (register): saturating left shift with per-lane signed amounts.
//   x == 0            -> 0
//   amt < 0           -> x >> min(-amt, W-1)   (arithmetic/logical, no sat)
//   amt >= W          -> saturate(x)
//   else              -> x << amt, saturate if bits were lost
// saturate(x) = (u(x) >> (W-1)) + MAX  (== MIN when x < 0, MAX otherwise);
// unsigned saturates to UMAX instead.
// ---------------------------------------------------------------------------

void EmitQShlReg(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps ops = LaneOpsFor(esize);
    const u32 W = esize;

    const u32 x = ctx.Temp(ValType::V128);
    const u32 tamt = ctx.Temp(ValType::V128);
    const u32 neg = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(x);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitSignExtendAmounts(ctx, ops, esize);
    ctx.code.LocalSet(tamt);
    VZero(ctx);
    ctx.code.LocalGet(tamt);
    ctx.code.Simd(ops.gt_s);
    ctx.code.LocalSet(neg);

    // Left path: aL = clamp(tamt, 0, W-1); xl = x << aL; round-trip check.
    const u32 aL = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(tamt);
    VZero(ctx);
    EmitMaxS(ctx, ops, esize);
    VLaneConst(ctx, esize, W - 1);
    EmitMinS(ctx, ops, esize);
    ctx.code.LocalSet(aL);

    const u32 xl = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(x);
    ctx.code.LocalSet(xl);
    EmitBarrelStages(ctx, ops, esize, ops.shl, xl, aL);

    const u32 rt = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(xl);
    ctx.code.LocalSet(rt);
    EmitBarrelStages(ctx, ops, esize, is_signed ? ops.shr_s : ops.shr_u, rt, aL);

    // satm = (tamt > W-1 | rt != x) & x != 0 & !neg
    const u32 satm = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(tamt);
    VLaneConst(ctx, esize, W - 1);
    ctx.code.Simd(ops.gt_s);
    ctx.code.LocalGet(rt);
    ctx.code.LocalGet(x);
    ctx.code.Simd(ops.eq);
    ctx.code.Simd(Op::V128Not);
    ctx.code.Simd(Op::V128Or);
    ctx.code.LocalGet(x);
    VZero(ctx);
    ctx.code.Simd(ops.eq);
    ctx.code.Simd(Op::V128Not);
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalGet(neg);
    ctx.code.Simd(Op::V128AndNot);
    ctx.code.LocalSet(satm);
    EmitSetQcIfAny(ctx, satm);

    // satv
    const u32 satv = ctx.Temp(ValType::V128);
    if (is_signed) {
        ctx.code.LocalGet(x);
        ctx.code.I32Const(static_cast<s32>(W - 1));
        ctx.code.Simd(ops.shr_u);
        VLaneConst(ctx, esize, (u64{1} << (W - 1)) - 1);
        ctx.code.Simd(ops.add);
    } else {
        VLaneConst(ctx, esize, ~u64{0});
    }
    ctx.code.LocalSet(satv);

    // rleft = satm ? satv : xl
    const u32 rleft = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(satv);
    ctx.code.LocalGet(xl);
    ctx.code.LocalGet(satm);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalSet(rleft);

    // Right path: aR = -max(tamt, -(W-1)); xr = x >> aR.
    const u32 aR = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(tamt);
    VLaneConst(ctx, esize, static_cast<u64>(-static_cast<s64>(W - 1)));
    EmitMaxS(ctx, ops, esize);
    ctx.code.Simd(ops.neg);
    ctx.code.LocalSet(aR);

    const u32 xr = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(x);
    ctx.code.LocalSet(xr);
    EmitBarrelStages(ctx, ops, esize, is_signed ? ops.shr_s : ops.shr_u, xr, aR);

    if (!is_signed) {
        // amt <= -W yields 0: killR = -(W-1) > tamt
        const u32 kill = ctx.Temp(ValType::V128);
        VLaneConst(ctx, esize, static_cast<u64>(-static_cast<s64>(W - 1)));
        ctx.code.LocalGet(tamt);
        ctx.code.Simd(ops.gt_s);
        ctx.code.LocalSet(kill);
        ctx.code.LocalGet(xr);
        ctx.code.LocalGet(kill);
        ctx.code.Simd(Op::V128AndNot);
        ctx.code.LocalSet(xr);
    }

    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.LocalGet(xr);
    ctx.code.LocalGet(rleft);
    ctx.code.LocalGet(neg);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// VQSHLU (immediate): unsigned-saturate(signed x << imm); negative x -> 0.
// ---------------------------------------------------------------------------

void EmitQShlUImm(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps ops = LaneOpsFor(esize);
    const u8 imm = args[1].GetImmediateU8();
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    const u32 x = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(x);

    const u32 shifted = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(x);
    ctx.code.I32Const(imm);
    ctx.code.Simd(ops.shl);
    ctx.code.LocalSet(shifted);

    // neg = 0 > x (x < 0)
    const u32 neg = ctx.Temp(ValType::V128);
    VZero(ctx);
    ctx.code.LocalGet(x);
    ctx.code.Simd(ops.gt_s);
    ctx.code.LocalSet(neg);

    // satm = (shifted >>u imm != x) & !neg
    const u32 satm = ctx.Temp(ValType::V128);
    const u32 neg_or_sat = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(shifted);
    ctx.code.I32Const(imm);
    ctx.code.Simd(ops.shr_u);
    ctx.code.LocalGet(x);
    ctx.code.Simd(ops.eq);
    ctx.code.Simd(Op::V128Not);
    ctx.code.LocalGet(neg);
    ctx.code.Simd(Op::V128AndNot);
    ctx.code.LocalSet(satm);
    // QC: any lane that saturated, including negative inputs clamped to 0
    ctx.code.LocalGet(satm);
    ctx.code.LocalGet(neg);
    ctx.code.Simd(Op::V128Or);
    ctx.code.LocalSet(neg_or_sat);
    EmitSetQcIfAny(ctx, neg_or_sat);

    VLaneConst(ctx, esize, ~u64{0});
    ctx.code.LocalGet(shifted);
    ctx.code.LocalGet(satm);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalGet(neg);
    ctx.code.Simd(Op::V128AndNot);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// VRSHL: rounding variable shift.
//   amt >= 0: amt >= W -> 0 else x << amt
//   amt < 0:  (x >> -amt) + round_bit, where round_bit = bit (-amt-1) of x;
//             signed: amt <= -W -> 0; unsigned: amt < -W -> 0,
//             amt == -W -> x >> (W-1) (the MSB as 0/1)
// ---------------------------------------------------------------------------

void EmitVRShl(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps ops = LaneOpsFor(esize);
    const u32 W = esize;

    const u32 x = ctx.Temp(ValType::V128);
    const u32 tamt = ctx.Temp(ValType::V128);
    const u32 neg = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(x);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitSignExtendAmounts(ctx, ops, esize);
    ctx.code.LocalSet(tamt);
    VZero(ctx);
    ctx.code.LocalGet(tamt);
    ctx.code.Simd(ops.gt_s);
    ctx.code.LocalSet(neg);

    // aC = min(|tamt|, W-1); |tamt| wraps only for tamt == -128/W == 8, which
    // the kill masks discard anyway.
    const u32 aC = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(tamt);
    ctx.code.Simd(ops.abs_);
    VLaneConst(ctx, esize, W - 1);
    EmitMinS(ctx, ops, esize);
    ctx.code.LocalSet(aC);

    const u32 xl = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(x);
    ctx.code.LocalSet(xl);
    EmitBarrelStages(ctx, ops, esize, ops.shl, xl, aC);

    const u32 rr = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(x);
    ctx.code.LocalSet(rr);
    EmitBarrelStages(ctx, ops, esize, is_signed ? ops.shr_s : ops.shr_u, rr, aC);

    // round bit: rb = ((x << 1) >>u aC) & 1
    const u32 rb = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(x);
    ctx.code.I32Const(1);
    ctx.code.Simd(ops.shl);
    ctx.code.LocalSet(rb);
    EmitBarrelStages(ctx, ops, esize, ops.shr_u, rb, aC);
    ctx.code.LocalGet(rb);
    VLaneConst(ctx, esize, 1);
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalSet(rb);

    const u32 gr = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(rr);
    ctx.code.LocalGet(rb);
    ctx.code.Simd(ops.add);
    ctx.code.LocalSet(gr);

    if (!is_signed) {
        // amt == -W -> x >> (W-1)
        const u32 eqW = ctx.Temp(ValType::V128);
        ctx.code.LocalGet(tamt);
        VLaneConst(ctx, esize, static_cast<u64>(-static_cast<s64>(W)));
        ctx.code.Simd(ops.eq);
        ctx.code.LocalSet(eqW);
        ctx.code.LocalGet(x);
        ctx.code.I32Const(static_cast<s32>(W - 1));
        ctx.code.Simd(ops.shr_u);
        ctx.code.LocalGet(gr);
        ctx.code.LocalGet(eqW);
        ctx.code.Simd(Op::V128Bitselect);
        ctx.code.LocalSet(gr);
    }

    // kill = tamt > W-1 | (signed ? -(W-1) > tamt : -W > tamt)
    const u32 kill = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(tamt);
    VLaneConst(ctx, esize, W - 1);
    ctx.code.Simd(ops.gt_s);
    VLaneConst(ctx, esize, static_cast<u64>(-static_cast<s64>(is_signed ? W - 1 : W)));
    ctx.code.LocalGet(tamt);
    ctx.code.Simd(ops.gt_s);
    ctx.code.Simd(Op::V128Or);
    ctx.code.LocalSet(kill);

    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.LocalGet(gr);
    ctx.code.LocalGet(xl);
    ctx.code.LocalGet(neg);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalGet(kill);
    ctx.code.Simd(Op::V128AndNot);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// VQDMULH/VQRDMULH: r = sat((2ab [+ 2^(W-1)]) >> W) computed in wide lanes as
// t = (p [+ 2^(W-2)]) >>s (W-1), p = a*b; saturation iff p == 2^(2W-2)
// (a == b == MIN), result clamped to MAX.
// ---------------------------------------------------------------------------

void EmitDblMulHigh(EmitContext& ctx, IR::Inst* inst, u32 esize, bool rounding) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 W = esize;
    const LaneOps wops = LaneOpsFor(2 * W);
    const u32 lb = W / 8;  // result lane bytes

    const u32 plo = ctx.Temp(ValType::V128);
    const u32 phi = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(W == 16 ? Op::I32x4ExtMulLowI16x8S : Op::I64x2ExtMulLowI32x4S);
    ctx.code.LocalSet(plo);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(W == 16 ? Op::I32x4ExtMulHighI16x8S : Op::I64x2ExtMulHighI32x4S);
    ctx.code.LocalSet(phi);

    // Saturation masks before rounding perturbs p.
    const u32 mlo = ctx.Temp(ValType::V128);
    const u32 mhi = ctx.Temp(ValType::V128);
    const u64 sat_p = u64{1} << (2 * W - 2);
    ctx.code.LocalGet(plo);
    VLaneConst(ctx, 2 * W, sat_p);
    ctx.code.Simd(wops.eq);
    ctx.code.LocalSet(mlo);
    ctx.code.LocalGet(phi);
    VLaneConst(ctx, 2 * W, sat_p);
    ctx.code.Simd(wops.eq);
    ctx.code.LocalSet(mhi);

    if (rounding) {
        for (u32 p : {plo, phi}) {
            ctx.code.LocalGet(p);
            VLaneConst(ctx, 2 * W, u64{1} << (W - 2));
            ctx.code.Simd(wops.add);
            ctx.code.LocalSet(p);
        }
    }

    const u32 tlo = ctx.Temp(ValType::V128);
    const u32 thi = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(plo);
    ctx.code.I32Const(static_cast<s32>(W - 1));
    ctx.code.Simd(wops.shr_s);
    ctx.code.LocalSet(tlo);
    ctx.code.LocalGet(phi);
    ctx.code.I32Const(static_cast<s32>(W - 1));
    ctx.code.Simd(wops.shr_s);
    ctx.code.LocalSet(thi);

    // Truncate wide lanes to their low halves: [lo lanes of tlo, lo of thi].
    std::array<u8, 16> pattern{};
    const u32 half = 16 / (2 * lb);
    for (u32 i = 0; i < half; i++) {
        for (u32 b = 0; b < lb; b++) {
            pattern[i * lb + b] = static_cast<u8>(i * 2 * lb + b);
            pattern[(half + i) * lb + b] = static_cast<u8>(16 + i * 2 * lb + b);
        }
    }

    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    // mask via same truncation
    const u32 mask = ctx.Temp(ValType::V128);
    ctx.code.LocalGet(mlo);
    ctx.code.LocalGet(mhi);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(pattern.data(), pattern.size());
    ctx.code.LocalSet(mask);

    VLaneConst(ctx, W, (u64{1} << (W - 1)) - 1);
    ctx.code.LocalGet(tlo);
    ctx.code.LocalGet(thi);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(pattern.data(), pattern.size());
    ctx.code.LocalGet(mask);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalSet(dst);
    EmitSetQcIfAny(ctx, mask);
}

// ---------------------------------------------------------------------------
// VQDMULL: wide result r = 2*a*b (low halves of inputs), saturated at MIN
// (only a == b == MIN can produce it) clamped to MAX via xor.
// ---------------------------------------------------------------------------

void EmitDblMulLong(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps wops = LaneOpsFor(2 * esize);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    const u32 p = ctx.Temp(ValType::V128);
    const u32 saturated = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(esize == 16 ? Op::I32x4ExtMulLowI16x8S : Op::I64x2ExtMulLowI32x4S);
    ctx.code.LocalSet(p);

    ctx.code.LocalGet(p);
    ctx.code.I32Const(1);
    ctx.code.Simd(wops.shl);
    ctx.code.LocalSet(p);

    ctx.code.LocalGet(p);
    ctx.code.LocalGet(p);
    VLaneConst(ctx, 2 * esize, u64{1} << (2 * esize - 1));
    ctx.code.Simd(wops.eq);
    ctx.code.LocalTee(saturated);
    ctx.code.Simd(Op::V128Xor);
    ctx.code.LocalSet(dst);
    // Only the input's low half participates in a widening multiply. Reuse
    // the actual product mask so unused upper lanes cannot spuriously set QC.
    EmitSetQcIfAny(ctx, saturated);
}

// ---------------------------------------------------------------------------
// VMUL.P8 / VMULL.P8: carryless (polynomial) multiply, 8 masked-shift terms.
// ---------------------------------------------------------------------------

void EmitPolyMul8(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps ops = LaneOpsFor(8);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    const u32 ta = ctx.Temp(ValType::V128);
    const u32 tb = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);

    const u32 res = ctx.Temp(ValType::V128);
    VZero(ctx);
    ctx.code.LocalSet(res);
    for (u32 i = 0; i < 8; i++) {
        ctx.code.LocalGet(res);
        ctx.code.LocalGet(tb);
        ctx.code.I32Const(static_cast<s32>(i));
        ctx.code.Simd(ops.shl);
        ctx.code.LocalGet(ta);
        VLaneConst(ctx, 8, u64{1} << i);
        ctx.code.Simd(Op::V128And);
        VLaneConst(ctx, 8, u64{1} << i);
        ctx.code.Simd(ops.eq);
        ctx.code.Simd(Op::V128And);
        ctx.code.Simd(Op::V128Xor);
        ctx.code.LocalSet(res);
    }
    ctx.code.LocalGet(res);
    ctx.code.LocalSet(dst);
}

void EmitPolyMulLong8(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const LaneOps ops = LaneOpsFor(16);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    const u32 ta = ctx.Temp(ValType::V128);
    const u32 tb = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I16x8ExtendLowI8x16U);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I16x8ExtendLowI8x16U);
    ctx.code.LocalSet(tb);

    const u32 res = ctx.Temp(ValType::V128);
    VZero(ctx);
    ctx.code.LocalSet(res);
    for (u32 i = 0; i < 8; i++) {
        ctx.code.LocalGet(res);
        ctx.code.LocalGet(tb);
        ctx.code.I32Const(static_cast<s32>(i));
        ctx.code.Simd(ops.shl);
        ctx.code.LocalGet(ta);
        VLaneConst(ctx, 16, u64{1} << i);
        ctx.code.Simd(Op::V128And);
        VLaneConst(ctx, 16, u64{1} << i);
        ctx.code.Simd(ops.eq);
        ctx.code.Simd(Op::V128And);
        ctx.code.Simd(Op::V128Xor);
        ctx.code.LocalSet(res);
    }
    ctx.code.LocalGet(res);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// VRECPE/VRSQRTE .u32: per-lane host LUT thunk (matches x64's fallback).
// ---------------------------------------------------------------------------

void EmitRecipEstimate(EmitContext& ctx, IR::Inst* inst, const char* name) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32}, {ValType::I32}});
    const u32 fn = ctx.Import(name, ty);

    VZero(ctx);
    for (u32 lane = 0; lane < 4; lane++) {
        ctx.code.LocalGet(kParamState);
        ctx.code.LocalGet(kParamEctx);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.Simd(Op::I32x4ExtractLane);
        ctx.code.U8(static_cast<u8>(lane));
        ctx.code.Call(fn);
        ctx.code.Simd(Op::I32x4ReplaceLane);
        ctx.code.U8(static_cast<u8>(lane));
    }
    ctx.code.LocalSet(dst);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Saturating add/sub 32
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSatAddSub32(ctx, inst, false, true);
    EmitQcCheck(ctx, inst, QcKind::Add, 32);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSatAddSub32(ctx, inst, true, true);
    EmitQcCheck(ctx, inst, QcKind::Sub, 32);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSatAddSub32(ctx, inst, false, false);
    EmitQcCheck(ctx, inst, QcKind::Add, 32);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSatAddSub32(ctx, inst, true, false);
    EmitQcCheck(ctx, inst, QcKind::Sub, 32);
}

// ---------------------------------------------------------------------------
// Saturating abs/neg
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAbs8>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 8, false);
    EmitQcCheck(ctx, inst, QcKind::Abs, 8);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAbs16>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 16, false);
    EmitQcCheck(ctx, inst, QcKind::Abs, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAbs32>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 32, false);
    EmitQcCheck(ctx, inst, QcKind::Abs, 32);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAbs64>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 64, false);
    EmitQcCheck(ctx, inst, QcKind::Abs, 64);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNeg8>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 8, true);
    EmitQcCheck(ctx, inst, QcKind::Neg, 8);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNeg16>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 16, true);
    EmitQcCheck(ctx, inst, QcKind::Neg, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNeg32>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 32, true);
    EmitQcCheck(ctx, inst, QcKind::Neg, 32);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNeg64>(EmitContext& ctx, IR::Inst* inst) {
    EmitQAbsNeg(ctx, inst, 64, true);
    EmitQcCheck(ctx, inst, QcKind::Neg, 64);
}

// ---------------------------------------------------------------------------
// Saturating shifts
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeft8>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeft16>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeft32>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeft64>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 64, true);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedShiftLeft8>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 8, false);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedShiftLeft16>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedShiftLeft32>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedShiftLeft64>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlReg(ctx, inst, 64, false);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeftUnsigned8>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlUImm(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeftUnsigned16>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlUImm(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeftUnsigned32>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlUImm(ctx, inst, 32);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedShiftLeftUnsigned64>(EmitContext& ctx, IR::Inst* inst) {
    EmitQShlUImm(ctx, inst, 64);
}

// ---------------------------------------------------------------------------
// Rounding variable shifts
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftS64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 64, true);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 8, false);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingShiftLeftU64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVRShl(ctx, inst, 64, false);
}

// ---------------------------------------------------------------------------
// Saturating doubling multiplies
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedDoublingMultiplyHigh16>(EmitContext& ctx, IR::Inst* inst) {
    EmitDblMulHigh(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedDoublingMultiplyHigh32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDblMulHigh(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding16>(EmitContext& ctx, IR::Inst* inst) {
    EmitDblMulHigh(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDblMulHigh(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedDoublingMultiplyLong16>(EmitContext& ctx, IR::Inst* inst) {
    EmitDblMulLong(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedDoublingMultiplyLong32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDblMulLong(ctx, inst, 32);
}

// ---------------------------------------------------------------------------
// Polynomial multiply
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::VectorPolynomialMultiply8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPolyMul8(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::VectorPolynomialMultiplyLong8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPolyMulLong8(ctx, inst);
}

// ---------------------------------------------------------------------------
// Reciprocal estimates
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::VectorUnsignedRecipEstimate>(EmitContext& ctx, IR::Inst* inst) {
    EmitRecipEstimate(ctx, inst, kImportRecipEstimate);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedRecipSqrtEstimate>(EmitContext& ctx, IR::Inst* inst) {
    EmitRecipEstimate(ctx, inst, kImportRecipSqrtEstimate);
}

}  // namespace Dynarmic::Backend::Wasm
