// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Self-loop transform, shared by both frontends: when a block's terminal
// links back to the block itself, the body + terminal run inside a wasm
// `loop` with guest registers cached in wasm locals (EmitContext::RegCache)
// and, where possible, the loop's NZCV producer deferred
// (EmitContext::DeferredFlags). FuncBody's before_call / after_call /
// before_exit hooks keep the state struct coherent wherever guest state can
// be observed, so no opcode emitter needs to know about the cache.
//
// The same register cache serves region compilation (several IR blocks in
// one function, EmitBeginRegion). The frontends decide which registers become
// cache units (CollectA32RegisterUnits / CollectA64RegisterUnits); everything
// else is here.

#include <algorithm>

#include <mcl/assert.hpp>

#include <boost/variant.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::Wasm {

void EmitCommitCycles(EmitContext& ctx) {
    if (ctx.cycle_local != 0) {
        ctx.code.LocalGet(kParamEctx);
        ctx.code.LocalGet(ctx.cycle_local);
        ctx.code.Store(Op::I64Store, offsetof(ExecutionContext, cycles_remaining), 3);
    }
}
void EmitReloadCycles(EmitContext& ctx) {
    if (ctx.cycle_local != 0) {
        ctx.code.LocalGet(kParamEctx);
        ctx.code.Load(Op::I64Load, offsetof(ExecutionContext, cycles_remaining), 3);
        ctx.code.LocalSet(ctx.cycle_local);
    }
}

namespace {

struct SelfLinkDetector {
    IR::LocationDescriptor self;
    bool found = false;

