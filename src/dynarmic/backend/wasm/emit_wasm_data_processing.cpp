// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Scalar data-processing opcodes: ALU, shifts (with ARM carry semantics),
// multiplies, extends, bit utilities, conditional selects.

#include <utility>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// Emits a plain binary op: dst = a <wasmop> b
template<u8 Op32>
void EmitBinop32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(Op32);
    ctx.code.LocalSet(dst);
}

template<u8 Op64>
void EmitBinop64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(Op64);
    ctx.code.LocalSet(dst);
}

// select(t, f, cond): pushes [t_value, f_value, cond] then select.
// `emit_t`/`emit_f` must each push exactly one value of the same type.
template<typename F, typename G, typename H>
void EmitSelect(FuncBody& code, F&& emit_t, G&& emit_f, H&& emit_cond) {
    emit_t();
    emit_f();
    emit_cond();
    code.U8(Op::Select);
}

// ---------------------------------------------------------------------------
// 32-bit add/sub with carry-in and flag fusion
// ---------------------------------------------------------------------------

// Pushes overflow flag of a±b=result (32-bit): add: (~(a^b) & (a^r)) >>> 31,
// sub: ((a^b) & (a^r)) >>> 31. a, b, r are pushed by the callers' lambdas.
template<typename A, typename B, typename R>
void EmitOverflow32Add(FuncBody& code, A&& a, B&& b, R&& r) {
    a();
    b();
    code.U8(Op::I32Xor);
    code.I32Const(-1);
    code.U8(Op::I32Xor);
    a();
    r();
    code.U8(Op::I32Xor);
    code.U8(Op::I32And);
    code.I32Const(31);
    code.U8(Op::I32ShrU);
}

template<typename A, typename B, typename R>
void EmitOverflow32Sub(FuncBody& code, A&& a, B&& b, R&& r) {
    a();
    b();
    code.U8(Op::I32Xor);
    a();
    r();
    code.U8(Op::I32Xor);
    code.U8(Op::I32And);
    code.I32Const(31);
    code.U8(Op::I32ShrU);
}

// Pushes NZCV (ARM order, bits 31..28) from parts.
template<typename N, typename Z, typename C, typename V>
void EmitPackNzcv32(FuncBody& code, N&& n, Z&& z, C&& c, V&& v) {
    n();
    code.I32Const(31);
    code.U8(Op::I32Shl);
    z();
    code.I32Const(30);
    code.U8(Op::I32Shl);
    code.U8(Op::I32Or);
    c();
    code.I32Const(29);
    code.U8(Op::I32Shl);
    code.U8(Op::I32Or);
    v();
    code.I32Const(28);
    code.U8(Op::I32Shl);
    code.U8(Op::I32Or);
}

// Deferred-flags producer (see EmitContext::DeferredFlags): plain i32 add/sub
// (the carry-in is the constant the analysis verified), operands and result
// recorded for later flag evaluation. GetNZCVFromOp gets a dead placeholder
// local; its only consumer (A32SetCpsrNZCV) is elided.
void EmitDeferredFlagsAddSub(EmitContext& ctx, IR::Inst* inst, bool is_sub) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(ctx.flags.a);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.LocalSet(ctx.flags.b);
    code.LocalGet(ctx.flags.a);
    code.LocalGet(ctx.flags.b);
    if (ctx.flags.is64) {
        code.U8(is_sub ? Op::I64Sub : Op::I64Add);
    } else {
        code.U8(is_sub ? Op::I32Sub : Op::I32Add);
    }
    code.LocalTee(dst);
    code.LocalSet(ctx.flags.res);
    code.I32Const(1);
    code.LocalSet(ctx.flags.valid);
    code.I32Const(0);
    code.LocalSet(ctx.reg_alloc.DefineLocal(code, ctx.flags.nzcv));
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Packing / extraction
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::Pack2x32To1x64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.I64Const(32);
    ctx.code.U8(Op::I64Shl);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.U8(Op::I64Or);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::Pack2x64To1x128>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);  // lane 0 = lo
    ctx.code.Simd(Op::I64x2Splat);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I64x2ReplaceLane);
    ctx.code.U8(1);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::LeastSignificantWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32WrapI64);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::LeastSignificantHalf>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::LeastSignificantByte>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0xFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
// GetCarryFromOp of MostSignificantWord is bit 31 of the discarded low word
// (the rounding carry of SMMULR / SMMLAR / SMMLSR).
void EmitIR<IR::Opcode::MostSignificantWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I64Const(32);
    ctx.code.U8(Op::I64ShrU);
    ctx.code.U8(Op::I32WrapI64);
    ctx.code.LocalSet(dst);
    if (IR::Inst* carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp)) {
        const u32 carry = ctx.reg_alloc.DefineLocal(ctx.code, carry_inst);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.I64Const(31);
        ctx.code.U8(Op::I64ShrU);
        ctx.code.U8(Op::I32WrapI64);
        ctx.code.I32Const(1);
        ctx.code.U8(Op::I32And);
        ctx.code.LocalSet(carry);
    }
}

