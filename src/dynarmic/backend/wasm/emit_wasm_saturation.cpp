// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Scalar min/max, saturating arithmetic and saturation ops (ARMv6/v7 media
// instructions: QADD/QSUB/SSAT/USAT/SEL and friends). Overflow is either
// returned through the GetOverflowFromOp pseudo-op (frontend folds it into
// CPSR.Q via A32OrQFlag) or, matching the x64 backend, ORed into the sticky
// FPSCR QC bit in the state struct.

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// select(v1, v2, cond) leaves cond ? v1 : v2.
void EmitSelect(EmitContext& ctx) {
    ctx.code.U8(Op::Select);
}

// Emit a signed/unsigned min/max select over two pushed i32/i64 values.
// Expects nothing on the stack; pushes pushes the two args itself.
void EmitMinMax(EmitContext& ctx, IR::Inst* inst, u8 cmp_op, bool pick_a_on_true) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    if (pick_a_on_true) {
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);  // v1 = a
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // v2 = b
    } else {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    }
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(cmp_op);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);
}

// Clamp the i32 on top of the stack to [lo, hi].
void EmitClampI32(EmitContext& ctx, s32 lo, s32 hi) {
    const u32 t = ctx.Temp(ValType::I32);
    ctx.code.LocalSet(t);
    // min(v, hi) = (v > hi) ? hi : v
    ctx.code.I32Const(hi);
    ctx.code.LocalGet(t);
    ctx.code.LocalGet(t);
    ctx.code.I32Const(hi);
    ctx.code.U8(Op::I32GtS);
    EmitSelect(ctx);
    ctx.code.LocalSet(t);
    // max(v, lo) = (v < lo) ? lo : v
    ctx.code.I32Const(lo);
    ctx.code.LocalGet(t);
    ctx.code.LocalGet(t);
    ctx.code.I32Const(lo);
    ctx.code.U8(Op::I32LtS);
    EmitSelect(ctx);
}

// Route the overflow bit (i32 0/1 on top of stack): to the associated
// GetOverflowFromOp pseudo-op if present, else -- for the saturating
// arithmetic ops (A64 scalar SQADD etc.), not for the WithFlag variants or
// SignedSaturation / UnsignedSaturation (SSAT / USAT) -- OR it into FPSCR.QC in the
// state struct (x64's fpsr_qc behaviour).
void EmitOverflowFlag(EmitContext& ctx, IR::Inst* inst, bool qc = true) {
    if (IR::Inst* ovf_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp)) {
        const u32 ovf = ctx.reg_alloc.DefineLocal(ctx.code, ovf_inst);
        ctx.code.LocalSet(ovf);
        return;
    }
    if (!qc) {
        ctx.code.U8(Op::Drop);
        return;
    }
    // fpsr |= ovf << 27
    const u32 t = ctx.Temp(ValType::I32);
    ctx.code.LocalSet(t);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, ctx.state.fpsr, 2);
    ctx.code.LocalGet(t);
    ctx.code.I32Const(27);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, ctx.state.fpsr, 2);
}

// Signed saturating add/sub on full 32-bit operands. The i32 result wraps, so
// clamping it can never detect overflow; use sign analysis instead:
//   add: ovf = ((a^r) & (b^r)) bit31;  sub: ovf = ((a^b) & (a^r)) bit31
//   boundary = 0x7FFFFFFF ^ (a >>s 31)  (a<0 -> INT32_MIN, else INT32_MAX)
//   result = r ^ ((r ^ boundary) & (ovf_bits >>s 31))
void EmitSignedSatOp32Wide(EmitContext& ctx, IR::Inst* inst, bool sub, bool qc = true) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 tr = ctx.Temp(ValType::I32);
    const u32 tm = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(sub ? Op::I32Sub : Op::I32Add);
    ctx.code.LocalSet(tr);

    // tm = overflow sign bits
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(sub ? tb : tr);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalGet(sub ? ta : tb);
    ctx.code.LocalGet(tr);
    ctx.code.U8(Op::I32Xor);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(tm);

    // dst = tr ^ ((tr ^ boundary) & (tm >>s 31))
    ctx.code.LocalGet(tr);
    ctx.code.LocalGet(tr);
    ctx.code.LocalGet(ta);
    ctx.code.I32Const(31);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.I32Const(0x7FFFFFFF);
    ctx.code.U8(Op::I32Xor);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalGet(tm);
    ctx.code.I32Const(31);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(dst);

    // overflow flag = tm >>> 31
    ctx.code.LocalGet(tm);
    ctx.code.I32Const(31);
    ctx.code.U8(Op::I32ShrU);
    EmitOverflowFlag(ctx, inst, qc);
}

