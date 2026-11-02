// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// A32 context get/set opcodes, location write-back and the self-loop
// transform (register caching in wasm locals).

#include <mcl/assert.hpp>

#include <boost/variant.hpp>

#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/emit_wasm_vector_util.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::Wasm {

namespace {

constexpr u32 RegOffset(A32::Reg reg) {
    return offsetof(A32JitState, regs) + sizeof(u32) * static_cast<size_t>(reg);
}

u32 ExtRegOffset(A32::ExtReg reg) {
    const size_t index = static_cast<size_t>(reg);
    if (index <= static_cast<size_t>(A32::ExtReg::S31)) {
        return offsetof(A32JitState, ext_regs) + sizeof(u32) * index;
    }
    if (index <= static_cast<size_t>(A32::ExtReg::D31)) {
        return offsetof(A32JitState, ext_regs) + sizeof(u64) * (index - static_cast<size_t>(A32::ExtReg::D0));
    }
    return offsetof(A32JitState, ext_regs) + 2 * sizeof(u64) * (index - static_cast<size_t>(A32::ExtReg::Q0));
}

// Pushes upper_location_descriptor onto the stack.
void EmitPushUpperLocdesc(EmitContext& ctx) {
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, upper_location_descriptor), 2);
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A32GetRegister>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const A32::Reg reg = args[0].value.GetA32RegRef();
    if (ctx.reg_cache.active && ctx.reg_cache.cached[static_cast<size_t>(reg)]) {
        ctx.code.LocalGet(ctx.reg_cache.local[static_cast<size_t>(reg)]);
    } else {
        ctx.code.LocalGet(kParamState);
        ctx.code.Load(Op::I32Load, RegOffset(reg), 2);
    }
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32SetRegister>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A32::Reg reg = args[0].value.GetA32RegRef();
    if (ctx.reg_cache.active && ctx.reg_cache.cached[static_cast<size_t>(reg)]) {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.LocalSet(ctx.reg_cache.local[static_cast<size_t>(reg)]);
        return;
    }
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Store(Op::I32Store, RegOffset(reg), 2);
}

// Cached unit lookup (self-loop ext register caching), or nullptr.
static const EmitContext::RegCache::ExtUnit* CachedExt(EmitContext& ctx, const IR::Value& ref, u32 size) {
    return ctx.reg_cache.FindExt(ExtRegOffset(ref.GetA32ExtRegRef()), size);
}

template<>
void EmitIR<IR::Opcode::A32GetExtendedRegister32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    if (const auto* unit = CachedExt(ctx, args[0].value, 4)) {
        ctx.code.LocalGet(unit->local);
    } else {
        ctx.code.LocalGet(kParamState);
        ctx.code.Load(Op::I32Load, ExtRegOffset(args[0].value.GetA32ExtRegRef()), 2);
    }
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32GetExtendedRegister64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    if (const auto* unit = CachedExt(ctx, args[0].value, 8)) {
        ctx.code.LocalGet(unit->local);
    } else {
        ctx.code.LocalGet(kParamState);
        ctx.code.Load(Op::I64Load, ExtRegOffset(args[0].value.GetA32ExtRegRef()), 3);
    }
    ctx.code.LocalSet(dst);
}

// A32GetVector / A32SetVector take a D or a Q register: a D register is the
// low 64 bits of the vector value (upper half zero on reads), and a write
// must not touch the next D register.
static u32 VectorAccessSize(const IR::Value& ref) {
    return A32::IsDoubleExtReg(ref.GetA32ExtRegRef()) ? 8 : 16;
}

