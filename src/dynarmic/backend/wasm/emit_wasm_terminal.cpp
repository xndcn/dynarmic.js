// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// ARM condition evaluation, the block-entry condition prelude and terminals,
// shared by the A32 and A64 frontends. The few frontend-specific pieces
// (writing the next location back, the current location descriptor, the
// single-stepping flag) are dispatched on EmitConfig::frontend below.

#include <optional>

#include <mcl/assert.hpp>

#include <boost/variant.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::Wasm {

// ---------------------------------------------------------------------------
// Frontend dispatch
// ---------------------------------------------------------------------------

void EmitWriteLocationDescriptor(EmitContext& ctx, IR::LocationDescriptor location) {
    switch (ctx.emit_conf.frontend) {
    case Frontend::A32:
        EmitA32WriteLocationDescriptor(ctx, location);
        return;
    case Frontend::A64:
        EmitA64WriteLocationDescriptor(ctx, location);
        return;
    }
    UNREACHABLE();
}

void EmitPushCurrentLocationDescriptor(EmitContext& ctx) {
    switch (ctx.emit_conf.frontend) {
    case Frontend::A32:
        EmitA32PushCurrentLocationDescriptor(ctx);
        return;
    case Frontend::A64:
        EmitA64PushCurrentLocationDescriptor(ctx);
        return;
    }
    UNREACHABLE();
}

void EmitRegionLocationTransition(EmitContext& ctx, const IR::Block& from, const IR::Block& to) {
    switch (ctx.emit_conf.frontend) {
    case Frontend::A32:
        EmitA32RegionLocationTransition(ctx, from.Location(), to.Location());
        return;
    case Frontend::A64:
        // The FPCR bits are the same across a region and nothing else but the
        // PC is part of an A64 location.
        return;
    }
    UNREACHABLE();
}

bool IsSingleStepping(const EmitContext& ctx, IR::LocationDescriptor location) {
    switch (ctx.emit_conf.frontend) {
    case Frontend::A32:
        return A32::LocationDescriptor{location}.SingleStepping();
    case Frontend::A64:
        return A64::LocationDescriptor{location}.SingleStepping();
    }
    UNREACHABLE();
}

IR::LocationDescriptor WithoutSingleStepping(const EmitContext& ctx, IR::LocationDescriptor location) {
    switch (ctx.emit_conf.frontend) {
    case Frontend::A32:
        return A32::LocationDescriptor{location}.SetSingleStepping(false);
    case Frontend::A64:
        return A64::LocationDescriptor{location}.SetSingleStepping(false);
    }
    UNREACHABLE();
}

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
namespace {

u32 TransferAddress(const void* pointer) {
    return static_cast<u32>(reinterpret_cast<uptr>(pointer));
}

void EmitTransferIncrement(EmitContext& ctx, const u64& counter) {
    const u32 address = TransferAddress(&counter);
    ctx.code.I32Const(static_cast<s32>(address));
    ctx.code.I32Const(static_cast<s32>(address));
    ctx.code.Load(Op::I64Load, 0, 3);
    ctx.code.I64Const(1);
    ctx.code.U8(Op::I64Add);
    ctx.code.Store(Op::I64Store, 0, 3);
}

void EmitTransferTargetPc(EmitContext& ctx, u32 descriptor) {
    ctx.code.LocalGet(descriptor);
    switch (ctx.emit_conf.frontend) {
    case Frontend::A32:
        ctx.code.I64Const(0xffffffff);
        ctx.code.U8(Op::I64And);
        return;
    case Frontend::A64:
        // A64 descriptors carry a 56-bit PC; sign-extend its low bits without
        // confusing them with the FPCR and single-step descriptor fields.
        ctx.code.I64Const(64 - A64::LocationDescriptor::pc_bit_count);
        ctx.code.U8(Op::I64Shl);
        ctx.code.I64Const(64 - A64::LocationDescriptor::pc_bit_count);
        ctx.code.U8(Op::I64ShrS);
        return;
    }
    UNREACHABLE();
}

}  // anonymous namespace

void EmitRegionTransferEntry(EmitContext& ctx) {
    if (ctx.emit_conf.transfer_record != nullptr) {
        EmitTransferIncrement(ctx, ctx.emit_conf.transfer_record->entry_count);
    }
}

void EmitRegionTransfer(EmitContext& ctx, RegionTransferExit& exit, u32 target_descriptor) {
    EmitTransferIncrement(ctx, exit.count);
    ctx.code.I32Const(static_cast<s32>(TransferAddress(&exit.last_dynamic_target_descriptor)));
    ctx.code.LocalGet(target_descriptor);
    ctx.code.Store(Op::I64Store, 0, 3);
    ctx.code.I32Const(static_cast<s32>(TransferAddress(&exit.last_dynamic_target_pc)));
    EmitTransferTargetPc(ctx, target_descriptor);
    ctx.code.Store(Op::I64Store, 0, 3);
}
#endif

// ---------------------------------------------------------------------------
// Condition evaluation
// ---------------------------------------------------------------------------

