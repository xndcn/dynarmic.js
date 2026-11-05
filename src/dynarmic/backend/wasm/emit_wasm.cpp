// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/emit_wasm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

// ---------------------------------------------------------------------------
// EmitContext
// ---------------------------------------------------------------------------

namespace {

// Thunks that only touch FP / crypto scratch state and never guest registers.
bool IsPureImport(const std::string& name) {
    return name.rfind("fp_", 0) == 0 || name == kImportCrypto || name == kImportRecipEstimate ||
           name == kImportRecipSqrtEstimate;
}

// Guest memory access thunks (plain and exclusive). They run user memory
// callbacks, which -- as with upstream's GetSetElimination -- see guest
// registers as of the last flush, and must not change them.
bool IsMemoryImport(const std::string& name) {
    const std::string base = name.rfind("a64_", 0) == 0 ? name.substr(4) : name;
    for (const char* prefix : {"read", "write", "xread", "xwrite"}) {
        const size_t n = std::char_traits<char>::length(prefix);
        if (base.compare(0, n, prefix) == 0 && base.size() > n && std::isdigit(static_cast<unsigned char>(base[n]))) {
            return true;
        }
    }
    return base == "xclear";
}

}  // anonymous namespace

u32 EmitContext::Import(const char* name, u32 type_index) {
    if (const auto it = import_indices.find(name); it != import_indices.end()) {
        return it->second;
    }
    const u32 index = mod.ImportFunc(kImportModule, name, type_index);
    import_indices.emplace(name, index);
    if (IsPureImport(name)) {
        pure_imports.insert(index);
    } else if (IsMemoryImport(name)) {
        memory_imports.insert(index);
    }
    return index;
}

u32 EmitContext::Type(const FuncType& sig) {
    return mod.AddType(sig);
}

u32 EmitContext::Temp(ValType type) {
    auto& free_list = temp_free[static_cast<u32>(type)];
    if (!free_list.empty()) {
        const u32 local = free_list.back();
        free_list.pop_back();
        temp_live.emplace_back(local, type);
        return local;
    }
    const u32 local = code.AddLocals(1, type);
    temp_live.emplace_back(local, type);
    return local;
}

void EmitContext::FreeTemps() {
    for (const auto& [local, type] : temp_live) {
        temp_free[static_cast<u32>(type)].push_back(local);
    }
    temp_live.clear();
}

// ---------------------------------------------------------------------------
// Value push helpers
// ---------------------------------------------------------------------------

void EmitPushConst(FuncBody& code, IR::Value imm) {
    ASSERT(imm.IsImmediate());
    switch (imm.GetType()) {
    case IR::Type::U1:
        code.I32Const(imm.GetU1() ? 1 : 0);
        return;
    case IR::Type::U8:
        code.I32Const(imm.GetU8());
        return;
    case IR::Type::U16:
        code.I32Const(imm.GetU16());
        return;
    case IR::Type::U32:
        code.I32Const(static_cast<s32>(imm.GetU32()));
        return;
    case IR::Type::U64:
        code.I64Const(static_cast<s64>(imm.GetU64()));
        return;
    default:
        UNREACHABLE();
    }
}

void EmitPush(FuncBody& code, RegAlloc& ra, IR::Value value) {
    if (value.IsImmediate()) {
        EmitPushConst(code, value);
        return;
    }
    if (const auto local = ra.LocalOf(value)) {
        code.LocalGet(*local);
        return;
    }
    EmitPushConst(code, *ra.ImmediateOf(value));
}

void EmitPush(FuncBody& code, RegAlloc& ra, const RegAlloc::Argument& arg) {
    EmitPush(code, ra, arg.value);
}

// ---------------------------------------------------------------------------
// Generic opcodes
// ---------------------------------------------------------------------------

template<>
void EmitIR<IR::Opcode::Void>(EmitContext&, IR::Inst*) {}

template<>
void EmitIR<IR::Opcode::Identity>(EmitContext&, IR::Inst*) {
    // Handled transparently by RegAlloc::ResolveIdentity.
}

template<>
void EmitIR<IR::Opcode::Breakpoint>(EmitContext&, IR::Inst*) {
    UNIMPLEMENTED();
}