    void operator()(const IR::Term::LinkBlock& term) { found |= term.next == self; }
    void operator()(const IR::Term::LinkBlockFast& term) { found |= term.next == self; }
    void operator()(const IR::Term::If& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckBit& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckHalt& term) { boost::apply_visitor(*this, term.else_); }
    template<typename T>
    void operator()(const T&) {}
};

// Typed load/store of a cached register unit (state pointer / address
// already on the stack).
void EmitUnitLoad(EmitContext& ctx, const EmitContext::RegCache::ExtUnit& unit) {
    switch (unit.size) {
    case 4:
        ctx.code.Load(Op::I32Load, unit.offset, 2);
        break;
    case 8:
        ctx.code.Load(Op::I64Load, unit.offset, 3);
        break;
    default:
        ctx.code.Simd(Op::V128Load);
        ctx.code.ULEB(0);
        ctx.code.ULEB(unit.offset);
        break;
    }
}

void EmitUnitStore(EmitContext& ctx, const EmitContext::RegCache::ExtUnit& unit) {
    switch (unit.size) {
    case 4:
        ctx.code.Store(Op::I32Store, unit.offset, 2);
        break;
    case 8:
        ctx.code.Store(Op::I64Store, unit.offset, 3);
        break;
    default:
        ctx.code.Simd(Op::V128Store);
        ctx.code.ULEB(0);
        ctx.code.ULEB(unit.offset);
        break;
    }
}

// Stores every written cached register back to the state struct, and
// materializes deferred NZCV if an iteration has recorded it.
void EmitFlushRegCache(EmitContext& ctx) {
    const auto& rc = ctx.reg_cache;
    if (ctx.flags.active && rc.nzcv_cached) {
        EmitMaterializeDeferredFlags(ctx);  // into the NZCV local, stored below
    } else if (ctx.flags.active) {
        ctx.code.LocalGet(ctx.flags.valid);
        ctx.code.If();
        ctx.code.LocalGet(kParamState);
        EmitDeferredPackedNzcv(ctx);
        ctx.code.Store(Op::I32Store, ctx.state.nzcv, 2);
        ctx.code.End();
    }
    if (rc.nzcv_written) {
        ctx.code.LocalGet(kParamState);
        ctx.code.LocalGet(rc.nzcv_local);
        ctx.code.Store(Op::I32Store, ctx.state.nzcv, 2);
    }
    for (size_t r = 0; r < rc.cached.size(); r++) {
        if (rc.written[r]) {
            ctx.code.LocalGet(kParamState);
            ctx.code.LocalGet(rc.local[r]);
            ctx.code.Store(Op::I32Store, rc.gpr_base + 4 * static_cast<u32>(r), 2);
        }
    }
    for (const auto& unit : rc.ext) {
        if (!unit.written) {
            continue;
        }
        if (unit.scalar64) {
            ctx.code.LocalGet(kParamState);
            ctx.code.LocalGet(unit.local);
            ctx.code.Store(Op::I64Store, unit.offset, 3);
            ctx.code.LocalGet(unit.dirty);
            ctx.code.If();
            ctx.code.LocalGet(kParamState);
            ctx.code.I64Const(0);
            ctx.code.Store(Op::I64Store, unit.offset + 8, 3);
            ctx.code.End();
            continue;
        }
        ctx.code.LocalGet(kParamState);
        ctx.code.LocalGet(unit.local);
        EmitUnitStore(ctx, unit);
    }
}

// Reloads every cached register from the state struct.
void EmitReloadRegCache(EmitContext& ctx) {
    const auto& rc = ctx.reg_cache;
    if (rc.nzcv_cached) {
        ctx.code.LocalGet(kParamState);
        ctx.code.Load(Op::I32Load, ctx.state.nzcv, 2);
        ctx.code.LocalSet(rc.nzcv_local);
    }
    for (size_t r = 0; r < rc.cached.size(); r++) {
        if (rc.cached[r]) {
            ctx.code.LocalGet(kParamState);
            ctx.code.Load(Op::I32Load, rc.gpr_base + 4 * static_cast<u32>(r), 2);
            ctx.code.LocalSet(rc.local[r]);
        }
    }
    for (const auto& unit : rc.ext) {
        ctx.code.LocalGet(kParamState);
        if (unit.scalar64) {
            // Memory is authoritative for the upper half again.
            ctx.code.Load(Op::I64Load, unit.offset, 3);
            ctx.code.LocalSet(unit.local);
            if (unit.written) {
                ctx.code.I32Const(0);
                ctx.code.LocalSet(unit.dirty);
            }
            continue;
        }
        EmitUnitLoad(ctx, unit);
        ctx.code.LocalSet(unit.local);
    }
}

IR::Value ResolveIdentity(IR::Value v) {
    while (!v.IsImmediate() && v.GetInst()->GetOpcode() == IR::Opcode::Identity) {
        v = v.GetInst()->GetArg(0);
    }
    return v;
}

// Opcodes other than the one deferred setter that read or write NZCV state.
bool TouchesNzcvState(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A32SetCpsrNZCVRaw:
    case IR::Opcode::A32SetCpsrNZCVQ:
    case IR::Opcode::A32SetCpsrNZ:
    case IR::Opcode::A32SetCpsrNZC:
    case IR::Opcode::A32SetCpsr:
    case IR::Opcode::A32GetCpsr:
    case IR::Opcode::A32GetCFlag:
    case IR::Opcode::A64GetCFlag:
    case IR::Opcode::A64GetNZCVRaw:
    case IR::Opcode::A64SetNZCVRaw:
    case IR::Opcode::ConditionalSelect32:
    case IR::Opcode::ConditionalSelect64:
    case IR::Opcode::ConditionalSelectNZCV:
        return true;
    default:
        return false;
    }
}

// Opcodes that write the packed NZCV state (A32SetCpsrNZ / NZC also read it).
bool WritesNzcvState(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A32SetCpsrNZCV:
    case IR::Opcode::A32SetCpsrNZCVRaw:
    case IR::Opcode::A32SetCpsrNZCVQ:
    case IR::Opcode::A32SetCpsrNZ:
    case IR::Opcode::A32SetCpsrNZC:
    case IR::Opcode::A32SetCpsr:
    case IR::Opcode::A64SetNZCV:
    case IR::Opcode::A64SetNZCVRaw:
        return true;
    default:
        return false;
    }
}