template<>
void EmitIR<IR::Opcode::MostSignificantBit>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(31);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Predicates
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::IsZero32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Eqz);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::IsZero64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I64Eqz);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::TestBit>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.U8(Op::I64ShrU);
    ctx.code.I64Const(1);
    ctx.code.U8(Op::I64And);
    ctx.code.U8(Op::I32WrapI64);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Conditional select
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::ConditionalSelect32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    EmitCond(ctx, args[0].GetImmediateCond());
    ctx.code.U8(Op::Select);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ConditionalSelect64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    EmitCond(ctx, args[0].GetImmediateCond());
    ctx.code.U8(Op::Select);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ConditionalSelectNZCV>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    EmitCond(ctx, args[0].GetImmediateCond());
    ctx.code.U8(Op::Select);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Shifts with ARM carry-out semantics
// ---------------------------------------------------------------------------

namespace {

enum class ShiftKind { Lsl, Lsr, Asr, Ror, Rrx };

// value: args[0], shift: args[1] (U8), carry_in: args[2] (U1).
// Computes the result into inst's local and the carry-out into the associated
// GetCarryFromOp inst's local (if any).
template<bool Is64>
void EmitShiftWithCarry(EmitContext& ctx, IR::Inst* inst, ShiftKind kind) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    IR::Inst* carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);

    FuncBody& code = ctx.code;
    const ValType vt = Is64 ? ValType::I64 : ValType::I32;
    const u8 ext_op = Is64 ? Op::I64ExtendI32U : Op::Nop;
    const u8 shl = Is64 ? Op::I64Shl : Op::I32Shl;
    const u8 shr_u = Is64 ? Op::I64ShrU : Op::I32ShrU;
    const u8 shr_s = Is64 ? Op::I64ShrS : Op::I32ShrS;
    const u8 rotr = Is64 ? Op::I64Rotr : Op::I32Rotr;
    const u8 eqz = Is64 ? Op::I64Eqz : Op::I32Eqz;
    const u8 and_op = Is64 ? Op::I64And : Op::I32And;
    const u8 or_op = Is64 ? Op::I64Or : Op::I32Or;
    const u8 sub_op = Is64 ? Op::I64Sub : Op::I32Sub;
    const u8 lt_u = Is64 ? Op::I64LtU : Op::I32LtU;
    const u32 bits = Is64 ? 64 : 32;

    auto push_s = [&] {
        EmitPush(code, ctx.reg_alloc, args[1]);
        if (Is64) {
            code.U8(ext_op);
        }
    };
    // Carry-in: RotateRightExtended(value, carry) has no shift amount.
    auto push_c = [&] {
        EmitPush(code, ctx.reg_alloc, args[kind == ShiftKind::Rrx ? 1 : 2]);
        if (Is64) {
            code.U8(ext_op);
        }
    };
    auto const_n = [&](u32 v) {
        if (Is64) {
            code.I64Const(v);
        } else {
            code.I32Const(static_cast<s32>(v));
        }
    };

    const u32 v = ctx.Temp(vt);  // value
    const u32 s = ctx.Temp(vt);  // shift amount
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(v);
    if (kind != ShiftKind::Rrx) {
        push_s();
        code.LocalSet(s);
    }

    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    if (kind == ShiftKind::Rrx) {
        // result = (c << (bits-1)) | (v >> 1); carry = v & 1
        const u32 r = ctx.Temp(vt);
        push_c();
        const_n(bits - 1);
        code.U8(shl);
        code.LocalGet(v);
        const_n(1);
        code.U8(shr_u);
        code.U8(or_op);
        code.LocalTee(r);
        if (Is64) {
            code.U8(Op::I32WrapI64);
        }
        code.LocalSet(dst);
        if (carry_inst) {
            const u32 cout = ctx.reg_alloc.DefineLocal(code, carry_inst);
            code.LocalGet(v);
            const_n(1);
            code.U8(and_op);
            if (Is64) {
                code.U8(Op::I32WrapI64);
            }
            code.LocalSet(cout);
        }
        return;
    }

    // result --------------------------------------------------------------
    if (args[1].IsImmediate()) {
        // Constant amount (the common case): the range cases resolve now.
        const u32 amount = args[1].GetImmediateU8();
        if (amount == 0) {
            code.LocalGet(v);
        } else if (kind == ShiftKind::Ror) {
            code.LocalGet(v);
            const_n(amount % bits);
            code.U8(rotr);
        } else if (amount < bits) {
            code.LocalGet(v);
            const_n(amount);
            code.U8(kind == ShiftKind::Lsl ? shl : (kind == ShiftKind::Asr ? shr_s : shr_u));
        } else if (kind == ShiftKind::Asr) {
            code.LocalGet(v);
            const_n(bits - 1);
            code.U8(shr_s);
        } else {
            const_n(0);
        }
    } else if (kind == ShiftKind::Ror) {
        // s==0 ? v : rotr(v, s)
        code.LocalGet(v);
        code.LocalGet(v);
        code.LocalGet(s);
        code.U8(rotr);
        code.LocalGet(s);
        code.U8(eqz);
        code.U8(Op::Select);
    } else {
        // s==0 ? v : (s<bits ? op(v,s) : fill)
        // inner = select(op(v,s), fill, s<bits)
        const bool is_lsl = kind == ShiftKind::Lsl;
        const bool is_asr = kind == ShiftKind::Asr;
        code.LocalGet(v);  // outer true value
        code.LocalGet(v);
        code.LocalGet(s);
        code.U8(is_lsl ? shl : (is_asr ? shr_s : shr_u));  // masked, valid when s<bits
        if (is_asr) {
            // fill = sar(v, bits-1) (sign extension)
            code.LocalGet(v);
            const_n(bits - 1);
            code.U8(shr_s);
        } else {
            const_n(0);
        }
        code.LocalGet(s);
        const_n(bits);
        code.U8(lt_u);
        code.U8(Op::Select);
        code.LocalGet(s);
        code.U8(eqz);
        code.U8(Op::Select);
    }
    code.LocalSet(dst);

    // carry ---------------------------------------------------------------
    if (!carry_inst) {
        return;
    }
    const u32 cout = ctx.reg_alloc.DefineLocal(code, carry_inst);

    if (kind == ShiftKind::Ror) {
        // s==0 ? c : result >> (bits-1)
        push_c();
        code.LocalGet(dst);
        const_n(bits - 1);
        code.U8(shr_u);
        code.LocalGet(s);
        code.U8(eqz);
        code.U8(Op::Select);
        if (Is64) {
            code.U8(Op::I32WrapI64);
        }
        code.LocalSet(cout);
        return;
    }

    // carry = s==0 ? c : (s<=bits ? bit shifted out : (asr ? signbit : 0))
    // For 1<=s<=bits: LSL: (v << (s-1)) >>> (bits-1); LSR/ASR: (v >> (s-1)) & 1
    push_c();
    {
        // inner
        code.LocalGet(v);
        code.LocalGet(s);
        const_n(1);
        code.U8(sub_op);
        code.U8(kind == ShiftKind::Lsl ? shl : (kind == ShiftKind::Asr ? shr_s : shr_u));
        if (kind == ShiftKind::Lsl) {
            const_n(bits - 1);
            code.U8(shr_u);
        } else {
            const_n(1);
            code.U8(and_op);
        }
        // fill for s>bits
        if (kind == ShiftKind::Asr) {
            code.LocalGet(v);
            const_n(bits - 1);
            code.U8(shr_s);
            const_n(1);
            code.U8(and_op);
        } else {
            const_n(0);
        }
        // cond s <= bits  ⟺  s < bits+1
        code.LocalGet(s);
        const_n(bits + 1);
        code.U8(lt_u);
        code.U8(Op::Select);
    }
    code.LocalGet(s);
    code.U8(eqz);
    code.U8(Op::Select);
    if (Is64) {
        code.U8(Op::I32WrapI64);
    }
    code.LocalSet(cout);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::LogicalShiftLeft32>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<false>(ctx, inst, ShiftKind::Lsl);
}

