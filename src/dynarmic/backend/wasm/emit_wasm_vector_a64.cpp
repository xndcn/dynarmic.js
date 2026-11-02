// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Integer vector opcodes only the A64 frontend emits: 64-bit lane min/max and
// multiply, paired min/max (full and lower-half forms), across-lane sums,
// bit reversal, whole-vector rotation, multiplies returning upper and lower
// product halves, SUQADD/USQADD, 64-bit polynomial multiply, and the SM4
// S-box.

#include <array>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"
#include "dynarmic/common/crypto/sm4.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// Shuffle pattern over the lane concatenation x:y (lanes of `esize` bits):
// output lane k takes input lane sel(k).
template<typename F>
std::array<u8, 16> LanePattern(u32 esize, F&& sel) {
    std::array<u8, 16> p{};
    const u32 lane_bytes = esize / 8;
    for (u32 k = 0; k < 16 / lane_bytes; k++) {
        const u32 src = sel(k);
        for (u32 b = 0; b < lane_bytes; b++) {
            p[k * lane_bytes + b] = static_cast<u8>(src * lane_bytes + b);
        }
    }
    return p;
}

void EmitShufflePattern(EmitContext& ctx, const std::array<u8, 16>& pattern) {
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(pattern.data(), pattern.size());
}

u32 StoreArgTemp(EmitContext& ctx, const RegAlloc::Argument& arg) {
    const u32 t = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, arg);
    ctx.code.LocalSet(t);
    return t;
}

// --- 64-bit lane min / max -------------------------------------------------

// dst = (a > b) == is_max ? a : b; unsigned compares flip the sign bits first.
void EmitMinMax64(EmitContext& ctx, IR::Inst* inst, bool is_max, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    for (int i = 0; i < 2; i++) {
        EmitPush(ctx.code, ctx.reg_alloc, args[i]);
        if (!is_signed) {
            VLaneConst(ctx, 64, u64{1} << 63);
            ctx.code.Simd(Op::V128Xor);
        }
    }
    ctx.code.Simd(is_max ? Op::I64x2GtS : Op::I64x2LtS);
    ctx.code.Simd(Op::V128Bitselect);
    ctx.code.LocalSet(dst);
}

// --- Paired min / max ------------------------------------------------------

u32 MinMaxOp(u32 esize, bool is_max, bool is_signed) {
    switch (esize) {
    case 8:
        return is_max ? (is_signed ? Op::I8x16MaxS : Op::I8x16MaxU) : (is_signed ? Op::I8x16MinS : Op::I8x16MinU);
    case 16:
        return is_max ? (is_signed ? Op::I16x8MaxS : Op::I16x8MaxU) : (is_signed ? Op::I16x8MinS : Op::I16x8MinU);
    case 32:
        return is_max ? (is_signed ? Op::I32x4MaxS : Op::I32x4MaxU) : (is_signed ? Op::I32x4MinS : Op::I32x4MinU);
    }
    UNREACHABLE();
}

// Pushes op(even lanes, odd lanes) of the lane concatenation x:y, i.e. the
// pairwise results of x followed by those of y.
void EmitPairedOp(EmitContext& ctx, u32 x, u32 y, u32 esize, u32 op) {
    ctx.code.LocalGet(x);
    ctx.code.LocalGet(y);
    EmitShufflePattern(ctx, LanePattern(esize, [](u32 k) { return 2 * k; }));
    ctx.code.LocalGet(x);
    ctx.code.LocalGet(y);
    EmitShufflePattern(ctx, LanePattern(esize, [](u32 k) { return 2 * k + 1; }));
    ctx.code.Simd(op);
}

void EmitPairedMinMax(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_max, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 x = StoreArgTemp(ctx, args[0]);
    const u32 y = StoreArgTemp(ctx, args[1]);
    EmitPairedOp(ctx, x, y, esize, MinMaxOp(esize, is_max, is_signed));
    ctx.code.LocalSet(dst);
}