struct CondTerminalDetector {
    bool found = false;

    void operator()(const IR::Term::If& term) {
        found = true;
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckBit& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckHalt& term) { boost::apply_visitor(*this, term.else_); }
    template<typename T>
    void operator()(const T&) {}
};

// Makes NZCV a cache unit when a cached block reads or writes it (flag
// opcodes, an entry condition, an If terminal) and it is not deferred.
void CollectNzcvUnit(EmitContext& ctx) {
    auto& rc = ctx.reg_cache;
    rc.nzcv_cached = false;
    rc.nzcv_written = false;
    if (ctx.flags.active) {
        return;
    }
    bool used = false;
    bool written = false;
    for (IR::Block* block : ctx.CachedBlocks()) {
        CondTerminalDetector detector;
        boost::apply_visitor(detector, block->GetTerminal());
        used |= detector.found || block->GetCondition() != IR::Cond::AL;
        for (auto& inst : *block) {
            written |= WritesNzcvState(inst.GetOpcode());
            used |= TouchesNzcvState(inst.GetOpcode());
        }
    }
    if (!used && !written) {
        return;
    }
    rc.nzcv_cached = true;
    rc.nzcv_written = written;
    rc.nzcv_local = ctx.code.AddLocals(1, ValType::I32);
}

// Matches setter = Set(NZCV)(GetNZCVFromOp(Add/Sub 32/64 with the constant
// carry-in of cmp/cmn/subs/adds)); fills out's is_sub / is64 / producer /
// nzcv / setter.
bool MatchAddSubFlagSetter(IR::Inst* setter, EmitContext::DeferredFlags& out) {
    const IR::Value packed = ResolveIdentity(setter->GetArg(0));
    if (packed.IsImmediate() || packed.GetInst()->GetOpcode() != IR::Opcode::GetNZCVFromOp) {
        return false;
    }
    IR::Inst* nzcv = packed.GetInst();
    const IR::Value prod = ResolveIdentity(nzcv->GetArg(0));
    if (prod.IsImmediate()) {
        return false;
    }
    IR::Inst* producer = prod.GetInst();
    bool is_sub, is64;
    switch (producer->GetOpcode()) {
    case IR::Opcode::Add32:
        is_sub = false, is64 = false;
        break;
    case IR::Opcode::Sub32:
        is_sub = true, is64 = false;
        break;
    case IR::Opcode::Add64:
        is_sub = false, is64 = true;
        break;
    case IR::Opcode::Sub64:
        is_sub = true, is64 = true;
        break;
    default:
        return false;
    }
    const IR::Value carry_in = ResolveIdentity(producer->GetArg(2));
    if (!carry_in.IsImmediate() || carry_in.GetU1() != is_sub) {
        return false;
    }
    out.is_sub = is_sub;
    out.is64 = is64;
    out.producer = producer;
    out.nzcv = nzcv;
    out.setter = setter;
    return true;
}

// Host calls after which guest flags may have changed (callbacks that can
// rewrite CPSR / PSTATE).
bool MayChangeFlagsInHost(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A32CallSupervisor:
    case IR::Opcode::A32ExceptionRaised:
    case IR::Opcode::A64CallSupervisor:
    case IR::Opcode::A64ExceptionRaised:
        return true;
    default:
        return false;
    }
}