template<>
void EmitIR<IR::Opcode::LogicalShiftLeft64>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<true>(ctx, inst, ShiftKind::Lsl);
}

template<>
void EmitIR<IR::Opcode::LogicalShiftRight32>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<false>(ctx, inst, ShiftKind::Lsr);
}

template<>
void EmitIR<IR::Opcode::LogicalShiftRight64>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<true>(ctx, inst, ShiftKind::Lsr);
}

template<>
void EmitIR<IR::Opcode::ArithmeticShiftRight32>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<false>(ctx, inst, ShiftKind::Asr);
}

template<>
void EmitIR<IR::Opcode::ArithmeticShiftRight64>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<true>(ctx, inst, ShiftKind::Asr);
}

template<>
void EmitIR<IR::Opcode::RotateRight32>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<false>(ctx, inst, ShiftKind::Ror);
}

template<>
void EmitIR<IR::Opcode::RotateRight64>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<true>(ctx, inst, ShiftKind::Ror);
}

template<>
void EmitIR<IR::Opcode::RotateRightExtended>(EmitContext& ctx, IR::Inst* inst) {
    EmitShiftWithCarry<false>(ctx, inst, ShiftKind::Rrx);
}

// Masked shifts: WASM shift counts are masked by the hardware, so these map
// directly onto the native shift ops.
template<>
void EmitIR<IR::Opcode::LogicalShiftLeftMasked32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32Shl>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::LogicalShiftLeftMasked64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // U64 amount
    ctx.code.U8(Op::I64Shl);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::LogicalShiftRightMasked32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32ShrU>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::LogicalShiftRightMasked64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // U64 amount
    ctx.code.U8(Op::I64ShrU);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ArithmeticShiftRightMasked32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32ShrS>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::ArithmeticShiftRightMasked64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // U64 amount
    ctx.code.U8(Op::I64ShrS);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::RotateRightMasked32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32Rotr>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::RotateRightMasked64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // U64 amount
    ctx.code.U8(Op::I64Rotr);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Add / Sub
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::Add32>(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.flags.active && inst == ctx.flags.producer) {
        EmitDeferredFlagsAddSub(ctx, inst, false);
        return;
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    IR::Inst* carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);
    IR::Inst* overflow_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp);
    IR::Inst* nzcv_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);

    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    if (!carry_inst && !nzcv_inst) {
        // No carry needed: plain i32 add.
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(Op::I32Add);
        EmitPush(code, ctx.reg_alloc, args[2]);
        code.U8(Op::I32Add);
        code.LocalSet(dst);
        if (overflow_inst) {
            const u32 ov = ctx.reg_alloc.DefineLocal(code, overflow_inst);
            EmitOverflow32Add(code,
                              [&] { EmitPush(code, ctx.reg_alloc, args[0]); },
                              [&] { EmitPush(code, ctx.reg_alloc, args[1]); },
                              [&] { code.LocalGet(dst); });
            code.LocalSet(ov);
        }
        return;
    }

    // Full path: compute in i64 to extract carry.
    const u32 sum = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.U8(Op::I64ExtendI32U);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64Add);
    EmitPush(code, ctx.reg_alloc, args[2]);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64Add);
    code.LocalSet(sum);

    code.LocalGet(sum);
    code.U8(Op::I32WrapI64);
    code.LocalSet(dst);

    const u32 carry = ctx.Temp(ValType::I32);
    code.LocalGet(sum);
    code.I64Const(32);
    code.U8(Op::I64ShrU);
    code.U8(Op::I32WrapI64);
    code.LocalSet(carry);

    const u32 overflow = ctx.Temp(ValType::I32);
    EmitOverflow32Add(code,
                      [&] { EmitPush(code, ctx.reg_alloc, args[0]); },
                      [&] { EmitPush(code, ctx.reg_alloc, args[1]); },
                      [&] { code.LocalGet(dst); });
    code.LocalSet(overflow);

    if (carry_inst) {
        code.LocalGet(carry);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, carry_inst));
    }
    if (overflow_inst) {
        code.LocalGet(overflow);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, overflow_inst));
    }
    if (nzcv_inst) {
        EmitPackNzcv32(code,
                       [&] {
                           code.LocalGet(dst);
                           code.I32Const(31);
                           code.U8(Op::I32ShrU);
                       },
                       [&] {
                           code.LocalGet(dst);
                           code.U8(Op::I32Eqz);
                       },
                       [&] { code.LocalGet(carry); },
                       [&] { code.LocalGet(overflow); });
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, nzcv_inst));
    }
}