template<>
void EmitIR<IR::Opcode::CallHostFunction>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    u32 count = 0;
    while (count < 3 && inst->GetArg(count + 1).GetType() != IR::Type::Void) ++count;
    FuncType type;
    for (u32 i = 0; i < count; ++i) type.params.push_back(ValType::I64);
    const u32 ty = ctx.Type(type);
    for (u32 i = 0; i < count; ++i) EmitPush(ctx.code, ctx.reg_alloc, args[i + 1]);
    ctx.code.I32Const(static_cast<s32>(inst->GetArg(0).GetU64()));
    ctx.code.CallIndirect(ty, 0);
}

template<>
void EmitIR<IR::Opcode::PushRSB>(EmitContext& ctx, IR::Inst* inst) {
    // Board returns use guarded descriptor dispatch rather than RSB guesses.
    // Keep the IR operation for region return-site discovery, but omit the
    // speculative stack writes that board returns never read.
    if (ctx.emit_conf.boundary_state != nullptr) return;
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate());
    const u64 descriptor = args[0].GetImmediateU64();

    // slot = ectx + offsetof(rsb) + rsb_top * sizeof(RsbEntry)
    const u32 top = ctx.Temp(ValType::I32);
    const u32 slot = ctx.Temp(ValType::I32);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.Load(Op::I32Load, offsetof(ExecutionContext, rsb_top), 2);
    ctx.code.LocalTee(top);
    ctx.code.I32Const(4);  // log2(sizeof(RsbEntry))
    ctx.code.U8(Op::I32Shl);
    ctx.code.U8(Op::I32Add);
    ctx.code.I32Const(static_cast<s32>(offsetof(ExecutionContext, rsb)));
    ctx.code.U8(Op::I32Add);
    ctx.code.LocalTee(slot);
    // Only overwrite the entry when the descriptor changes, so a previously
    // linked index survives (e.g. a bl/bx-lr loop re-pushing the same return
    // address every iteration).
    ctx.code.LocalGet(slot);
    ctx.code.Load(Op::I64Load, offsetof(ExecutionContext::RsbEntry, descriptor), 3);
    ctx.code.I64Const(static_cast<s64>(descriptor));
    ctx.code.U8(Op::I64Eq);
    ctx.code.If();
    ctx.code.Else();
    // entry = { descriptor, index }: the return target's block from the fast
    // dispatch table if it is there (two call sites of one function alternate
    // in this slot), else 0 (linked lazily at pop time).
    ctx.code.LocalGet(slot);
    ctx.code.I64Const(static_cast<s64>(descriptor));
    ctx.code.Store(Op::I64Store, offsetof(ExecutionContext::RsbEntry, descriptor), 3);
    ctx.code.LocalGet(slot);
    if (ctx.emit_conf.fast_dispatch_base != 0 && ctx.emit_conf.boundary_state == nullptr) {
        const u32 fd = ctx.emit_conf.fast_dispatch_base + FastDispatchSlot(descriptor) * static_cast<u32>(sizeof(FastDispatchEntry));
        ctx.code.I32Const(static_cast<s32>(fd));
        ctx.code.Load(Op::I32Load, offsetof(FastDispatchEntry, index), 2);
        ctx.code.I32Const(0);
        ctx.code.I32Const(static_cast<s32>(fd));
        ctx.code.Load(Op::I64Load, offsetof(FastDispatchEntry, descriptor), 3);
        ctx.code.I64Const(static_cast<s64>(descriptor));
        ctx.code.U8(Op::I64Eq);
        ctx.code.U8(Op::Select);
    } else {
        ctx.code.I32Const(0);
    }
    ctx.code.Store(Op::I32Store, offsetof(ExecutionContext::RsbEntry, index), 2);
    ctx.code.End();
    // rsb_top = (top + 1) & kRsbMask
    ctx.code.LocalGet(kParamEctx);
    ctx.code.LocalGet(top);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32Add);
    ctx.code.I32Const(static_cast<s32>(kRsbMask));
    ctx.code.U8(Op::I32And);
    ctx.code.Store(Op::I32Store, offsetof(ExecutionContext, rsb_top), 2);
}