// result = [pairs of a.lo | pairs of b.lo | zero upper half]
void EmitPairedMinMaxLower(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_max, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 t = ctx.Temp(ValType::V128);
    const u32 zero = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitShufflePattern(ctx, {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23});
    ctx.code.LocalSet(t);
    VZero(ctx);
    ctx.code.LocalSet(zero);
    // Pairs of the zero vector fill the upper half with op(0, 0) = 0.
    EmitPairedOp(ctx, t, zero, esize, MinMaxOp(esize, is_max, is_signed));
    ctx.code.LocalSet(dst);
}

// --- Across-lane sums ------------------------------------------------------

// Sum of all lanes in lane 0 (element-sized, wrapping), every other lane zero.
void EmitReduceAdd(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 v = StoreArgTemp(ctx, args[0]);

    // v += v with the upper half (then quarter, ...) moved down.
    auto fold = [&](u32 add_op, u32 half_bytes) {
        std::array<u8, 16> pattern{};
        for (u32 i = 0; i < 16; i++) {
            pattern[i] = static_cast<u8>(half_bytes + (i % half_bytes));
        }
        code.LocalGet(v);
        code.LocalGet(v);
        code.LocalGet(v);
        EmitShufflePattern(ctx, pattern);
        code.Simd(add_op);
        code.LocalSet(v);
    };
    const u32 add_op = esize == 8 ? Op::I8x16Add : esize == 16 ? Op::I16x8Add : esize == 32 ? Op::I32x4Add : Op::I64x2Add;
    for (u32 half_bytes = 8; half_bytes >= esize / 8; half_bytes /= 2) {
        fold(add_op, half_bytes);
    }

    std::array<u8, 16> mask{};
    for (u32 b = 0; b < esize / 8; b++) {
        mask[b] = 0xFF;
    }
    code.LocalGet(v);
    VConst(ctx, mask);
    code.Simd(Op::V128And);
    code.LocalSet(dst);
}

// --- Multiplies returning both product halves -----------------------------

// VectorSignedMultiply / VectorUnsignedMultiply: the producer has no value of
// its own; GetLowerFromOp gets the low half of each lane's product,
// GetUpperFromOp the high half.
void EmitMultiplyUpperLower(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    IR::Inst* const upper_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetUpperFromOp);
    IR::Inst* const lower_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetLowerFromOp);

    if (lower_inst) {
        const u32 dst = ctx.reg_alloc.DefineLocal(code, lower_inst);
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.Simd(esize == 16 ? Op::I16x8Mul : Op::I32x4Mul);
        code.LocalSet(dst);
    }
    if (upper_inst) {
        const u32 dst = ctx.reg_alloc.DefineLocal(code, upper_inst);
        const u32 low_op = esize == 16 ? (is_signed ? Op::I32x4ExtMulLowI16x8S : Op::I32x4ExtMulLowI16x8U)
                                       : (is_signed ? Op::I64x2ExtMulLowI32x4S : Op::I64x2ExtMulLowI32x4U);
        const u32 high_op = esize == 16 ? (is_signed ? Op::I32x4ExtMulHighI16x8S : Op::I32x4ExtMulHighI16x8U)
                                        : (is_signed ? Op::I64x2ExtMulHighI32x4S : Op::I64x2ExtMulHighI32x4U);
        for (const u32 op : {low_op, high_op}) {
            EmitPush(code, ctx.reg_alloc, args[0]);
            EmitPush(code, ctx.reg_alloc, args[1]);
            code.Simd(op);
        }
        // Keep the upper half of every double-width product.
        EmitShufflePattern(ctx, LanePattern(esize, [](u32 k) { return 2 * k + 1; }));
        code.LocalSet(dst);
    }
}

// --- SUQADD / USQADD -------------------------------------------------------

u32 ShrSOp(u32 esize) {
    switch (esize) {
    case 8:
        return Op::I8x16ShrS;
    case 16:
        return Op::I16x8ShrS;
    case 32:
        return Op::I32x4ShrS;
    case 64:
        return Op::I64x2ShrS;
    }
    UNREACHABLE();
}

u32 AddOp(u32 esize) {
    switch (esize) {
    case 8:
        return Op::I8x16Add;
    case 16:
        return Op::I16x8Add;
    case 32:
        return Op::I32x4Add;
    case 64:
        return Op::I64x2Add;
    }
    UNREACHABLE();
}