namespace {

// Individual flags (i32 0/1) from the recorded operands of a deferred
// Add/Sub (EmitContext::flags or term_flags), 32 or 64-bit.
void EmitDeferredN(EmitContext& ctx, const EmitContext::DeferredFlags& f) {
    ctx.code.LocalGet(f.res);
    if (f.is64) {
        ctx.code.I64Const(63);
        ctx.code.U8(Op::I64ShrU);
        ctx.code.U8(Op::I32WrapI64);
    } else {
        ctx.code.I32Const(31);
        ctx.code.U8(Op::I32ShrU);
    }
}

void EmitDeferredZ(EmitContext& ctx, const EmitContext::DeferredFlags& f) {
    ctx.code.LocalGet(f.res);
    ctx.code.U8(f.is64 ? Op::I64Eqz : Op::I32Eqz);
}

void EmitDeferredC(EmitContext& ctx, const EmitContext::DeferredFlags& f) {
    ctx.code.LocalGet(f.is_sub ? f.a : f.res);
    ctx.code.LocalGet(f.is_sub ? f.b : f.a);
    ctx.code.U8(f.is_sub ? (f.is64 ? Op::I64GeU : Op::I32GeU) : (f.is64 ? Op::I64LtU : Op::I32LtU));
}

void EmitDeferredV(EmitContext& ctx, const EmitContext::DeferredFlags& f) {
    // sub: ((a ^ b) & (a ^ res)) >>> msb;  add: ((a ^ res) & (b ^ res)) >>> msb
    ctx.code.LocalGet(f.a);
    ctx.code.LocalGet(f.is_sub ? f.b : f.res);
    ctx.code.U8(f.is64 ? Op::I64Xor : Op::I32Xor);
    ctx.code.LocalGet(f.is_sub ? f.a : f.b);
    ctx.code.LocalGet(f.res);
    ctx.code.U8(f.is64 ? Op::I64Xor : Op::I32Xor);
    if (f.is64) {
        ctx.code.U8(Op::I64And);
        ctx.code.I64Const(63);
        ctx.code.U8(Op::I64ShrU);
        ctx.code.U8(Op::I32WrapI64);
    } else {
        ctx.code.U8(Op::I32And);
        ctx.code.I32Const(31);
        ctx.code.U8(Op::I32ShrU);
    }
}

}  // anonymous namespace

void EmitDeferredPackedNzcv(EmitContext& ctx) {
    const auto& f = ctx.flags;
    EmitDeferredN(ctx, f);
    ctx.code.I32Const(31);
    ctx.code.U8(Op::I32Shl);
    EmitDeferredZ(ctx, f);
    ctx.code.I32Const(30);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    EmitDeferredC(ctx, f);
    ctx.code.I32Const(29);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
    EmitDeferredV(ctx, f);
    ctx.code.I32Const(28);
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Or);
}

namespace {

// Branch condition straight from the recorded operands. After a subtract the
// integer comparisons map to a single wasm compare; everything else goes
// through the individual flag expressions.
void EmitDeferredCond(EmitContext& ctx, const EmitContext::DeferredFlags& f, IR::Cond cond) {
    FuncBody& code = ctx.code;
    if (f.is_sub) {
        u8 cmp = 0;
        const bool w = f.is64;
        switch (cond) {
        case IR::Cond::EQ: cmp = w ? Op::I64Eq : Op::I32Eq; break;
        case IR::Cond::NE: cmp = w ? Op::I64Ne : Op::I32Ne; break;
        case IR::Cond::CS: cmp = w ? Op::I64GeU : Op::I32GeU; break;
        case IR::Cond::CC: cmp = w ? Op::I64LtU : Op::I32LtU; break;
        case IR::Cond::HI: cmp = w ? Op::I64GtU : Op::I32GtU; break;
        case IR::Cond::LS: cmp = w ? Op::I64LeU : Op::I32LeU; break;
        case IR::Cond::GE: cmp = w ? Op::I64GeS : Op::I32GeS; break;
        case IR::Cond::LT: cmp = w ? Op::I64LtS : Op::I32LtS; break;
        case IR::Cond::GT: cmp = w ? Op::I64GtS : Op::I32GtS; break;
        case IR::Cond::LE: cmp = w ? Op::I64LeS : Op::I32LeS; break;
        default: break;
        }
        if (cmp != 0) {
            code.LocalGet(f.a);
            code.LocalGet(f.b);
            code.U8(cmp);
            return;
        }
    }
    switch (cond) {
    case IR::Cond::EQ: EmitDeferredZ(ctx, f); return;
    case IR::Cond::NE: EmitDeferredZ(ctx, f); code.U8(Op::I32Eqz); return;
    case IR::Cond::CS: EmitDeferredC(ctx, f); return;
    case IR::Cond::CC: EmitDeferredC(ctx, f); code.U8(Op::I32Eqz); return;
    case IR::Cond::MI: EmitDeferredN(ctx, f); return;
    case IR::Cond::PL: EmitDeferredN(ctx, f); code.U8(Op::I32Eqz); return;
    case IR::Cond::VS: EmitDeferredV(ctx, f); return;
    case IR::Cond::VC: EmitDeferredV(ctx, f); code.U8(Op::I32Eqz); return;
    case IR::Cond::HI:
        EmitDeferredC(ctx, f);
        EmitDeferredZ(ctx, f);
        code.U8(Op::I32Eqz);
        code.U8(Op::I32And);
        return;
    case IR::Cond::LS:
        EmitDeferredC(ctx, f);
        code.U8(Op::I32Eqz);
        EmitDeferredZ(ctx, f);
        code.U8(Op::I32Or);
        return;
    case IR::Cond::GE:
        EmitDeferredN(ctx, f);
        EmitDeferredV(ctx, f);
        code.U8(Op::I32Eq);
        return;
    case IR::Cond::LT:
        EmitDeferredN(ctx, f);
        EmitDeferredV(ctx, f);
        code.U8(Op::I32Ne);
        return;
    case IR::Cond::GT:
        EmitDeferredZ(ctx, f);
        code.U8(Op::I32Eqz);
        EmitDeferredN(ctx, f);
        EmitDeferredV(ctx, f);
        code.U8(Op::I32Eq);
        code.U8(Op::I32And);
        return;
    case IR::Cond::LE:
        EmitDeferredZ(ctx, f);
        EmitDeferredN(ctx, f);
        EmitDeferredV(ctx, f);
        code.U8(Op::I32Ne);
        code.U8(Op::I32Or);
        return;
    case IR::Cond::AL:
    case IR::Cond::NV:  // A64: NV behaves like AL
        code.I32Const(1);
        return;
    default:
        UNREACHABLE();
    }
}

}  // anonymous namespace

