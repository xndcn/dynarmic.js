// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/address_space.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <unordered_set>
#include <utility>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/compile_stats.h"
#include "dynarmic/backend/wasm/region_diagnostics.h"
#include "dynarmic/ir/microinstruction.h"

namespace Dynarmic::Backend::Wasm {

namespace {

bool BoundaryRegionsEnabled(const EmitConfig& emit_conf) {
    return emit_conf.boundary_state != nullptr &&
           (emit_conf.boundary_state->flags & kBoundaryDisableRegions) == 0;
}

}  // anonymous namespace

AddressSpace::~AddressSpace() = default;

std::optional<RegionSelectionFocus> AddressSpace::FocusForEntry(IR::LocationDescriptor) const {
    return std::nullopt;
}

AddressSpace::BlockEntry::~BlockEntry() {
    ReleaseBlock(fn);
    std::free(link_slots);
}

std::pair<u32, u32> AddressSpace::FindFlatWindow(uptr fastmem_base, void* const* page_table, u64 entries) {
    u64 pages = 0;
    while (pages < entries && reinterpret_cast<uptr>(page_table[pages]) == fastmem_base + (static_cast<uptr>(pages) << 12)) {
        pages++;
    }
    if (pages == 0 || fastmem_base + (pages << 12) > u64{0xFFFFFFFF} + 1) {
        return {0, 0};
    }
    // Keep 16 bytes of slack so an access starting below the limit never
    // runs past the window (the widest access is 128 bits).
    return {static_cast<u32>(fastmem_base), static_cast<u32>((pages << 12) - 16)};
}

BlockFunc AddressSpace::Get(IR::LocationDescriptor descriptor) const {
    const DispatchEntry& fast = dispatch_cache[FastDispatchSlot(descriptor.Value())];
    if (fast.entry != nullptr && fast.descriptor == descriptor.Value()) {
        fast.entry->last_used = ++use_serial;
        return fast.entry->fn;
    }
    if (const auto iter = block_entries.find(descriptor.Value()); iter != block_entries.end()) {
        iter->second->last_used = ++use_serial;
        dispatch_cache[FastDispatchSlot(descriptor.Value())] = {descriptor.Value(), iter->second.get()};
        return iter->second->fn;
    }
    return nullptr;
}

BlockFunc AddressSpace::GetOrEmitForLink(IR::LocationDescriptor descriptor) {
    struct RestoreTrim {
        bool& flag;
        bool previous;
        ~RestoreTrim() { flag = previous; }
    } restore{suspend_trim, suspend_trim};
    suspend_trim = true;
    return GetOrEmit(descriptor);
}

BlockFunc AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (BlockFunc block_entry = Get(descriptor)) {
        DYNARMIC_WASM_PROFILE_INC(profiler, dispatch_cache_hits);
        return block_entry;
    }

    DYNARMIC_WASM_PROFILE_INC(profiler, dispatch_cache_misses);

    if (!suspend_trim) {
        TrimCache();
    }
    std::vector<u64> live_region_ids;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    if (region_diagnostics != nullptr) {
        live_region_ids.reserve(block_entries.size());
        for (const auto& [cached_descriptor, cached_entry] : block_entries) {
            ASSERT(cached_entry->region_id != 0);
            live_region_ids.push_back(cached_entry->region_id);
        }
        std::sort(live_region_ids.begin(), live_region_ids.end());
        live_region_ids.erase(std::unique(live_region_ids.begin(), live_region_ids.end()), live_region_ids.end());
    }
#endif
    auto [blocks, entry_index] = GenerateRegion(descriptor);
    GuestRange range = RangeOf(blocks[0]);
    for (const IR::Block& block : blocks) {
        const GuestRange r = RangeOf(block);
        range.start = std::min(range.start, r.start);
        range.end = std::max(range.end, r.end);
    }
    std::vector<u64> fallback_aliases;
    auto entry = Emit(std::move(blocks), entry_index, std::move(live_region_ids), fallback_aliases);
    entry->range = range;
    entry->last_used = ++use_serial;
    return Publish(std::move(entry), descriptor, fallback_aliases);
}