// Pushes the lane mask (all ones / zeros) of the sign bits of the v128 on the stack.
void EmitSignMask(EmitContext& ctx, u32 esize) {
    ctx.code.I32Const(static_cast<s32>(esize - 1));
    ctx.code.Simd(ShrSOp(esize));
}

// SUQADD: args[0] is the unsigned addend, args[1] the signed accumulator;
// result = SignedSat(acc + addend). Only positive overflow is possible:
// ovf = majority(~acc, addend, sum) (sign bits), result = ovf ? INT_MAX : sum.
void EmitSignedSaturatedAccumulateUnsigned(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 x = StoreArgTemp(ctx, args[0]);
    const u32 y = StoreArgTemp(ctx, args[1]);
    const u32 sum = ctx.Temp(ValType::V128);
    const u32 ovf = ctx.Temp(ValType::V128);

    code.LocalGet(x);
    code.LocalGet(y);
    code.Simd(AddOp(esize));
    code.LocalSet(sum);

    // majority(n, x, s) = (n & (x | s)) | (x & s), n = ~y
    code.LocalGet(y);
    code.Simd(Op::V128Not);
    code.LocalGet(x);
    code.LocalGet(sum);
    code.Simd(Op::V128Or);
    code.Simd(Op::V128And);
    code.LocalGet(x);
    code.LocalGet(sum);
    code.Simd(Op::V128And);
    code.Simd(Op::V128Or);
    EmitSignMask(ctx, esize);
    code.LocalSet(ovf);

    VLaneConst(ctx, esize, (esize == 64 ? ~u64{0} : (u64{1} << esize) - 1) >> 1);
    code.LocalGet(sum);
    code.LocalGet(ovf);
    code.Simd(Op::V128Bitselect);
    code.LocalSet(dst);
    EmitSetQcIfAny(ctx, ovf);
}

// USQADD: args[0] is the signed addend, args[1] the unsigned accumulator;
// result = UnsignedSat(acc + addend):
//   up   = ~x & y & ~sum  -> UINT_MAX
//   down =  x & ~y & sum  -> 0
void EmitUnsignedSaturatedAccumulateSigned(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 x = StoreArgTemp(ctx, args[0]);
    const u32 y = StoreArgTemp(ctx, args[1]);
    const u32 sum = ctx.Temp(ValType::V128);
    const u32 up = ctx.Temp(ValType::V128);
    const u32 down = ctx.Temp(ValType::V128);

    code.LocalGet(x);
    code.LocalGet(y);
    code.Simd(AddOp(esize));
    code.LocalSet(sum);

    code.LocalGet(y);
    code.LocalGet(x);
    code.LocalGet(sum);
    code.Simd(Op::V128Or);
    code.Simd(Op::V128AndNot);  // y & ~(x | sum)
    EmitSignMask(ctx, esize);
    code.LocalSet(up);

    code.LocalGet(x);
    code.LocalGet(sum);
    code.Simd(Op::V128And);
    code.LocalGet(y);
    code.Simd(Op::V128AndNot);  // (x & sum) & ~y
    EmitSignMask(ctx, esize);
    code.LocalSet(down);

    // (sum | up) & ~down
    code.LocalGet(sum);
    code.LocalGet(up);
    code.Simd(Op::V128Or);
    code.LocalGet(down);
    code.Simd(Op::V128AndNot);
    code.LocalSet(dst);

    code.LocalGet(up);
    code.LocalGet(down);
    code.Simd(Op::V128Or);
    code.LocalSet(up);
    EmitSetQcIfAny(ctx, up);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::VectorMaxS64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax64(ctx, inst, true, true);
}

template<>
void EmitIR<IR::Opcode::VectorMaxU64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax64(ctx, inst, true, false);
}

template<>
void EmitIR<IR::Opcode::VectorMinS64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax64(ctx, inst, false, true);
}

template<>
void EmitIR<IR::Opcode::VectorMinU64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax64(ctx, inst, false, false);
}