void EmitCond(EmitContext& ctx, IR::Cond cond) {
    if (ctx.flags.active) {
        EmitDeferredCond(ctx, ctx.flags, cond);
        return;
    }
    if (ctx.term_flags.active) {
        EmitDeferredCond(ctx, ctx.term_flags, cond);
        return;
    }
    const u32 nzcv = ctx.Temp(ValType::I32);
    EmitPushNzcv(ctx);
    ctx.code.LocalSet(nzcv);

    auto bit = [&](u32 pos) {
        ctx.code.LocalGet(nzcv);
        ctx.code.I32Const(pos);
        ctx.code.U8(Op::I32ShrU);
        ctx.code.I32Const(1);
        ctx.code.U8(Op::I32And);
    };
    auto n_flag = [&] { bit(31); };
    auto z_flag = [&] { bit(30); };
    auto c_flag = [&] { bit(29); };
    auto v_flag = [&] { bit(28); };

    switch (cond) {
    case IR::Cond::EQ:
        z_flag();
        return;
    case IR::Cond::NE:
        z_flag();
        ctx.code.U8(Op::I32Eqz);
        return;
    case IR::Cond::CS:
        c_flag();
        return;
    case IR::Cond::CC:
        c_flag();
        ctx.code.U8(Op::I32Eqz);
        return;
    case IR::Cond::MI:
        n_flag();
        return;
    case IR::Cond::PL:
        n_flag();
        ctx.code.U8(Op::I32Eqz);
        return;
    case IR::Cond::VS:
        v_flag();
        return;
    case IR::Cond::VC:
        v_flag();
        ctx.code.U8(Op::I32Eqz);
        return;
    case IR::Cond::HI:
        // c & !z
        c_flag();
        z_flag();
        ctx.code.U8(Op::I32Eqz);
        ctx.code.U8(Op::I32And);
        return;
    case IR::Cond::LS:
        // !c | z
        c_flag();
        ctx.code.U8(Op::I32Eqz);
        z_flag();
        ctx.code.U8(Op::I32Or);
        return;
    case IR::Cond::GE:
        // n == v
        n_flag();
        v_flag();
        ctx.code.U8(Op::I32Eq);
        return;
    case IR::Cond::LT:
        // n != v
        n_flag();
        v_flag();
        ctx.code.U8(Op::I32Ne);
        return;
    case IR::Cond::GT:
        // !z & (n == v)
        z_flag();
        ctx.code.U8(Op::I32Eqz);
        n_flag();
        v_flag();
        ctx.code.U8(Op::I32Eq);
        ctx.code.U8(Op::I32And);
        return;
    case IR::Cond::LE:
        // z | (n != v)
        z_flag();
        n_flag();
        v_flag();
        ctx.code.U8(Op::I32Ne);
        ctx.code.U8(Op::I32Or);
        return;
    case IR::Cond::AL:
    case IR::Cond::NV:  // A64: NV behaves like AL
        ctx.code.I32Const(1);
        return;
    default:
        UNREACHABLE();
    }
}

// ---------------------------------------------------------------------------
// Terminals
// ---------------------------------------------------------------------------

namespace {

void EmitReturnToDispatch(EmitContext& ctx) {
    ctx.code.I32Const(0);
    ctx.code.Return();
}

struct TerminalVisitor {
    EmitContext& ctx;
    // Index of the next chaining slot: ctx.next_link_slot, preorder over the
    // terminal tree, matching CountLinkBlockFastTerminals.
    u32& next_link_slot = ctx.next_link_slot;

    // Single-step blocks never chain (the dispatcher must regain control after
    // one instruction) and leave the single-stepping flag behind.
    bool SingleStep() const { return IsSingleStepping(ctx, ctx.block->Location()); }
    void EmitStepExit(const IR::LocationDescriptor& next) {
        EmitWriteLocationDescriptor(ctx, WithoutSingleStepping(ctx, next));
        EmitReturnToDispatch(ctx);
    }

    void operator()(const IR::Term::Invalid&) {
        ctx.code.U8(Op::Unreachable);
    }