BlockFunc AddressSpace::Publish(std::shared_ptr<BlockEntry> entry, IR::LocationDescriptor descriptor,
                                const std::vector<u64>& fallback_aliases) {
    std::vector<u64> inserted;
    inserted.reserve(fallback_aliases.size() + 1);
    const auto [original, inserted_original] = block_entries.emplace(descriptor.Value(), entry);
    if (!inserted_original) {
        dispatch_cache[FastDispatchSlot(descriptor.Value())] = {descriptor.Value(), original->second.get()};
        return original->second->fn;
    }
    inserted.push_back(descriptor.Value());
    cached_module_bytes += entry->module_bytes;
    ++cached_block_count;
    try {
        for (const u64 alias : fallback_aliases) {
            if (block_entries.emplace(alias, entry).second) {
                inserted.push_back(alias);
            }
        }
    } catch (...) {
        for (const u64 inserted_descriptor : inserted) {
            block_entries.erase(inserted_descriptor);
        }
        cached_module_bytes -= entry->module_bytes;
        --cached_block_count;
        throw;
    }
#if defined(DYNARMIC_WASM_PROFILE)
    if (profiler != nullptr) {
        profiler->peak_cached_blocks = std::max<u64>(profiler->peak_cached_blocks, cached_block_count);
        profiler->peak_cached_bytes = std::max<u64>(profiler->peak_cached_bytes, cached_module_bytes);
    }
#endif
    // Publish only after every alias insertion succeeds: a fast entry must
    // never outlive the owning map entry if allocation throws and rolls back.
    for (const u64 inserted_descriptor : inserted) {
        dispatch_cache[FastDispatchSlot(inserted_descriptor)] = {inserted_descriptor, entry.get()};
    }
    return entry->fn;
}

// Boundary dispatch releases cold modules only between native calls, when no
// generated block is on the stack. Browser native-code mapping limits can be
// reached before shared linear memory is exhausted.
void AddressSpace::TrimCache() {
    // A compile failure also requests trimming. Defer it while the dispatcher
    // still holds a link slot owned by the source allocation.
    if (suspend_trim) return;
    const EmitConfig emit_conf = MakeEmitConfig();
    if (!emit_conf.enable_cycle_counting || emit_conf.boundary_state == nullptr ||
        cached_block_count < cache_capacity) {
        return;
    }
    const size_t batch = std::min(cached_block_count, cached_block_count - cache_capacity + 1024);
    if (batch == 0) {
        return;
    }
    // Capacity and recency belong to compiled allocations, not to each of
    // their lookup aliases. Evicting aliases independently causes repeated
    // compilation while keeping most of the underlying code allocations.
    std::vector<std::pair<u64, const BlockEntry*>> cold;
    cold.reserve(cached_block_count);
    std::unordered_set<const BlockEntry*> allocations;
    allocations.reserve(cached_block_count);
    for (const auto& [key, entry] : block_entries) {
        if (allocations.insert(entry.get()).second) {
            cold.emplace_back(entry->last_used, entry.get());
        }
    }
    std::nth_element(cold.begin(), cold.begin() + batch, cold.end());
    std::unordered_set<const BlockEntry*> dropped_allocations;
    dropped_allocations.reserve(batch);
    size_t dropped_module_bytes = 0;
    for (size_t i = 0; i < batch; ++i) {
        dropped_allocations.insert(cold[i].second);
        dropped_module_bytes += cold[i].second->module_bytes;
    }
    // Finish allocating metadata before mutating any live lookup or link.
    std::unordered_set<u64> dropped_descriptors;
    for (const auto& [key, entry] : block_entries) {
        if (dropped_allocations.contains(entry.get())) {
            dropped_descriptors.insert(key);
        }
    }
    for (const BlockEntry* entry : dropped_allocations) {
        for (u32 i = 0; i < entry->num_link_slots; ++i) {
            patched_slots.erase(entry->link_slots + i);
        }
    }
    for (auto it = patched_slots.begin(); it != patched_slots.end();) {
        if (dropped_descriptors.count(it->second) != 0) {
            *it->first = 0;
            it = patched_slots.erase(it);
        } else {
            ++it;
        }
    }
    for (const u64 descriptor : dropped_descriptors) {
        // The last alias releases the function and its chaining slots.
        block_entries.erase(descriptor);
    }
    cached_module_bytes -= dropped_module_bytes;
    cached_block_count -= batch;
    // Neither lookup cache may retain an evicted BlockEntry or function index.
    std::fill(fast_dispatch.begin(), fast_dispatch.end(), FastDispatchEntry{});
    std::fill(dispatch_cache.begin(), dispatch_cache.end(), DispatchEntry{});
}