template<>
void EmitIR<IR::Opcode::VectorMultiply64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I64x2Mul);
}

#define PAIRED_MINMAX(op, sfx, esize, is_max, is_signed)                                        \
    template<>                                                                                  \
    void EmitIR<IR::Opcode::VectorPaired##op##sfx>(EmitContext & ctx, IR::Inst * inst) {        \
        EmitPairedMinMax(ctx, inst, esize, is_max, is_signed);                                  \
    }                                                                                           \
    template<>                                                                                  \
    void EmitIR<IR::Opcode::VectorPaired##op##Lower##sfx>(EmitContext & ctx, IR::Inst * inst) { \
        EmitPairedMinMaxLower(ctx, inst, esize, is_max, is_signed);                             \
    }

PAIRED_MINMAX(Max, S8, 8, true, true)
PAIRED_MINMAX(Max, S16, 16, true, true)
PAIRED_MINMAX(Max, S32, 32, true, true)
PAIRED_MINMAX(Max, U8, 8, true, false)
PAIRED_MINMAX(Max, U16, 16, true, false)
PAIRED_MINMAX(Max, U32, 32, true, false)
PAIRED_MINMAX(Min, S8, 8, false, true)
PAIRED_MINMAX(Min, S16, 16, false, true)
PAIRED_MINMAX(Min, S32, 32, false, true)
PAIRED_MINMAX(Min, U8, 8, false, false)
PAIRED_MINMAX(Min, U16, 16, false, false)
PAIRED_MINMAX(Min, U32, 32, false, false)

#undef PAIRED_MINMAX

template<>
void EmitIR<IR::Opcode::VectorReduceAdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitReduceAdd(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorReduceAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitReduceAdd(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorReduceAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitReduceAdd(ctx, inst, 32);
}

template<>
void EmitIR<IR::Opcode::VectorReduceAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitReduceAdd(ctx, inst, 64);
}

// Bit reversal within each byte: two nibble lookups.
template<>
void EmitIR<IR::Opcode::VectorReverseBits>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 v = StoreArgTemp(ctx, args[0]);
    std::array<u8, 16> rev_lo_to_hi{};  // rev4(n) << 4
    std::array<u8, 16> rev_hi_to_lo{};  // rev4(n)
    for (u32 n = 0; n < 16; n++) {
        const u32 r = ((n & 1) << 3) | ((n & 2) << 1) | ((n & 4) >> 1) | ((n & 8) >> 3);
        rev_lo_to_hi[n] = static_cast<u8>(r << 4);
        rev_hi_to_lo[n] = static_cast<u8>(r);
    }
    VConst(ctx, rev_lo_to_hi);
    code.LocalGet(v);
    VLaneConst(ctx, 8, 0x0F);
    code.Simd(Op::V128And);
    code.Simd(Op::I8x16Swizzle);
    VConst(ctx, rev_hi_to_lo);
    code.LocalGet(v);
    code.I32Const(4);
    code.Simd(Op::I8x16ShrU);
    code.Simd(Op::I8x16Swizzle);
    code.Simd(Op::V128Or);
    code.LocalSet(dst);
}

// Rotate the whole 128-bit value right by a multiple of 32 bits.
template<>
void EmitIR<IR::Opcode::VectorRotateWholeVectorRight>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u32 amount = args[1].GetImmediateU8();
    ASSERT(amount % 8 == 0);
    std::array<u8, 16> pattern{};
    for (u32 i = 0; i < 16; i++) {
        pattern[i] = static_cast<u8>((i + amount / 8) % 16);
    }
    EmitShuffle1(ctx, inst, pattern);
}