// Signed saturating add/sub in the i32 domain (element sizes 8/16/32).
// sign_ext_op: Op::I32Extend8S / Op::I32Extend16S, or 0 for full 32-bit.
void EmitSignedSatOp32(EmitContext& ctx, IR::Inst* inst, bool sub, u8 sign_ext_op, u32 mask, s32 lo, s32 hi) {
    if (sign_ext_op == 0) {
        EmitSignedSatOp32Wide(ctx, inst, sub);
        return;
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tr = ctx.Temp(ValType::I32);
    const u32 tc = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    if (sign_ext_op != 0) {
        ctx.code.U8(sign_ext_op);
    }
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    if (sign_ext_op != 0) {
        ctx.code.U8(sign_ext_op);
    }
    ctx.code.U8(sub ? Op::I32Sub : Op::I32Add);
    ctx.code.LocalSet(tr);

    // clamped = clamp(r, lo, hi); dst = clamped & mask
    ctx.code.LocalGet(tr);
    EmitClampI32(ctx, lo, hi);
    ctx.code.LocalSet(tc);
    ctx.code.LocalGet(tc);
    if (mask != 0xFFFFFFFFu) {
        ctx.code.I32Const(static_cast<s32>(mask));
        ctx.code.U8(Op::I32And);
    }
    ctx.code.LocalSet(dst);

    // overflow = clamped != r (compare before masking: e.g. r=-1 must not
    // look like an overflow just because dst = 0xFF)
    ctx.code.LocalGet(tc);
    ctx.code.LocalGet(tr);
    ctx.code.U8(Op::I32Ne);
    EmitOverflowFlag(ctx, inst);
}

// Signed saturating add/sub on i64 (no wider type available): overflow iff
// the result sign differs from what the operands imply.
void EmitSignedSatOp64(EmitContext& ctx, IR::Inst* inst, bool sub) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I64);
    const u32 tb = ctx.Temp(ValType::I64);
    const u32 tr = ctx.Temp(ValType::I64);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(sub ? Op::I64Sub : Op::I64Add);
    ctx.code.LocalSet(tr);

    // ovf (i64 all-ones/zero):
    //   add: (a ^ r) & (b ^ r)
    //   sub: (a ^ b) & (a ^ r)
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tr);
    ctx.code.U8(Op::I64Xor);
    ctx.code.LocalGet(sub ? ta : tb);
    ctx.code.LocalGet(sub ? tb : tr);
    ctx.code.U8(Op::I64Xor);
    ctx.code.U8(Op::I64And);
    ctx.code.I64Const(63);
    ctx.code.U8(Op::I64ShrS);
    ctx.code.LocalSet(tb);  // reuse tb as ovf mask

    // boundary = (a < 0) ? INT64_MIN : INT64_MAX  (v1 = MIN: cond picks it)
    ctx.code.I64Const(static_cast<s64>(0x8000000000000000ULL));
    ctx.code.I64Const(0x7FFFFFFFFFFFFFFFLL);
    ctx.code.LocalGet(ta);
    ctx.code.I64Const(63);
    ctx.code.U8(Op::I64ShrS);
    ctx.code.U8(Op::I32WrapI64);
    EmitSelect(ctx);  // boundary
    ctx.code.LocalGet(tr);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32WrapI64);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);

    // overflow flag = ovf != 0
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I64Eqz);
    ctx.code.U8(Op::I32Eqz);
    EmitOverflowFlag(ctx, inst);
}

// Unsigned saturating add in the i32 domain (element sizes 8/16).
void EmitUnsignedSatAddNarrow(EmitContext& ctx, IR::Inst* inst, u32 mask) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ts = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(mask));
    ctx.code.U8(Op::I32And);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.I32Const(static_cast<s32>(mask));
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Add);
    ctx.code.LocalSet(ts);

    // carry = sum > mask; result = carry ? mask : sum
    ctx.code.I32Const(static_cast<s32>(mask));
    ctx.code.LocalGet(ts);
    ctx.code.LocalGet(ts);
    ctx.code.I32Const(static_cast<s32>(mask));
    ctx.code.U8(Op::I32GtU);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(ts);
    ctx.code.I32Const(static_cast<s32>(mask));
    ctx.code.U8(Op::I32GtU);
    EmitOverflowFlag(ctx, inst);
}