template<>
void EmitIR<IR::Opcode::A32GetVector>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 size = VectorAccessSize(args[0].value);
    if (const auto* unit = CachedExt(ctx, args[0].value, size)) {
        if (size == 8) {
            VZero(ctx);
            ctx.code.LocalGet(unit->local);
            ctx.code.Simd(Op::I64x2ReplaceLane);
            ctx.code.U8(0);
        } else {
            ctx.code.LocalGet(unit->local);
        }
    } else {
        ctx.code.LocalGet(kParamState);
        ctx.code.Simd(size == 8 ? Op::V128Load64Zero : Op::V128Load);
        ctx.code.ULEB(0);  // align
        ctx.code.ULEB(ExtRegOffset(args[0].value.GetA32ExtRegRef()));
    }
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32SetExtendedRegister32>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (const auto* unit = CachedExt(ctx, args[0].value, 4)) {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.LocalSet(unit->local);
        return;
    }
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Store(Op::I32Store, ExtRegOffset(args[0].value.GetA32ExtRegRef()), 2);
}

template<>
void EmitIR<IR::Opcode::A32SetExtendedRegister64>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (const auto* unit = CachedExt(ctx, args[0].value, 8)) {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.LocalSet(unit->local);
        return;
    }
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Store(Op::I64Store, ExtRegOffset(args[0].value.GetA32ExtRegRef()), 3);
}

template<>
void EmitIR<IR::Opcode::A32SetVector>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 size = VectorAccessSize(args[0].value);
    const auto* unit = CachedExt(ctx, args[0].value, size);
    if (!unit) {
        ctx.code.LocalGet(kParamState);
    }
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    if (size == 8) {
        ctx.code.Simd(Op::I64x2ExtractLane);
        ctx.code.U8(0);
    }
    if (unit) {
        ctx.code.LocalSet(unit->local);
    } else if (size == 8) {
        ctx.code.Store(Op::I64Store, ExtRegOffset(args[0].value.GetA32ExtRegRef()), 3);
    } else {
        ctx.code.Simd(Op::V128Store);
        ctx.code.ULEB(0);  // align
        ctx.code.ULEB(ExtRegOffset(args[0].value.GetA32ExtRegRef()));
    }
}

// ---------------------------------------------------------------------------
// CPSR / flags
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A32GetCpsr>(EmitContext& ctx, IR::Inst* inst) {
    // cpsr = nzcv | q | ge[19:16] | E(bit9) | T(bit5) | jaifm
    // (IT bits are not reconstructed, matching the x64 backend.)
    // In this backend cpsr_nzcv is stored in ARM order (N,Z,C,V at 31..28)
    // and cpsr_q holds bit27-aligned data, so both are plain ORs.
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    // E/T: (upper * 0x120) & 0x220   (T: bit0 -> bit5, E: bit1 -> bit9)
    EmitPushUpperLocdesc(ctx);
    ctx.code.I32Const(0x120);
    ctx.code.U8(Op::I32Mul);
    ctx.code.I32Const(0x220);
    ctx.code.U8(Op::I32And);

    // GE: ((ge & 0x80808080) * 0x00204081 >> 12) & 0x000F0000
    // (lane i's top bit lands on bit 16+i; i32.mul wraps like x64 imul)
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, cpsr_ge), 2);
    ctx.code.I32Const(static_cast<s32>(0x80808080));
    ctx.code.U8(Op::I32And);
    ctx.code.I32Const(0x00204081);
    ctx.code.U8(Op::I32Mul);
    ctx.code.I32Const(12);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.I32Const(0x000F0000);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);

    // q (bit27-aligned) | nzcv (ARM order) | jaifm
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, cpsr_q), 2);
    ctx.code.U8(Op::I32Or);
    EmitPushNzcv(ctx);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, cpsr_jaifm), 2);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32SetCpsr>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 v = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(v);

    // cpsr_nzcv = v & 0xF0000000
    EmitBeginNzcvStore(ctx);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    EmitEndNzcvStore(ctx);
    // cpsr_q = v & (1<<27)
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1 << 27);
    ctx.code.U8(Op::I32And);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, cpsr_q), 2);
    // cpsr_ge = expand bits 19..16 to byte lanes
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1 << 19);
    ctx.code.U8(Op::I32And);
    ctx.code.If({{ValType::I32}});
    ctx.code.I32Const(static_cast<s32>(0xFF000000));
    ctx.code.Else();
    ctx.code.I32Const(0);
    ctx.code.End();
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1 << 18);
    ctx.code.U8(Op::I32And);
    ctx.code.If({{ValType::I32}});
    ctx.code.I32Const(0x00FF0000);
    ctx.code.Else();
    ctx.code.I32Const(0);
    ctx.code.End();
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1 << 17);
    ctx.code.U8(Op::I32And);
    ctx.code.If({{ValType::I32}});
    ctx.code.I32Const(0x0000FF00);
    ctx.code.Else();
    ctx.code.I32Const(0);
    ctx.code.End();
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1 << 16);
    ctx.code.U8(Op::I32And);
    ctx.code.If({{ValType::I32}});
    ctx.code.I32Const(0x000000FF);
    ctx.code.Else();
    ctx.code.I32Const(0);
    ctx.code.End();
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, cpsr_ge), 2);
    // cpsr_jaifm = v & 0x010001DF
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(0x010001DF);
    ctx.code.U8(Op::I32And);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, cpsr_jaifm), 2);
    // upper = (upper & 0xFFFF0000) | E(bit1)/T(bit0)   (IT/FPSCR bits preserved)
    ctx.code.LocalGet(kParamState);
    EmitPushUpperLocdesc(ctx);
    ctx.code.I32Const(static_cast<s32>(0xFFFF0000));
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1 << 9);
    ctx.code.U8(Op::I32And);
    ctx.code.If({{ValType::I32}});
    ctx.code.I32Const(2);
    ctx.code.Else();
    ctx.code.I32Const(0);
    ctx.code.End();
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1 << 5);
    ctx.code.U8(Op::I32And);
    ctx.code.If({{ValType::I32}});
    ctx.code.I32Const(1);
    ctx.code.Else();
    ctx.code.I32Const(0);
    ctx.code.End();
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, upper_location_descriptor), 2);
}

