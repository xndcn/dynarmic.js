// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Vector (NEON/ASIMD) integer ops for the WASM backend, mapped onto wasm v128
// SIMD. Per-lane formulas that were carry-prone as packed i32 scalar code are
// safe here because wasm vector lanes never carry into each other.

#include <algorithm>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

void EmitV128ConstBytes(EmitContext& ctx, const std::array<u8, 16>& bytes) {
    ctx.code.Simd(Op::V128Const);
    ctx.code.Bytes(bytes.data(), bytes.size());
}

void EmitV128Zero(EmitContext& ctx) {
    EmitV128ConstBytes(ctx, {});
}

// Splatted per-lane constant (same byte pattern repeated per esize lane).
void EmitV128LaneConst(EmitContext& ctx, u32 esize, u64 value) {
    std::array<u8, 16> bytes{};
    const size_t lane_bytes = esize / 8;
    for (size_t i = 0; i < 16; i += lane_bytes) {
        for (size_t b = 0; b < lane_bytes; b++) {
            bytes[i + b] = static_cast<u8>(value >> (8 * b));
        }
    }
    EmitV128ConstBytes(ctx, bytes);
}

void EmitVUnOp(EmitContext& ctx, IR::Inst* inst, u32 subop) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(subop);
    ctx.code.LocalSet(dst);
}

void EmitVBinOp(EmitContext& ctx, IR::Inst* inst, u32 subop) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(subop);
    ctx.code.LocalSet(dst);
}

// (vec, imm8 shift amount) -> vec. A64 allows amounts up to the element size
// (USHR #esize, SSHR #esize, ...), which wasm would take modulo esize: a
// logical shift by >= esize gives 0, an arithmetic one fills with the sign.
void EmitVShiftImmOp(EmitContext& ctx, IR::Inst* inst, u32 subop, u32 esize, bool arithmetic = false) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    u32 amount = args[1].GetImmediateU8();
    if (amount >= esize && !arithmetic) {
        EmitV128Zero(ctx);
        ctx.code.LocalSet(dst);
        return;
    }
    amount = std::min(amount, esize - 1);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(amount));
    ctx.code.Simd(subop);
    ctx.code.LocalSet(dst);
}

// Splats lane `index` (of elem_bytes-wide lanes) of the vector on the stack.
void EmitSplatLane(EmitContext& ctx, u32 esize, u32 index) {
    switch (esize) {
    case 8:
        ctx.code.Simd(Op::I8x16ExtractLaneU);
        ctx.code.U8(index);
        ctx.code.Simd(Op::I8x16Splat);
        return;
    case 16:
        ctx.code.Simd(Op::I16x8ExtractLaneU);
        ctx.code.U8(index);
        ctx.code.Simd(Op::I16x8Splat);
        return;
    case 32:
        ctx.code.Simd(Op::I32x4ExtractLane);
        ctx.code.U8(index);
        ctx.code.Simd(Op::I32x4Splat);
        return;
    case 64:
        ctx.code.Simd(Op::I64x2ExtractLane);
        ctx.code.U8(index);
        ctx.code.Simd(Op::I64x2Splat);
        return;
    default:
        UNREACHABLE();
    }
}

void EmitBroadcastElement(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitSplatLane(ctx, esize, args[1].GetImmediateU8());
    ctx.code.LocalSet(dst);
}

void EmitGetElement(EmitContext& ctx, IR::Inst* inst, u32 extract_op) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(extract_op);
    ctx.code.U8(args[1].GetImmediateU8());
    ctx.code.LocalSet(dst);
}

void EmitSetElement(EmitContext& ctx, IR::Inst* inst, u32 replace_op) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    ctx.code.Simd(replace_op);
    ctx.code.U8(args[1].GetImmediateU8());
    ctx.code.LocalSet(dst);
}

