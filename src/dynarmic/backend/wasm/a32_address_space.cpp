// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/a32_address_space.h"

#include <optional>
#include <tuple>

#include <boost/variant.hpp>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/compile_stats.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// Forwards to the user's callbacks, ending the block (LinkBlock to the next
// instruction) after kMaxBlockInstructions.
class BoundedTranslateCallbacks final : public A32::TranslateCallbacks {
public:
    explicit BoundedTranslateCallbacks(A32::UserCallbacks* inner)
            : inner(inner) {}

    std::optional<u32> MemoryReadCode(u32 vaddr) override { return inner->MemoryReadCode(vaddr); }
    bool PreCodeReadHook(bool is_thumb, u32 pc, A32::IREmitter& ir) override {
        if (++count > kMaxBlockInstructions) {
            ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
            return false;
        }
        return inner->PreCodeReadHook(is_thumb, pc, ir);
    }
    void PreCodeTranslationHook(bool is_thumb, u32 pc, A32::IREmitter& ir) override { inner->PreCodeTranslationHook(is_thumb, pc, ir); }
    u64 GetTicksForCode(bool is_thumb, u32 vaddr, u32 instruction) override { return inner->GetTicksForCode(is_thumb, vaddr, instruction); }

private:
    A32::UserCallbacks* inner;
    size_t count = 0;
};

std::optional<BoundaryHookSnapshot> SnapshotBoundaryHooks(const BoundaryState* boundary) {
    if (boundary == nullptr) {
        return std::nullopt;
    }
    return BoundaryHookSnapshot{
        .instruction_hooks = boundary->instruction_hooks,
        .hook_slots = reinterpret_cast<const u8*>(static_cast<uptr>(boundary->hook_slots)),
    };
}

}  // anonymous namespace

A32AddressSpace::A32AddressSpace(const A32::UserConfig& conf, ProfileCounters* profiler, CompileStats* compile_stats,
                                 RegionDiagnostics* region_diagnostics, const BoundaryState* boundary_state,
                                 bool specialize_boundary_hooks)
        : AddressSpace(profiler, compile_stats, region_diagnostics)
        , conf(conf)
        , boundary_state(boundary_state)
        , specialize_boundary_hooks(specialize_boundary_hooks) {
    // Flat window: leading run of pages that the page table maps contiguously
    // starting at fastmem_pointer. The host opts in by setting fastmem_pointer
    // and must keep those pages mapped for the lifetime of the Jit.
    if (conf.fastmem_pointer && conf.page_table && !conf.absolute_offset_page_table && conf.page_table_pointer_mask_bits == 0) {
        std::tie(flat_base, flat_limit) = FindFlatWindow(*conf.fastmem_pointer, reinterpret_cast<void* const*>(conf.page_table->data()),
                                                         A32::UserConfig::NUM_PAGE_TABLE_ENTRIES);
    }
}

A32AddressSpace::~A32AddressSpace() = default;

// ARM conditional branches (`cmp; bne loop`) translate to a separate, empty
// block with a non-AL entry condition. When `block` falls into such a block,
// fold it into the terminal -- If(cond, taken, LinkBlock(fall-through)) -- so
// the pair is one block with both exits chained directly (and a loop whose
// back-edge is a conditional branch becomes a self-loop). Skipped with cycle
// counting on, where the two paths' cycle counts differ.
void A32AddressSpace::FoldConditionalBranch(IR::Block& block) const {
    if (conf.enable_cycle_counting || !block.HasTerminal() || A32::LocationDescriptor{block.Location()}.SingleStepping()) {
        return;
    }
    const IR::Terminal term = block.GetTerminal();
    std::optional<IR::LocationDescriptor> next_location;
    if (const auto* link = boost::get<IR::Term::LinkBlock>(&term)) {
        next_location = link->next;
    } else if (const auto* link_fast = boost::get<IR::Term::LinkBlockFast>(&term)) {
        next_location = link_fast->next;
    }
    if (!next_location || *next_location == block.Location()) {
        return;
    }

    BoundedTranslateCallbacks tcb{conf.callbacks};
    IR::Block next = A32::Translate(A32::LocationDescriptor{*next_location}, &tcb, {conf.arch_version, conf.define_unpredictable_behaviour, conf.hook_hint_instructions});
    if (!next.empty() || next.GetCondition() == IR::Cond::AL || !next.HasConditionFailedLocation() || !next.HasTerminal()) {
        return;
    }
    const IR::Terminal taken = next.GetTerminal();
    if (!boost::get<IR::Term::LinkBlock>(&taken) && !boost::get<IR::Term::LinkBlockFast>(&taken)) {
        return;
    }
    block.ReplaceTerminal(IR::Term::If{next.GetCondition(), taken, IR::Term::LinkBlock{next.ConditionFailedLocation()}});
}