void AddressSpace::ClearCache() {
    DYNARMIC_WASM_PROFILE_INC(profiler, invalidation_count);
    DYNARMIC_WASM_PROFILE_ADD(profiler, invalidated_blocks, cached_block_count);
    block_entries.clear();
    cached_module_bytes = 0;
    cached_block_count = 0;
    patched_slots.clear();
    std::fill(fast_dispatch.begin(), fast_dispatch.end(), FastDispatchEntry{});
    std::fill(dispatch_cache.begin(), dispatch_cache.end(), DispatchEntry{});
}

void AddressSpace::RecordLink(u32* slot, IR::LocationDescriptor target) {
    DYNARMIC_WASM_PROFILE_INC(profiler, link_patches);
    patched_slots[slot] = target.Value();
}

void AddressSpace::RecordDispatchTarget(IR::LocationDescriptor location, BlockFunc fn) {
    FastDispatchEntry& entry = fast_dispatch[FastDispatchSlot(location.Value())];
    entry.descriptor = location.Value();
    entry.index = static_cast<u32>(reinterpret_cast<uptr>(fn));
}

void AddressSpace::InvalidateRanges(const boost::icl::interval_set<u64>& ranges) {
    std::unordered_set<u64> dropped;
    std::unordered_set<BlockEntry*> dropped_allocations;
    for (const auto& [descriptor, entry] : block_entries) {
        const auto block_range = boost::icl::discrete_interval<u64>::closed(entry->range.start, entry->range.end);
        if (boost::icl::intersects(ranges, block_range)) {
            dropped.insert(descriptor);
            dropped_allocations.insert(entry.get());
        }
    }
    if (dropped.empty()) {
        return;
    }
    DYNARMIC_WASM_PROFILE_INC(profiler, invalidation_count);
    DYNARMIC_WASM_PROFILE_ADD(profiler, invalidated_blocks, dropped_allocations.size());
    // Entries hold table indices of possibly dropped blocks.
    std::fill(fast_dispatch.begin(), fast_dispatch.end(), FastDispatchEntry{});
    std::fill(dispatch_cache.begin(), dispatch_cache.end(), DispatchEntry{});
    // Forget the dropped blocks' own slots (their memory is about to be freed)
    // before zeroing the slots of survivors that chain into them.
    for (const BlockEntry* entry : dropped_allocations) {
        for (u32 i = 0; i < entry->num_link_slots; i++) {
            patched_slots.erase(entry->link_slots + i);
        }
    }
    for (auto it = patched_slots.begin(); it != patched_slots.end();) {
        if (dropped.count(it->second) != 0) {
            *it->first = 0;
            it = patched_slots.erase(it);
        } else {
            ++it;
        }
    }
    for (const BlockEntry* entry : dropped_allocations) {
        cached_module_bytes -= entry->module_bytes;
    }
    cached_block_count -= dropped_allocations.size();
    for (const u64 descriptor : dropped) {
        block_entries.erase(descriptor);
    }
}

std::pair<std::vector<IR::Block>, size_t> AddressSpace::GenerateRegion(IR::LocationDescriptor descriptor) const {
    const EmitConfig emit_conf = MakeEmitConfig();
    const bool boundary_regions = BoundaryRegionsEnabled(emit_conf);
    auto selected = RegionSelector::Select(
        descriptor,
        [this](IR::LocationDescriptor location) { return GenerateIR(location); },
        [this](const IR::Block& block) { return RegionGuestRange{RangeOf(block).start, RangeOf(block).end}; },
        [this](IR::LocationDescriptor location) { return GuestPC(location); },
        [this, reuse_entries = emit_conf.enable_region_multi_entry](IR::LocationDescriptor entry, IR::LocationDescriptor target) {
            // Shared entries already have an owner. Reach them through the
            // guarded cross-module edge instead of compiling overlapping IR.
            return (!reuse_entries || !block_entries.contains(target.Value())) && RegionCompatible(entry, target);
        },
        ChainLinkBlock() || boundary_regions, RegionSelectionLimits{}, FocusForEntry(descriptor));
    DYNARMIC_WASM_PROFILE_ADD(profiler, region_candidates, selected.stats.candidates);
    DYNARMIC_WASM_PROFILE_ADD(profiler, region_rejected_incompatible,
                              selected.stats.rejected_incompatible);
    DYNARMIC_WASM_PROFILE_ADD(profiler, region_rejected_distance, selected.stats.rejected_distance);
    DYNARMIC_WASM_PROFILE_ADD(profiler, region_rejected_block_budget,
                              selected.stats.rejected_block_budget);
    DYNARMIC_WASM_PROFILE_ADD(profiler, region_rejected_ir_budget, selected.stats.rejected_ir_budget);
    return {std::move(selected.blocks), selected.entry_index};
}