// floor((a+b)/2) per lane = (a&b) + ((a^b)>>1); arithmetic shift for signed,
// logical for unsigned. Lane-local carries are impossible in vector ops.
void EmitHalvingAdd(EmitContext& ctx, IR::Inst* inst, u32 esize, bool rounding, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 add_op = esize == 8 ? Op::I8x16Add : esize == 16 ? Op::I16x8Add : Op::I32x4Add;
    const u32 shr_op = esize == 8 ? (is_signed ? Op::I8x16ShrS : Op::I8x16ShrU)
                     : esize == 16 ? (is_signed ? Op::I16x8ShrS : Op::I16x8ShrU)
                                   : (is_signed ? Op::I32x4ShrS : Op::I32x4ShrU);
    const u32 tx = ctx.Temp(ValType::V128);
    // tx = a^b
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::V128Xor);
    ctx.code.LocalSet(tx);
    // (a&b) + ((a^b)>>1)
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalGet(tx);
    ctx.code.I32Const(1);
    ctx.code.Simd(shr_op);
    ctx.code.Simd(add_op);
    if (rounding) {
        // + ((a^b)&1): floor((a+b+1)/2) = floor((a+b)/2) + (a+b is odd)
        ctx.code.LocalGet(tx);
        EmitV128LaneConst(ctx, esize, 1);
        ctx.code.Simd(Op::V128And);
        ctx.code.Simd(add_op);
    }
    ctx.code.LocalSet(dst);
}

// floor((a-b)/2) per lane via widening: a-b is exact in the wider lane,
// arithmetic shift floors, truncation back is lossless.
void EmitHalvingSub(EmitContext& ctx, IR::Inst* inst, u32 esize, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tlo = ctx.Temp(ValType::V128);
    const u32 thi = ctx.Temp(ValType::V128);

    u32 ext_lo, ext_hi, sub_op, shr_op;
    std::array<u8, 16> trunc_pattern{};
    switch (esize) {
    case 8:
        ext_lo = is_signed ? Op::I16x8ExtendLowI8x16S : Op::I16x8ExtendLowI8x16U;
        ext_hi = is_signed ? Op::I16x8ExtendHighI8x16S : Op::I16x8ExtendHighI8x16U;
        sub_op = Op::I16x8Sub;
        shr_op = Op::I16x8ShrS;
        trunc_pattern = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30};
        break;
    case 16:
        ext_lo = is_signed ? Op::I32x4ExtendLowI16x8S : Op::I32x4ExtendLowI16x8U;
        ext_hi = is_signed ? Op::I32x4ExtendHighI16x8S : Op::I32x4ExtendHighI16x8U;
        sub_op = Op::I32x4Sub;
        shr_op = Op::I32x4ShrS;
        trunc_pattern = {0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29};
        break;
    default:
        ASSERT(esize == 32);
        ext_lo = is_signed ? Op::I64x2ExtendLowI32x4S : Op::I64x2ExtendLowI32x4U;
        ext_hi = is_signed ? Op::I64x2ExtendHighI32x4S : Op::I64x2ExtendHighI32x4U;
        sub_op = Op::I64x2Sub;
        shr_op = Op::I64x2ShrS;
        trunc_pattern = {0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27};
        break;
    }

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(ext_lo);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(ext_lo);
    ctx.code.Simd(sub_op);
    ctx.code.I32Const(1);
    ctx.code.Simd(shr_op);
    ctx.code.LocalSet(tlo);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(ext_hi);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(ext_hi);
    ctx.code.Simd(sub_op);
    ctx.code.I32Const(1);
    ctx.code.Simd(shr_op);
    ctx.code.LocalSet(thi);
    ctx.code.LocalGet(tlo);
    ctx.code.LocalGet(thi);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(trunc_pattern.data(), trunc_pattern.size());
    ctx.code.LocalSet(dst);
}

// VMUL.I8 has no wasm equivalent: multiply even/odd byte lanes as 16-bit
// lanes (masking the operand's other byte), then merge.
void EmitMultiply8(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::V128);
    const u32 tb = ctx.Temp(ValType::V128);
    const u32 tprod = ctx.Temp(ValType::V128);

    EmitV128LaneConst(ctx, 16, 0x00FF);
    ctx.code.LocalSet(ta);  // mask 0x00FF per 16-bit lane
    // even bytes: (a & m) * (b & m), keep low byte of each 16-bit lane
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalGet(ta);
    ctx.code.Simd(Op::V128And);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalGet(ta);
    ctx.code.Simd(Op::V128And);
    ctx.code.Simd(Op::I16x8Mul);
    ctx.code.LocalGet(ta);
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalSet(tprod);
    // odd bytes: ((a>>8) * (b>>8)) low byte, shifted back up
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(8);
    ctx.code.Simd(Op::I16x8ShrU);
    ctx.code.LocalSet(tb);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.I32Const(8);
    ctx.code.Simd(Op::I16x8ShrU);
    ctx.code.LocalGet(tb);
    ctx.code.Simd(Op::I16x8Mul);
    ctx.code.LocalGet(ta);
    ctx.code.Simd(Op::V128And);
    ctx.code.I32Const(8);
    ctx.code.Simd(Op::I16x8Shl);
    // merge
    ctx.code.LocalGet(tprod);
    ctx.code.Simd(Op::V128Or);
    ctx.code.LocalSet(dst);
}

