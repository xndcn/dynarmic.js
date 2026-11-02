// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// ARMv6/v7 parallel media ops (SADD8/UADD16/QADD8/SHSUB16/SASX/USADA8/...).
// Most operations use scalar i32 lane arithmetic; saturating add/sub maps
// directly to Wasm SIMD. Ops that produce GE flags define GetGEFromOp
// pseudo-op with an all-ones-per-lane mask (same format as the x64 backend).

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

constexpr u32 M7_8 = 0x7F7F7F7F;
constexpr u32 M8_8 = 0x80808080;
constexpr u32 M7_16 = 0x7FFF7FFF;
constexpr u32 M8_16 = 0x80008000;

void EmitConst(EmitContext& ctx, u32 v) {
    ctx.code.I32Const(static_cast<s32>(v));
}

// Grab the GetGEFromOp pseudo-op local, if the frontend asked for GE flags.
// Returns 0 (and defines nothing) when absent.
u32 DefineGELocal(EmitContext& ctx, IR::Inst* inst) {
    IR::Inst* ge_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetGEFromOp);
    if (!ge_inst) {
        return 0;
    }
    return ctx.reg_alloc.DefineLocal(ctx.code, ge_inst);
}

// Expand lane high-bits (bit 7/15 of each lane set where the flag is set)
// into an all-ones-per-lane mask. Stack in: i32 with relevant bits at lane
// top positions; other bits must be clear.
void EmitExpandGE8(EmitContext& ctx) {
    ctx.code.I32Const(7);
    ctx.code.U8(Op::I32ShrU);
    EmitConst(ctx, 0x01010101);
    ctx.code.U8(Op::I32And);
    EmitConst(ctx, 0xFF);
    ctx.code.U8(Op::I32Mul);
}

void EmitExpandGE16(EmitContext& ctx) {
    ctx.code.I32Const(15);
    ctx.code.U8(Op::I32ShrU);
    EmitConst(ctx, 0x00010001);
    ctx.code.U8(Op::I32And);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32Mul);
}

// Lane-wrapping partitioned add/sub results (no flags).
// padd: ((a & M7) + (b & M7)) ^ ((a ^ b) & M8)
void EmitPartitionedAdd(EmitContext& ctx, u32 ta, u32 tb, u32 m7, u32 m8) {
    ctx.code.LocalGet(ta);
    EmitConst(ctx, m7);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(tb);
    EmitConst(ctx, m7);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Add);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Xor);
}

// psub: ((a | M8) - (b & M7)) ^ ((a ^ b) & M8) ^ M8
void EmitPartitionedSub(EmitContext& ctx, u32 ta, u32 tb, u32 m7, u32 m8) {
    ctx.code.LocalGet(ta);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(tb);
    EmitConst(ctx, m7);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Sub);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Xor);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32Xor);
}

// GE for unsigned add: carry out of each lane.
// cv = (a & b) | ((a ^ b) & sum7) where sum7 = (a & M7) + (b & M7)
void EmitGEAddU(EmitContext& ctx, u32 ta, u32 tb, u32 m7, u32 m8, bool is8) {
    const u32 t7 = ctx.Temp(ValType::I32);
    ctx.code.LocalGet(ta);
    EmitConst(ctx, m7);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(tb);
    EmitConst(ctx, m7);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Add);
    ctx.code.LocalSet(t7);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalGet(t7);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32And);
    if (is8) {
        EmitExpandGE8(ctx);
    } else {
        EmitExpandGE16(ctx);
    }
}

// GE for unsigned sub: no borrow out of the lane.
// bv = (~a & b) | (~(a ^ b) & ~diff7) where diff7 = (a | M8) - (b & M7);
// ge = ~bv at lane tops.
void EmitGESubU(EmitContext& ctx, u32 ta, u32 tb, u32 m7, u32 m8, bool is8) {
    const u32 t7 = ctx.Temp(ValType::I32);
    ctx.code.LocalGet(ta);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(tb);
    EmitConst(ctx, m7);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Sub);
    ctx.code.LocalSet(t7);
    ctx.code.LocalGet(ta);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);  // ~a & b
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);  // ~(a^b)
    ctx.code.LocalGet(t7);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);  // ~diff7
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);  // ~bv
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32And);
    if (is8) {
        EmitExpandGE8(ctx);
    } else {
        EmitExpandGE16(ctx);
    }
}