template<>
void EmitIR<IR::Opcode::A32SetCpsrNZCV>(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.flags.active && inst == ctx.flags.setter) {
        return;  // deferred: materialized on exit (see EmitFlushRegCache)
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitBeginNzcvStore(ctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    EmitEndNzcvStore(ctx);
}

template<>
void EmitIR<IR::Opcode::A32SetCpsrNZCVRaw>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitBeginNzcvStore(ctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    EmitEndNzcvStore(ctx);
}

template<>
void EmitIR<IR::Opcode::A32SetCpsrNZCVQ>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitBeginNzcvStore(ctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    EmitEndNzcvStore(ctx);
    // cpsr_q is kept bit27-aligned, like the Q bit of the operand.
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(1 << 27);
    ctx.code.U8(Op::I32And);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, cpsr_q), 2);
}

template<>
void EmitIR<IR::Opcode::A32SetCpsrNZ>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitBeginNzcvStore(ctx);
    EmitPushNzcv(ctx);
    ctx.code.I32Const(0x3FFFFFFF);
    ctx.code.U8(Op::I32And);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0xC0000000));
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    EmitEndNzcvStore(ctx);
}

template<>
void EmitIR<IR::Opcode::A32SetCpsrNZC>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    // nzcv = (nzcv & 0x1FFFFFFF) | (nz & 0xC0000000) | (c << 29)
    // An immediate NZ is A32GetSetElimination's EmptyNZCVImmediateMarker
    // (N/Z are overwritten later): set C only.
    const bool set_nz = !args[0].IsImmediate();
    EmitBeginNzcvStore(ctx);
    EmitPushNzcv(ctx);
    ctx.code.I32Const(set_nz ? 0x1FFFFFFF : static_cast<s32>(0xDFFFFFFF));
    ctx.code.U8(Op::I32And);
    if (set_nz) {
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.I32Const(static_cast<s32>(0xC0000000));
        ctx.code.U8(Op::I32And);
        ctx.code.U8(Op::I32Or);
    }
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.I32Const(29);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    EmitEndNzcvStore(ctx);
}