// Truncating narrow: pick the low half of each lane, zero the upper half.
void EmitNarrow(EmitContext& ctx, IR::Inst* inst, const std::array<u8, 16>& pattern) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitV128Zero(ctx);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(pattern.data(), pattern.size());
    ctx.code.LocalSet(dst);
}

// Paired add: result low half = adjacent pairs of a, high half = pairs of b.
void EmitPairedAdd(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 add_op = esize == 8 ? Op::I8x16Add : esize == 16 ? Op::I16x8Add : esize == 32 ? Op::I32x4Add : Op::I64x2Add;
    const u32 lane_bytes = esize / 8;
    const u32 lanes_per_vec = 16 / lane_bytes;

    std::array<u8, 16> even{}, odd{};
    for (u32 i = 0; i < lanes_per_vec / 2; i++) {
        for (u32 b = 0; b < lane_bytes; b++) {
            even[i * lane_bytes + b] = static_cast<u8>(2 * i * lane_bytes + b);
            odd[i * lane_bytes + b] = static_cast<u8>((2 * i + 1) * lane_bytes + b);
            even[(lanes_per_vec / 2 + i) * lane_bytes + b] = static_cast<u8>(16 + 2 * i * lane_bytes + b);
            odd[(lanes_per_vec / 2 + i) * lane_bytes + b] = static_cast<u8>(16 + (2 * i + 1) * lane_bytes + b);
        }
    }
    const u32 te = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(even.data(), even.size());
    ctx.code.LocalSet(te);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(odd.data(), odd.size());
    ctx.code.LocalGet(te);
    ctx.code.Simd(add_op);
    ctx.code.LocalSet(dst);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::ZeroVector>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitV128Zero(ctx);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorZeroUpper>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitV128ConstBytes(ctx, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0});
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcast8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I8x16Splat);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcast16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I16x8Splat);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcast32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I32x4Splat);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcast64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I64x2Splat);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastLower8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I8x16Splat);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastLower16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I16x8Splat);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastLower32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I32x4Splat);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastElement8>(EmitContext& ctx, IR::Inst* inst) {
    EmitBroadcastElement(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastElement16>(EmitContext& ctx, IR::Inst* inst) {
    EmitBroadcastElement(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastElement32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBroadcastElement(ctx, inst, 32);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastElement64>(EmitContext& ctx, IR::Inst* inst) {
    EmitBroadcastElement(ctx, inst, 64);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastElementLower8>(EmitContext& ctx, IR::Inst* inst) {
    EmitBroadcastElement(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastElementLower16>(EmitContext& ctx, IR::Inst* inst) {
    EmitBroadcastElement(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorBroadcastElementLower32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBroadcastElement(ctx, inst, 32);
}

template<>
void EmitIR<IR::Opcode::VectorGetElement8>(EmitContext& ctx, IR::Inst* inst) {
    EmitGetElement(ctx, inst, Op::I8x16ExtractLaneU);
}

template<>
void EmitIR<IR::Opcode::VectorGetElement16>(EmitContext& ctx, IR::Inst* inst) {
    EmitGetElement(ctx, inst, Op::I16x8ExtractLaneU);
}

template<>
void EmitIR<IR::Opcode::VectorGetElement32>(EmitContext& ctx, IR::Inst* inst) {
    EmitGetElement(ctx, inst, Op::I32x4ExtractLane);
}

template<>
void EmitIR<IR::Opcode::VectorGetElement64>(EmitContext& ctx, IR::Inst* inst) {
    if (const u32* alias = ctx.ScalarAliasOf(inst->GetArg(0))) {
        // Lane 0 of a scalar64-cached A64 register (see EmitContext::scalar_alias).
        const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
        ctx.code.LocalGet(*alias);
        ctx.code.LocalSet(dst);
        return;
    }
    EmitGetElement(ctx, inst, Op::I64x2ExtractLane);
}

template<>
void EmitIR<IR::Opcode::VectorSetElement8>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetElement(ctx, inst, Op::I8x16ReplaceLane);
}

template<>
void EmitIR<IR::Opcode::VectorSetElement16>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetElement(ctx, inst, Op::I16x8ReplaceLane);
}

template<>
void EmitIR<IR::Opcode::VectorSetElement32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetElement(ctx, inst, Op::I32x4ReplaceLane);
}

template<>
void EmitIR<IR::Opcode::VectorSetElement64>(EmitContext& ctx, IR::Inst* inst) {
    EmitSetElement(ctx, inst, Op::I64x2ReplaceLane);
}

template<>
void EmitIR<IR::Opcode::VectorAnd>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::V128And);
}

template<>
void EmitIR<IR::Opcode::VectorAndNot>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::V128AndNot);
}

template<>
void EmitIR<IR::Opcode::VectorOr>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::V128Or);
}

