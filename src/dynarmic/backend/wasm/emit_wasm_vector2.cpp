// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Vector ops tranche 2 for the WASM backend: shuffle family (deinterleave,
// interleave, transpose, extract, reverse), saturating arithmetic, absolute
// difference, saturating narrow, count-leading-zeros, variable shifts
// (barrel shifter via bitselect), and VTBL/VTBX via i8x16.swizzle.

#include <algorithm>
#include <array>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// [even/odd lanes of a, even/odd lanes of b].
void EmitDeinterleave(EmitContext& ctx, IR::Inst* inst, u32 esize, bool odd) {
    const u32 lane_bytes = esize / 8;
    const u32 lanes = 16 / lane_bytes;
    std::array<u8, 16> pattern{};
    for (u32 i = 0; i < lanes / 2; i++) {
        SetLane(pattern, i * lane_bytes, 0, 2 * i + odd, lane_bytes);
        SetLane(pattern, (lanes / 2 + i) * lane_bytes, 1, 2 * i + odd, lane_bytes);
    }
    EmitShuffle2(ctx, inst, pattern);
}

// D-register VUZP: even/odd lanes of the low halves of a and b, upper zeroed.
void EmitDeinterleaveLower(EmitContext& ctx, IR::Inst* inst, u32 esize, bool odd) {
    const u32 lane_bytes = esize / 8;
    const u32 lanes = 16 / lane_bytes;
    std::array<u8, 16> pattern{};
    for (u32 i = 0; i < lanes / 4; i++) {
        SetLane(pattern, i * lane_bytes, 0, 2 * i + odd, lane_bytes);
        SetLane(pattern, (lanes / 4 + i) * lane_bytes, 1, 2 * i + odd, lane_bytes);
    }
    // upper half of the pattern is garbage; the mask zeroes it
    EmitShuffle2Masked(ctx, inst, pattern, kLow64Mask);
}

// [a_i, b_i, ...] from the low (upper=false) or high (upper=true) halves.
void EmitInterleave(EmitContext& ctx, IR::Inst* inst, u32 esize, bool upper) {
    const u32 lane_bytes = esize / 8;
    const u32 lanes = 16 / lane_bytes;
    const u32 first = upper ? lanes / 2 : 0;
    std::array<u8, 16> pattern{};
    for (u32 i = 0; i < lanes / 2; i++) {
        SetLane(pattern, 2 * i * lane_bytes, 0, first + i, lane_bytes);
        SetLane(pattern, (2 * i + 1) * lane_bytes, 1, first + i, lane_bytes);
    }
    EmitShuffle2(ctx, inst, pattern);
}

// VTRN: part 0 interleaves even lanes, part 1 interleaves odd lanes.
void EmitTranspose(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[2].IsImmediate());
    const u32 part = args[2].GetImmediateU1() ? 1 : 0;
    const u32 lane_bytes = esize / 8;
    const u32 lanes = 16 / lane_bytes;
    std::array<u8, 16> pattern{};
    for (u32 i = 0; i < lanes / 2; i++) {
        SetLane(pattern, 2 * i * lane_bytes, 0, 2 * i + part, lane_bytes);
        SetLane(pattern, (2 * i + 1) * lane_bytes, 1, 2 * i + part, lane_bytes);
    }
    EmitShuffle2(ctx, inst, pattern);
}

// Reverse elements of elem_bytes within groups of group_bytes (VREV16/32/64).
void EmitReverseElements(EmitContext& ctx, IR::Inst* inst, u32 elem_bytes, u32 group_bytes) {
    std::array<u8, 16> pattern{};
    for (u32 p = 0; p < 16; p++) {
        const u32 group = p / group_bytes;
        const u32 elem = (p % group_bytes) / elem_bytes;
        const u32 byte = p % elem_bytes;
        const u32 src_elem = group_bytes / elem_bytes - 1 - elem;
        pattern[p] = static_cast<u8>(group * group_bytes + src_elem * elem_bytes + byte);
    }
    EmitShuffle1(ctx, inst, pattern);
}