template<>
void EmitIR<IR::Opcode::A32GetCFlag>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPushNzcv(ctx);
    ctx.code.I32Const(29);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32OrQFlag>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, cpsr_q), 2);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(27);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, cpsr_q), 2);
}

template<>
void EmitIR<IR::Opcode::A32GetGEFlags>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, cpsr_ge), 2);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32SetGEFlags>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.code.LocalGet(kParamState);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, cpsr_ge), 2);
}

template<>
void EmitIR<IR::Opcode::A32SetGEFlagsCompressed>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    // Each 4-bit nibble expands to 0xFF (nonzero) or 0x00 in its byte lane:
    // ge = ((v & 0x8888) * 0x1F >> 3) & 0x01010101 * 0xFF... use the classic:
    //   ge = ((v & 0x8) ? 0xFF000000 : 0) | ... computed with masks.
    // Compact form: t = (v | v>>1 | v>>2 | v>>3) & 0x1111; ge = t*0xFF.
    const u32 t = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.U8(Op::I32Or);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(2);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.U8(Op::I32Or);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(3);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.U8(Op::I32Or);
    ctx.code.I32Const(0x1111);
    ctx.code.U8(Op::I32And);
    ctx.code.I32Const(0xFF);
    ctx.code.U8(Op::I32Mul);
    ctx.code.LocalSet(t);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(t);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, cpsr_ge), 2);
}

// ---------------------------------------------------------------------------
// PC / location descriptor
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A32BXWritePC>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 v = ctx.Temp(ValType::I32);
    const auto end = A32::LocationDescriptor{ctx.block->EndLocation()}.SetSingleStepping(false);
    const u32 upper_without_t = static_cast<u32>(end.UniqueHash() >> 32) & ~u32{1};
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(v);
    // Thumb targets are halfword aligned; ARM targets are word aligned.
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(-2);
    ctx.code.I32Const(-4);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::Select);
    ctx.code.U8(Op::I32And);
    ctx.code.Store(Op::I32Store, RegOffset(A32::Reg::R15), 2);
    // The frontend advances ITSTATE while translating the block. A return
    // after an IT sequence must not retain the entry's IT bits in its target
    // descriptor, even when there was no intervening direct branch.
    ctx.code.LocalGet(kParamState);
    ctx.code.I32Const(static_cast<s32>(upper_without_t));
    ctx.code.LocalGet(v);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, upper_location_descriptor), 2);
}

// regs[15] = low32(location); upper_location_descriptor = high32(location)
void EmitA32WriteLocationDescriptor(EmitContext& ctx, IR::LocationDescriptor location) {
    const u64 value = location.Value();
    ctx.code.LocalGet(kParamState);
    ctx.code.I32Const(static_cast<s32>(static_cast<u32>(value)));
    ctx.code.Store(Op::I32Store, RegOffset(A32::Reg::R15), 2);
    ctx.code.LocalGet(kParamState);
    ctx.code.I32Const(static_cast<s32>(static_cast<u32>(value >> 32)));
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, upper_location_descriptor), 2);
}

// Region branch: the target block runs with its own upper descriptor bits.
void EmitA32RegionLocationTransition(EmitContext& ctx, IR::LocationDescriptor from, IR::LocationDescriptor to) {
    const u32 upper = static_cast<u32>(to.Value() >> 32);
    if (upper == static_cast<u32>(from.Value() >> 32)) {
        return;
    }
    ctx.code.LocalGet(kParamState);
    ctx.code.I32Const(static_cast<s32>(upper));
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, upper_location_descriptor), 2);
}

// upper_location_descriptor << 32 | r15
void EmitA32PushCurrentLocationDescriptor(EmitContext& ctx) {
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, RegOffset(A32::Reg::R15), 2);
    ctx.code.U8(Op::I64ExtendI32U);
    EmitPushUpperLocdesc(ctx);
    ctx.code.U8(Op::I64ExtendI32U);
    ctx.code.I64Const(32);
    ctx.code.U8(Op::I64Shl);
    ctx.code.U8(Op::I64Or);
}