// Unsigned saturating sub in the i32 domain (element sizes 8/16/32).
void EmitUnsignedSatSub32(EmitContext& ctx, IR::Inst* inst, u32 mask) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    if (mask != 0xFFFFFFFFu) {
        ctx.code.I32Const(static_cast<s32>(mask));
        ctx.code.U8(Op::I32And);
    }
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    if (mask != 0xFFFFFFFFu) {
        ctx.code.I32Const(static_cast<s32>(mask));
        ctx.code.U8(Op::I32And);
    }
    ctx.code.LocalSet(tb);

    // borrow = a < b; result = borrow ? 0 : a - b
    ctx.code.I32Const(0);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Sub);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32LtU);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32LtU);
    EmitOverflowFlag(ctx, inst);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::MaxSigned32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I32GtS, true);
}

template<>
void EmitIR<IR::Opcode::MaxSigned64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I64GtS, true);
}

template<>
void EmitIR<IR::Opcode::MaxUnsigned32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I32GtU, true);
}

template<>
void EmitIR<IR::Opcode::MaxUnsigned64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I64GtU, true);
}

template<>
void EmitIR<IR::Opcode::MinSigned32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I32LtS, true);
}

template<>
void EmitIR<IR::Opcode::MinSigned64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I64LtS, true);
}

template<>
void EmitIR<IR::Opcode::MinUnsigned32>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I32LtU, true);
}

template<>
void EmitIR<IR::Opcode::MinUnsigned64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMinMax(ctx, inst, Op::I64LtU, true);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedAddWithFlag32>(EmitContext& ctx, IR::Inst* inst) {
    // This opcode exposes overflow as a value; removing that value must not
    // turn integer CPSR.Q arithmetic into a write of the unrelated FPSCR.QC.
    EmitSignedSatOp32Wide(ctx, inst, false, false);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedSubWithFlag32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp32Wide(ctx, inst, true, false);
}