template<>
void EmitIR<IR::Opcode::VectorEor>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::V128Xor);
}

template<>
void EmitIR<IR::Opcode::VectorNot>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::V128Not);
}

template<>
void EmitIR<IR::Opcode::VectorAdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16Add);
}

template<>
void EmitIR<IR::Opcode::VectorAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8Add);
}

template<>
void EmitIR<IR::Opcode::VectorAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4Add);
}

template<>
void EmitIR<IR::Opcode::VectorAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I64x2Add);
}

template<>
void EmitIR<IR::Opcode::VectorSub8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16Sub);
}

template<>
void EmitIR<IR::Opcode::VectorSub16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8Sub);
}

template<>
void EmitIR<IR::Opcode::VectorSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4Sub);
}

template<>
void EmitIR<IR::Opcode::VectorSub64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I64x2Sub);
}

template<>
void EmitIR<IR::Opcode::VectorMultiply8>(EmitContext& ctx, IR::Inst* inst) {
    EmitMultiply8(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::VectorMultiply16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8Mul);
}

template<>
void EmitIR<IR::Opcode::VectorMultiply32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4Mul);
}

template<>
void EmitIR<IR::Opcode::VectorEqual8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16Eq);
}

template<>
void EmitIR<IR::Opcode::VectorEqual16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8Eq);
}

template<>
void EmitIR<IR::Opcode::VectorEqual32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4Eq);
}

template<>
void EmitIR<IR::Opcode::VectorEqual64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I64x2Eq);
}

template<>
void EmitIR<IR::Opcode::VectorEqual128>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 teq = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I64x2Eq);
    ctx.code.LocalSet(teq);
    // and with the halves swapped: both 64-bit halves must be equal
    ctx.code.LocalGet(teq);
    ctx.code.LocalGet(teq);
    ctx.code.LocalGet(teq);
    ctx.code.Simd(Op::I8x16Shuffle);
    const std::array<u8, 16> swap64 = {8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7};
    ctx.code.Bytes(swap64.data(), swap64.size());
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorGreaterS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16GtS);
}

template<>
void EmitIR<IR::Opcode::VectorGreaterS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8GtS);
}

template<>
void EmitIR<IR::Opcode::VectorGreaterS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4GtS);
}

template<>
void EmitIR<IR::Opcode::VectorGreaterS64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I64x2GtS);
}

template<>
void EmitIR<IR::Opcode::VectorAbs8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I8x16Abs);
}

template<>
void EmitIR<IR::Opcode::VectorAbs16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I16x8Abs);
}

template<>
void EmitIR<IR::Opcode::VectorAbs32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I32x4Abs);
}

template<>
void EmitIR<IR::Opcode::VectorAbs64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I64x2Abs);
}

template<>
void EmitIR<IR::Opcode::VectorPopulationCount>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I8x16Popcnt);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftLeft8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I8x16Shl, 8);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftLeft16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I16x8Shl, 16);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftLeft32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I32x4Shl, 32);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftLeft64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I64x2Shl, 64);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftRight8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I8x16ShrU, 8);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftRight16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I16x8ShrU, 16);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftRight32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I32x4ShrU, 32);
}

template<>
void EmitIR<IR::Opcode::VectorLogicalShiftRight64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I64x2ShrU, 64);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticShiftRight8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I8x16ShrS, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticShiftRight16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I16x8ShrS, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticShiftRight32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I32x4ShrS, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorArithmeticShiftRight64>(EmitContext& ctx, IR::Inst* inst) {
    EmitVShiftImmOp(ctx, inst, Op::I64x2ShrS, 64, true);
}

template<>
void EmitIR<IR::Opcode::VectorMinS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16MinS);
}

template<>
void EmitIR<IR::Opcode::VectorMinS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8MinS);
}