template<>
void EmitIR<IR::Opcode::A32UpdateUpperLocationDescriptor>(EmitContext& ctx, IR::Inst*) {
    // BX writes the completed descriptor itself, including its dynamic T bit.
    // Other dynamic exits (table branches, supervisor calls and exceptions)
    // still need the frontend's advanced ITSTATE before returning to dispatch.
    for (const auto& inst : *ctx.block) {
        if (inst.GetOpcode() == IR::Opcode::A32BXWritePC) {
            return;
        }
    }
    const auto upper = [](IR::LocationDescriptor location) {
        return static_cast<u32>(A32::LocationDescriptor{location}.SetSingleStepping(false).UniqueHash() >> 32);
    };
    const u32 next = upper(ctx.block->EndLocation()) & ~(ctx.emit_conf.always_little_endian ? u32{2} : u32{0});
    if (next != upper(ctx.block->Location())) {
        ctx.code.LocalGet(kParamState);
        ctx.code.I32Const(static_cast<s32>(next));
        ctx.code.Store(Op::I32Store, offsetof(A32JitState, upper_location_descriptor), 2);
    }
}

// ---------------------------------------------------------------------------
// Exceptions / barriers / FPSCR
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::A32CallSupervisor>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32}, {}});
    const u32 fn = ctx.Import(kImportCallSupervisor, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Call(fn);
}

template<>
void EmitIR<IR::Opcode::A32ExceptionRaised>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I64}, {}});
    const u32 fn = ctx.Import(kImportExceptionRaised, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Call(fn);
}

template<>
void EmitIR<IR::Opcode::A32DataSynchronizationBarrier>(EmitContext& ctx, IR::Inst*) {
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32}, {}});
    const u32 fn = ctx.Import(kImportDataSynchronizationBarrier, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.Call(fn);
}

template<>
void EmitIR<IR::Opcode::A32DataMemoryBarrier>(EmitContext&, IR::Inst*) {
    // Single-threaded WASM: no-op.
}

template<>
void EmitIR<IR::Opcode::A32InstructionSynchronizationBarrier>(EmitContext&, IR::Inst*) {
    // No self-modifying-code pipeline hazard for already-compiled blocks.
}

template<>
void EmitIR<IR::Opcode::A32GetFpscr>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPushUpperLocdesc(ctx);
    ctx.code.I32Const(static_cast<s32>(0xFFFF0000));
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, fpsr), 2);
    ctx.code.U8(Op::I32Or);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32SetFpscr>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    constexpr u32 FPSR_MASK = 0xF800009F;
    constexpr u32 FPCR_MASK = A32::LocationDescriptor::FPSCR_MODE_MASK;
    const u32 v = ctx.Temp(ValType::I32);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(v);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(static_cast<s32>(FPSR_MASK));
    ctx.code.U8(Op::I32And);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, fpsr), 2);
    ctx.code.LocalGet(kParamState);
    EmitPushUpperLocdesc(ctx);
    ctx.code.I32Const(0x0000FFFF);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalGet(v);
    ctx.code.I32Const(static_cast<s32>(FPCR_MASK));
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, upper_location_descriptor), 2);
}

// FPSCR.NZCV lives in fpsr bits 31..28, like the rest of the FPSR part of
// FPSCR (so VMRS / VMSR of the whole register and Jit::Fpscr() see it).
template<>
void EmitIR<IR::Opcode::A32GetFpscrNZCV>(EmitContext& ctx, IR::Inst* inst) {
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, fpsr), 2);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32SetFpscrNZCV>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, offsetof(A32JitState, fpsr), 2);
    ctx.code.I32Const(0x0FFFFFFF);
    ctx.code.U8(Op::I32And);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    ctx.code.U8(Op::I32Or);
    ctx.code.Store(Op::I32Store, offsetof(A32JitState, fpsr), 2);
}

template<>
void EmitIR<IR::Opcode::A32SetCheckBit>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.LocalSet(ctx.CheckBitLocal());
}