template<>
void EmitIR<IR::Opcode::VectorSignedMultiply16>(EmitContext& ctx, IR::Inst* inst) {
    EmitMultiplyUpperLower(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorSignedMultiply32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMultiplyUpperLower(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedMultiply16>(EmitContext& ctx, IR::Inst* inst) {
    EmitMultiplyUpperLower(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedMultiply32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMultiplyUpperLower(ctx, inst, 32, false);
}

#define SAT_ACCUMULATE(esize)                                                                                  \
    template<>                                                                                                 \
    void EmitIR<IR::Opcode::VectorSignedSaturatedAccumulateUnsigned##esize>(EmitContext & ctx, IR::Inst * inst) { \
        EmitSignedSaturatedAccumulateUnsigned(ctx, inst, esize);                                               \
    }                                                                                                          \
    template<>                                                                                                 \
    void EmitIR<IR::Opcode::VectorUnsignedSaturatedAccumulateSigned##esize>(EmitContext & ctx, IR::Inst * inst) { \
        EmitUnsignedSaturatedAccumulateSigned(ctx, inst, esize);                                               \
    }

SAT_ACCUMULATE(8)
SAT_ACCUMULATE(16)
SAT_ACCUMULATE(32)
SAT_ACCUMULATE(64)

#undef SAT_ACCUMULATE

// Carry-less 64x64 -> 128 multiply of the lower lanes (PMULL .1q): a wasm
// loop over the 64 bits of b, accumulating a << i into a 128-bit result.
template<>
void EmitIR<IR::Opcode::VectorPolynomialMultiplyLong64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 a = ctx.Temp(ValType::I64);
    const u32 b = ctx.Temp(ValType::I64);
    const u32 lo = ctx.Temp(ValType::I64);
    const u32 hi = ctx.Temp(ValType::I64);
    const u32 i = ctx.Temp(ValType::I64);

    EmitPush(code, ctx.reg_alloc, args[0]);
    code.Simd(Op::I64x2ExtractLane);
    code.U8(0);
    code.LocalSet(a);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.Simd(Op::I64x2ExtractLane);
    code.U8(0);
    code.LocalSet(b);
    code.I64Const(0);
    code.LocalSet(lo);
    code.I64Const(0);
    code.LocalSet(hi);
    code.I64Const(0);
    code.LocalSet(i);

    code.Loop();
    {
        // if (b >> i) & 1: lo ^= a << i; hi ^= i ? a >> (64 - i) : 0
        code.LocalGet(b);
        code.LocalGet(i);
        code.U8(Op::I64ShrU);
        code.I64Const(1);
        code.U8(Op::I64And);
        code.U8(Op::I32WrapI64);
        code.If();
        code.LocalGet(lo);
        code.LocalGet(a);
        code.LocalGet(i);
        code.U8(Op::I64Shl);
        code.U8(Op::I64Xor);
        code.LocalSet(lo);
        code.LocalGet(i);
        code.U8(Op::I64Eqz);
        code.U8(Op::I32Eqz);
        code.If();
        code.LocalGet(hi);
        code.LocalGet(a);
        code.I64Const(64);
        code.LocalGet(i);
        code.U8(Op::I64Sub);
        code.U8(Op::I64ShrU);
        code.U8(Op::I64Xor);
        code.LocalSet(hi);
        code.End();
        code.End();

        code.LocalGet(i);
        code.I64Const(1);
        code.U8(Op::I64Add);
        code.LocalTee(i);
        code.I64Const(64);
        code.U8(Op::I64LtU);
        code.BrIf(0);
    }
    code.End();

    code.LocalGet(lo);
    code.Simd(Op::I64x2Splat);
    code.LocalGet(hi);
    code.Simd(Op::I64x2ReplaceLane);
    code.U8(1);
    code.LocalSet(dst);
}

// SM4 S-box on one byte: a load from the 256-entry table in the main module.
template<>
void EmitIR<IR::Opcode::SM4AccessSubstitutionBox>(EmitContext& ctx, IR::Inst* inst) {
    static const std::array<u8, 256> sbox = [] {
        std::array<u8, 256> table{};
        for (u32 i = 0; i < 256; i++) {
            table[i] = Common::Crypto::SM4::AccessSubstitutionBox(static_cast<u8>(i));
        }
        return table;
    }();
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0xFF);
    ctx.code.U8(Op::I32And);
    ctx.code.Load(Op::I32Load8U, static_cast<u32>(reinterpret_cast<uptr>(sbox.data())), 0);
    ctx.code.LocalSet(dst);
}

}  // namespace Dynarmic::Backend::Wasm