// GE for signed add: true lane sum >= 0.
// sign = (a & b) | ((a ^ b) & s) at lane tops (s = wrapped result);
// ge = ~sign.
void EmitGEAddS(EmitContext& ctx, u32 ta, u32 tb, u32 ts, u32 m8, bool is8) {
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalGet(ts);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32And);
    if (is8) {
        EmitExpandGE8(ctx);
    } else {
        EmitExpandGE16(ctx);
    }
}

// GE for signed sub: true lane diff >= 0.
// sign = (a & ~b) | (~(a ^ b) & s); ge = ~sign.
void EmitGESubS(EmitContext& ctx, u32 ta, u32 tb, u32 ts, u32 m8, bool is8) {
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);
    ctx.code.U8(Op::I32And);  // a & ~b
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);  // ~(a^b)
    ctx.code.LocalGet(ts);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    EmitConst(ctx, 0xFFFFFFFFu);
    ctx.code.U8(Op::I32Xor);
    EmitConst(ctx, m8);
    ctx.code.U8(Op::I32And);
    if (is8) {
        EmitExpandGE8(ctx);
    } else {
        EmitExpandGE16(ctx);
    }
}

// Plain partitioned add/sub with optional GE.
void EmitPackedAddSub(EmitContext& ctx, IR::Inst* inst, bool sub, bool is_signed, bool is8) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tge = DefineGELocal(ctx, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 m7 = is8 ? M7_8 : M7_16;
    const u32 m8 = is8 ? M8_8 : M8_16;

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);

    if (sub) {
        EmitPartitionedSub(ctx, ta, tb, m7, m8);
    } else {
        EmitPartitionedAdd(ctx, ta, tb, m7, m8);
    }
    ctx.code.LocalSet(dst);

    if (tge != 0) {
        if (sub) {
            if (is_signed) {
                EmitGESubS(ctx, ta, tb, dst, m8, is8);
            } else {
                EmitGESubU(ctx, ta, tb, m7, m8, is8);
            }
        } else {
            if (is_signed) {
                EmitGEAddS(ctx, ta, tb, dst, m8, is8);
            } else {
                EmitGEAddU(ctx, ta, tb, m7, m8, is8);
            }
        }
        ctx.code.LocalSet(tge);
    }
}

// Extract lane at bit offset, zero- or sign-extended to i32.
void EmitExtractLane(EmitContext& ctx, u32 src, u32 bit_off, u32 bits, bool sign) {
    ctx.code.LocalGet(src);
    if (sign) {
        const u32 lshift = 32 - bit_off - bits;
        if (lshift != 0) {
            ctx.code.I32Const(lshift);
            ctx.code.U8(Op::I32Shl);
        }
        ctx.code.I32Const(32 - bits);
        ctx.code.U8(Op::I32ShrS);
    } else {
        if (bit_off != 0) {
            ctx.code.I32Const(bit_off);
            ctx.code.U8(Op::I32ShrU);
        }
        ctx.code.I32Const(static_cast<s32>((1u << bits) - 1));
        ctx.code.U8(Op::I32And);
    }
}