template<>
void EmitIR<IR::Opcode::Add64>(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.flags.active && inst == ctx.flags.producer) {
        EmitDeferredFlagsAddSub(ctx, inst, false);
        return;
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    IR::Inst* carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);
    IR::Inst* overflow_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp);
    IR::Inst* nzcv_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);

    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    if (!carry_inst && !overflow_inst && !nzcv_inst) {
        // No flags needed (address arithmetic etc.): a + b + c.
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(Op::I64Add);
        if (!args[2].IsImmediate() || args[2].GetImmediateU1()) {
            EmitPush(code, ctx.reg_alloc, args[2]);
            code.U8(Op::I64ExtendI32U);
            code.U8(Op::I64Add);
        }
        code.LocalSet(dst);
        return;
    }

    // sum = a + b; c1 = sum < a; sum += c; c2 = sum < prev_sum... track carry
    // via two unsigned comparisons.
    const u32 sum = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[0]);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.U8(Op::I64Add);
    code.LocalSet(sum);

    const u32 carry = ctx.Temp(ValType::I32);
    // c1 = sum < a
    code.LocalGet(sum);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.U8(Op::I64LtU);
    // then sum += c; c2 = new_sum < old_sum
    code.LocalGet(sum);
    EmitPush(code, ctx.reg_alloc, args[2]);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64Add);
    code.LocalSet(sum);
    code.LocalGet(sum);
    EmitPush(code, ctx.reg_alloc, args[2]);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64LtU);  // sum < c (carry out of second add)
    code.U8(Op::I32Or);
    code.LocalSet(carry);

    code.LocalGet(sum);
    code.LocalSet(dst);

    const bool need_overflow = overflow_inst || nzcv_inst;
    const u32 overflow = ctx.Temp(ValType::I32);
    if (need_overflow) {
        // overflow = (~(a^b) & (a^sum)) >>> 63
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(Op::I64Xor);
        code.I64Const(-1);
        code.U8(Op::I64Xor);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.LocalGet(sum);
        code.U8(Op::I64Xor);
        code.U8(Op::I64And);
        code.I64Const(63);
        code.U8(Op::I64ShrU);
        code.U8(Op::I32WrapI64);
        code.LocalSet(overflow);
    }

    if (carry_inst) {
        code.LocalGet(carry);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, carry_inst));
    }
    if (overflow_inst) {
        code.LocalGet(overflow);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, overflow_inst));
    }
    if (nzcv_inst) {
        EmitPackNzcv32(code,
                       [&] {
                           code.LocalGet(sum);
                           code.I64Const(63);
                           code.U8(Op::I64ShrU);
                           code.U8(Op::I32WrapI64);
                       },
                       [&] {
                           code.LocalGet(sum);
                           code.U8(Op::I64Eqz);
                       },
                       [&] { code.LocalGet(carry); },
                       [&] { code.LocalGet(overflow); });
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, nzcv_inst));
    }
}