// Pseudo-operations: computed eagerly at their producer. Reaching one here
// means the producer handled it (it holds a local) or the producer does not
// support it, which is a bug.
// N/Z (bits 31, 30) of a constant: the standalone form a producer folded to
// an immediate leaves behind (C = V = 0 for GetNZCVFromOp).
static void EmitConstantNZ(EmitContext& ctx, IR::Inst* inst, const char* name) {
    const IR::Value arg = inst->GetArg(0);
    const auto imm = ctx.reg_alloc.ImmediateOf(arg);
    ASSERT_MSG(imm.has_value(), "producer {} did not handle {}", arg.GetInst()->GetOpcode(), name);
    const u64 value = imm->GetImmediateAsU64();
    const bool is64 = imm->GetType() == IR::Type::U64;
    const bool negative = is64 ? (value >> 63) != 0 : ((value >> 31) & 1) != 0;
    const bool zero = is64 ? value == 0 : static_cast<u32>(value) == 0;
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.I32Const(static_cast<s32>((negative ? 0x80000000u : 0) | (zero ? 0x40000000u : 0)));
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::GetCarryFromOp>(EmitContext& ctx, IR::Inst* inst) {
    ASSERT_MSG(ctx.reg_alloc.HasLocal(inst), "producer did not handle GetCarryFromOp");
}

template<>
void EmitIR<IR::Opcode::GetOverflowFromOp>(EmitContext& ctx, IR::Inst* inst) {
    ASSERT_MSG(ctx.reg_alloc.HasLocal(inst), "producer did not handle GetOverflowFromOp");
}

template<>
void EmitIR<IR::Opcode::GetGEFromOp>(EmitContext& ctx, IR::Inst* inst) {
    ASSERT_MSG(ctx.reg_alloc.HasLocal(inst), "producer did not handle GetGEFromOp");
}

template<>
void EmitIR<IR::Opcode::GetNZCVFromOp>(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.reg_alloc.HasLocal(inst)) {
        EmitConstantNZ(ctx, inst, "GetNZCVFromOp");
    }
}

template<>
void EmitIR<IR::Opcode::GetNZFromOp>(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.reg_alloc.HasLocal(inst)) {
        EmitConstantNZ(ctx, inst, "GetNZFromOp");
    }
}

template<>
void EmitIR<IR::Opcode::GetUpperFromOp>(EmitContext& ctx, IR::Inst* inst) {
    ASSERT_MSG(ctx.reg_alloc.HasLocal(inst), "producer did not handle GetUpperFromOp");
}

template<>
void EmitIR<IR::Opcode::GetLowerFromOp>(EmitContext& ctx, IR::Inst* inst) {
    ASSERT_MSG(ctx.reg_alloc.HasLocal(inst), "producer did not handle GetLowerFromOp");
}

template<>
void EmitIR<IR::Opcode::GetCFlagFromNZCV>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(29);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.I32Const(1);
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::NZCVFromPackedFlags>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.I32Const(static_cast<s32>(0xF0000000));
    ctx.code.U8(Op::I32And);
    ctx.code.LocalSet(dst);
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

namespace {

// Ops whose GetNZCVFromOp is N/Z of the result with C = V = 0.
bool IsLogicalNzcvProducer(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::And32:
    case IR::Opcode::And64:
    case IR::Opcode::AndNot32:
    case IR::Opcode::AndNot64:
    case IR::Opcode::Eor32:
    case IR::Opcode::Eor64:
    case IR::Opcode::Or32:
    case IR::Opcode::Or64:
    case IR::Opcode::Not32:
    case IR::Opcode::Not64:
        return true;
    default:
        return false;
    }
}

StateLayout LayoutOf(Frontend frontend) {
    switch (frontend) {
    case Frontend::A32:
        return {offsetof(A32JitState, cpsr_nzcv), offsetof(A32JitState, fpsr), offsetof(A32JitState, crypto_scratch)};
    case Frontend::A64:
        return {offsetof(A64JitState, cpsr_nzcv), offsetof(A64JitState, fpsr), offsetof(A64JitState, crypto_scratch)};
    }
    UNREACHABLE();
}