    void operator()(const IR::Term::Interpret& term) {
        if (ctx.emit_conf.frontend == Frontend::A64) {
            // (state, ectx, pc, num_instructions) -> halt bits
            const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I64, ValType::I32}, {ValType::I32}});
            const u32 fn = ctx.Import(kImportA64InterpreterFallback, ty);
            ctx.code.LocalGet(kParamState);
            ctx.code.LocalGet(kParamEctx);
            ctx.code.I64Const(static_cast<s64>(A64::LocationDescriptor{term.next}.PC()));
            ctx.code.I32Const(static_cast<s32>(term.num_instructions));
            ctx.code.Call(fn);
            ctx.code.Return();
            return;
        }
        // (state, ectx, next_low, next_high, num_instructions) -> halt bits
        const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32}, {ValType::I32}});
        const u32 fn = ctx.Import(kImportInterpreterFallback, ty);
        ctx.code.LocalGet(kParamState);
        ctx.code.LocalGet(kParamEctx);
        ctx.code.I32Const(static_cast<s32>(static_cast<u32>(term.next.Value())));
        ctx.code.I32Const(static_cast<s32>(static_cast<u32>(term.next.Value() >> 32)));
        ctx.code.I32Const(static_cast<s32>(term.num_instructions));
        ctx.code.Call(fn);
        ctx.code.Return();
    }

    void operator()(const IR::Term::ReturnToDispatch&) {
        EmitReturnToDispatch(ctx);
    }

    // A cycle-counted region may skip the native dispatcher only when the next
    // block cannot be observed by the board. Slow exits use the ordinary
    // location write/return path, which also flushes the region register cache.
    void EmitFastBoundary(const IR::LocationDescriptor& next) {
        const BoundaryState* boundary = ctx.emit_conf.boundary_state;
        if (boundary == nullptr) {
            return;
        }
        const u32 address = static_cast<u32>(reinterpret_cast<uptr>(boundary));
        const u64 pc = ctx.emit_conf.frontend == Frontend::A32 ? A32::LocationDescriptor{next}.PC()
                                                               : A64::LocationDescriptor{next}.PC();
        const u32 slot_index = static_cast<u32>((pc >> 1) & 0xffff);
        bool check_hooks = true;
        if (const auto& snapshot = ctx.emit_conf.boundary_hook_snapshot) {
            const u32 slot = (snapshot->instruction_hooks & 1) == 0 || snapshot->hook_slots == nullptr
                                ? 0 : snapshot->hook_slots[slot_index];
            check_hooks = ((snapshot->instruction_hooks | slot) & kBoundaryInstructionHookMask) != 0;
        }
        const u32 slow = ctx.Temp(ValType::I32);
        if (ctx.emit_conf.enable_cycle_counting) {
            if (ctx.cycle_local != 0) {
                ctx.code.LocalGet(ctx.cycle_local);
            } else {
                ctx.code.LocalGet(kParamEctx);
                ctx.code.Load(Op::I64Load, offsetof(ExecutionContext, cycles_remaining), 3);
            }
            ctx.code.I64Const(0);
            ctx.code.U8(Op::I64LeS);
        } else {
            ctx.code.I32Const(0);
        }
        ctx.code.LocalGet(kParamEctx);
        ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
        ctx.code.U8(Op::I32Or);
        ctx.code.I32Const(static_cast<s32>(address));
        ctx.code.Load(Op::I32Load, offsetof(BoundaryState, flags), 2);
        ctx.code.U8(Op::I32Or);
        if (check_hooks) {
            const u32 hooks = ctx.Temp(ValType::I32);
            ctx.code.I32Const(static_cast<s32>(address));
            ctx.code.Load(Op::I32Load, offsetof(BoundaryState, instruction_hooks), 2);
            ctx.code.LocalSet(hooks);
            // The filter is meaningful only when bit 0 is set. A disabled
            // filter may be null or contain stale slot bits.
            ctx.code.LocalGet(hooks);
            ctx.code.I32Const(1);
            ctx.code.U8(Op::I32And);
            ctx.code.If();
            ctx.code.I32Const(static_cast<s32>(address));
            ctx.code.Load(Op::I32Load, offsetof(BoundaryState, hook_slots), 2);
            ctx.code.I32Const(static_cast<s32>(slot_index));
            ctx.code.U8(Op::I32Add);
            ctx.code.Load(Op::I32Load8U, 0, 0);
            ctx.code.LocalGet(hooks);
            ctx.code.U8(Op::I32Or);
            ctx.code.LocalSet(hooks);
            ctx.code.End();
            ctx.code.LocalGet(hooks);
            ctx.code.I32Const(static_cast<s32>(kBoundaryInstructionHookMask));
            ctx.code.U8(Op::I32And);
            ctx.code.U8(Op::I32Or);
        }
        ctx.code.LocalSet(slow);
        ctx.code.LocalGet(slow);
        ctx.code.If();
        EmitWriteLocationDescriptor(ctx, next);
        EmitReturnToDispatch(ctx);
        ctx.code.End();
    }

    // Dynamic target variant used before fast-dispatch lookup and miss
    // reporting. The current PC and upper descriptor have already been
    // materialized, so a slow exit can return directly and flush cached regs.
    void EmitDynamicFastBoundary(u32 cur) {
        const BoundaryState* boundary = ctx.emit_conf.boundary_state;
        if (boundary == nullptr) {
            return;
        }
        const u32 address = static_cast<u32>(reinterpret_cast<uptr>(boundary));
        const auto& snapshot = ctx.emit_conf.boundary_hook_snapshot;
        const bool check_global_hooks = !snapshot || (snapshot->instruction_hooks & kBoundaryInstructionHookMask) != 0;
        const bool check_hook_slots = !snapshot || (snapshot->instruction_hooks & 1) != 0;
        const u32 slow = ctx.Temp(ValType::I32);
        if (ctx.emit_conf.enable_cycle_counting) {
            if (ctx.cycle_local != 0) {
                ctx.code.LocalGet(ctx.cycle_local);
            } else {
                ctx.code.LocalGet(kParamEctx);
                ctx.code.Load(Op::I64Load, offsetof(ExecutionContext, cycles_remaining), 3);
            }
            ctx.code.I64Const(0);
            ctx.code.U8(Op::I64LeS);
        } else {
            ctx.code.I32Const(0);
        }
        ctx.code.LocalGet(kParamEctx);
        ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
        ctx.code.U8(Op::I32Or);
        ctx.code.I32Const(static_cast<s32>(address));
        ctx.code.Load(Op::I32Load, offsetof(BoundaryState, flags), 2);
        ctx.code.U8(Op::I32Or);
        if (check_global_hooks || check_hook_slots) {
            const u32 hooks = ctx.Temp(ValType::I32);
            if (check_global_hooks) {
                ctx.code.I32Const(static_cast<s32>(address));
                ctx.code.Load(Op::I32Load, offsetof(BoundaryState, instruction_hooks), 2);
            } else {
                ctx.code.I32Const(0);
            }
            ctx.code.LocalSet(hooks);
            if (check_hook_slots) {
                // Bit 0 says whether the conservative filter can contain any
                // local hooks. In particular, a null hook_slots pointer with
                // no hooks must not read unrelated low linear-memory bytes.
                if (check_global_hooks) {
                    ctx.code.LocalGet(hooks);
                    ctx.code.I32Const(1);
                    ctx.code.U8(Op::I32And);
                    ctx.code.If();
                }
                ctx.code.I32Const(static_cast<s32>(address));
                ctx.code.Load(Op::I32Load, offsetof(BoundaryState, hook_slots), 2);
                ctx.code.LocalGet(cur);
                ctx.code.U8(Op::I32WrapI64);
                ctx.code.I32Const(1);
                ctx.code.U8(Op::I32ShrU);
                ctx.code.I32Const(0xffff);
                ctx.code.U8(Op::I32And);
                ctx.code.U8(Op::I32Add);
                ctx.code.Load(Op::I32Load8U, 0, 0);
                ctx.code.LocalGet(hooks);
                ctx.code.U8(Op::I32Or);
                ctx.code.LocalSet(hooks);
                if (check_global_hooks) {
                    ctx.code.End();
                }
            }
            ctx.code.LocalGet(hooks);
            ctx.code.I32Const(static_cast<s32>(kBoundaryInstructionHookMask));
            ctx.code.U8(Op::I32And);
            ctx.code.U8(Op::I32Or);
        }
        ctx.code.LocalSet(slow);
        ctx.code.LocalGet(slow);
        ctx.code.If();
        EmitReturnToDispatch(ctx);
        ctx.code.End();
    }

    // Self-loop back-edge: branch to the loop head unless a halt is pending
    // (registers stay cached in locals; nothing is flushed).
    void EmitSelfBackEdge(const IR::LocationDescriptor& next) {
        if (ctx.emit_conf.link_slots_base != 0) {
            next_link_slot++;  // keep slot numbering aligned with CountLinkBlockFastTerminals
        }
        EmitFastBoundary(next);
        // Iteration budget: periodically leave through the dispatcher so the
        // engine's tier-up (which only takes effect on a fresh call -- wasm has
        // no OSR) can replace this function's baseline code in a long loop.
        // A halt requested by a host call drops the budget to 1 (see the
        // register cache's after_call hook); requests from outside wait for
        // the budget to run out (bounded latency).
        ctx.code.LocalGet(ctx.reg_cache.budget);
        ctx.code.I32Const(1);
        ctx.code.U8(Op::I32Sub);
        ctx.code.LocalTee(ctx.reg_cache.budget);
        ctx.code.BrIf(ctx.code.Depth() - ctx.reg_cache.loop_depth);
        // Halt pending or budget spent: leave through the dispatcher at the loop head.
        EmitWriteLocationDescriptor(ctx, next);
        EmitReturnToDispatch(ctx);
    }

    bool IsSelfEdge(const IR::LocationDescriptor& next) const {
        return ctx.reg_cache.active && (!ctx.region.Multi() || ctx.region.self_loop_open) && next == ctx.block->Location();
    }

    // Region: the index of `next` if it is one of the region's blocks.
    std::optional<u32> RegionIndex(const IR::LocationDescriptor& next) const {
        if (!ctx.region.Multi()) {
            return std::nullopt;
        }
        const auto it = ctx.region.index_of.find(next.Value());
        if (it == ctx.region.index_of.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Branch to block `target` of the region. Forward: its label is still
    // open. Backward: through the dispatch br_table, counting against the
    // iteration budget (tier-up re-entry, halts requested by host calls).
    void EmitRegionEdge(u32 target, const IR::LocationDescriptor& next, bool link_edge = true) {
        auto& region = ctx.region;
        if (!region.CanBranchTo(target)) {
            // Structured layout without a label for the target here: leave
            // the region like an outside edge (using this edge's slot).
            if (link_edge && ctx.emit_conf.link_slots_base != 0) {
                EmitChainedLink(next);
            } else {
                EmitWriteLocationDescriptor(ctx, next);
                EmitReturnToDispatch(ctx);
            }
            return;
        }
        if (link_edge && ctx.emit_conf.link_slots_base != 0) {
            next_link_slot++;  // keep slot numbering aligned with CountLinkBlockFastTerminals
        }
        if (ctx.flags.active) {
            EmitMaterializeDeferredFlags(ctx);  // the target reads the NZCV local
        }
        // In-region edges leave the PC unwritten (no code reads it), but each
        // block expects the state's other location bits to be its own (A32:
        // T/E/IT state in upper_location_descriptor).
        EmitRegionLocationTransition(ctx, *ctx.block, *region.blocks[target]);
        EmitFastBoundary(next);
        const bool backward = region.structured ? region.open_loop_depth[target] != 0 : target <= region.current;
        if (!backward) {
            ctx.code.Br(ctx.code.Depth() - (region.structured ? region.open_label_depth[target] : region.label_depth[target]));
            return;
        }
        ctx.code.LocalGet(ctx.reg_cache.budget);
        ctx.code.I32Const(1);
        ctx.code.U8(Op::I32Sub);
        ctx.code.LocalTee(ctx.reg_cache.budget);
        ctx.code.If();
        if (region.structured) {
            ctx.code.Br(ctx.code.Depth() - region.open_loop_depth[target]);
        } else {
            ctx.code.I32Const(static_cast<s32>(target));
            ctx.code.LocalSet(region.next);
            ctx.code.Br(ctx.code.Depth() - region.dispatch_depth);
        }
        ctx.code.End();
        // Budget spent or halt pending: leave through the dispatcher.
        EmitWriteLocationDescriptor(ctx, next);
        EmitReturnToDispatch(ctx);
    }

    void operator()(const IR::Term::LinkBlock& term) {
        if (SingleStep()) {
            EmitStepExit(term.next);
            return;
        }
        if (IsSelfEdge(term.next)) {
            EmitSelfBackEdge(term.next);
            return;
        }
        if (const auto target = RegionIndex(term.next)) {
            EmitRegionEdge(*target, term.next);
            return;
        }
        // LinkBlock carries halt/tick-check semantics. Ordinary cycle-counted
        // blocks still need the dispatcher; boundary-aware regions instead
        // repeat the complete guard in EmitChainedLink before every tail-call.
        if ((ctx.emit_conf.enable_cycle_counting && ctx.emit_conf.boundary_state == nullptr) ||
            ctx.emit_conf.link_slots_base == 0) {
            EmitWriteLocationDescriptor(ctx, term.next);
            EmitReturnToDispatch(ctx);
            return;
        }
        EmitChainedLink(term.next);
    }

    void operator()(const IR::Term::LinkBlockFast& term) {
        if (SingleStep()) {
            EmitStepExit(term.next);
            return;
        }
        if (IsSelfEdge(term.next)) {
            EmitSelfBackEdge(term.next);
            return;
        }
        if (const auto target = RegionIndex(term.next)) {
            EmitRegionEdge(*target, term.next);
            return;
        }
        if (ctx.emit_conf.link_slots_base == 0) {
            EmitWriteLocationDescriptor(ctx, term.next);
            EmitReturnToDispatch(ctx);
            return;
        }
        EmitChainedLink(term.next);
    }

    // Direct chaining: if this edge has been linked, tail-call the target
    // block through the imported function table. A halt request or an
    // unlinked edge falls back to the dispatcher, which patches the slot
    // (via ExecutionContext::link_slot_addr) before re-dispatching.
    void EmitChainedLink(const IR::LocationDescriptor& next) {
        EmitFastBoundary(next);
        EmitWriteLocationDescriptor(ctx, next);

        const u32 slot_addr = ctx.emit_conf.link_slots_base + 4 * next_link_slot++;
        const u32 idx = ctx.Temp(ValType::I32);
        const u32 block_ty = ctx.Type({{ValType::I32, ValType::I32}, {ValType::I32}});

        ctx.code.I32Const(static_cast<s32>(slot_addr));
        ctx.code.Load(Op::I32Load, 0, 2);
        ctx.code.LocalTee(idx);
        // Chain only when the slot is linked and no halt is pending (the
        // dispatcher's halt check is bypassed while chaining).
        ctx.code.U8(Op::I32Eqz);
        ctx.code.U8(Op::I32Eqz);
        ctx.code.LocalGet(kParamEctx);
        ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
        ctx.code.U8(Op::I32Eqz);
        ctx.code.U8(Op::I32And);
        ctx.code.If();
        {
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
            if (ctx.emit_conf.transfer_record != nullptr) {
                RegionTransferExit& exit = ctx.emit_conf.transfer_record->AddExit(
                    ctx.block->Location().Value(), RegionTransferKind::ExternalDirectCall, next.Value());
                const u32 target = ctx.Temp(ValType::I64);
                ctx.code.I64Const(static_cast<s64>(next.Value()));
                ctx.code.LocalSet(target);
                EmitRegionTransfer(ctx, exit, target);
            }
#endif
            ctx.code.LocalGet(kParamState);
            ctx.code.LocalGet(kParamEctx);
            ctx.code.LocalGet(idx);
            ctx.code.ReturnCallIndirect(block_ty, 0);
        }
        ctx.code.Else();
        {
            // Miss path: if unlinked, ask the dispatcher to patch this slot.
            ctx.code.LocalGet(idx);
            ctx.code.U8(Op::I32Eqz);
            ctx.code.If();
            ctx.code.LocalGet(kParamEctx);
            ctx.code.I32Const(static_cast<s32>(slot_addr));
            ctx.code.Store(Op::I32Store, offsetof(ExecutionContext, link_slot_addr), 2);
            ctx.code.End();
            EmitReturnToDispatch(ctx);
        }
        ctx.code.End();
    }

    // Region: a return to a call site inside the region branches there
    // (popping the RSB entry the call pushed); otherwise falls through.
    void EmitRegionReturn() {
        if (!ctx.region.Multi() || ctx.region.return_sites.empty() || SingleStep()) {
            return;
        }
        const u32 cur = ctx.Temp(ValType::I64);
        EmitPushCurrentLocationDescriptor(ctx);
        ctx.code.LocalSet(cur);
        for (const u32 site : ctx.region.return_sites) {
            if (!ctx.region.CanBranchTo(site)) {
                continue;
            }
            const IR::LocationDescriptor location = ctx.region.blocks[site]->Location();
            ctx.code.LocalGet(cur);
            ctx.code.I64Const(static_cast<s64>(location.Value()));
            ctx.code.U8(Op::I64Eq);
            ctx.code.If();
            // rsb_top = (rsb_top - 1) & kRsbMask
            ctx.code.LocalGet(kParamEctx);
            ctx.code.LocalGet(kParamEctx);
            ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, rsb_top), 2);
            ctx.code.I32Const(1);
            ctx.code.U8(Op::I32Sub);
            ctx.code.I32Const(static_cast<s32>(kRsbMask));
            ctx.code.U8(Op::I32And);
            ctx.code.Store(Op::I32Store, offsetof(ExecutionContext, rsb_top), 2);
            EmitRegionEdge(site, location, false);
            ctx.code.End();
        }
    }

    void operator()(const IR::Term::PopRSBHint&) {
        EmitRegionReturn();
        if (SingleStep()) {
            EmitReturnToDispatch(ctx);
            return;
        }
        if (ctx.emit_conf.boundary_state != nullptr) {
            // In board mode a return is an ordinary dynamic target. Reuse the
            // full-descriptor cache and its budget/halt/hook guards; do not
            // speculate through an RSB index without those observations.
            EmitFastDispatch();
            return;
        }
        if (!ctx.table_imported || SingleStep()) {
            EmitReturnToDispatch(ctx);
            return;
        }

        // Return stack buffer speculation: pop the top entry and tail-call its
        // block if the descriptor matches the current location. Unlinked
        // entries (index == 0) ask the dispatcher to patch them via
        // link_slot_addr; mismatches return to the dispatcher.
        const u32 top = ctx.Temp(ValType::I32);
        const u32 slot = ctx.Temp(ValType::I32);
        const u32 idx = ctx.Temp(ValType::I32);
        const u32 match = ctx.Temp(ValType::I32);
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        u32 transfer_target = 0;
#endif
        const u32 block_ty = ctx.Type({{ValType::I32, ValType::I32}, {ValType::I32}});

        // top = (rsb_top - 1) & kRsbMask; rsb_top = top (commit the pop)
        ctx.code.LocalGet(kParamEctx);
        ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, rsb_top), 2);
        ctx.code.I32Const(1);
        ctx.code.U8(Op::I32Sub);
        ctx.code.I32Const(static_cast<s32>(kRsbMask));
        ctx.code.U8(Op::I32And);
        ctx.code.LocalTee(top);
        // slot = ectx + offsetof(rsb) + top * sizeof(RsbEntry)
        ctx.code.I32Const(4);  // log2(sizeof(RsbEntry))
        ctx.code.U8(Op::I32Shl);
        ctx.code.LocalGet(kParamEctx);
        ctx.code.U8(Op::I32Add);
        ctx.code.I32Const(static_cast<s32>(offsetof(ExecutionContext, rsb)));
        ctx.code.U8(Op::I32Add);
        ctx.code.LocalTee(slot);
        ctx.code.LocalGet(kParamEctx);
        ctx.code.LocalGet(top);
        ctx.code.Store(Op::I32Store, offsetof(ExecutionContext, rsb_top), 2);

        // match = rsb[slot].descriptor == current location descriptor
        ctx.code.LocalGet(slot);
        ctx.code.Load(Op::I64Load, offsetof(ExecutionContext::RsbEntry, descriptor), 3);
        EmitPushCurrentLocationDescriptor(ctx);
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        if (ctx.emit_conf.transfer_record != nullptr) {
            transfer_target = ctx.Temp(ValType::I64);
            ctx.code.LocalTee(transfer_target);
        }