template<>
void EmitIR<IR::Opcode::Sub32>(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.flags.active && inst == ctx.flags.producer) {
        EmitDeferredFlagsAddSub(ctx, inst, true);
        return;
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    IR::Inst* carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);
    IR::Inst* overflow_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp);
    IR::Inst* nzcv_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);

    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    if (!carry_inst && !nzcv_inst) {
        // result = a - b - (1 - c) = a - b + c - 1
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(Op::I32Sub);
        EmitPush(code, ctx.reg_alloc, args[2]);
        code.U8(Op::I32Add);
        code.I32Const(-1);
        code.U8(Op::I32Add);
        code.LocalSet(dst);
        if (overflow_inst) {
            const u32 ov = ctx.reg_alloc.DefineLocal(code, overflow_inst);
            EmitOverflow32Sub(code,
                              [&] { EmitPush(code, ctx.reg_alloc, args[0]); },
                              [&] { EmitPush(code, ctx.reg_alloc, args[1]); },
                              [&] { code.LocalGet(dst); });
            code.LocalSet(ov);
        }
        return;
    }

    // Full path: t = zext(a) - zext(b) - (1 - c); borrow = t < 0 (bit63)
    const u32 diff = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.U8(Op::I64ExtendI32U);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64Sub);
    EmitPush(code, ctx.reg_alloc, args[2]);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64Add);
    code.I64Const(-1);
    code.U8(Op::I64Add);
    code.LocalSet(diff);

    code.LocalGet(diff);
    code.U8(Op::I32WrapI64);
    code.LocalSet(dst);

    const u32 carry = ctx.Temp(ValType::I32);
    code.LocalGet(diff);
    code.I64Const(63);
    code.U8(Op::I64ShrU);
    code.U8(Op::I32WrapI64);
    code.I32Const(1);
    code.U8(Op::I32Xor);  // carry = NOT borrow
    code.LocalSet(carry);

    const u32 overflow = ctx.Temp(ValType::I32);
    EmitOverflow32Sub(code,
                      [&] { EmitPush(code, ctx.reg_alloc, args[0]); },
                      [&] { EmitPush(code, ctx.reg_alloc, args[1]); },
                      [&] { code.LocalGet(dst); });
    code.LocalSet(overflow);

    if (carry_inst) {
        code.LocalGet(carry);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, carry_inst));
    }
    if (overflow_inst) {
        code.LocalGet(overflow);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, overflow_inst));
    }
    if (nzcv_inst) {
        EmitPackNzcv32(code,
                       [&] {
                           code.LocalGet(dst);
                           code.I32Const(31);
                           code.U8(Op::I32ShrU);
                       },
                       [&] {
                           code.LocalGet(dst);
                           code.U8(Op::I32Eqz);
                       },
                       [&] { code.LocalGet(carry); },
                       [&] { code.LocalGet(overflow); });
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, nzcv_inst));
    }
}

