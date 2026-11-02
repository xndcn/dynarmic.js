// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <boost/icl/interval_set.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/compile_stats.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/profiler.h"
#include "dynarmic/backend/wasm/region_selector.h"
#include "dynarmic/backend/wasm/wasm_runtime.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Wasm {

class RegionDiagnostics;

// Guest instructions per translated block. Upstream IR passes recurse along
// Identity chains (Value::IsImmediate), which grow with block length: an
// unbounded block (e.g. a page of zeros, `movs r0, r0` in Thumb) overflows
// the Emscripten stack. Longer straight-line code continues in a block linked
// to the next instruction (and usually compiled into the same region).
constexpr size_t kMaxBlockInstructions = 256;

// Frontend-independent block cache: compiles IR blocks to wasm modules on
// demand, owns their chaining slots, and drops / unlinks blocks on cache
// invalidation. Subclasses supply translation (GenerateIR) and the
// frontend-specific part of the emit configuration.
class AddressSpace {
    friend struct AddressSpaceTestAccess;

public:
    explicit AddressSpace(ProfileCounters* profiler = nullptr, CompileStats* compile_stats = nullptr,
                          RegionDiagnostics* region_diagnostics = nullptr)
            : profiler(profiler), compile_stats(compile_stats), region_diagnostics(region_diagnostics) {}
    virtual ~AddressSpace();

    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    BlockFunc Get(IR::LocationDescriptor descriptor) const;
    BlockFunc GetOrEmit(IR::LocationDescriptor descriptor);
    // Resolves an unlinked generated-code edge without evicting the source
    // allocation that owns its outstanding link_slot_addr.
    BlockFunc GetOrEmitForLink(IR::LocationDescriptor descriptor);

    void ClearCache();

    // Drops every block whose guest code overlaps one of the closed intervals,
    // and unpatches every chaining slot that still points at a dropped block.
    void InvalidateRanges(const boost::icl::interval_set<u64>& ranges);

    // Dispatcher callback: `slot` (a chaining slot in some block's link array
    // or in the RSB) now holds the table index of the block for `target`.
    void RecordLink(u32* slot, IR::LocationDescriptor target);

    // Dispatcher callback: `fn` is the block for `location`; remember it in
    // the fast dispatch table (where RSB pushes also look up return targets).
    void RecordDispatchTarget(IR::LocationDescriptor location, BlockFunc fn);

protected:
    using GuestRange = RegionGuestRange;

    virtual IR::Block GenerateIR(IR::LocationDescriptor descriptor) const = 0;
    // Guest code covered by a translated block (for range invalidation).
    virtual GuestRange RangeOf(const IR::Block& block) const = 0;
    // The guest PC of a location descriptor.
    virtual u64 GuestPC(IR::LocationDescriptor location) const = 0;
    // Emit configuration for this address space; link_slots_base is filled
    // in per block.
    virtual EmitConfig MakeEmitConfig() const = 0;
    // LinkBlock terminals are chained without cycle counting, or with the
    // opt-in A32 boundary guard on every generated cross-region edge.
    virtual bool ChainLinkBlock() const = 0;
    // Whether `target` may be compiled into the region entered at `entry`:
    // neither single-stepping, and the same FP mode bits (the region is
    // emitted under the entry's FPCR / FPSCR).
    virtual bool RegionCompatible(IR::LocationDescriptor entry, IR::LocationDescriptor target) const = 0;
    // Optional focused selection for an exact entry descriptor. The default
    // preserves ordinary selection; focused failures never retry ordinary.
    virtual std::optional<RegionSelectionFocus> FocusForEntry(IR::LocationDescriptor entry) const;

    // Leading run of pages that a page table maps contiguously from
    // `fastmem_base` (see EmitConfig::flat_base/flat_limit). `entries` is the
    // number of page-table entries to scan; returns {base, limit} or {0, 0}.
    static std::pair<u32, u32> FindFlatWindow(uptr fastmem_base, void* const* page_table, u64 entries);

    CompileStats* CompileStatsPtr() const { return compile_stats; }

private:
    struct BlockEntry {
        BlockFunc fn = nullptr;
        u32* link_slots = nullptr;  // malloc'd array of chaining slots, if any
        u32 num_link_slots = 0;
        size_t module_bytes = 0;
        GuestRange range{};
        mutable u64 last_used = 0;
        u64 region_id = 0;  // diagnostic generation; zero when not recorded

        BlockEntry() = default;
        ~BlockEntry();
        BlockEntry(const BlockEntry&) = delete;
        BlockEntry& operator=(const BlockEntry&) = delete;
    };

    // Translates the region entered at `descriptor`: the entry block plus the
    // blocks reachable through direct branches, bounded in size, ordered by
    // guest address. Returns the blocks and the entry's index.
    std::pair<std::vector<IR::Block>, size_t> GenerateRegion(IR::LocationDescriptor descriptor) const;
    std::shared_ptr<BlockEntry> Emit(std::vector<IR::Block> blocks, size_t entry_index,
                                     std::vector<u64> live_region_ids, std::vector<u64>& fallback_aliases);
    BlockFunc Publish(std::shared_ptr<BlockEntry> entry, IR::LocationDescriptor descriptor,
                      const std::vector<u64>& fallback_aliases);

    void TrimCache();
    bool suspend_trim = false;
    mutable u64 use_serial = 0;
    size_t cache_capacity = 65536;

    struct DispatchEntry {
        u64 descriptor = 0;
        BlockEntry* entry = nullptr;
    };

    // Descriptors are lookup keys; the pointed-to allocation owns the module.
    std::unordered_map<u64, std::shared_ptr<BlockEntry>> block_entries;
    [[maybe_unused]] ProfileCounters* profiler = nullptr;
    CompileStats* compile_stats = nullptr;
    [[maybe_unused]] RegionDiagnostics* region_diagnostics = nullptr;
    // Updated only at compile/invalidation boundaries; they are not on the
    // dispatch hot path. Both counts describe allocations, not descriptors.
    size_t cached_module_bytes = 0;
    size_t cached_block_count = 0;
    // Native dispatcher lookup cache. Unlike the generated-code table below,
    // entries retain their owner so embedders can update cache eviction metadata.
    mutable std::vector<DispatchEntry> dispatch_cache = std::vector<DispatchEntry>(kFastDispatchEntries);
    // Fast dispatch table (see EmitConfig::fast_dispatch_base).
    std::vector<FastDispatchEntry> fast_dispatch = std::vector<FastDispatchEntry>(kFastDispatchEntries);
    // Patched chaining slots -> descriptor of the block they target.
    std::unordered_map<u32*, u64> patched_slots;
};

}  // namespace Dynarmic::Backend::Wasm