u32 FpcrOf(Frontend frontend, IR::LocationDescriptor location) {
    switch (frontend) {
    case Frontend::A32:
        return A32::LocationDescriptor{location}.FPSCR().Value();
    case Frontend::A64:
        return A64::LocationDescriptor{location}.FPCR().Value();
    }
    UNREACHABLE();
}

}  // anonymous namespace

// Emits the instructions of ctx.block (plus its cycle accounting).
static void EmitBlockBody(EmitContext& ctx) {
    RegAlloc& reg_alloc = ctx.reg_alloc;
    FuncBody& code = ctx.code;
    FindTerminalFlags(ctx);
    const bool is_a64 = ctx.emit_conf.frontend == Frontend::A64;
    for (auto iter = ctx.block->begin(); iter != ctx.block->end(); ++iter) {
        IR::Inst* inst = &*iter;
        switch (inst->GetOpcode()) {
#define OPCODE(name, type, ...)              \
    case IR::Opcode::name:                   \
        EmitIR<IR::Opcode::name>(ctx, inst); \
        break;
#define A32OPC(name, type, ...)                   \
    case IR::Opcode::A32##name:                   \
        ASSERT(!is_a64);                          \
        EmitIR<IR::Opcode::A32##name>(ctx, inst); \
        break;
#define A64OPC(name, type, ...)                   \
    case IR::Opcode::A64##name:                   \
        ASSERT(is_a64);                           \
        EmitIR<IR::Opcode::A64##name>(ctx, inst); \
        break;
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
        default:
            ASSERT_FALSE("Invalid opcode: {}", inst->GetOpcode());
            break;
        }
        EmitCaptureTerminalFlags(ctx, inst);

        // GetNZFromOp is generic: N/Z depend only on the producer's result
        // value, so compute it here regardless of which op produced it. The
        // same goes for GetNZCVFromOp of the logical ops (C and V are 0);
        // Add/Sub compute their own NZCV.
        // Pseudo-op insts must not be queried: their next_pseudoop link is
        // the producer's sibling chain, not their own list.
        if (!inst->IsAPseudoOperation()) {
            IR::Inst* nz_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZFromOp);
            if (!nz_inst && IsLogicalNzcvProducer(inst->GetOpcode())) {
                nz_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);
            }
            if (nz_inst) {
                ASSERT_MSG(inst->GetType() == IR::Type::U32 || inst->GetType() == IR::Type::U64,
                           "GetNZFromOp on unsupported result type");
                const u32 nz = reg_alloc.DefineLocal(code, nz_inst);
                if (inst->GetType() == IR::Type::U64) {
                    EmitPush(code, reg_alloc, IR::Value{inst});
                    code.I64Const(63);
                    code.U8(Op::I64ShrU);
                    code.U8(Op::I32WrapI64);
                    code.I32Const(31);
                    code.U8(Op::I32Shl);
                    EmitPush(code, reg_alloc, IR::Value{inst});
                    code.U8(Op::I64Eqz);
                    code.I32Const(30);
                    code.U8(Op::I32Shl);
                    code.U8(Op::I32Or);
                } else {
                    EmitPush(code, reg_alloc, IR::Value{inst});
                    code.I32Const(static_cast<s32>(0x80000000));
                    code.U8(Op::I32And);
                    EmitPush(code, reg_alloc, IR::Value{inst});
                    code.U8(Op::I32Eqz);
                    code.I32Const(30);
                    code.U8(Op::I32Shl);
                    code.U8(Op::I32Or);
                }
                code.LocalSet(nz);
            }
        }

        ctx.FreeTemps();
        reg_alloc.FreeArgsIfDead(inst);
    }

    reg_alloc.AssertNoMoreUses();

    EmitChargeCycles(ctx, ctx.block->CycleCount());

}

void EmitChargeCycles(EmitContext& ctx, u64 cycles) {
    if (!ctx.emit_conf.enable_cycle_counting) return;
    auto& code = ctx.code;
    if (ctx.cycle_local != 0) {
        code.LocalGet(ctx.cycle_local);
        code.I64Const(static_cast<s64>(cycles));
        code.U8(Op::I64Sub);
        code.LocalSet(ctx.cycle_local);
    } else {
        code.LocalGet(kParamEctx);
        code.LocalGet(kParamEctx);
        code.Load(Op::I64Load, offsetof(ExecutionContext, cycles_remaining), 3);
        code.I64Const(static_cast<s64>(cycles));
        code.U8(Op::I64Sub);
        code.Store(Op::I64Store, offsetof(ExecutionContext, cycles_remaining), 3);
    }
}