// |a-b| per lane, signed inputs, via widening (the result can exceed the
// signed range: e.g. s8 abs-diff can be 255).
void EmitAbsDiffSigned(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tlo = ctx.Temp(ValType::V128);
    const u32 thi = ctx.Temp(ValType::V128);
    u32 ext_lo, ext_hi, sub_op, abs_op;
    std::array<u8, 16> trunc_pattern{};
    switch (esize) {
    case 8:
        ext_lo = Op::I16x8ExtendLowI8x16S; ext_hi = Op::I16x8ExtendHighI8x16S;
        sub_op = Op::I16x8Sub; abs_op = Op::I16x8Abs;
        trunc_pattern = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30};
        break;
    case 16:
        ext_lo = Op::I32x4ExtendLowI16x8S; ext_hi = Op::I32x4ExtendHighI16x8S;
        sub_op = Op::I32x4Sub; abs_op = Op::I32x4Abs;
        trunc_pattern = {0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29};
        break;
    default:
        ASSERT(esize == 32);
        ext_lo = Op::I64x2ExtendLowI32x4S; ext_hi = Op::I64x2ExtendHighI32x4S;
        sub_op = Op::I64x2Sub; abs_op = Op::I64x2Abs;
        trunc_pattern = {0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27};
        break;
    }
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(ext_lo);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(ext_lo);
    ctx.code.Simd(sub_op);
    ctx.code.Simd(abs_op);
    ctx.code.LocalSet(tlo);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(ext_hi);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(ext_hi);
    ctx.code.Simd(sub_op);
    ctx.code.Simd(abs_op);
    ctx.code.LocalSet(thi);
    ctx.code.LocalGet(tlo);
    ctx.code.LocalGet(thi);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(trunc_pattern.data(), trunc_pattern.size());
    ctx.code.LocalSet(dst);
}

// |a-b| per lane, unsigned inputs.
void EmitAbsDiffUnsigned(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 t = ctx.Temp(ValType::V128);
    if (esize == 32) {
        // max(a,b) - min(a,b) (no 32-bit saturating sub in wasm)
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.Simd(Op::I32x4MinU);
        ctx.code.LocalSet(t);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.Simd(Op::I32x4MaxU);
        ctx.code.LocalGet(t);
        ctx.code.Simd(Op::I32x4Sub);
        ctx.code.LocalSet(dst);
        return;
    }
    // or(sub_sat_u(a,b), sub_sat_u(b,a)): exactly one side is nonzero
    const u32 sat_op = esize == 8 ? Op::I8x16SubSatU : Op::I16x8SubSatU;
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(sat_op);
    ctx.code.LocalSet(t);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(sat_op);
    ctx.code.LocalGet(t);
    ctx.code.Simd(Op::V128Or);
    ctx.code.LocalSet(dst);
}

// clz per lane = esize - popcnt(v) where v has every bit below the topmost
// set bit also set (fill via shifted ORs).
void EmitCountLeadingZeros(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tv = ctx.Temp(ValType::V128);
    const u32 shr_op = esize == 8 ? Op::I8x16ShrU : esize == 16 ? Op::I16x8ShrU : Op::I32x4ShrU;
    const u32 sub_op = esize == 8 ? Op::I8x16Sub : esize == 16 ? Op::I16x8Sub : Op::I32x4Sub;
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(tv);
    for (u32 shift = 1; shift < esize; shift <<= 1) {
        ctx.code.LocalGet(tv);
        ctx.code.LocalGet(tv);
        ctx.code.I32Const(static_cast<s32>(shift));
        ctx.code.Simd(shr_op);
        ctx.code.Simd(Op::V128Or);
        ctx.code.LocalSet(tv);
    }
    // popcnt per byte, then fold pairs of byte sums into wider lanes
    ctx.code.LocalGet(tv);
    ctx.code.Simd(Op::I8x16Popcnt);
    if (esize >= 16) {
        ctx.code.Simd(Op::I16x8ExtAddPairwiseI8x16U);
    }
    if (esize == 32) {
        ctx.code.Simd(Op::I32x4ExtAddPairwiseI16x8U);
    }
    ctx.code.LocalSet(tv);
    VLaneConst(ctx, esize, esize);
    ctx.code.LocalGet(tv);
    ctx.code.Simd(sub_op);
    ctx.code.LocalSet(dst);
}