namespace {

// Picks the extended-register accesses of the block that can live in locals:
// each distinct (offset,size) access that overlaps no other access.
void CollectExtUnits(EmitContext& ctx) {
    struct Access {
        u32 offset, size;
        bool written;
    };
    std::vector<Access> accesses;
    auto note = [&](u32 offset, u32 size, bool written) {
        for (auto& a : accesses) {
            if (a.offset == offset && a.size == size) {
                a.written |= written;
                return;
            }
        }
        accesses.push_back({offset, size, written});
    };
    for (IR::Block* block : ctx.CachedBlocks()) {
    for (auto& inst : *block) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::A32GetExtendedRegister32:
            note(ExtRegOffset(inst.GetArg(0).GetA32ExtRegRef()), 4, false);
            break;
        case IR::Opcode::A32SetExtendedRegister32:
            note(ExtRegOffset(inst.GetArg(0).GetA32ExtRegRef()), 4, true);
            break;
        case IR::Opcode::A32GetExtendedRegister64:
            note(ExtRegOffset(inst.GetArg(0).GetA32ExtRegRef()), 8, false);
            break;
        case IR::Opcode::A32SetExtendedRegister64:
            note(ExtRegOffset(inst.GetArg(0).GetA32ExtRegRef()), 8, true);
            break;
        case IR::Opcode::A32GetVector:
            note(ExtRegOffset(inst.GetArg(0).GetA32ExtRegRef()), VectorAccessSize(inst.GetArg(0)), false);
            break;
        case IR::Opcode::A32SetVector:
            note(ExtRegOffset(inst.GetArg(0).GetA32ExtRegRef()), VectorAccessSize(inst.GetArg(0)), true);
            break;
        default:
            break;
        }
    }
    }
    auto& rc = ctx.reg_cache;
    for (size_t i = 0; i < accesses.size(); i++) {
        bool overlaps = false;
        for (size_t j = 0; j < accesses.size(); j++) {
            if (i != j && accesses[i].offset < accesses[j].offset + accesses[j].size &&
                accesses[j].offset < accesses[i].offset + accesses[i].size) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            continue;
        }
        const ValType type = accesses[i].size == 4 ? ValType::I32 : accesses[i].size == 8 ? ValType::I64
                                                                                          : ValType::V128;
        rc.ext.push_back({accesses[i].offset, accesses[i].size, ctx.code.AddLocals(1, type), accesses[i].written});
    }
}

}  // anonymous namespace

bool CollectA32RegisterUnits(EmitContext& ctx, bool region) {
    // Registers R0..R14 touched by the code are cached in locals (R15 never
    // is). A self-loop body that rewrites the PC (R15 writes, BX) is not a
    // loop; in a region such a block just exits through its terminal.
    std::array<bool, 16> cached{};
    std::array<bool, 16> written{};
    for (IR::Block* block : ctx.CachedBlocks()) {
    for (auto& inst : *block) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::A32GetRegister: {
            const A32::Reg reg = inst.GetArg(0).GetA32RegRef();
            if (reg != A32::Reg::R15) {
                cached[static_cast<size_t>(reg)] = true;
            }
            break;
        }
        case IR::Opcode::A32SetRegister: {
            const A32::Reg reg = inst.GetArg(0).GetA32RegRef();
            if (reg == A32::Reg::R15) {
                if (!region) {
                    return false;
                }
                break;
            }
            cached[static_cast<size_t>(reg)] = true;
            written[static_cast<size_t>(reg)] = true;
            break;
        }
        case IR::Opcode::A32BXWritePC:
            if (!region) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    }

    auto& rc = ctx.reg_cache;
    rc.gpr_base = offsetof(A32JitState, regs);
    rc.cached = cached;
    rc.written = written;
    for (size_t r = 0; r < rc.cached.size(); r++) {
        if (rc.cached[r]) {
            rc.local[r] = ctx.code.AddLocals(1, ValType::I32);
        }
    }
    CollectExtUnits(ctx);
    return true;
}


}  // namespace Dynarmic::Backend::Wasm