template<>
void EmitIR<IR::Opcode::Sub64>(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.flags.active && inst == ctx.flags.producer) {
        EmitDeferredFlagsAddSub(ctx, inst, true);
        return;
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    IR::Inst* carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);
    IR::Inst* overflow_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp);
    IR::Inst* nzcv_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);

    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    if (!carry_inst && !overflow_inst && !nzcv_inst) {
        // No flags needed: a - b - (1 - c).
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(Op::I64Sub);
        if (!args[2].IsImmediate() || !args[2].GetImmediateU1()) {
            EmitPush(code, ctx.reg_alloc, args[2]);
            code.I32Const(1);
            code.U8(Op::I32Xor);
            code.U8(Op::I64ExtendI32U);
            code.U8(Op::I64Sub);
        }
        code.LocalSet(dst);
        return;
    }

    // diff = a - b; borrow1 = a < b; diff2 = diff - x (x = 1 - c);
    // borrow2 = diff < x; borrow = borrow1 | borrow2; carry = !borrow
    const u32 diff = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[0]);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.U8(Op::I64Sub);
    code.LocalSet(diff);

    const u32 x = ctx.Temp(ValType::I64);  // x = 1 - c
    EmitPush(code, ctx.reg_alloc, args[2]);
    code.I32Const(1);
    code.U8(Op::I32Xor);
    code.U8(Op::I64ExtendI32U);
    code.LocalSet(x);

    const u32 borrow = ctx.Temp(ValType::I32);
    EmitPush(code, ctx.reg_alloc, args[0]);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.U8(Op::I64LtU);
    code.LocalGet(diff);
    code.LocalGet(x);
    code.U8(Op::I64LtU);
    code.U8(Op::I32Or);
    code.LocalSet(borrow);

    code.LocalGet(diff);
    code.LocalGet(x);
    code.U8(Op::I64Sub);
    code.LocalSet(diff);
    code.LocalGet(diff);
    code.LocalSet(dst);

    const u32 carry = ctx.Temp(ValType::I32);
    code.LocalGet(borrow);
    code.I32Const(1);
    code.U8(Op::I32Xor);
    code.LocalSet(carry);

    const bool need_overflow = overflow_inst || nzcv_inst;
    const u32 overflow = ctx.Temp(ValType::I32);
    if (need_overflow) {
        // overflow = ((a^b) & (a^diff)) >>> 63
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(Op::I64Xor);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.LocalGet(diff);
        code.U8(Op::I64Xor);
        code.U8(Op::I64And);
        code.I64Const(63);
        code.U8(Op::I64ShrU);
        code.U8(Op::I32WrapI64);
        code.LocalSet(overflow);
    }

    if (carry_inst) {
        code.LocalGet(carry);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, carry_inst));
    }
    if (overflow_inst) {
        code.LocalGet(overflow);
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, overflow_inst));
    }
    if (nzcv_inst) {
        EmitPackNzcv32(code,
                       [&] {
                           code.LocalGet(diff);
                           code.I64Const(63);
                           code.U8(Op::I64ShrU);
                           code.U8(Op::I32WrapI64);
                       },
                       [&] {
                           code.LocalGet(diff);
                           code.U8(Op::I64Eqz);
                       },
                       [&] { code.LocalGet(carry); },
                       [&] { code.LocalGet(overflow); });
        code.LocalSet(ctx.reg_alloc.DefineLocal(code, nzcv_inst));
    }
}

// ---------------------------------------------------------------------------
// Multiply / divide
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::Mul32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32Mul>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::Mul64>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop64<Op::I64Mul>(ctx, inst);
}

// High 64 bits of the 128-bit product (UMULH / SMULH), from 32-bit limbs:
//   lo*lo, lo*hi, hi*lo, hi*hi partial products (each exact in u64), summing
//   the middle column with its carries. The signed form corrects the unsigned
//   result: smulh = umulh - (a < 0 ? b : 0) - (b < 0 ? a : 0).
static void EmitMultiplyHigh64(EmitContext& ctx, IR::Inst* inst, bool is_signed) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    const u32 a = ctx.Temp(ValType::I64);
    const u32 b = ctx.Temp(ValType::I64);
    const u32 lh = ctx.Temp(ValType::I64);
    const u32 hl = ctx.Temp(ValType::I64);
    const u32 mid = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(a);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.LocalSet(b);

    auto lo = [&](u32 v) {
        code.LocalGet(v);
        code.I64Const(0xFFFFFFFF);
        code.U8(Op::I64And);
    };
    auto hi = [&](u32 v) {
        code.LocalGet(v);
        code.I64Const(32);
        code.U8(Op::I64ShrU);
    };

    lo(a);
    hi(b);
    code.U8(Op::I64Mul);
    code.LocalSet(lh);
    hi(a);
    lo(b);
    code.U8(Op::I64Mul);
    code.LocalSet(hl);
    // mid = (lo(a)*lo(b) >> 32) + lo(lh) + lo(hl)   (< 3 * 2^32, no overflow)
    lo(a);
    lo(b);
    code.U8(Op::I64Mul);
    code.I64Const(32);
    code.U8(Op::I64ShrU);
    lo(lh);
    code.U8(Op::I64Add);
    lo(hl);
    code.U8(Op::I64Add);
    code.LocalSet(mid);
    // hi(a)*hi(b) + hi(lh) + hi(hl) + hi(mid)
    hi(a);
    hi(b);
    code.U8(Op::I64Mul);
    hi(lh);
    code.U8(Op::I64Add);
    hi(hl);
    code.U8(Op::I64Add);
    hi(mid);
    code.U8(Op::I64Add);

    if (is_signed) {
        // - (a >> 63 arithmetic) & b - (b >> 63 arithmetic) & a
        for (const auto [x, y] : {std::pair{a, b}, std::pair{b, a}}) {
            code.LocalGet(x);
            code.I64Const(63);
            code.U8(Op::I64ShrS);
            code.LocalGet(y);
            code.U8(Op::I64And);
            code.U8(Op::I64Sub);
        }
    }
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::SignedMultiplyHigh64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMultiplyHigh64(ctx, inst, true);
}