// 16-bit add/sub exchange family (SASX/SSAX/UASX/USAX + halving variants).
// hi_is_sum: high half is the add, low half is the subtract (SASX form).
void EmitPackedSubAdd16(EmitContext& ctx, IR::Inst* inst, bool hi_is_sum, bool is_signed, bool is_halving) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tge = DefineGELocal(ctx, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 ta_lo = ctx.Temp(ValType::I32);
    const u32 ta_hi = ctx.Temp(ValType::I32);
    const u32 tb_lo = ctx.Temp(ValType::I32);
    const u32 tb_hi = ctx.Temp(ValType::I32);
    const u32 tsum = ctx.Temp(ValType::I32);
    const u32 tdiff = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);

    EmitExtractLane(ctx, ta, 0, 16, is_signed);
    ctx.code.LocalSet(ta_lo);
    EmitExtractLane(ctx, ta, 16, 16, is_signed);
    ctx.code.LocalSet(ta_hi);
    EmitExtractLane(ctx, tb, 0, 16, is_signed);
    ctx.code.LocalSet(tb_lo);
    EmitExtractLane(ctx, tb, 16, 16, is_signed);
    ctx.code.LocalSet(tb_hi);

    if (hi_is_sum) {
        // diff = a_lo - b_hi; sum = a_hi + b_lo
        ctx.code.LocalGet(ta_lo);
        ctx.code.LocalGet(tb_hi);
        ctx.code.U8(Op::I32Sub);
        ctx.code.LocalSet(tdiff);
        ctx.code.LocalGet(ta_hi);
        ctx.code.LocalGet(tb_lo);
        ctx.code.U8(Op::I32Add);
        ctx.code.LocalSet(tsum);
    } else {
        // sum = a_lo + b_hi; diff = a_hi - b_lo
        ctx.code.LocalGet(ta_lo);
        ctx.code.LocalGet(tb_hi);
        ctx.code.U8(Op::I32Add);
        ctx.code.LocalSet(tsum);
        ctx.code.LocalGet(ta_hi);
        ctx.code.LocalGet(tb_lo);
        ctx.code.U8(Op::I32Sub);
        ctx.code.LocalSet(tdiff);
    }

    if (tge != 0) {
        const u32 t1 = ctx.Temp(ValType::I32);
        // sum lane: unsigned -> carry (bit 16); signed -> sum >= 0
        if (is_signed) {
            ctx.code.LocalGet(tsum);
            EmitConst(ctx, 0xFFFFFFFFu);
            ctx.code.U8(Op::I32Xor);
            ctx.code.I32Const(31);
            ctx.code.U8(Op::I32ShrS);
        } else {
            ctx.code.LocalGet(tsum);
            ctx.code.I32Const(15);
            ctx.code.U8(Op::I32Shl);
            ctx.code.I32Const(31);
            ctx.code.U8(Op::I32ShrS);
        }
        EmitConst(ctx, hi_is_sum ? 0xFFFF0000u : 0x0000FFFFu);
        ctx.code.U8(Op::I32And);
        ctx.code.LocalSet(t1);
        // diff lane: diff >= 0 (signed i32 compare works for both:
        // zero-extended values never go negative)
        ctx.code.LocalGet(tdiff);
        EmitConst(ctx, 0xFFFFFFFFu);
        ctx.code.U8(Op::I32Xor);
        ctx.code.I32Const(31);
        ctx.code.U8(Op::I32ShrS);
        EmitConst(ctx, hi_is_sum ? 0x0000FFFFu : 0xFFFF0000u);
        ctx.code.U8(Op::I32And);
        ctx.code.LocalGet(t1);
        ctx.code.U8(Op::I32Or);
        ctx.code.LocalSet(tge);
    }

    const u32 lo = hi_is_sum ? tdiff : tsum;
    const u32 hi = hi_is_sum ? tsum : tdiff;
    if (is_halving) {
        // lo: ((v << 15) >>u 16); hi: ((v >>u 1) << 16)
        ctx.code.LocalGet(lo);
        ctx.code.I32Const(15);
        ctx.code.U8(Op::I32Shl);
        ctx.code.I32Const(16);
        ctx.code.U8(Op::I32ShrU);
        ctx.code.LocalGet(hi);
        ctx.code.I32Const(1);
        ctx.code.U8(Op::I32ShrU);
        ctx.code.I32Const(16);
        ctx.code.U8(Op::I32Shl);
        ctx.code.U8(Op::I32Or);
    } else {
        ctx.code.LocalGet(lo);
        EmitConst(ctx, 0xFFFF);
        ctx.code.U8(Op::I32And);
        ctx.code.LocalGet(hi);
        ctx.code.I32Const(16);
        ctx.code.U8(Op::I32Shl);
        ctx.code.U8(Op::I32Or);
    }
    ctx.code.LocalSet(dst);
}

