// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// FPSCR.QC (cumulative saturation) support for the NEON saturating ops, and
// the 64-bit-lane saturating add/sub (no wasm equivalent: built from i64x2
// arithmetic with sign analysis).

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"

namespace Dynarmic::Backend::Wasm {

namespace {

constexpr u32 kQcBit = u32{1} << 27;

struct QcLaneOps {
    u32 add, sub, abs_, neg, eq;
};

QcLaneOps QcOpsFor(u32 esize) {
    switch (esize) {
    case 8:
        return {Op::I8x16Add, Op::I8x16Sub, Op::I8x16Abs, Op::I8x16Neg, Op::I8x16Eq};
    case 16:
        return {Op::I16x8Add, Op::I16x8Sub, Op::I16x8Abs, Op::I16x8Neg, Op::I16x8Eq};
    case 32:
        return {Op::I32x4Add, Op::I32x4Sub, Op::I32x4Abs, Op::I32x4Neg, Op::I32x4Eq};
    case 64:
        return {Op::I64x2Add, Op::I64x2Sub, Op::I64x2Abs, Op::I64x2Neg, Op::I64x2Eq};
    default:
        UNREACHABLE();
    }
}

// Pushes the u128 value as v128 and stores it in a fresh temp.
u32 Spill(EmitContext& ctx, const RegAlloc::Argument& arg) {
    const u32 t = ctx.Temp(ValType::V128);
    EmitPush(ctx.code, ctx.reg_alloc, arg);
    ctx.code.LocalSet(t);
    return t;
}

}  // namespace

void EmitSetQcIfAny(EmitContext& ctx, u32 mask_local) {
    ctx.code.LocalGet(mask_local);
    ctx.code.Simd(Op::V128AnyTrue);
    ctx.code.If();
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, ctx.state.fpsr, 2);
    ctx.code.I32Const(static_cast<s32>(kQcBit));
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, ctx.state.fpsr, 2);
    ctx.code.End();
}

void EmitQcCheck(EmitContext& ctx, IR::Inst* inst, QcKind kind, u32 esize) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = *ctx.reg_alloc.LocalOf(IR::Value{inst});
    const u32 diff = ctx.Temp(ValType::V128);
    const QcLaneOps ops = QcOpsFor(esize);

    switch (kind) {
    case QcKind::Add:
    case QcKind::Sub:
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.Simd(kind == QcKind::Add ? ops.add : ops.sub);
        ctx.code.LocalGet(dst);
        ctx.code.Simd(Op::V128Xor);
        break;
    case QcKind::Abs:
    case QcKind::Neg:
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.Simd(kind == QcKind::Abs ? ops.abs_ : ops.neg);
        ctx.code.LocalGet(dst);
        ctx.code.Simd(Op::V128Xor);
        break;
    case QcKind::NarrowSigned:
    case QcKind::NarrowUnsigned: {
        const bool sign = kind == QcKind::NarrowSigned;
        ctx.code.LocalGet(dst);
        switch (esize) {
        case 16:
            ctx.code.Simd(sign ? Op::I16x8ExtendLowI8x16S : Op::I16x8ExtendLowI8x16U);
            break;
        case 32:
            ctx.code.Simd(sign ? Op::I32x4ExtendLowI16x8S : Op::I32x4ExtendLowI16x8U);
            break;
        case 64:
            ctx.code.Simd(sign ? Op::I64x2ExtendLowI32x4S : Op::I64x2ExtendLowI32x4U);
            break;
        default:
            UNREACHABLE();
        }
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.Simd(Op::V128Xor);
        break;
    }
    }
    ctx.code.LocalSet(diff);
    EmitSetQcIfAny(ctx, diff);
}

namespace {

enum class Sat64 { SignedAdd, SignedSub, UnsignedAdd, UnsignedSub };

void EmitSat64(EmitContext& ctx, IR::Inst* inst, Sat64 kind) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 a = Spill(ctx, args[0]);
    const u32 b = Spill(ctx, args[1]);
    const u32 r = ctx.Temp(ValType::V128);
    const u32 mask = ctx.Temp(ValType::V128);  // lanes that saturated (all ones)

    const bool is_add = kind == Sat64::SignedAdd || kind == Sat64::UnsignedAdd;
    ctx.code.LocalGet(a);
    ctx.code.LocalGet(b);
    ctx.code.Simd(is_add ? Op::I64x2Add : Op::I64x2Sub);
    ctx.code.LocalSet(r);

    switch (kind) {
    case Sat64::SignedAdd:
    case Sat64::SignedSub: {
        // add: ((a ^ r) & (b ^ r)) < 0; sub: ((a ^ b) & (a ^ r)) < 0
        ctx.code.LocalGet(a);
        ctx.code.LocalGet(is_add ? r : b);
        ctx.code.Simd(Op::V128Xor);
        ctx.code.LocalGet(is_add ? b : a);
        ctx.code.LocalGet(r);
        ctx.code.Simd(Op::V128Xor);
        ctx.code.Simd(Op::V128And);
        ctx.code.I32Const(63);
        ctx.code.Simd(Op::I64x2ShrS);
        ctx.code.LocalSet(mask);
        // sat value = (a >>s 63) ^ INT64_MAX
        ctx.code.LocalGet(a);
        ctx.code.I32Const(63);
        ctx.code.Simd(Op::I64x2ShrS);
        VLaneConst(ctx, 64, 0x7FFFFFFFFFFFFFFFull);
        ctx.code.Simd(Op::V128Xor);
        ctx.code.LocalGet(r);
        ctx.code.LocalGet(mask);
        ctx.code.Simd(Op::V128Bitselect);
        ctx.code.LocalSet(dst);
        break;
    }
    case Sat64::UnsignedAdd:
    case Sat64::UnsignedSub: {
        // unsigned compare through the sign-bit flip: add carries iff r <u a,
        // sub borrows iff a <u b
        const u32 lhs = is_add ? a : b;
        const u32 rhs = is_add ? r : a;
        ctx.code.LocalGet(lhs);
        VLaneConst(ctx, 64, 0x8000000000000000ull);
        ctx.code.Simd(Op::V128Xor);
        ctx.code.LocalGet(rhs);
        VLaneConst(ctx, 64, 0x8000000000000000ull);
        ctx.code.Simd(Op::V128Xor);
        ctx.code.Simd(Op::I64x2GtS);  // lhs >u rhs
        ctx.code.LocalSet(mask);
        if (is_add) {
            ctx.code.LocalGet(r);
            ctx.code.LocalGet(mask);
            ctx.code.Simd(Op::V128Or);
        } else {
            ctx.code.LocalGet(r);
            ctx.code.LocalGet(mask);
            ctx.code.Simd(Op::V128AndNot);
        }
        ctx.code.LocalSet(dst);
        break;
    }
    }
    EmitSetQcIfAny(ctx, mask);
}

}  // namespace

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitSat64(ctx, inst, Sat64::SignedAdd);
}

template<>
void EmitIR<IR::Opcode::VectorSignedSaturatedSub64>(EmitContext& ctx, IR::Inst* inst) {
    EmitSat64(ctx, inst, Sat64::SignedSub);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitSat64(ctx, inst, Sat64::UnsignedAdd);
}

template<>
void EmitIR<IR::Opcode::VectorUnsignedSaturatedSub64>(EmitContext& ctx, IR::Inst* inst) {
    EmitSat64(ctx, inst, Sat64::UnsignedSub);
}

}  // namespace Dynarmic::Backend::Wasm