// Finds the deferred-flags pattern (see EmitContext::DeferredFlags): exactly
// one NZCV writer, Set(NZCV)(GetNZCVFromOp(Add/Sub 32/64 with constant
// carry-in)), the packed value used nowhere else, no carry/overflow
// consumers, and nothing else in the block reading or writing NZCV.
bool FindDeferredFlags(EmitContext& ctx, EmitContext::DeferredFlags& out) {
    const IR::Opcode setter_op = ctx.emit_conf.frontend == Frontend::A64 ? IR::Opcode::A64SetNZCV : IR::Opcode::A32SetCpsrNZCV;
    IR::Inst* setter = nullptr;
    for (auto& inst : *ctx.block) {
        if (inst.GetOpcode() == setter_op) {
            if (setter) {
                return false;
            }
            setter = &inst;
        } else if (TouchesNzcvState(inst.GetOpcode())) {
            return false;
        }
    }
    if (!setter || !MatchAddSubFlagSetter(setter, out)) {
        return false;
    }
    IR::Inst* const producer = out.producer;
    IR::Inst* const nzcv = out.nzcv;
    if (producer->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp) ||
        producer->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp)) {
        return false;
    }
    // The packed value must have no consumer besides the setter.
    size_t uses = 0;
    for (auto& inst : *ctx.block) {
        for (size_t i = 0; i < inst.NumArgs(); i++) {
            const IR::Value arg = ResolveIdentity(inst.GetArg(i));
            if (!arg.IsImmediate() && arg.GetInst() == nzcv) {
                uses++;
            }
        }
    }
    return uses == 1;
}

// Back-edges taken per invocation of a self-loop block before it re-enters
// through the dispatcher (see the terminal emitter's self back-edge).
constexpr s32 kLoopIterationBudget = 1 << 16;

}  // anonymous namespace

namespace {

// Allocates the register cache (frontend collector), emits the initial load
// and installs the coherence hooks. A host call that requests a halt sets
// the iteration budget to 1, so the next back-edge leaves through the
// dispatcher (which sees the halt); back-edges themselves only test the
// budget.
bool SetupRegisterCache(EmitContext& ctx, bool region) {
    const bool collected = ctx.emit_conf.frontend == Frontend::A64 ? CollectA64RegisterUnits(ctx, region) : CollectA32RegisterUnits(ctx, region);
    if (!collected) {
        ctx.reg_cache = {};
        ctx.scalar_alias.clear();
        ctx.elided_widen.clear();
        return false;
    }
    auto& rc = ctx.reg_cache;
    rc.budget = ctx.code.AddLocals(1, ValType::I32);
    CollectNzcvUnit(ctx);
    EmitReloadRegCache(ctx);  // initial load from the state struct

    ctx.code.before_call = [&ctx](u32 func) {
        EmitCommitCycles(ctx);
        if (!ctx.pure_imports.count(func) && !ctx.memory_imports.count(func)) {
            EmitFlushRegCache(ctx);
        }
    };
    ctx.code.after_call = [&ctx](u32 func) {
        EmitReloadCycles(ctx);
        if (!ctx.pure_imports.count(func)) {
            if (!ctx.memory_imports.count(func)) {
                EmitReloadRegCache(ctx);
            }
            ctx.code.LocalGet(kParamEctx);
            ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, halt_reason), 2);
            ctx.code.If();
            ctx.code.I32Const(1);
            ctx.code.LocalSet(ctx.reg_cache.budget);
            ctx.code.End();
        }
    };
    ctx.code.before_exit = [&ctx] { EmitCommitCycles(ctx); EmitFlushRegCache(ctx); };

    rc.active = true;
    ctx.code.I32Const(kLoopIterationBudget);
    ctx.code.LocalSet(rc.budget);
    return true;
}

void TeardownRegisterCache(EmitContext& ctx) {
    ctx.reg_cache.active = false;
    ctx.reg_cache.nzcv_cached = false;
    ctx.reg_cache.nzcv_written = false;
    ctx.reg_cache.ext.clear();
    ctx.scalar_alias.clear();
    ctx.elided_widen.clear();
    ctx.flags.active = false;
    ctx.code.before_call = nullptr;
    ctx.code.after_call = nullptr;
    ctx.code.before_exit = nullptr;
}

}  // anonymous namespace

void EmitPushNzcv(EmitContext& ctx) {
    if (ctx.reg_cache.NzcvInLocal()) {
        ctx.code.LocalGet(ctx.reg_cache.nzcv_local);
        return;
    }
    ctx.code.LocalGet(kParamState);
    ctx.code.Load(Op::I32Load, ctx.state.nzcv, 2);
}