EmittedBlockInfo EmitWASM(IR::Block block, const EmitConfig& emit_conf) {
    std::vector<IR::Block> blocks;
    blocks.push_back(std::move(block));
    return EmitWASM(std::move(blocks), emit_conf, 0);
}

EmittedBlockInfo EmitWASM(std::vector<IR::Block> blocks, const EmitConfig& emit_conf, size_t entry_index) {
    ASSERT(entry_index < blocks.size());
    EmittedBlockInfo ebi;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    ebi.region_blocks.emitted.assign(blocks.size(), 0);
    ebi.region_blocks.entry_reachable.assign(blocks.size(), 0);
    ebi.region_blocks.return_sites.assign(blocks.size(), 0);
    if (blocks.size() == 1) {
        ebi.region_blocks.emitted[0] = 1;
        ebi.region_blocks.entry_reachable[0] = 1;
    }
#endif

    ModuleBuilder mod;
    FuncBody code{kParamCount};
    RegAlloc reg_alloc;
    for (IR::Block& block : blocks) {
        reg_alloc.PrecomputeUseCounts(block);
    }
    IR::Block& entry = blocks[entry_index];
    EmitContext ctx{&entry, reg_alloc, emit_conf, ebi, mod, code};
    ctx.state = LayoutOf(emit_conf.frontend);
    ctx.fpcr = FpcrOf(emit_conf.frontend, entry.Location());
    const bool is_a64 = emit_conf.frontend == Frontend::A64;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    EmitRegionTransferEntry(ctx);
#endif

    // Module skeleton: shared memory; the entry function is registered after
    // emission (below) so that all function imports are already known and its
    // index is final (imports occupy the lowest function indices).
    mod.ImportMemory(kImportModule, kMemoryImport, {1, std::nullopt});
    bool any_pop_rsb = false;
    bool any_fast_dispatch = false;
    bool any_host_call = false;
    for (const IR::Block& block : blocks) {
        for (const auto& inst : block) any_host_call |= inst.GetOpcode() == IR::Opcode::CallHostFunction;
        any_pop_rsb |= TerminalHasPopRSB(block.GetTerminal());
        any_fast_dispatch |= TerminalHasFastDispatch(block.GetTerminal());
    }
    const bool needs_indirect_table =
        (any_pop_rsb && (emit_conf.boundary_state == nullptr || emit_conf.fast_dispatch_base != 0)) ||
        (any_fast_dispatch && emit_conf.fast_dispatch_base != 0);
    if (emit_conf.link_slots_base != 0 || needs_indirect_table || any_host_call) {
        // Direct block chaining and enabled indirect speculation tail-call
        // through the main module's indirect function table (imported as table 0).
        mod.ImportTable(kImportModule, kTableImport, ValType::FuncRef, {0, std::nullopt});
        ctx.table_imported = true;
    }
    const u32 block_ty = ctx.Type({{ValType::I32, ValType::I32}, {ValType::I32}});

    if (emit_conf.enable_cycle_counting && emit_conf.boundary_state != nullptr) {
        ctx.cycle_local = code.AddLocals(1, ValType::I64);
        code.LocalGet(kParamEctx);
        code.Load(Op::I64Load, offsetof(ExecutionContext, cycles_remaining), 3);
        code.LocalSet(ctx.cycle_local);
        code.before_call = [&ctx](u32) { EmitCommitCycles(ctx); };
        code.after_call = [&ctx](u32) { EmitReloadCycles(ctx); };
        code.before_exit = [&ctx] { EmitCommitCycles(ctx); };
    }
    bool loops = false;
    if (blocks.size() > 1) {
        // All blocks of a region share the FPCR / FPSCR mode bits of the entry
        // (the address space only groups blocks with equal upper descriptors).
        auto& region = ctx.region;
        for (u32 i = 0; i < blocks.size(); i++) {
            region.blocks.push_back(&blocks[i]);
            region.index_of.emplace(blocks[i].Location().Value(), i);
        }
        region.entry = static_cast<u32>(entry_index);
        for (const IR::Block& block : blocks) {
            for (const auto& inst : block) {
                if (inst.GetOpcode() != IR::Opcode::PushRSB) {
                    continue;
                }
                const auto it = region.index_of.find(inst.GetArg(0).GetU64());
                if (it != region.index_of.end() &&
                    std::find(region.return_sites.begin(), region.return_sites.end(), it->second) == region.return_sites.end()) {
                    region.return_sites.push_back(it->second);
                }
            }
        }
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        for (const u32 site : region.return_sites) {
            ebi.region_blocks.return_sites[site] = 1;
        }
#endif
        const std::function<void(u32)> emit_block = [&](u32 i) {
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
            ebi.region_blocks.emitted[i] = 1;
#endif
            ctx.block = &blocks[i];
            region.current = i;
            // A block that links back to itself runs as a nested wasm loop
            // (entered before its condition check, which each iteration redoes).
            const bool self_loop = EmitBeginRegionSelfLoop(ctx);
            EmitCondPrelude(ctx);
            EmitBlockBody(ctx);
            EmitTerminal(ctx);
            if (self_loop) {
                EmitEndRegionSelfLoop(ctx);
            }
        };
        std::vector<u8>* entry_reachable = nullptr;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        entry_reachable = &ebi.region_blocks.entry_reachable;
#endif
        const bool all_entries = emit_conf.enable_region_multi_entry &&
                                 !IsSingleStepping(ctx, entry.Location());
        if (!EmitStructuredRegion(ctx, emit_block, entry_reachable)) {
            if (all_entries) {
                for (const auto& block : blocks) {
                    if (block.Location() != entry.Location()) {
                        ebi.fallback_aliases.push_back(block.Location().Value());
                    }
                }
            }
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
            ebi.region_layout = RegionDiagnosticLayout::Fallback;
            std::fill(ebi.region_blocks.emitted.begin(), ebi.region_blocks.emitted.end(), 1);
#endif
            if (!all_entries && emit_conf.enable_a64_fallback_multi_entry && is_a64 && !IsSingleStepping(ctx, entry.Location())) {
                for (const u32 site : region.return_sites) {
                    if (site != region.entry) {
                        ebi.fallback_aliases.push_back(blocks[site].Location().Value());
                    }
                }
            }
            EmitBeginRegion(ctx);
            for (u32 i = 0; i < blocks.size(); i++) {
                EmitRegionBlockStart(ctx, i);
                emit_block(i);
            }
            EmitEndRegion(ctx);
        }
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        else {
            ebi.region_layout = RegionDiagnosticLayout::Structured;
            ASSERT(ebi.region_blocks.emitted == ebi.region_blocks.entry_reachable);
        }
#endif
        loops = true;
    } else {
        EmitCondPrelude(ctx);
        loops = EmitBeginSelfLoop(ctx);
        EmitBlockBody(ctx);
        EmitTerminal(ctx);
        if (loops) {
            EmitEndSelfLoop(ctx);
        }
    }

    const u32 entry_fn = mod.AddFunction(block_ty);
    mod.AddExport("f", ExternalKind::Function, entry_fn);
    ebi.entry_body_bytes = mod.SetCode(entry_fn, code);
    if (emit_conf.function_names) {
        char name[48];
        if (is_a64) {
            std::snprintf(name, sizeof(name), "a64_%llx%s", static_cast<unsigned long long>(A64::LocationDescriptor{entry.Location()}.PC()), blocks.size() > 1 ? "_region" : "");
        } else {
            std::snprintf(name, sizeof(name), "a32_%x%s", A32::LocationDescriptor{entry.Location()}.PC(), blocks.size() > 1 ? "_region" : "");
        }
        mod.SetFunctionName(entry_fn, name);
    }
    if (loops) {
        mod.SetCompilationHint(0x2A);  // eager TurboFan: a loop entered once never tiers up
    }
    ebi.module_bytes = mod.Finalize();
    return ebi;
}

}  // namespace Dynarmic::Backend::Wasm