template<>
void EmitIR<IR::Opcode::VectorMinS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4MinS);
}

template<>
void EmitIR<IR::Opcode::VectorMinU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16MinU);
}

template<>
void EmitIR<IR::Opcode::VectorMinU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8MinU);
}

template<>
void EmitIR<IR::Opcode::VectorMinU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4MinU);
}

template<>
void EmitIR<IR::Opcode::VectorMaxS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16MaxS);
}

template<>
void EmitIR<IR::Opcode::VectorMaxS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8MaxS);
}

template<>
void EmitIR<IR::Opcode::VectorMaxS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4MaxS);
}

template<>
void EmitIR<IR::Opcode::VectorMaxU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16MaxU);
}

template<>
void EmitIR<IR::Opcode::VectorMaxU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8MaxU);
}

template<>
void EmitIR<IR::Opcode::VectorMaxU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4MaxU);
}

template<>
void EmitIR<IR::Opcode::VectorNarrow16>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrow(ctx, inst, {0, 2, 4, 6, 8, 10, 12, 14, 16, 16, 16, 16, 16, 16, 16, 16});
}

template<>
void EmitIR<IR::Opcode::VectorNarrow32>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrow(ctx, inst, {0, 1, 4, 5, 8, 9, 12, 13, 16, 16, 16, 16, 16, 16, 16, 16});
}

template<>
void EmitIR<IR::Opcode::VectorNarrow64>(EmitContext& ctx, IR::Inst* inst) {
    EmitNarrow(ctx, inst, {0, 1, 2, 3, 8, 9, 10, 11, 16, 16, 16, 16, 16, 16, 16, 16});
}

template<>
void EmitIR<IR::Opcode::VectorSignExtend8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I16x8ExtendLowI8x16S);
}

template<>
void EmitIR<IR::Opcode::VectorSignExtend16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I32x4ExtendLowI16x8S);
}

template<>
void EmitIR<IR::Opcode::VectorSignExtend32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I64x2ExtendLowI32x4S);
}

template<>
void EmitIR<IR::Opcode::VectorSignExtend64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tsign = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(63);
    ctx.code.Simd(Op::I64x2ShrS);
    ctx.code.LocalSet(tsign);
    // low 64 bits of a, sign-extension (all-ones or all-zeros) in the high half
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalGet(tsign);
    ctx.code.Simd(Op::I8x16Shuffle);
    const std::array<u8, 16> pattern = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
    ctx.code.Bytes(pattern.data(), pattern.size());
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorZeroExtend8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I16x8ExtendLowI8x16U);
}

template<>
void EmitIR<IR::Opcode::VectorZeroExtend16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I32x4ExtendLowI16x8U);
}

template<>
void EmitIR<IR::Opcode::VectorZeroExtend32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I64x2ExtendLowI32x4U);
}

template<>
void EmitIR<IR::Opcode::VectorZeroExtend64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitV128ConstBytes(ctx, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0});
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingAddS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 8, false, true);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingAddS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 16, false, true);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingAddS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 32, false, true);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingAddU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 8, false, false);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingAddU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 16, false, false);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingAddU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 32, false, false);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingHalvingAddS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 8, true, true);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingHalvingAddS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 16, true, true);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingHalvingAddS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 32, true, true);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingHalvingAddU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I8x16AvgrU);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingHalvingAddU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8AvgrU);
}

template<>
void EmitIR<IR::Opcode::VectorRoundingHalvingAddU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingAdd(ctx, inst, 32, true, false);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingSubS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingSub(ctx, inst, 8, true);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingSubS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingSub(ctx, inst, 16, true);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingSubS32>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingSub(ctx, inst, 32, true);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingSubU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingSub(ctx, inst, 8, false);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingSubU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingSub(ctx, inst, 16, false);
}

template<>
void EmitIR<IR::Opcode::VectorHalvingSubU32>(EmitContext& ctx, IR::Inst* inst) {
    EmitHalvingSub(ctx, inst, 32, false);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPairedAdd(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPairedAdd(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitPairedAdd(ctx, inst, 32);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitPairedAdd(ctx, inst, 64);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddSignedWiden8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I16x8ExtAddPairwiseI8x16S);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddSignedWiden16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I32x4ExtAddPairwiseI16x8S);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddUnsignedWiden8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I16x8ExtAddPairwiseI8x16U);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddUnsignedWiden16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVUnOp(ctx, inst, Op::I32x4ExtAddPairwiseI16x8U);
}