void EmitBeginNzcvStore(EmitContext& ctx) {
    if (!ctx.reg_cache.NzcvInLocal()) {
        ctx.code.LocalGet(kParamState);
    }
}

void EmitEndNzcvStore(EmitContext& ctx) {
    if (ctx.reg_cache.NzcvInLocal()) {
        ASSERT(ctx.reg_cache.nzcv_written);  // see WritesNzcvState
        ctx.code.LocalSet(ctx.reg_cache.nzcv_local);
        return;
    }
    ctx.code.Store(Op::I32Store, ctx.state.nzcv, 2);
}

void FindTerminalFlags(EmitContext& ctx) {
    auto& tf = ctx.term_flags;
    tf.active = false;
    tf.producer = nullptr;
    if (ctx.flags.active) {
        return;
    }
    CondTerminalDetector detector;
    boost::apply_visitor(detector, ctx.block->GetTerminal());
    if (!detector.found) {
        return;
    }
    // The last flag writer, with nothing after it reading or writing NZCV or
    // calling into the host in a way that could change it.
    IR::Inst* last = nullptr;
    for (auto& inst : *ctx.block) {
        const IR::Opcode op = inst.GetOpcode();
        if (WritesNzcvState(op)) {
            last = &inst;
        } else if (last && (TouchesNzcvState(op) || MayChangeFlagsInHost(op))) {
            return;
        }
    }
    const IR::Opcode setter_op = ctx.emit_conf.frontend == Frontend::A64 ? IR::Opcode::A64SetNZCV : IR::Opcode::A32SetCpsrNZCV;
    EmitContext::DeferredFlags match;
    if (!last || last->GetOpcode() != setter_op || !MatchAddSubFlagSetter(last, match)) {
        return;
    }
    const ValType t = match.is64 ? ValType::I64 : ValType::I32;
    auto& locals = match.is64 ? ctx.term_flag_locals64 : ctx.term_flag_locals32;
    if (locals[0] == 0) {
        for (u32& local : locals) {
            local = ctx.code.AddLocals(1, t);
        }
    }
    match.a = locals[0];
    match.b = locals[1];
    match.res = locals[2];
    tf = match;
}

void EmitCaptureTerminalFlags(EmitContext& ctx, IR::Inst* inst) {
    auto& tf = ctx.term_flags;
    if (!tf.producer) {
        return;
    }
    if (inst == tf.producer) {
        auto args = ctx.reg_alloc.GetArgumentInfo(inst);
        EmitPush(ctx.code, ctx.reg_alloc, args[0]);
        ctx.code.LocalSet(tf.a);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        ctx.code.LocalSet(tf.b);
        EmitPush(ctx.code, ctx.reg_alloc, IR::Value{inst});
        ctx.code.LocalSet(tf.res);
    } else if (inst == tf.setter) {
        tf.active = true;  // from here on, the flags are the producer's
    }
}

bool EmitBeginSelfLoop(EmitContext& ctx) {
    if (ctx.emit_conf.enable_cycle_counting || ctx.block->GetCondition() != IR::Cond::AL ||
        IsSingleStepping(ctx, ctx.block->Location())) {
        return false;
    }
    SelfLinkDetector detector{ctx.block->Location()};
    boost::apply_visitor(detector, ctx.block->GetTerminal());
    if (!detector.found) {
        return false;
    }

    EmitContext::DeferredFlags deferred;
    const bool defer = FindDeferredFlags(ctx, deferred);
    if (defer) {
        // Checked before the units are collected: with deferred flags NZCV
        // must not also become a cache unit.
        ctx.flags.active = true;
    }
    if (!SetupRegisterCache(ctx, false)) {
        ctx.flags = {};
        return false;
    }
    if (defer) {
        const ValType t = deferred.is64 ? ValType::I64 : ValType::I32;
        deferred.a = ctx.code.AddLocals(1, t);
        deferred.b = ctx.code.AddLocals(1, t);
        deferred.res = ctx.code.AddLocals(1, t);
        deferred.valid = ctx.code.AddLocals(1, ValType::I32);
        deferred.active = true;
        ctx.flags = deferred;
    }

    ctx.code.Loop();
    ctx.reg_cache.loop_depth = ctx.code.Depth();
    return true;
}