#endif
        ctx.code.U8(Op::I64Eq);
        ctx.code.LocalSet(match);

        ctx.code.LocalGet(match);
        ctx.code.If();
        {
            // idx != 0 && halt_reason == 0 -> tail-call the predicted block
            ctx.code.LocalGet(slot);
            ctx.code.Load(Op::I32Load, offsetof(ExecutionContext::RsbEntry, index), 2);
            ctx.code.LocalTee(idx);
            ctx.code.U8(Op::I32Eqz);
            ctx.code.U8(Op::I32Eqz);
            ctx.code.LocalGet(kParamEctx);
            ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
            ctx.code.U8(Op::I32Eqz);
            ctx.code.U8(Op::I32And);
            ctx.code.If();
            {
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
                if (ctx.emit_conf.transfer_record != nullptr) {
                    RegionTransferExit& exit = ctx.emit_conf.transfer_record->AddExit(
                        ctx.block->Location().Value(), RegionTransferKind::RsbReturn);
                    EmitRegionTransfer(ctx, exit, transfer_target);
                }
#endif
                ctx.code.LocalGet(kParamState);
                ctx.code.LocalGet(kParamEctx);
                ctx.code.LocalGet(idx);
                ctx.code.ReturnCallIndirect(block_ty, 0);
            }
            ctx.code.Else();
            {
                // Linked to nothing yet: ask the dispatcher to patch this entry.
                ctx.code.LocalGet(idx);
                ctx.code.U8(Op::I32Eqz);
                ctx.code.If();
                ctx.code.LocalGet(kParamEctx);
                ctx.code.LocalGet(slot);
                ctx.code.I32Const(static_cast<s32>(offsetof(ExecutionContext::RsbEntry, index)));
                ctx.code.U8(Op::I32Add);
                ctx.code.Store(Op::I32Store, offsetof(ExecutionContext, link_slot_addr), 2);
                ctx.code.End();
                EmitReturnToDispatch(ctx);
            }
            ctx.code.End();
        }
        ctx.code.Else();
        EmitFastDispatch();
        ctx.code.End();
    }

    void operator()(const IR::Term::FastDispatchHint&) {
        EmitRegionReturn();
        if (!ctx.table_imported || SingleStep()) {
            EmitReturnToDispatch(ctx);
            return;
        }
        EmitFastDispatch();
    }

    // Indirect branch through the fast dispatch table: tail-call the block
    // cached for the current location, or ask the dispatcher to cache it.
    void EmitFastDispatch() {
        if (ctx.emit_conf.fast_dispatch_base == 0) {
            EmitReturnToDispatch(ctx);
            return;
        }
        const u32 cur = ctx.Temp(ValType::I64);
        const u32 entry = ctx.Temp(ValType::I32);
        const u32 idx = ctx.Temp(ValType::I32);
        const u32 block_ty = ctx.Type({{ValType::I32, ValType::I32}, {ValType::I32}});

        EmitPushCurrentLocationDescriptor(ctx);
        ctx.code.LocalSet(cur);
        EmitDynamicFastBoundary(cur);
        ctx.code.LocalGet(cur);
        // entry = base + (((pc >> 2) ^ (pc >> 14)) & mask) * 16
        ctx.code.U8(Op::I32WrapI64);
        ctx.code.LocalTee(entry);
        ctx.code.I32Const(2);
        ctx.code.U8(Op::I32ShrU);
        ctx.code.LocalGet(entry);
        ctx.code.I32Const(14);
        ctx.code.U8(Op::I32ShrU);
        ctx.code.U8(Op::I32Xor);
        ctx.code.I32Const(static_cast<s32>(kFastDispatchEntries - 1));
        ctx.code.U8(Op::I32And);
        ctx.code.I32Const(4);  // log2(sizeof(FastDispatchEntry))
        ctx.code.U8(Op::I32Shl);
        ctx.code.I32Const(static_cast<s32>(ctx.emit_conf.fast_dispatch_base));
        ctx.code.U8(Op::I32Add);
        ctx.code.LocalTee(entry);

        // hit: full descriptor matches and the entry is linked. Boundary mode
        // already checked halt in EmitDynamicFastBoundary before the lookup.
        ctx.code.Load(Op::I64Load, offsetof(FastDispatchEntry, descriptor), 3);
        ctx.code.LocalGet(cur);
        ctx.code.U8(Op::I64Eq);
        ctx.code.LocalGet(entry);
        ctx.code.Load(Op::I32Load, offsetof(FastDispatchEntry, index), 2);
        ctx.code.LocalTee(idx);
        ctx.code.U8(Op::I32Eqz);
        ctx.code.U8(Op::I32Eqz);
        ctx.code.U8(Op::I32And);
        if (ctx.emit_conf.boundary_state == nullptr) {
            ctx.code.LocalGet(kParamEctx);
            ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
            ctx.code.U8(Op::I32Eqz);
            ctx.code.U8(Op::I32And);
        }
        ctx.code.If();
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        if (ctx.emit_conf.transfer_record != nullptr) {
            RegionTransferExit& exit = ctx.emit_conf.transfer_record->AddExit(
                ctx.block->Location().Value(), RegionTransferKind::OtherTailTransfer);
            EmitRegionTransfer(ctx, exit, cur);
        }
#endif
        ctx.code.LocalGet(kParamState);
        ctx.code.LocalGet(kParamEctx);
        ctx.code.LocalGet(idx);
        ctx.code.ReturnCallIndirect(block_ty, 0);
        ctx.code.End();
        // miss
        ctx.code.LocalGet(kParamEctx);
        ctx.code.LocalGet(entry);
        ctx.code.Store(Op::I32Store, offsetof(ExecutionContext, fast_dispatch_entry), 2);
        EmitReturnToDispatch(ctx);
    }

    void operator()(const IR::Term::If& term) {
        EmitCond(ctx, term.if_);
        ctx.code.If();
        boost::apply_visitor(*this, term.then_);
        ctx.code.Else();
        boost::apply_visitor(*this, term.else_);
        ctx.code.End();
    }

    void operator()(const IR::Term::CheckBit& term) {
        ctx.code.LocalGet(ctx.CheckBitLocal());
        ctx.code.If();
        boost::apply_visitor(*this, term.then_);
        ctx.code.Else();
        boost::apply_visitor(*this, term.else_);
        ctx.code.End();
    }

    void operator()(const IR::Term::CheckHalt& term) {
        ctx.code.Block();
        ctx.code.LocalGet(kParamEctx);
        ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
        ctx.code.U8(Op::I32Eqz);
        ctx.code.BrIf(0);
        ctx.code.LocalGet(kParamEctx);
        ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
        ctx.code.Return();
        ctx.code.End();
        boost::apply_visitor(*this, term.else_);
    }
};