IR::Block A32AddressSpace::GenerateIR(IR::LocationDescriptor descriptor) const {
    const double t0 = CompileStatsNowMs();
    BoundedTranslateCallbacks tcb{conf.callbacks};
    IR::Block ir_block = A32::Translate(A32::LocationDescriptor{descriptor}, &tcb, {conf.arch_version, conf.define_unpredictable_behaviour, conf.hook_hint_instructions});
    FoldConditionalBranch(ir_block);
    const double t1 = CompileStatsNowMs();
    RecordCompilePhase<CompilePhase::Translate>(CompileStatsPtr(), t1 - t0);

    Optimization::PolyfillPass(ir_block, {});
    if (conf.HasOptimization(OptimizationFlag::GetSetElimination)) {
        Optimization::A32GetSetElimination(ir_block, {.convert_nzc_to_nz = true});
        Optimization::DeadCodeElimination(ir_block);
    }
    if (conf.HasOptimization(OptimizationFlag::ConstProp)) {
        Optimization::A32ConstantMemoryReads(ir_block, conf.callbacks);
        Optimization::ConstantPropagation(ir_block);
        Optimization::DeadCodeElimination(ir_block);
    }
    Optimization::IdentityRemovalPass(ir_block);
    Optimization::VerificationPass(ir_block);
    RecordCompilePhase<CompilePhase::Optimize>(CompileStatsPtr(), CompileStatsNowMs() - t1);

    return ir_block;
}

A32AddressSpace::GuestRange A32AddressSpace::RangeOf(const IR::Block& block) const {
    // The fall-through of a folded conditional branch reads one more
    // instruction, so pad by 4 bytes.
    const u32 start_pc = A32::LocationDescriptor{block.Location()}.PC();
    const u32 end_pc = A32::LocationDescriptor{block.EndLocation()}.PC() + 4;
    return {start_pc, end_pc};
}

bool A32AddressSpace::RegionCompatible(IR::LocationDescriptor entry, IR::LocationDescriptor target) const {
    const A32::LocationDescriptor e{entry}, t{target};
    return !e.SingleStepping() && !t.SingleStepping() && e.FPSCR() == t.FPSCR();
}

EmitConfig A32AddressSpace::MakeEmitConfig() const {
    return {
        .enable_cycle_counting = conf.enable_cycle_counting,
        .always_little_endian = conf.always_little_endian,
        .page_table_base = conf.page_table ? static_cast<u32>(reinterpret_cast<uptr>(conf.page_table->data())) : 0,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .page_table_pointer_mask_bits = conf.page_table_pointer_mask_bits,
        .flat_base = flat_base,
        .flat_limit = flat_limit,
        .boundary_state = boundary_state,
        .boundary_hook_snapshot = specialize_boundary_hooks ? SnapshotBoundaryHooks(boundary_state) : std::nullopt,
        .coprocessors = &conf.coprocessors,
        .frontend = Frontend::A32,
        .enable_region_multi_entry = conf.HasOptimization(OptimizationFlag::WasmRegionMultiEntry) &&
                                         boundary_state != nullptr && conf.enable_cycle_counting,
        .enable_shared_exit = conf.HasOptimization(OptimizationFlag::WasmSharedExit),
        .enable_constant_memory = conf.HasOptimization(OptimizationFlag::WasmConstantMemory),
    };
}

}  // namespace Dynarmic::Backend::Wasm
