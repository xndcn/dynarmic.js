// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/frontend.h"

namespace Dynarmic::Backend::Wasm {

enum class RegionDiagnosticLayout : u64 {
    Single = 0,
    Structured = 1,
    Fallback = 2,
};

// Parallel arrays describe selected blocks in their original selection order.
// They are observations made during emission, not dynamic execution counters.
struct RegionDiagnosticBlocks {
    std::vector<u64> descriptors;
    std::vector<u8> emitted;
    std::vector<u8> entry_reachable;
    std::vector<u8> return_sites;
};

// Fixed-width counter words live in the Emscripten main module's linear
// memory. A deque gives every emitted site a stable address while compilation
// discovers more exits; unique ownership keeps the record alive after code
// invalidation. Dynamic target fields are the last observation, not a histogram.
enum class RegionTransferKind : u64 {
    ExternalDirectCall = 0,
    RsbReturn = 1,
    OtherTailTransfer = 2,
};

struct RegionTransferExit {
    u64 count = 0;
    u64 last_dynamic_target_descriptor = 0;
    u64 last_dynamic_target_pc = 0;
    u64 exit_block_descriptor = 0;
    u64 kind = 0;
    u64 static_target_descriptor = 0;
    u64 has_static_target = 0;
};

struct RegionTransferRecord {
    RegionTransferExit& AddExit(u64 block_descriptor, RegionTransferKind exit_kind,
                                u64 static_target = 0) {
        exits.push_back({.exit_block_descriptor = block_descriptor,
                         .kind = static_cast<u64>(exit_kind),
                         .static_target_descriptor = static_target,
                         .has_static_target = static_cast<u64>(exit_kind == RegionTransferKind::ExternalDirectCall)});
        return exits.back();
    }

    u64 region_id = 0;
    u64 entry_count = 0;
    std::deque<RegionTransferExit> exits;
};

// Fixed-width words also form the Emscripten publication ABI. Guest
// descriptors and PCs are serialized as strings by the JS boundary.
struct RegionDiagnosticRecord {
    u64 region_id;
    u64 frontend;
    u64 entry_descriptor;
    u64 entry_pc;
    u64 entry_index;
    u64 module_bytes;
    u64 entry_body_bytes;
    u64 block_offset;
    u64 block_count;
    u64 layout;
    u64 live_offset;
    u64 live_count;
};

// Append-only compilation history owned by one JIT. This is deliberately not
// live-cache state: invalidation never rewrites records and recompilation gets
// a fresh region ID. Detailed storage is only instantiated by diagnostic builds.
class RegionDiagnostics final {
public:
    u64 Record(Frontend frontend, u64 entry_descriptor, u64 entry_pc, size_t entry_index,
               size_t module_bytes, size_t entry_body_bytes, RegionDiagnosticLayout layout,
               RegionDiagnosticBlocks blocks, std::vector<u64> live_region_ids = {},
               std::unique_ptr<RegionTransferRecord> transfers = nullptr);

    const std::vector<RegionDiagnosticRecord>& Regions() const { return regions; }
    const std::vector<std::unique_ptr<RegionTransferRecord>>& TransferRegions() const { return transfer_regions; }
    const std::vector<u64>& SelectedDescriptors() const { return selected_descriptors; }
    const std::vector<u8>& EmittedBlocks() const { return emitted_blocks; }
    const std::vector<u8>& EntryReachableBlocks() const { return entry_reachable_blocks; }
    const std::vector<u8>& ReturnSites() const { return return_sites; }
    const std::vector<u64>& LiveRegionIds() const { return live_region_ids; }
    bool NeedsPublication(u64 instance_id) const {
        return !published || published_instance_id != instance_id ||
               published_region_count != regions.size() ||
               published_descriptor_count != selected_descriptors.size() ||
               published_live_count != live_region_ids.size();
    }
    void MarkPublished(u64 instance_id) const {
        published_instance_id = instance_id;
        published_region_count = regions.size();
        published_descriptor_count = selected_descriptors.size();
        published_live_count = live_region_ids.size();
        published = true;
    }

private:
    std::vector<RegionDiagnosticRecord> regions;
    std::vector<std::unique_ptr<RegionTransferRecord>> transfer_regions;
    std::vector<u64> selected_descriptors;
    std::vector<u8> emitted_blocks;
    std::vector<u8> entry_reachable_blocks;
    std::vector<u8> return_sites;
    std::vector<u64> live_region_ids;
    u64 next_region_id = 1;
    mutable u64 published_instance_id = 0;
    mutable size_t published_region_count = 0;
    mutable size_t published_descriptor_count = 0;
    mutable size_t published_live_count = 0;
    mutable bool published = false;
};

#if defined(__EMSCRIPTEN__) && defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
// Publishes one immutable, per-instance snapshot of the append-only history.
void PublishRegionDiagnostics(const RegionDiagnostics& diagnostics, u64 instance_id);
void PublishRegionTransferDiagnostics(const RegionDiagnostics& diagnostics, u64 instance_id);
#endif

}  // namespace Dynarmic::Backend::Wasm