void EmitTerminalTree(EmitContext& ctx, const IR::Terminal& terminal) {
    boost::apply_visitor(TerminalVisitor{ctx}, terminal);
}

}  // anonymous namespace

void EmitCondPrelude(EmitContext& ctx) {
    const IR::Cond cond = ctx.block->GetCondition();
    if (cond == IR::Cond::AL) {
        ASSERT(!ctx.block->HasConditionFailedLocation());
        return;
    }

    ASSERT(ctx.block->HasConditionFailedLocation());

    // A failed entry condition continues at the cond-fail location like a
    // LinkBlock edge (chained, or a branch within a region).
    EmitCond(ctx, cond);
    ctx.code.If();
    ctx.code.Else();
    // Failed conditions still enter guest instructions. This path bypasses
    // the ordinary IR body and must debit its own cycle count before linking.
    EmitChargeCycles(ctx, ctx.block->ConditionFailedCycleCount());
    TerminalVisitor{ctx}(IR::Term::LinkBlock{ctx.block->ConditionFailedLocation()});
    ctx.code.End();
}

void EmitTerminal(EmitContext& ctx) {
    EmitTerminalTree(ctx, ctx.block->GetTerminal());
    ctx.term_flags.active = false;
    ctx.term_flags.producer = nullptr;
    // The terminal always returns or traps; this is unreachable but keeps the
    // validator happy about the implicit function end.
    ctx.code.U8(Op::Unreachable);
}