template<>
void EmitIR<IR::Opcode::UnsignedMultiplyHigh64>(EmitContext& ctx, IR::Inst* inst) {
    EmitMultiplyHigh64(ctx, inst, false);
}

// ARM UDIV/SDIV return 0 on division by zero (no trap); SDIV overflow
// (INT_MIN / -1) returns INT_MIN. WASM div traps on both, so guard with if/else.
template<bool Is64, bool IsSigned>
void EmitDivGuarded(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const ValType vt = Is64 ? ValType::I64 : ValType::I32;
    const u8 eqz = Is64 ? Op::I64Eqz : Op::I32Eqz;
    const u8 eq = Is64 ? Op::I64Eq : Op::I32Eq;
    const u8 div = Is64 ? (IsSigned ? Op::I64DivS : Op::I64DivU) : (IsSigned ? Op::I32DivS : Op::I32DivU);

    const u32 a = ctx.Temp(vt);
    const u32 b = ctx.Temp(vt);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(a);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.LocalSet(b);

    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    const BlockType bt{{vt}};
    auto push_zero = [&] {
        if (Is64) {
            code.I64Const(0);
        } else {
            code.I32Const(0);
        }
    };

    // if (b == 0) -> 0 else ...
    code.LocalGet(b);
    code.U8(eqz);
    code.If(bt);
    push_zero();
    code.Else();
    if (IsSigned) {
        // if (a == INT_MIN && b == -1) -> a else a/b
        const s64 int_min = Is64 ? INT64_MIN : INT32_MIN;
        code.LocalGet(a);
        if (Is64) {
            code.I64Const(int_min);
        } else {
            code.I32Const(static_cast<s32>(int_min));
        }
        code.U8(eq);
        code.LocalGet(b);
        if (Is64) {
            code.I64Const(-1);
        } else {
            code.I32Const(-1);
        }
        code.U8(eq);
        code.U8(Op::I32And);
        code.If(bt);
        code.LocalGet(a);
        code.Else();
        code.LocalGet(a);
        code.LocalGet(b);
        code.U8(div);
        code.End();
    } else {
        code.LocalGet(a);
        code.LocalGet(b);
        code.U8(div);
    }
    code.End();
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::UnsignedDiv32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDivGuarded<false, false>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::SignedDiv32>(EmitContext& ctx, IR::Inst* inst) {
    EmitDivGuarded<false, true>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::UnsignedDiv64>(EmitContext& ctx, IR::Inst* inst) {
    EmitDivGuarded<true, false>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::SignedDiv64>(EmitContext& ctx, IR::Inst* inst) {
    EmitDivGuarded<true, true>(ctx, inst);
}

// ---------------------------------------------------------------------------
// Logical
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::And32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32And>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::And64>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop64<Op::I64And>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::AndNot32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.I32Const(-1);
    ctx.code.U8(Op::I32Xor);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::AndNot64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.I64Const(-1);
    ctx.code.U8(Op::I64Xor);
    ctx.code.U8(Op::I64And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::Eor32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32Xor>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::Eor64>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop64<Op::I64Xor>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::Or32>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop32<Op::I32Or>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::Or64>(EmitContext& ctx, IR::Inst* inst) {
    EmitBinop64<Op::I64Or>(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::Not32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(-1);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::Not64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I64Const(-1);
    ctx.code.U8(Op::I64Xor);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Extends
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::SignExtendByteToWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Extend8S);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::SignExtendHalfToWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Extend16S);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::SignExtendByteToLong>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Extend8S);
    ctx.code.U8(Op::I64ExtendI32S);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::SignExtendHalfToLong>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Extend16S);
    ctx.code.U8(Op::I64ExtendI32S);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::SignExtendWordToLong>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I64ExtendI32S);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ZeroExtendByteToWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0xFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ZeroExtendHalfToWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ZeroExtendByteToLong>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0xFF);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ZeroExtendHalfToLong>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ZeroExtendWordToLong>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ZeroExtendLongToQuad>(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.elided_widen.count(inst)) {
        // Its only use stores the operand into a scalar64 register unit,
        // which reads it directly: keep the operand live until then.
        ctx.reg_alloc.DeferArgUse(inst);
        return;
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I64x2Splat);
    ctx.code.I64Const(0);
    ctx.code.Simd(Op::I64x2ReplaceLane);
    ctx.code.U8(1);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Byte reversal / CLZ
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::ByteReverseWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 v = ctx.Temp(ValType::I32);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(v);
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    // bswap32(v) = rotr(v & 0x00FF00FF, 8) | rotl(v & 0xFF00FF00, 8)
    code.LocalGet(v);
    code.I32Const(0x00FF00FF);
    code.U8(Op::I32And);
    code.I32Const(8);
    code.U8(Op::I32Rotr);
    code.LocalGet(v);
    code.I32Const(static_cast<s32>(0xFF00FF00));
    code.U8(Op::I32And);
    code.I32Const(8);
    code.U8(Op::I32Rotl);
    code.U8(Op::I32Or);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ByteReverseHalf>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    // 0xHHLL -> 0xLLHH: ((v >> 8) & 0xFF) | ((v << 8) & 0xFF00)
    const u32 v = ctx.Temp(ValType::I32);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalTee(v);
    code.I32Const(8);
    code.U8(Op::I32ShrU);
    code.I32Const(0xFF);
    code.U8(Op::I32And);
    code.LocalGet(v);
    code.I32Const(8);
    code.U8(Op::I32Shl);
    code.I32Const(0xFF00);
    code.U8(Op::I32And);
    code.U8(Op::I32Or);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ByteReverseDual>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 v = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalSet(v);
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    // bswap64(v) = (bswap32(lo) << 32) | bswap32(hi), using the same
    // mask-rotate trick widened to 64 bits.
    const u32 lo = ctx.Temp(ValType::I32);
    const u32 hi = ctx.Temp(ValType::I32);
    code.LocalGet(v);
    code.U8(Op::I32WrapI64);
    code.LocalSet(lo);
    code.LocalGet(v);
    code.I64Const(32);
    code.U8(Op::I64ShrU);
    code.U8(Op::I32WrapI64);
    code.LocalSet(hi);

    auto bswap32 = [&](u32 x) {
        code.LocalGet(x);
        code.I32Const(0x00FF00FF);
        code.U8(Op::I32And);
        code.I32Const(8);
        code.U8(Op::I32Rotr);
        code.LocalGet(x);
        code.I32Const(static_cast<s32>(0xFF00FF00));
        code.U8(Op::I32And);
        code.I32Const(8);
        code.U8(Op::I32Rotl);
        code.U8(Op::I32Or);
    };

    bswap32(lo);
    code.U8(Op::I64ExtendI32U);
    code.I64Const(32);
    code.U8(Op::I64Shl);
    bswap32(hi);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64Or);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::CountLeadingZeros32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I32Clz);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::CountLeadingZeros64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.U8(Op::I64Clz);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Misc bit utilities
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::ExtractRegister32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    // (lo >> lsb) | (hi << (32 - lsb)); lsb == 0 -> lo
    const u32 lsb = ctx.Temp(ValType::I32);
    EmitPush(code, ctx.reg_alloc, args[2]);
    code.LocalSet(lsb);
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    EmitPush(code, ctx.reg_alloc, args[0]);  // lsb==0 arm
    // funnel arm
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalGet(lsb);
    code.U8(Op::I32ShrU);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.I32Const(32);
    code.LocalGet(lsb);
    code.U8(Op::I32Sub);
    code.U8(Op::I32Shl);  // masked; garbage when lsb==0 but that arm is not selected
    code.U8(Op::I32Or);
    code.LocalGet(lsb);
    code.U8(Op::I32Eqz);
    code.U8(Op::Select);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ExtractRegister64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 lsb = ctx.Temp(ValType::I64);
    EmitPush(code, ctx.reg_alloc, args[2]);
    code.U8(Op::I64ExtendI32U);
    code.LocalSet(lsb);
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    EmitPush(code, ctx.reg_alloc, args[0]);
    EmitPush(code, ctx.reg_alloc, args[0]);
    code.LocalGet(lsb);
    code.U8(Op::I64ShrU);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.I64Const(64);
    code.LocalGet(lsb);
    code.U8(Op::I64Sub);
    code.U8(Op::I64Shl);
    code.U8(Op::I64Or);
    code.LocalGet(lsb);
    code.U8(Op::I64Eqz);
    code.U8(Op::Select);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ReplicateBit32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    code.I32Const(0);
    EmitPush(code, ctx.reg_alloc, args[0]);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.U8(Op::I32ShrU);
    code.I32Const(1);
    code.U8(Op::I32And);
    code.U8(Op::I32Sub);
    code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::ReplicateBit64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);
    code.I64Const(0);
    EmitPush(code, ctx.reg_alloc, args[0]);
    EmitPush(code, ctx.reg_alloc, args[1]);
    code.U8(Op::I64ExtendI32U);
    code.U8(Op::I64ShrU);
    code.I64Const(1);
    code.U8(Op::I64And);
    code.U8(Op::I64Sub);
    code.LocalSet(dst);
}

}  // namespace Dynarmic::Backend::Wasm