void EmitEndSelfLoop(EmitContext& ctx) {
    TeardownRegisterCache(ctx);
    ctx.code.End();
    // Every path out of the loop body returns or branches back; keep the
    // validator happy about the implicit function end.
    ctx.code.U8(Op::Unreachable);
}

// Select before loading the register cache: every published descriptor is a
// valid entry with its own ARM/Thumb, IT, endian and FPSCR state.
void EmitRegionEntrySelector(EmitContext& ctx) {
    const u32 location = ctx.code.AddLocals(1, ValType::I64);
    EmitPushCurrentLocationDescriptor(ctx);
    ctx.code.LocalSet(location);
    ctx.code.Block();
    const u32 selected_depth = ctx.code.Depth();
    // The original entry is the common path, even when guest-address sorting
    // put it after its successors. Check it before the remaining aliases.
    const auto emit_match = [&](u32 i) {
        ctx.code.LocalGet(location);
        ctx.code.I64Const(static_cast<s64>(ctx.region.blocks[i]->Location().Value()));
        ctx.code.U8(Op::I64Eq);
        ctx.code.If();
        ctx.code.I32Const(static_cast<s32>(i));
        ctx.code.LocalSet(ctx.region.next);
        ctx.code.Br(ctx.code.Depth() - selected_depth);
        ctx.code.End();
    };
    emit_match(ctx.region.entry);
    // Region order is by guest PC, which does not order Thumb/IT/endian
    // variants of the same PC. Sort the full descriptors before branching.
    std::vector<u32> aliases;
    for (u32 i = 0; i < ctx.region.blocks.size(); ++i) {
        if (i != ctx.region.entry) aliases.push_back(i);
    }
    const auto descriptor = [&](u32 i) { return ctx.region.blocks[i]->Location().Value(); };
    // A bounded decision tree avoids scanning up to 63 aliases on every
    // native resume. Leaves still test equality, so missing keys fail closed.
    const auto emit_search = [&](auto&& self, size_t begin, size_t end) -> void {
        if (end - begin == 1) {
            emit_match(aliases[begin]);
            return;
        }
        const size_t middle = begin + (end - begin) / 2;
        ctx.code.LocalGet(location);
        ctx.code.I64Const(static_cast<s64>(descriptor(aliases[middle])));
        ctx.code.U8(Op::I64LtU);
        ctx.code.If();
        self(self, begin, middle);
        ctx.code.Else();
        self(self, middle, end);
        ctx.code.End();
    };
    if (aliases.size() > 16) {
        std::sort(aliases.begin(), aliases.end(), [&](u32 a, u32 b) { return descriptor(a) < descriptor(b); });
        emit_search(emit_search, 0, aliases.size());
    } else {
        // Small regions retain the compact linear layout. Avoid perturbing
        // hot loop bodies merely to save a few entry comparisons.
        for (u32 i = 0; i < ctx.region.blocks.size(); ++i) {
            if (i != ctx.region.entry) emit_match(i);
        }
    }
    // A stale or mismatched descriptor must never execute another entry.
    ctx.code.I32Const(0);
    ctx.code.Return();
    ctx.code.End();
}