// Per-lane saturating add/sub (QADD8/UQSUB16 etc.). These operations do not
// update CPSR.Q or FPSCR.QC. The low i32 lane contains the four bytes/two
// halfwords; duplicating it in the other lanes does not affect the result.
void EmitPackedSat(EmitContext& ctx, IR::Inst* inst, u32 bits, bool is_signed, bool is_sub) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 op = bits == 8
        ? (is_sub ? (is_signed ? Op::I8x16SubSatS : Op::I8x16SubSatU)
                  : (is_signed ? Op::I8x16AddSatS : Op::I8x16AddSatU))
        : (is_sub ? (is_signed ? Op::I16x8SubSatS : Op::I16x8SubSatU)
                  : (is_signed ? Op::I16x8AddSatS : Op::I16x8AddSatU));
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I32x4Splat);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I32x4Splat);
    ctx.code.Simd(op);
    ctx.code.Simd(Op::I32x4ExtractLane);
    ctx.code.U8(0);
    ctx.code.LocalSet(dst);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::PackedAddU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, false, false, true);
}

template<>
void EmitIR<IR::Opcode::PackedAddS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, false, true, true);
}

template<>
void EmitIR<IR::Opcode::PackedSubU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, true, false, true);
}

template<>
void EmitIR<IR::Opcode::PackedSubS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, true, true, true);
}

template<>
void EmitIR<IR::Opcode::PackedAddU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, false, false, false);
}

template<>
void EmitIR<IR::Opcode::PackedAddS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, false, true, false);
}

template<>
void EmitIR<IR::Opcode::PackedSubU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, true, false, false);
}

template<>
void EmitIR<IR::Opcode::PackedSubS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedAddSub(ctx, inst, true, true, false);
}

template<>
void EmitIR<IR::Opcode::PackedAddSubU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, true, false, false);
}

template<>
void EmitIR<IR::Opcode::PackedAddSubS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, true, true, false);
}

template<>
void EmitIR<IR::Opcode::PackedSubAddU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, false, false, false);
}