// Variable shift (VSHL register form): per-lane signed shift amounts.
// amount >= 0: shift left; amount < 0: shift right (logical for unsigned
// lanes, arithmetic for signed). |amount| >= esize: 0, or sign-fill for
// arithmetic right shifts.
//
// Barrel shifter: |amount| is clamped to esize-1 (that clamp alone yields the
// correct sign-fill for over-shifted arithmetic right shifts), then one
// bitselect stage per amount bit shifts both directions; a final kill mask
// zeroes lanes that overshifted (left, or logical right).
void EmitVShift(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    u32 shl_op, shr_log_op, shr_arith_op, abs_op, gt_u_op, gt_s_op, eq_op;
    switch (esize) {
    case 8:
        shl_op = Op::I8x16Shl; shr_log_op = Op::I8x16ShrU; shr_arith_op = Op::I8x16ShrS;
        abs_op = Op::I8x16Abs; gt_u_op = Op::I8x16GtU; gt_s_op = Op::I8x16GtS;
        eq_op = Op::I8x16Eq;
        break;
    case 16:
        shl_op = Op::I16x8Shl; shr_log_op = Op::I16x8ShrU; shr_arith_op = Op::I16x8ShrS;
        abs_op = Op::I16x8Abs; gt_u_op = Op::I16x8GtU; gt_s_op = Op::I16x8GtS;
        eq_op = Op::I16x8Eq;
        break;
    case 32:
        shl_op = Op::I32x4Shl; shr_log_op = Op::I32x4ShrU; shr_arith_op = Op::I32x4ShrS;
        abs_op = Op::I32x4Abs; gt_u_op = Op::I32x4GtU; gt_s_op = Op::I32x4GtS;
        eq_op = Op::I32x4Eq;
        break;
    default:
        ASSERT(esize == 64);
        shl_op = Op::I64x2Shl; shr_log_op = Op::I64x2ShrU; shr_arith_op = Op::I64x2ShrS;
        abs_op = Op::I64x2Abs; gt_u_op = 0; gt_s_op = Op::I64x2GtS;
        eq_op = Op::I64x2Eq;
        break;
    }

    const u32 tamt = ctx.Temp(ValType::V128);   // amounts (low byte of each lane, sign-extended)
    const u32 tabs = ctx.Temp(ValType::V128);   // |amount|
    const u32 tover = ctx.Temp(ValType::V128);  // |amount| >=u esize
    const u32 tneg = ctx.Temp(ValType::V128);   // amount <s 0
    const u32 tlr = ctx.Temp(ValType::V128);    // left-shifted
    const u32 trr = ctx.Temp(ValType::V128);    // right-shifted

    // Only the low byte of each amount lane is meaningful (ARM ARM: the shift
    // amount is the least significant byte, signed). Sign-extend it.
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    if (esize != 8) {
        ctx.code.I32Const(static_cast<s32>(esize - 8));
        ctx.code.Simd(shl_op);
        ctx.code.I32Const(static_cast<s32>(esize - 8));
        ctx.code.Simd(shr_arith_op);
    }
    ctx.code.LocalSet(tamt);

    // tabs = abs(amount)
    ctx.code.LocalGet(tamt);
    ctx.code.Simd(abs_op);
    ctx.code.LocalSet(tabs);
    // tneg = amount <s 0  <=>  0 >s amount
    VZero(ctx);
    ctx.code.LocalGet(tamt);
    ctx.code.Simd(gt_s_op);
    ctx.code.LocalSet(tneg);
    // tover = tabs >u esize-1 (for 64-bit: no unsigned compare; abs values
    // >= 2^63 read as negative, so also treat tabs <s 0 as over)
    if (esize == 64) {
        ctx.code.LocalGet(tabs);
        VLaneConst(ctx, 64, 63);
        ctx.code.Simd(gt_s_op);
        VZero(ctx);
        ctx.code.LocalGet(tabs);
        ctx.code.Simd(gt_s_op);  // 0 >s tabs  <=>  tabs "negative" (abs wrapped)
        ctx.code.Simd(Op::V128Or);
        ctx.code.LocalSet(tover);
    } else {
        ctx.code.LocalGet(tabs);
        VLaneConst(ctx, esize, esize - 1);
        ctx.code.Simd(gt_u_op);
        ctx.code.LocalSet(tover);
    }
    // clamp: tabs = tover ? esize-1 : tabs   (bitselect(v1,v2,c) = c?v1:v2)
    VLaneConst(ctx, esize, esize - 1);
    ctx.code.LocalGet(tabs);
    ctx.code.LocalGet(tover);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalSet(tabs);

    // barrel: for each amount bit k, conditionally shift by k
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(tlr);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(trr);
    const u32 shr_op = is_signed ? shr_arith_op : shr_log_op;
    for (u32 k = 1; k < esize; k <<= 1) {
        // mask = (tabs & k) == k
        const auto stage = [&](u32 tval, u32 shift_op) {
            const u32 tmask = ctx.Temp(ValType::V128);
            ctx.code.LocalGet(tabs);
            VLaneConst(ctx, esize, k);
            ctx.code.Simd(Op::V128And);
            VLaneConst(ctx, esize, k);
            ctx.code.Simd(eq_op);
            ctx.code.LocalSet(tmask);
            ctx.code.LocalGet(tval);
            ctx.code.I32Const(static_cast<s32>(k));
            ctx.code.Simd(shift_op);
            ctx.code.LocalGet(tval);
            ctx.code.LocalGet(tmask);
            ctx.code.Simd(Op::V128Bitselect);   // mask ? shifted : orig
            ctx.code.LocalSet(tval);
        };
        stage(tlr, shl_op);
        stage(trr, shr_op);
    }
    // select: neg ? right : left
    ctx.code.LocalGet(trr);
    ctx.code.LocalGet(tlr);
    ctx.code.LocalGet(tneg);
    ctx.code.Simd(Op::V128Bitselect);
    // kill mask: logical -> over; arithmetic -> over & ~neg
    if (is_signed) {
        ctx.code.LocalGet(tover);
        ctx.code.LocalGet(tneg);
        ctx.code.Simd(Op::V128AndNot);  // over & ~neg
        ctx.code.Simd(Op::V128AndNot);  // result & ~kill
    } else {
        ctx.code.LocalGet(tover);
        ctx.code.Simd(Op::V128AndNot);  // result & ~over
    }
    ctx.code.LocalSet(dst);
}