namespace {

struct LinkSlotCounter {
    bool count_link_block;
    u32 count = 0;

    void operator()(const IR::Term::LinkBlock&) {
        // Chained only when cycle counting is disabled (see TerminalVisitor).
        if (count_link_block) {
            count++;
        }
    }
    void operator()(const IR::Term::LinkBlockFast&) {
        count++;
    }
    void operator()(const IR::Term::If& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckBit& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckHalt& term) {
        boost::apply_visitor(*this, term.else_);
    }
    template<typename T>
    void operator()(const T&) {}
};

}  // anonymous namespace

u32 CountLinkBlockFastTerminals(const IR::Terminal& terminal, bool count_link_block) {
    LinkSlotCounter counter{count_link_block};
    boost::apply_visitor(counter, terminal);
    return counter.count;
}

namespace {

struct IndirectTerminalDetector {
    bool detect_pop_rsb;
    bool detect_fast_dispatch;
    bool found = false;

    void operator()(const IR::Term::PopRSBHint&) {
        found |= detect_pop_rsb;
    }
    void operator()(const IR::Term::FastDispatchHint&) {
        found |= detect_fast_dispatch;
    }
    void operator()(const IR::Term::If& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckBit& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckHalt& term) {
        boost::apply_visitor(*this, term.else_);
    }
    template<typename T>
    void operator()(const T&) {}
};

}  // anonymous namespace

bool TerminalHasPopRSB(const IR::Terminal& terminal) {
    IndirectTerminalDetector detector{true, false};
    boost::apply_visitor(detector, terminal);
    return detector.found;
}

bool TerminalHasFastDispatch(const IR::Terminal& terminal) {
    IndirectTerminalDetector detector{false, true};
    boost::apply_visitor(detector, terminal);
    return detector.found;
}

}  // namespace Dynarmic::Backend::Wasm