void EmitBeginRegion(EmitContext& ctx) {
    auto& region = ctx.region;
    const bool all_entries = ctx.emit_conf.enable_region_multi_entry;
    const bool multi_entry = (all_entries ||
                             (ctx.emit_conf.enable_a64_fallback_multi_entry &&
                              ctx.emit_conf.frontend == Frontend::A64)) &&
                             !IsSingleStepping(ctx, region.blocks[region.entry]->Location());
    if (multi_entry) {
        region.next = ctx.code.AddLocals(1, ValType::I32);
        if (all_entries) EmitRegionEntrySelector(ctx);
        else EmitA64FallbackEntrySelector(ctx);
    }
    const bool cached = SetupRegisterCache(ctx, true);
    ASSERT(cached);
    if (!multi_entry) {
        region.next = ctx.code.AddLocals(1, ValType::I32);
        ctx.code.I32Const(static_cast<s32>(region.entry));
        ctx.code.LocalSet(region.next);
    }

    const u32 n = static_cast<u32>(region.blocks.size());
    region.label_depth.assign(n, 0);
    ctx.code.Loop();
    region.dispatch_depth = ctx.code.Depth();
    for (u32 k = n - 1; k >= 1; k--) {
        ctx.code.Block();
        region.label_depth[k] = ctx.code.Depth();
    }
    ctx.code.Block();  // $B_0
    std::vector<u32> targets(n);
    for (u32 k = 0; k < n; k++) {
        targets[k] = k;  // $B_k is k frames out from inside $B_0
    }
    ctx.code.LocalGet(region.next);
    ctx.code.BrTable(targets, 0);
    ctx.code.End();  // $B_0: block 0's code follows
}

void EmitBeginStructuredRegion(EmitContext& ctx) {
    const bool cached = SetupRegisterCache(ctx, true);
    ASSERT(cached);
}

void EmitEndStructuredRegion(EmitContext& ctx) {
    TeardownRegisterCache(ctx);
    ctx.code.U8(Op::Unreachable);
}

void EmitRegionBlockStart(EmitContext& ctx, u32 index) {
    ctx.region.current = index;
    if (index != 0) {
        ctx.code.End();  // $B_index: its code follows
    }
}

void EmitMaterializeDeferredFlags(EmitContext& ctx) {
    ASSERT(ctx.flags.active && ctx.reg_cache.nzcv_cached);
    ctx.code.LocalGet(ctx.flags.valid);
    ctx.code.If();
    EmitDeferredPackedNzcv(ctx);
    ctx.code.LocalSet(ctx.reg_cache.nzcv_local);
    ctx.code.End();
}

bool EmitBeginRegionSelfLoop(EmitContext& ctx) {
    SelfLinkDetector detector{ctx.block->Location()};
    boost::apply_visitor(detector, ctx.block->GetTerminal());
    if (!detector.found || IsSingleStepping(ctx, ctx.block->Location())) {
        return false;
    }
    // Deferred flags as in a standalone self-loop (the entry condition would
    // read them before the first iteration recorded any, hence AL only);
    // leaving the block materializes them into the NZCV local.
    EmitContext::DeferredFlags deferred;
    if (ctx.block->GetCondition() == IR::Cond::AL && ctx.reg_cache.nzcv_cached && FindDeferredFlags(ctx, deferred)) {
        const ValType t = deferred.is64 ? ValType::I64 : ValType::I32;
        deferred.a = ctx.code.AddLocals(1, t);
        deferred.b = ctx.code.AddLocals(1, t);
        deferred.res = ctx.code.AddLocals(1, t);
        deferred.valid = ctx.code.AddLocals(1, ValType::I32);
        deferred.active = true;
        ctx.flags = deferred;
        ctx.code.I32Const(0);
        ctx.code.LocalSet(ctx.flags.valid);
    }
    ctx.code.Loop();
    ctx.reg_cache.loop_depth = ctx.code.Depth();
    ctx.region.self_loop_open = true;
    return true;
}

void EmitEndRegionSelfLoop(EmitContext& ctx) {
    ctx.flags = {};
    ctx.region.self_loop_open = false;
    ctx.code.End();
    // The block's terminal never falls out of the loop.
    ctx.code.U8(Op::Unreachable);
}

void EmitEndRegion(EmitContext& ctx) {
    TeardownRegisterCache(ctx);
    ctx.code.End();  // $dispatch
    ctx.code.U8(Op::Unreachable);
}

}  // namespace Dynarmic::Backend::Wasm