std::shared_ptr<AddressSpace::BlockEntry> AddressSpace::Emit(std::vector<IR::Block> blocks, size_t entry_index,
                                                             std::vector<u64> live_region_ids,
                                                             std::vector<u64>& fallback_aliases) {
#if !defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    (void)live_region_ids;
#endif
    DYNARMIC_WASM_PROFILE_INC(profiler, compiled_blocks);
    if (blocks.size() > 1) {
        DYNARMIC_WASM_PROFILE_INC(profiler, compiled_regions);
        DYNARMIC_WASM_PROFILE_ADD(profiler, region_blocks, blocks.size());
    }
    // One chaining slot per chainable link edge, over the blocks in emission
    // order (see EmitConfig::link_slots_base). Boundary-aware A32 may chain
    // LinkBlock because every cross-module edge repeats the boundary guard.
    EmitConfig emit_conf = MakeEmitConfig();
    const bool chain_link_blocks = ChainLinkBlock() || BoundaryRegionsEnabled(emit_conf);
    u32 num_link_slots = 0;
    for (const IR::Block& block : blocks) {
        // A failed entry condition is a LinkBlock edge too (emitted first).
        if (block.HasConditionFailedLocation() && chain_link_blocks) {
            num_link_slots++;
        }
        num_link_slots += CountLinkBlockFastTerminals(block.GetTerminal(), chain_link_blocks);
    }
    std::unique_ptr<u32, decltype(&std::free)> link_slots{nullptr, &std::free};
    emit_conf.function_names = DebugFunctionNames();
    // Boundary-aware indirect edges repeat the dynamic-target guard before
    // using this table. RSB speculation remains on the native boundary.
    emit_conf.fast_dispatch_base = static_cast<u32>(reinterpret_cast<uptr>(fast_dispatch.data()));
    if (num_link_slots != 0) {
        link_slots.reset(static_cast<u32*>(std::calloc(num_link_slots, sizeof(u32))));
        if (!link_slots) {
            throw std::bad_alloc{};
        }
        emit_conf.link_slots_base = static_cast<u32>(reinterpret_cast<uptr>(link_slots.get()));
    }

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    std::vector<u64> selected_descriptors;
    std::unique_ptr<RegionTransferRecord> transfers;
    u64 entry_descriptor = 0;
    u64 entry_pc = 0;
    if (region_diagnostics != nullptr) {
        transfers = std::make_unique<RegionTransferRecord>();
        emit_conf.transfer_record = transfers.get();
        selected_descriptors.reserve(blocks.size());
        for (const IR::Block& block : blocks) {
            selected_descriptors.push_back(block.Location().Value());
        }
        entry_descriptor = blocks[entry_index].Location().Value();
        entry_pc = GuestPC(blocks[entry_index].Location());
    }
#endif
    const double t_emit = CompileStatsNowMs();
    EmittedBlockInfo block_info = EmitWASM(std::move(blocks), emit_conf, entry_index);
    DYNARMIC_WASM_PROFILE_ADD(profiler, emitted_bytes, block_info.module_bytes.size());
    RecordCompilePhase<CompilePhase::Emit>(compile_stats, CompileStatsNowMs() - t_emit);
    const BlockFunc entry = InstantiateBlock(block_info.module_bytes.data(), block_info.module_bytes.size(), compile_stats);
    ASSERT(entry != nullptr);
    std::shared_ptr<BlockEntry> result;
    try {
        result = std::make_shared<BlockEntry>();
    } catch (...) {
        ReleaseBlock(entry);
        throw;
    }
    result->fn = entry;
    result->link_slots = link_slots.release();
    result->num_link_slots = num_link_slots;
    result->module_bytes = block_info.module_bytes.size();
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    if (region_diagnostics != nullptr) {
        block_info.region_blocks.descriptors = std::move(selected_descriptors);
        result->region_id = region_diagnostics->Record(
            emit_conf.frontend, entry_descriptor, entry_pc, entry_index,
            block_info.module_bytes.size(), block_info.entry_body_bytes, block_info.region_layout,
            std::move(block_info.region_blocks), std::move(live_region_ids), std::move(transfers));
    }
#endif
    fallback_aliases = std::move(block_info.fallback_aliases);
    return result;
}

}  // namespace Dynarmic::Backend::Wasm