// Saturating narrow to the low half: narrow_op(a, zero). clamp_unsigned is
// for unsigned sources: wasm's narrow_u treats the source as signed and would
// map values above INT_MAX to 0, so clamp with min_u first.
void EmitNarrowSat(EmitContext& ctx, IR::Inst* inst, u32 esize, u32 narrow_op, bool clamp_unsigned) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    if (clamp_unsigned) {
        VLaneConst(ctx, esize, esize == 16 ? 0xFF : 0xFFFF);
        ctx.code.Simd(esize == 16 ? Op::I16x8MinU : Op::I32x4MinU);
    }
    VZero(ctx);
    ctx.code.Simd(narrow_op);
    ctx.code.LocalSet(dst);
}

// Saturating narrow of two 64-bit lanes to 32 bits (wasm has no i32x4.narrow_i64x2):
// per-lane scalar clamp then i32x4.replace_lane into a zeroed vector, so the
// upper 64 bits are zero like the other narrows. min/max use select, which
// yields its first-pushed operand when the condition is true.
enum class Narrow64Kind { SignedToSigned, SignedToUnsigned, UnsignedToUnsigned };

void EmitNarrowSat64(EmitContext& ctx, IR::Inst* inst, Narrow64Kind kind) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 src = ctx.Temp(ValType::V128);
    const u32 x = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(src);

    VZero(ctx);
    for (u8 lane = 0; lane < 2; lane++) {
        code.LocalGet(src);
        code.Simd(Op::I64x2ExtractLane);
        code.U8(lane);
        code.LocalSet(x);

        if (kind != Narrow64Kind::UnsignedToUnsigned) {
            const s64 lo = kind == Narrow64Kind::SignedToSigned ? -0x80000000LL : 0;
            const s64 hi = kind == Narrow64Kind::SignedToSigned ? 0x7FFFFFFFLL : 0xFFFFFFFFLL;
            // x = max(x, lo)
            code.LocalGet(x);
            code.I64Const(lo);
            code.LocalGet(x);
            code.I64Const(lo);
            code.U8(Op::I64GtS);
            code.U8(Op::Select);
            code.LocalSet(x);
            // x = min(x, hi)
            code.LocalGet(x);
            code.I64Const(hi);
            code.LocalGet(x);
            code.I64Const(hi);
            code.U8(Op::I64LtS);
            code.U8(Op::Select);
            code.LocalSet(x);
        } else {
            // x = min_u(x, 0xFFFFFFFF)
            code.LocalGet(x);
            code.I64Const(0xFFFFFFFFLL);
            code.LocalGet(x);
            code.I64Const(0xFFFFFFFFLL);
            code.U8(Op::I64LtU);
            code.U8(Op::Select);
            code.LocalSet(x);
        }
        code.LocalGet(x);
        code.U8(Op::I32WrapI64);
        code.Simd(Op::I32x4ReplaceLane);
        code.U8(lane);
    }
    code.LocalSet(dst);
}