template<>
void EmitIR<IR::Opcode::SignedSaturation>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const size_t N = args[1].GetImmediateU8();
    ASSERT(N >= 1 && N <= 32);

    if (N == 32) {
        // identity, never overflows
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.LocalSet(dst);
        ctx.code.I32Const(0);
        EmitOverflowFlag(ctx, inst, false);
        return;
    }

    const s32 lo = -(1 << (N - 1));
    const s32 hi = (1 << (N - 1)) - 1;
    const u32 tx = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(tx);
    ctx.code.LocalGet(tx);
    EmitClampI32(ctx, lo, hi);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(dst);
    ctx.code.LocalGet(tx);
    ctx.code.U8(Op::I32Ne);
    EmitOverflowFlag(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturation>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const size_t N = args[1].GetImmediateU8();
    ASSERT(N <= 32);  // USAT #0 saturates to [0, 0]

    const u32 tx = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(tx);

    // The operand is signed (frontend sign-extends it): clamp low first.
    // max(x, 0)
    ctx.code.I32Const(0);
    ctx.code.LocalGet(tx);
    ctx.code.LocalGet(tx);
    ctx.code.I32Const(0);
    ctx.code.U8(Op::I32LtS);
    EmitSelect(ctx);
    ctx.code.LocalSet(tx);
    if (N != 32) {
        // min(x, 2^N - 1) (value is non-negative now, signed compare is fine)
        const s32 hi = static_cast<s32>((1u << N) - 1);
        ctx.code.I32Const(hi);
        ctx.code.LocalGet(tx);
        ctx.code.LocalGet(tx);
        ctx.code.I32Const(hi);
        ctx.code.U8(Op::I32GtS);
        EmitSelect(ctx);
    } else {
        ctx.code.LocalGet(tx);
    }
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(dst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Ne);
    EmitOverflowFlag(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedAdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp32(ctx, inst, false, Op::I32Extend8S, 0xFF, -128, 127);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp32(ctx, inst, false, Op::I32Extend16S, 0xFFFF, -32768, 32767);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedAdd32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp32(ctx, inst, false, 0, 0xFFFFFFFFu, static_cast<s32>(0x80000000), 0x7FFFFFFF);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedAdd64>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp64(ctx, inst, false);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedSub8>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp32(ctx, inst, true, Op::I32Extend8S, 0xFF, -128, 127);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedSub16>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp32(ctx, inst, true, Op::I32Extend16S, 0xFFFF, -32768, 32767);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp32(ctx, inst, true, 0, 0xFFFFFFFFu, static_cast<s32>(0x80000000), 0x7FFFFFFF);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedSub64>(EmitContext& ctx, IR::Inst* inst) {
    EmitSignedSatOp64(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedAdd8>(EmitContext& ctx, IR::Inst* inst) {
    EmitUnsignedSatAddNarrow(ctx, inst, 0xFF);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedAdd16>(EmitContext& ctx, IR::Inst* inst) {
    EmitUnsignedSatAddNarrow(ctx, inst, 0xFFFF);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedAdd32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ts = ctx.Temp(ValType::I64);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I64ExtendI32U);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.U8(Op::I64Add);
    ctx.code.LocalSet(ts);

    // carry = sum > 0xFFFFFFFF; result = carry ? 0xFFFFFFFF : (u32)sum
    ctx.code.I32Const(-1);
    ctx.code.LocalGet(ts);
    ctx.code.U8(Op::I32WrapI64);
    ctx.code.LocalGet(ts);
    ctx.code.I64Const(0x100000000ULL);
    ctx.code.U8(Op::I64GeU);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(ts);
    ctx.code.I64Const(0x100000000ULL);
    ctx.code.U8(Op::I64GeU);
    EmitOverflowFlag(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedAdd64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I64);
    const u32 tr = ctx.Temp(ValType::I64);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalGet(ta);
    ctx.code.U8(Op::I64Add);
    ctx.code.LocalSet(tr);

    // carry = r < a (unsigned); result = carry ? ~0 : r
    ctx.code.I64Const(-1);
    ctx.code.LocalGet(tr);
    ctx.code.LocalGet(tr);
    ctx.code.LocalGet(ta);
    ctx.code.U8(Op::I64LtU);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(tr);
    ctx.code.LocalGet(ta);
    ctx.code.U8(Op::I64LtU);
    EmitOverflowFlag(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedSub8>(EmitContext& ctx, IR::Inst* inst) {
    EmitUnsignedSatSub32(ctx, inst, 0xFF);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedSub16>(EmitContext& ctx, IR::Inst* inst) {
    EmitUnsignedSatSub32(ctx, inst, 0xFFFF);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedSub32>(EmitContext& ctx, IR::Inst* inst) {
    EmitUnsignedSatSub32(ctx, inst, 0xFFFFFFFFu);
}

template<>
void EmitIR<IR::Opcode::UnsignedSaturatedSub64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I64);
    const u32 tb = ctx.Temp(ValType::I64);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);

    ctx.code.I64Const(0);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I64Sub);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I64LtU);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I64LtU);
    EmitOverflowFlag(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedDoublingMultiplyReturnHigh16>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tp = ctx.Temp(ValType::I32);
    const u32 tsat = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Extend16S);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(Op::I32Extend16S);
    ctx.code.U8(Op::I32Mul);
    ctx.code.LocalSet(tp);

    // saturation only when p == 2^30 (x = y = -32768)
    ctx.code.LocalGet(tp);
    ctx.code.I32Const(1 << 30);
    ctx.code.U8(Op::I32Eq);
    ctx.code.LocalSet(tsat);

    // result = sat ? 0x7FFF : ((u32)p >> 15) & 0xFFFF
    ctx.code.I32Const(0x7FFF);
    ctx.code.LocalGet(tp);
    ctx.code.I32Const(15);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.I32Const(0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(tsat);
    EmitSelect(ctx);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(tsat);
    EmitOverflowFlag(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::SignedSaturatedDoublingMultiplyReturnHigh32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tp = ctx.Temp(ValType::I64);
    const u32 tsat = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I64ExtendI32S);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(Op::I64ExtendI32S);
    ctx.code.U8(Op::I64Mul);
    ctx.code.LocalSet(tp);

    // saturation only when p == 2^62 (x = y = INT32_MIN)
    ctx.code.LocalGet(tp);
    ctx.code.I64Const(1LL << 62);
    ctx.code.U8(Op::I64Eq);
    ctx.code.LocalSet(tsat);

    // result = sat ? 0x7FFFFFFF : (u32)((u64)p >> 31)
    ctx.code.I64Const(0x7FFFFFFF);
    ctx.code.LocalGet(tp);
    ctx.code.I64Const(31);
    ctx.code.U8(Op::I64ShrU);
    ctx.code.I64Const(0xFFFFFFFF);
    ctx.code.U8(Op::I64And);
    ctx.code.LocalGet(tsat);
    EmitSelect(ctx);
    ctx.code.U8(Op::I32WrapI64);
    ctx.code.LocalSet(dst);

    ctx.code.LocalGet(tsat);
    EmitOverflowFlag(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::PackedSelect>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tge = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(tge);
    // (b & ge) | (a & ~ge)
    EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    ctx.code.LocalGet(tge);
    ctx.code.U8(Op::I32And);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalGet(tge);
    ctx.code.I32Const(-1);
    ctx.code.U8(Op::I32Xor);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalSet(dst);
}

}  // namespace Dynarmic::Backend::Wasm