// 32->64 pairwise widen: extend the even and odd i32 lanes separately, then
// add as i64x2: [a0+a1, a2+a3].
template<>
void EmitIR<IR::Opcode::VectorPairedAddSignedWiden32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tev = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I8x16Shuffle);
    const std::array<u8, 16> evens = {0, 1, 2, 3, 8, 9, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0};
    ctx.code.Bytes(evens.data(), evens.size());
    ctx.code.Simd(Op::I64x2ExtendLowI32x4S);
    ctx.code.LocalSet(tev);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I8x16Shuffle);
    const std::array<u8, 16> odds = {4, 5, 6, 7, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0};
    ctx.code.Bytes(odds.data(), odds.size());
    ctx.code.Simd(Op::I64x2ExtendLowI32x4S);
    ctx.code.LocalGet(tev);
    ctx.code.Simd(Op::I64x2Add);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddUnsignedWiden32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tev = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I8x16Shuffle);
    const std::array<u8, 16> evens = {0, 1, 2, 3, 8, 9, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0};
    ctx.code.Bytes(evens.data(), evens.size());
    ctx.code.Simd(Op::I64x2ExtendLowI32x4U);
    ctx.code.LocalSet(tev);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I8x16Shuffle);
    const std::array<u8, 16> odds = {4, 5, 6, 7, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0};
    ctx.code.Bytes(odds.data(), odds.size());
    ctx.code.Simd(Op::I64x2ExtendLowI32x4U);
    ctx.code.LocalGet(tev);
    ctx.code.Simd(Op::I64x2Add);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorMultiplySignedWiden8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8ExtMulLowI8x16S);
}

template<>
void EmitIR<IR::Opcode::VectorMultiplySignedWiden16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4ExtMulLowI16x8S);
}

template<>
void EmitIR<IR::Opcode::VectorMultiplySignedWiden32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I64x2ExtMulLowI32x4S);
}

template<>
void EmitIR<IR::Opcode::VectorMultiplyUnsignedWiden8>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I16x8ExtMulLowI8x16U);
}

template<>
void EmitIR<IR::Opcode::VectorMultiplyUnsignedWiden16>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I32x4ExtMulLowI16x8U);
}

template<>
void EmitIR<IR::Opcode::VectorMultiplyUnsignedWiden32>(EmitContext& ctx, IR::Inst* inst) {
    EmitVBinOp(ctx, inst, Op::I64x2ExtMulLowI32x4U);
}

// D-register VPADD: result low half = pairs of a's low 64 bits then pairs of
// b's low 64 bits; upper 64 bits are zero.
void EmitPairedAddLower(EmitContext& ctx, IR::Inst* inst, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 add_op = esize == 8 ? Op::I8x16Add : esize == 16 ? Op::I16x8Add : Op::I32x4Add;
    const u32 lane_bytes = esize / 8;
    // 8 result lanes total: 4 from a's low 64, 4 from b's low 64.
    const u32 lanes = 8 / lane_bytes;

    std::array<u8, 16> even{}, odd{};
    for (u32 i = 0; i < lanes; i++) {
        const u32 src_vec = i < lanes / 2 ? 0 : 16;
        const u32 src_lane = (i % (lanes / 2)) * 2;
        for (u32 b = 0; b < lane_bytes; b++) {
            even[i * lane_bytes + b] = static_cast<u8>(src_vec + src_lane * lane_bytes + b);
            odd[i * lane_bytes + b] = static_cast<u8>(src_vec + (src_lane + 1) * lane_bytes + b);
            even[8 + i * lane_bytes + b] = 16;
            odd[8 + i * lane_bytes + b] = 16;
        }
    }
    const u32 te = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(even.data(), even.size());
    ctx.code.LocalSet(te);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(odd.data(), odd.size());
    ctx.code.LocalGet(te);
    ctx.code.Simd(add_op);
    // zero the upper 64 bits
    EmitV128ConstBytes(ctx, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0});
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddLower8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPairedAddLower(ctx, inst, 8);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddLower16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPairedAddLower(ctx, inst, 16);
}

template<>
void EmitIR<IR::Opcode::VectorPairedAddLower32>(EmitContext& ctx, IR::Inst* inst) {
    EmitPairedAddLower(ctx, inst, 32);
}

}  // namespace Dynarmic::Backend::Wasm