// VTBL/VTBX via i8x16.swizzle per 16-byte table chunk. For chunk k the index
// bytes are adjusted by a wrapping i8x16 subtract of 16k, so indices belonging
// to other chunks (or beyond the table) select a zero/oversized lane and
// produce 0; the per-chunk results are ORed. vtbx (non-zero default) keeps
// the default wherever the original index is >=u the table's byte size.
// Table entries are D registers (U64, A32 VTBL: two per chunk) or Q
// registers (U128, A64 TBL: one per chunk).
void EmitTableLookup(EmitContext& ctx, IR::Inst* inst, bool is_128) {
    ASSERT(inst->GetArg(1).GetInst()->GetOpcode() == IR::Opcode::VectorTable);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto table = ctx.reg_alloc.GetArgumentInfo(inst->GetArg(1).GetInst());
    const size_t table_size = static_cast<size_t>(std::count_if(table.begin(), table.end(), [](const auto& elem) { return !elem.value.IsEmpty(); }));
    const bool is_defaults_zero = inst->GetArg(0).IsZero();
    const bool wide_entries = table[0].GetType() == IR::Type::U128;
    const size_t table_bytes = table_size * (wide_entries ? 16 : 8);

    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tidx = ctx.Temp(ValType::V128);
    const u32 tvec = ctx.Temp(ValType::V128);
    const u32 tresult = ctx.Temp(ValType::V128);

    // index vector (broadcast into both halves for the U64 form)
    if (is_128) {
        EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    } else {
        VZero(ctx);
        EmitPush(ctx.code, ctx.reg_alloc, args[2]);
        ctx.code.Simd(Op::I64x2ReplaceLane);
        ctx.code.U8(0);
    }
    ctx.code.LocalSet(tidx);

    const size_t chunk_count = wide_entries ? table_size : (table_size + 1) / 2;
    for (size_t k = 0; k < chunk_count; k++) {
        if (wide_entries) {
            EmitPush(ctx.code, ctx.reg_alloc, table[k]);
        } else {
            // chunk vector = [table[2k], table[2k+1] or zero]
            VZero(ctx);
            EmitPush(ctx.code, ctx.reg_alloc, table[2 * k]);
            ctx.code.Simd(Op::I64x2ReplaceLane);
            ctx.code.U8(0);
            if (2 * k + 1 < table_size) {
                EmitPush(ctx.code, ctx.reg_alloc, table[2 * k + 1]);
                ctx.code.Simd(Op::I64x2ReplaceLane);
                ctx.code.U8(1);
            }
        }
        ctx.code.LocalSet(tvec);

        ctx.code.LocalGet(tvec);
        ctx.code.LocalGet(tidx);
        if (k != 0) {
            VLaneConst(ctx, 8, 16 * k);
            ctx.code.Simd(Op::I8x16Sub);
        }
        ctx.code.Simd(Op::I8x16Swizzle);
        if (k != 0) {
            ctx.code.LocalGet(tresult);
            ctx.code.Simd(Op::V128Or);
        }
        ctx.code.LocalSet(tresult);
    }

    if (!is_defaults_zero) {
        // default vector
        if (is_128) {
            EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        } else {
            VZero(ctx);
            EmitPush(ctx.code, ctx.reg_alloc, args[0]);
            ctx.code.Simd(Op::I64x2ReplaceLane);
            ctx.code.U8(0);
        }
        ctx.code.LocalSet(tvec);

        ctx.code.LocalGet(tresult);
        ctx.code.LocalGet(tvec);
        ctx.code.LocalGet(tidx);
        VLaneConst(ctx, 8, table_bytes);
        ctx.code.Simd(Op::I8x16LtU);  // in-range mask
        ctx.code.Simd(Op::V128Bitselect);  // in-range ? result : default
        ctx.code.LocalSet(tresult);
    }

    ctx.code.LocalGet(tresult);
    if (!is_128) {
        ctx.code.Simd(Op::I64x2ExtractLane);
        ctx.code.U8(0);
    }
    ctx.code.LocalSet(dst);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Shuffles: deinterleave (VUZP), interleave (VZIP), transpose (VTRN)

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveEven8>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 8, false);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveEven16>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveEven32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveEven64>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 64, false);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveOdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveOdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveOdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveOdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleave(ctx, inst, 64, true);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveEvenLower8>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleaveLower(ctx, inst, 8, false);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveEvenLower16>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleaveLower(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveEvenLower32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleaveLower(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveOddLower8>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleaveLower(ctx, inst, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveOddLower16>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleaveLower(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorDeinterleaveOddLower32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDeinterleaveLower(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveLower8>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 8, false);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveLower16>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveLower32>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveLower64>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 64, false);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveUpper8>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveUpper16>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveUpper32>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorInterleaveUpper64>(EmitContext& ctx, IR::Inst* inst) {
    EmitInterleave(ctx, inst, 64, true);
}

template<>
void EmitIR<IR::Opcode::VectorTranspose8>(EmitContext& ctx, IR::Inst* inst) {
    EmitTranspose(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorTranspose16>(EmitContext& ctx, IR::Inst* inst) {
    EmitTranspose(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorTranspose32>(EmitContext& ctx, IR::Inst* inst) {
    EmitTranspose(ctx, inst, 32);
}

template<>
void EmitIR<IR::Opcode::VectorTranspose64>(EmitContext& ctx, IR::Inst* inst) {
    EmitTranspose(ctx, inst, 64);
}

// ---------------------------------------------------------------------------
// Extract (VEXT)

template<>
void EmitIR<IR::Opcode::VectorExtract>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[2].IsImmediate());
    const u32 k = args[2].GetImmediateU8() / 8;
    std::array<u8, 16> pattern{};
    for (u32 i = 0; i < 16; i++) {
        pattern[i] = static_cast<u8>(i + k);  // 16..31 index the second operand
    }
    EmitShuffle2(ctx, inst, pattern);
}

template<>
void EmitIR<IR::Opcode::VectorExtractLower>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[2].IsImmediate());
    const u32 k = args[2].GetImmediateU8() / 8;
    std::array<u8, 16> pattern{};
    for (u32 i = 0; i < 8; i++) {
        const u32 src = i + k;
        pattern[i] = static_cast<u8>(src < 8 ? src : 16 + (src - 8));
    }
    // k == 0 degenerates to zero-upper(a), which the mask provides
    EmitShuffle2Masked(ctx, inst, pattern, kLow64Mask);
}

// ---------------------------------------------------------------------------
// Reverse elements (VREV16/32/64)

template<>
void EmitIR<IR::Opcode::VectorReverseElementsInHalfGroups8>(EmitContext& ctx, IR::Inst* inst) {
    EmitReverseElements(ctx, inst, 1, 2);
}

template<>
void EmitIR<IR::Opcode::VectorReverseElementsInWordGroups8>(EmitContext& ctx, IR::Inst* inst) {
    EmitReverseElements(ctx, inst, 1, 4);
}

template<>
void EmitIR<IR::Opcode::VectorReverseElementsInWordGroups16>(EmitContext& ctx, IR::Inst* inst) {
    EmitReverseElements(ctx, inst, 2, 4);
}

template<>
void EmitIR<IR::Opcode::VectorReverseElementsInLongGroups8>(EmitContext& ctx, IR::Inst* inst) {
    EmitReverseElements(ctx, inst, 1, 8);
}

template<>
void EmitIR<IR::Opcode::VectorReverseElementsInLongGroups16>(EmitContext& ctx, IR::Inst* inst) {
    EmitReverseElements(ctx, inst, 2, 8);
}

template<>
void EmitIR<IR::Opcode::VectorReverseElementsInLongGroups32>(EmitContext& ctx, IR::Inst* inst) {
    EmitReverseElements(ctx, inst, 4, 8);
}

// ---------------------------------------------------------------------------
// Saturating add/sub (VQADD/VQSUB), 8/16-bit lanes map to wasm directly
// (32/64-bit lanes need widening and are a later tranche)

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16AddSatS);
    EmitQcCheck(ctx, inst, QcKind::Add, 8);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8AddSatS);
    EmitQcCheck(ctx, inst, QcKind::Add, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedSub8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16SubSatS);
    EmitQcCheck(ctx, inst, QcKind::Sub, 8);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedSub16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8SubSatS);
    EmitQcCheck(ctx, inst, QcKind::Sub, 16);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedAdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16AddSatU);
    EmitQcCheck(ctx, inst, QcKind::Add, 8);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8AddSatU);
    EmitQcCheck(ctx, inst, QcKind::Add, 16);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedSub8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16SubSatU);
    EmitQcCheck(ctx, inst, QcKind::Sub, 8);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedSub16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8SubSatU);
    EmitQcCheck(ctx, inst, QcKind::Sub, 16);
}