template<>
void EmitIR<IR::Opcode::PackedSubAddS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, false, true, false);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingAddU8>(EmitContext& ctx, IR::Inst* inst) {
    // (a & b) + (((a ^ b) >> 1) & 0x7F7F7F7F)
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrU);
    EmitConst(ctx, M7_8);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Add);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingAddS8>(EmitContext& ctx, IR::Inst* inst) {
    // t = a ^ b; ((a & b) + ((t >> 1) & M7)) ^ (t & M8)
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 tt = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tt);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(tt);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrU);
    EmitConst(ctx, M7_8);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Add);
    ctx.code.LocalGet(tt);
    EmitConst(ctx, M8_8);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingSubU8>(EmitContext& ctx, IR::Inst* inst) {
    // t = a ^ b; ((((t >> 1) | M8) - (t & b)) ^ M8)
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 tt = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tt);
    ctx.code.LocalGet(tt);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrU);
    EmitConst(ctx, M8_8);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(tt);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Sub);
    EmitConst(ctx, M8_8);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingSubS8>(EmitContext& ctx, IR::Inst* inst) {
    // t = a ^ b; ((((t >> 1) | M8) - (t & b)) ^ M8 ^ (t & M8))
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 tt = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tt);
    ctx.code.LocalGet(tt);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrU);
    EmitConst(ctx, M8_8);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(tt);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Sub);
    EmitConst(ctx, M8_8);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalGet(tt);
    EmitConst(ctx, M8_8);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingAddU16>(EmitContext& ctx, IR::Inst* inst) {
    // (a & b) + (((a ^ b) >> 1) & 0x7FFF7FFF)
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrU);
    EmitConst(ctx, M7_16);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Add);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingAddS16>(EmitContext& ctx, IR::Inst* inst) {
    // Per lane: (sar16(a) + sar16(b)) >>s 1, truncated to 16 bits. A
    // whole-register (a&b)+((a^b)>>1) formula can carry across lanes once the
    // sign bit is folded in (x64 relies on paddw discarding carries), so
    // compute the lanes independently.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    // low lane
    ctx.code.LocalGet(ta);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32Shl);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.LocalGet(tb);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32Shl);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.U8(Op::I32Add);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrS);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32And);
    // high lane
    ctx.code.LocalGet(ta);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.LocalGet(tb);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.U8(Op::I32Add);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingSubU16>(EmitContext& ctx, IR::Inst* inst) {
    // per lane: ((x >> 1) - (x & b)) & 0xFFFF where x = a ^ b
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 tx = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tx);
    // low lane
    ctx.code.LocalGet(tx);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.LocalGet(tx);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Sub);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32And);
    // high lane
    ctx.code.LocalGet(tx);
    ctx.code.I32Const(17);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.LocalGet(tx);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.LocalGet(tb);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Sub);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingSubS16>(EmitContext& ctx, IR::Inst* inst) {
    // per lane: (sar16(x, 1) - (x & b)) & 0xFFFF where x = a ^ b
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 tx = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.LocalGet(ta);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tx);
    // low lane: sar16 = ((x << 16) >>s 17)
    ctx.code.LocalGet(tx);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32Shl);
    ctx.code.I32Const(17);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.LocalGet(tx);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Sub);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32And);
    // high lane: sar16 = (x >>s 17)
    ctx.code.LocalGet(tx);
    ctx.code.I32Const(17);
    ctx.code.U8(Op::I32ShrS);
    ctx.code.LocalGet(tx);
    ctx.code.LocalGet(tb);
    ctx.code.U8(Op::I32And);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.U8(Op::I32Sub);
    EmitConst(ctx, 0xFFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.I32Const(16);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingAddSubU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, true, false, true);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingAddSubS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, true, true, true);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingSubAddU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, false, false, true);
}

template<>
void EmitIR<IR::Opcode::PackedHalvingSubAddS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSubAdd16(ctx, inst, false, true, true);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedAddU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 8, false, false);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedAddS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 8, true, false);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedSubU8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 8, false, true);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedSubS8>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 8, true, true);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedAddU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 16, false, false);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedAddS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 16, true, false);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedSubU16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 16, false, true);
}

template<>
void EmitIR<IR::Opcode::PackedSaturatedSubS16>(EmitContext& ctx, IR::Inst* inst) {
    EmitPackedSat(ctx, inst, 16, true, true);
}

template<>
void EmitIR<IR::Opcode::PackedAbsDiffSumU8>(EmitContext& ctx, IR::Inst* inst) {
    // sum_i |a_i - b_i| over the four bytes
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 ta = ctx.Temp(ValType::I32);
    const u32 tb = ctx.Temp(ValType::I32);
    const u32 tacc = ctx.Temp(ValType::I32);
    const u32 td = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ta);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tb);
    ctx.code.I32Const(0);
    ctx.code.LocalSet(tacc);
    for (u32 i = 0; i < 4; i++) {
        EmitExtractLane(ctx, ta, i * 8, 8, false);
        EmitExtractLane(ctx, tb, i * 8, 8, false);
        ctx.code.U8(Op::I32Sub);
        ctx.code.LocalSet(td);
        // abs(d) = (d ^ (d >> 31)) - (d >> 31)
        ctx.code.LocalGet(td);
        ctx.code.LocalGet(td);
        ctx.code.I32Const(31);
        ctx.code.U8(Op::I32ShrS);
        ctx.code.U8(Op::I32Xor);
        ctx.code.LocalGet(td);
        ctx.code.I32Const(31);
        ctx.code.U8(Op::I32ShrS);
        ctx.code.U8(Op::I32Sub);
        ctx.code.LocalGet(tacc);
        ctx.code.U8(Op::I32Add);
        ctx.code.LocalSet(tacc);
    }
    ctx.code.LocalGet(tacc);
    ctx.code.LocalSet(dst);
}

}  // namespace Dynarmic::Backend::Wasm
