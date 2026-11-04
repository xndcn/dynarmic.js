// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/a64_address_space.h"

#include <optional>
#include <stdexcept>
#include <tuple>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/a64_region_focus.h"
#include "dynarmic/backend/wasm/compile_stats.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

std::optional<RegionSelectionFocus> ConfiguredRegionFocus() {
#if defined(DYNARMIC_JS_A64_REGION_FOCUS)
    auto focus = ParseA64RegionFocus(DYNARMIC_JS_A64_REGION_FOCUS);
    if (!focus) throw std::invalid_argument{"invalid compile-time A64 region focus"};
    return focus;
#else
    return std::nullopt;
#endif
}

IR::Value ResolveIdentity(IR::Value v) {
    while (!v.IsImmediate() && v.GetInst()->GetOpcode() == IR::Opcode::Identity) {
        v = v.GetInst()->GetArg(0);
    }
    return v;
}

// A64 scalar FP / SIMD-scalar code moves values through 128-bit vector
// registers: ZeroExtendLongToQuad (after ZeroExtendWordToLong for 32-bit
// values) on the way in, VectorGetElement on the way out. Reading lane 0 back
// out of a value that was just widened is the original scalar; skipping the
// round trip keeps the value in a plain i32/i64 wasm local.
void ScalarLanePeephole(IR::Block& block) {
    for (auto& inst : block) {
        const IR::Opcode op = inst.GetOpcode();
        if (op != IR::Opcode::VectorGetElement32 && op != IR::Opcode::VectorGetElement64) {
            continue;
        }
        if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() != 0) {
            continue;
        }
        const IR::Value vec = ResolveIdentity(inst.GetArg(0));
        if (vec.IsImmediate() || vec.GetInst()->GetOpcode() != IR::Opcode::ZeroExtendLongToQuad) {
            continue;
        }
        const IR::Value wide = ResolveIdentity(vec.GetInst()->GetArg(0));
        if (op == IR::Opcode::VectorGetElement64) {
            inst.ReplaceUsesWith(wide);
        } else if (!wide.IsImmediate() && wide.GetInst()->GetOpcode() == IR::Opcode::ZeroExtendWordToLong) {
            inst.ReplaceUsesWith(wide.GetInst()->GetArg(0));
        }
    }
}

}  // anonymous namespace

A64AddressSpace::A64AddressSpace(const A64::UserConfig& conf, ProfileCounters* profiler, CompileStats* compile_stats,
                                 RegionDiagnostics* region_diagnostics, bool enable_fallback_multi_entry,
                                 const BoundaryState* boundary_state)
        : AddressSpace(profiler, compile_stats, region_diagnostics), conf(conf),
          enable_fallback_multi_entry(enable_fallback_multi_entry), boundary_state(boundary_state),
          region_focus(ConfiguredRegionFocus()) {
    // The page table holds 32-bit linear-memory addresses; its size must be
    // addressable from wasm32.
    ASSERT(conf.page_table == nullptr || (conf.page_table_address_space_bits > 12 && conf.page_table_address_space_bits <= 32));
    // Flat window: leading run of pages that the page table maps contiguously
    // starting at fastmem_pointer (the host must keep them mapped for the
    // lifetime of the Jit). Every vaddr outside it, mirrored or not, takes
    // the thunk path.
    if (conf.fastmem_pointer && conf.page_table && !conf.absolute_offset_page_table && conf.page_table_pointer_mask_bits == 0) {
        std::tie(flat_base, flat_limit) = FindFlatWindow(*conf.fastmem_pointer, conf.page_table,
                                                         u64{1} << (conf.page_table_address_space_bits - 12));
    }
}

A64AddressSpace::~A64AddressSpace() = default;

IR::Block A64AddressSpace::GenerateIR(IR::LocationDescriptor descriptor) const {
    const double t0 = CompileStatsNowMs();
    // After kMaxBlockInstructions, `b #4` ends the block with a LinkBlock to
    // the next instruction (not counted as a guest cycle).
    size_t count = 0;
    const auto get_code = [this, &count](u64 vaddr) -> std::optional<u32> {
        if (++count > kMaxBlockInstructions) {
            return 0x14000001;
        }
        return conf.callbacks->MemoryReadCode(vaddr);
    };
    IR::Block ir_block = A64::Translate(A64::LocationDescriptor{descriptor}, get_code,
                                        {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct, conf.hook_hint_instructions});
    if (count > kMaxBlockInstructions) {
        ir_block.CycleCount()--;
    }
    const double t1 = CompileStatsNowMs();
    RecordCompilePhase<CompilePhase::Translate>(CompileStatsPtr(), t1 - t0);

    Optimization::PolyfillPass(ir_block, {});
    Optimization::A64CallbackConfigPass(ir_block, conf);
    if (conf.HasOptimization(OptimizationFlag::GetSetElimination) && !conf.check_halt_on_memory_access) {
        Optimization::A64GetSetElimination(ir_block);
        Optimization::DeadCodeElimination(ir_block);
    }
    if (conf.HasOptimization(OptimizationFlag::ConstProp)) {
        Optimization::ConstantPropagation(ir_block);
        Optimization::DeadCodeElimination(ir_block);
    }
    if (conf.HasOptimization(OptimizationFlag::MiscIROpt)) {
        Optimization::A64MergeInterpretBlocksPass(ir_block, conf.callbacks);
        ScalarLanePeephole(ir_block);
        Optimization::DeadCodeElimination(ir_block);
    }
    Optimization::IdentityRemovalPass(ir_block);
    Optimization::VerificationPass(ir_block);
    RecordCompilePhase<CompilePhase::Optimize>(CompileStatsPtr(), CompileStatsNowMs() - t1);

    return ir_block;
}

A64AddressSpace::GuestRange A64AddressSpace::RangeOf(const IR::Block& block) const {
    const u64 start_pc = A64::LocationDescriptor{block.Location()}.PC();
    const u64 end_pc = A64::LocationDescriptor{block.EndLocation()}.PC();
    return {start_pc, end_pc - 1};
}

bool A64AddressSpace::RegionCompatible(IR::LocationDescriptor entry, IR::LocationDescriptor target) const {
    const A64::LocationDescriptor e{entry}, t{target};
    return !e.SingleStepping() && !t.SingleStepping() && e.FPCR() == t.FPCR();
}

std::optional<RegionSelectionFocus> A64AddressSpace::FocusForEntry(IR::LocationDescriptor entry) const {
    if (region_focus && entry == region_focus->entry) return region_focus;
    return std::nullopt;
}

EmitConfig A64AddressSpace::MakeEmitConfig() const {
    return {
        .enable_cycle_counting = conf.enable_cycle_counting,
        .always_little_endian = true,
        .page_table_base = conf.page_table ? static_cast<u32>(reinterpret_cast<uptr>(conf.page_table)) : 0,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .page_table_pointer_mask_bits = conf.page_table_pointer_mask_bits,
        .flat_base = flat_base,
        .flat_limit = flat_limit,
        .boundary_state = boundary_state,
        .boundary_hook_snapshot = std::nullopt,
        .frontend = Frontend::A64,
        .enable_a64_fallback_multi_entry = enable_fallback_multi_entry,
        .enable_region_multi_entry = conf.HasOptimization(OptimizationFlag::WasmRegionMultiEntry) &&
                                     boundary_state != nullptr && conf.enable_cycle_counting,
        .page_table_address_space_bits = static_cast<u32>(conf.page_table_address_space_bits),
        .silently_mirror_page_table = conf.silently_mirror_page_table,
        .a64_conf = &conf,
    };
}

}  // namespace Dynarmic::Backend::Wasm