// ---------------------------------------------------------------------------
// Absolute difference (VABD)

template<>
void EmitIR<IR::Opcode::VectorSignedAbsoluteDifference8>(EmitContext& ctx, IR::Inst* inst) {
    EmitAbsDiffSigned(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorSignedAbsoluteDifference16>(EmitContext& ctx, IR::Inst* inst) {
    EmitAbsDiffSigned(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedAbsoluteDifference32>(EmitContext& ctx, IR::Inst* inst) {
    EmitAbsDiffSigned(ctx, inst, 32);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedAbsoluteDifference8>(EmitContext& ctx, IR::Inst* inst) {
    EmitAbsDiffUnsigned(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedAbsoluteDifference16>(EmitContext& ctx, IR::Inst* inst) {
    EmitAbsDiffUnsigned(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedAbsoluteDifference32>(EmitContext& ctx, IR::Inst* inst) {
    EmitAbsDiffUnsigned(ctx, inst, 32);
}

// ---------------------------------------------------------------------------
// Saturating narrow (VQMOVN/VQMOVUN)

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedNarrow16>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat(ctx, inst, 16, Op::I8x16NarrowI16x8U, true);
    EmitQcCheck(ctx, inst, QcKind::NarrowUnsigned, 16);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedNarrow32>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat(ctx, inst, 32, Op::I16x8NarrowI32x4U, true);
    EmitQcCheck(ctx, inst, QcKind::NarrowUnsigned, 32);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNarrowToSigned16>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat(ctx, inst, 16, Op::I8x16NarrowI16x8S, false);
    EmitQcCheck(ctx, inst, QcKind::NarrowSigned, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNarrowToSigned32>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat(ctx, inst, 32, Op::I16x8NarrowI32x4S, false);
    EmitQcCheck(ctx, inst, QcKind::NarrowSigned, 32);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNarrowToUnsigned16>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat(ctx, inst, 16, Op::I8x16NarrowI16x8U, false);
    EmitQcCheck(ctx, inst, QcKind::NarrowUnsigned, 16);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNarrowToUnsigned32>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat(ctx, inst, 32, Op::I16x8NarrowI32x4U, false);
    EmitQcCheck(ctx, inst, QcKind::NarrowUnsigned, 32);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedNarrow64>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat64(ctx, inst, Narrow64Kind::UnsignedToUnsigned);
    EmitQcCheck(ctx, inst, QcKind::NarrowUnsigned, 64);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNarrowToSigned64>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat64(ctx, inst, Narrow64Kind::SignedToSigned);
    EmitQcCheck(ctx, inst, QcKind::NarrowSigned, 64);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedNarrowToUnsigned64>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrowSat64(ctx, inst, Narrow64Kind::SignedToUnsigned);
    EmitQcCheck(ctx, inst, QcKind::NarrowUnsigned, 64);
}

// ---------------------------------------------------------------------------
// Count leading zeros (VCLZ)

template<>
void EmitIR<IR::Opcode::VectorCountLeadingZeros8>(EmitContext& ctx, IR::Inst* inst) {
    EmitCountLeadingZeros(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorCountLeadingZeros16>(EmitContext& ctx, IR::Inst* inst) {
    EmitCountLeadingZeros(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorCountLeadingZeros32>(EmitContext& ctx, IR::Inst* inst) {
    EmitCountLeadingZeros(ctx, inst, 32);
}

// ---------------------------------------------------------------------------
// Variable shifts (VSHL register form)

template<>
void EmitIR<IR::Opcode::VectorLogicalVShift8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 8, false);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalVShift16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalVShift32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalVShift64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 64, false);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticVShift8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticVShift16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticVShift32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticVShift64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShift(ctx, inst, 64, true);
}

// ---------------------------------------------------------------------------
// Table lookup (VTBL/VTBX)

template<>
void EmitIR<IR::Opcode::VectorTable>(EmitContext& ctx, IR::Inst* inst) {
    // Nothing to emit: the lookup consumes this instruction's arguments
    // directly, so keep them live until then.
    ctx.reg_alloc.DeferArgUse(inst);
}

template<>
void EmitIR<IR::Opcode::VectorTableLookup64>(EmitContext& ctx, IR::Inst* inst) {
    EmitTableLookup(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::VectorTableLookup128>(EmitContext& ctx, IR::Inst* inst) {
    EmitTableLookup(ctx, inst, true);
}

}  // namespace Dynarmic::Backend::Wasm
