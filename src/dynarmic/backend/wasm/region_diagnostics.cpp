// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/region_diagnostics.h"

#include <algorithm>
#include <iterator>

#include <mcl/assert.hpp>

namespace Dynarmic::Backend::Wasm {

u64 RegionDiagnostics::Record(Frontend frontend, u64 entry_descriptor, u64 entry_pc, size_t entry_index,
                              size_t module_bytes, size_t entry_body_bytes, RegionDiagnosticLayout layout,
                              RegionDiagnosticBlocks blocks, std::vector<u64> live_region_ids,
                              std::unique_ptr<RegionTransferRecord> transfers) {
    const size_t count = blocks.descriptors.size();
    ASSERT(entry_index < count);
    ASSERT(blocks.descriptors[entry_index] == entry_descriptor);
    ASSERT(entry_body_bytes > 0 && entry_body_bytes <= module_bytes);
    ASSERT(blocks.emitted.size() == count && blocks.entry_reachable.size() == count &&
           blocks.return_sites.size() == count);
    ASSERT(std::all_of(blocks.emitted.begin(), blocks.emitted.end(), [](u8 value) { return value <= 1; }));
    ASSERT(std::all_of(blocks.entry_reachable.begin(), blocks.entry_reachable.end(), [](u8 value) { return value <= 1; }));
    ASSERT(std::all_of(blocks.return_sites.begin(), blocks.return_sites.end(), [](u8 value) { return value <= 1; }));
    ASSERT(blocks.emitted[entry_index] != 0 && blocks.entry_reachable[entry_index] != 0);
    ASSERT((count == 1) == (layout == RegionDiagnosticLayout::Single));
    if (layout == RegionDiagnosticLayout::Structured) {
        ASSERT(blocks.emitted == blocks.entry_reachable);
    } else if (layout == RegionDiagnosticLayout::Fallback) {
        ASSERT(std::all_of(blocks.emitted.begin(), blocks.emitted.end(), [](u8 value) { return value != 0; }));
    } else {
        ASSERT(layout == RegionDiagnosticLayout::Single);
    }
    ASSERT(std::is_sorted(live_region_ids.begin(), live_region_ids.end()) &&
           std::adjacent_find(live_region_ids.begin(), live_region_ids.end()) == live_region_ids.end());
    ASSERT(std::all_of(live_region_ids.begin(), live_region_ids.end(),
                       [this](u64 id) { return id > 0 && id < next_region_id; }));

    // Reserve every flattened array before changing any of them. Subsequent
    // insertion of trivially copyable values cannot throw, so one failed
    // allocation leaves the complete history unchanged.
    regions.reserve(regions.size() + 1);
    if (transfers) {
        transfer_regions.reserve(transfer_regions.size() + 1);
    }
    selected_descriptors.reserve(selected_descriptors.size() + count);
    emitted_blocks.reserve(emitted_blocks.size() + count);
    entry_reachable_blocks.reserve(entry_reachable_blocks.size() + count);
    return_sites.reserve(return_sites.size() + count);
    this->live_region_ids.reserve(this->live_region_ids.size() + live_region_ids.size());

    const size_t block_offset = selected_descriptors.size();
    const size_t live_offset = this->live_region_ids.size();
    selected_descriptors.insert(selected_descriptors.end(),
                                std::make_move_iterator(blocks.descriptors.begin()),
                                std::make_move_iterator(blocks.descriptors.end()));
    emitted_blocks.insert(emitted_blocks.end(), blocks.emitted.begin(), blocks.emitted.end());
    entry_reachable_blocks.insert(entry_reachable_blocks.end(), blocks.entry_reachable.begin(), blocks.entry_reachable.end());
    return_sites.insert(return_sites.end(), blocks.return_sites.begin(), blocks.return_sites.end());
    this->live_region_ids.insert(this->live_region_ids.end(), live_region_ids.begin(), live_region_ids.end());
    const RegionDiagnosticRecord record{
        .region_id = next_region_id,
        .frontend = static_cast<u64>(frontend),
        .entry_descriptor = entry_descriptor,
        .entry_pc = entry_pc,
        .entry_index = entry_index,
        .module_bytes = module_bytes,
        .entry_body_bytes = entry_body_bytes,
        .block_offset = block_offset,
        .block_count = count,
        .layout = static_cast<u64>(layout),
        .live_offset = live_offset,
        .live_count = live_region_ids.size(),
    };
    regions.push_back(record);
    if (transfers) {
        transfers->region_id = record.region_id;
        transfer_regions.push_back(std::move(transfers));
    }
    ++next_region_id;
    return record.region_id;
}

}  // namespace Dynarmic::Backend::Wasm

#if defined(__EMSCRIPTEN__) && defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
#include <emscripten.h>

namespace Dynarmic::Backend::Wasm {

EM_JS(void, js_publish_region_diagnostics, (const u8* records_ptr, const u8* descriptors_ptr,
                                             const u8* emitted_ptr, const u8* reachable_ptr,
                                             const u8* return_sites_ptr, const u8* live_ptr,
                                             u64 region_count, u64 instance_id), {
    const stats = Module.dynarmicWasmStats || (Module.dynarmicWasmStats = {});
    const instances = stats.instances || (stats.instances = {});
    if (!stats.getInstance) stats.getInstance = id => stats.instances?.[String(id)] || null;
    const snapshot = instances[String(instance_id)] || (instances[String(instance_id)] = {});
    const word = offset => {
        const index = offset >>> 2;
        return HEAPU32[index] + HEAPU32[index + 1] * 4294967296;
    };
    const hex = offset => {
        const index = offset >>> 2;
        return '0x' + HEAPU32[index + 1].toString(16).padStart(8, '0') +
            HEAPU32[index].toString(16).padStart(8, '0');
    };
    const number = offset => {
        const value = word(offset);
        if (!Number.isSafeInteger(value)) throw new Error('region diagnostics: unsafe integer');
        return value;
    };
    const flag = (pointer, index) => {
        const value = HEAPU8[pointer + index];
        if (value > 1) throw new Error('region diagnostics: invalid block flag');
        return value === 1;
    };
    const layouts = ['single', 'structured', 'fallback'];
    const regions = [];
    for (let i = 0; i < region_count; ++i) {
        const record = records_ptr + i * 96;
        const offset = number(record + 56);
        const count = number(record + 64);
        const blocks = [];
        for (let j = 0; j < count; ++j) {
            blocks.push(Object.freeze({
                descriptor: hex(descriptors_ptr + (offset + j) * 8),
                emitted: flag(emitted_ptr, offset + j),
                entryReachable: flag(reachable_ptr, offset + j),
                returnSite: flag(return_sites_ptr, offset + j),
            }));
        }
        const liveOffset = number(record + 80);
        const liveCount = number(record + 88);
        const liveRegionIdsBeforeCompile = [];
        for (let j = 0; j < liveCount; ++j) {
            liveRegionIdsBeforeCompile.push(number(live_ptr + (liveOffset + j) * 8));
        }
        const frontend = number(record + 8);
        if (frontend > 1) throw new Error('region diagnostics: invalid frontend');
        const layout = number(record + 72);
        if (layout >= layouts.length) throw new Error('region diagnostics: invalid layout');
        regions.push(Object.freeze({
            regionId: number(record),
            frontend: frontend === 0 ? 'a32' : 'a64',
            entryDescriptor: hex(record + 16),
            entryPc: hex(record + 24),
            entryIndex: number(record + 32),
            moduleBytes: number(record + 40),
            entryBodyBytes: number(record + 48),
            layout: layouts[layout],
            blocks: Object.freeze(blocks),
            liveRegionIdsBeforeCompile: Object.freeze(liveRegionIdsBeforeCompile),
        }));
    }
    snapshot.regionDiagnostics = Object.freeze({
        schema: 'dynarmic.js/region-diagnostics-v2',
        regions: Object.freeze(regions),
    });
});

struct TransferPublicationRegion {
    u64 region_id;
    u64 frontend;
    u64 entry_descriptor;
    u64 entry_pc;
    u64 entry_count;
    u64 exit_offset;
    u64 exit_count;
};

static_assert(sizeof(TransferPublicationRegion) == 56);
static_assert(sizeof(RegionTransferExit) == 56);

EM_JS(void, js_publish_region_transfer_diagnostics, (const u8* records_ptr, const u8* exits_ptr,
                                                      u64 region_count, u64 instance_id), {
    const stats = Module.dynarmicWasmStats || (Module.dynarmicWasmStats = {});
    const instances = stats.instances || (stats.instances = {});
    if (!stats.getInstance) stats.getInstance = id => stats.instances?.[String(id)] || null;
    const snapshot = instances[String(instance_id)] || (instances[String(instance_id)] = {});
    const word = offset => {
        const index = offset >>> 2;
        return HEAPU32[index] + HEAPU32[index + 1] * 4294967296;
    };
    const hex = offset => {
        const index = offset >>> 2;
        return '0x' + HEAPU32[index + 1].toString(16).padStart(8, '0') +
            HEAPU32[index].toString(16).padStart(8, '0');
    };
    const number = offset => {
        const value = word(offset);
        if (!Number.isSafeInteger(value)) throw new Error('transfer diagnostics: unsafe integer');
        return value;
    };
    const kinds = ['external-direct-call', 'rsb-return', 'other-tail-transfer'];
    const regions = [];
    for (let i = 0; i < region_count; ++i) {
        const record = records_ptr + i * 56;
        const offset = number(record + 40);
        const count = number(record + 48);
        const exits = [];
        for (let j = 0; j < count; ++j) {
            const exit = exits_ptr + (offset + j) * 56;
            const kind = number(exit + 32);
            if (kind >= kinds.length) throw new Error('transfer diagnostics: invalid kind');
            const hasStaticTarget = number(exit + 48);
            if (hasStaticTarget > 1) throw new Error('transfer diagnostics: invalid static-target flag');
            exits.push(Object.freeze({
                exitSite: j,
                exitBlockDescriptor: hex(exit + 24),
                kind: kinds[kind],
                staticTargetDescriptor: hasStaticTarget ? hex(exit + 40) : null,
                count: hex(exit),
                lastDynamicTargetDescriptor: hex(exit + 8),
                lastDynamicTargetPc: hex(exit + 16),
            }));
        }
        const frontend = number(record + 8);
        if (frontend > 1) throw new Error('transfer diagnostics: invalid frontend');
        regions.push(Object.freeze({
            regionId: number(record),
            frontend: frontend === 0 ? 'a32' : 'a64',
            callerEntryDescriptor: hex(record + 16),
            callerEntryPc: hex(record + 24),
            entryCount: hex(record + 32),
            exits: Object.freeze(exits),
        }));
    }
    snapshot.regionTransferDiagnostics = Object.freeze({
        schema: 'dynarmic.js/region-transfer-diagnostics-v1',
        regions: Object.freeze(regions),
    });
});

void PublishRegionTransferDiagnostics(const RegionDiagnostics& diagnostics, u64 instance_id) {
    const auto& regions = diagnostics.Regions();
    const auto& transfers = diagnostics.TransferRegions();
    ASSERT(regions.size() == transfers.size());
    std::vector<TransferPublicationRegion> publication_regions;
    std::vector<RegionTransferExit> publication_exits;
    publication_regions.reserve(regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& source = regions[i];
        const auto& counters = *transfers[i];
        ASSERT(source.region_id == counters.region_id);
        publication_exits.insert(publication_exits.end(), counters.exits.begin(), counters.exits.end());
        publication_regions.push_back({source.region_id, source.frontend, source.entry_descriptor,
                                       source.entry_pc, counters.entry_count,
                                       publication_exits.size() - counters.exits.size(), counters.exits.size()});
    }
    js_publish_region_transfer_diagnostics(
        reinterpret_cast<const u8*>(publication_regions.data()),
        reinterpret_cast<const u8*>(publication_exits.data()), publication_regions.size(), instance_id);
}

void PublishRegionDiagnostics(const RegionDiagnostics& diagnostics, u64 instance_id) {
    // Publication happens at Run/Step boundaries. Each recorder owns its cache,
    // so alternating unchanged JITs never rebuilds either complete history.
    if (!diagnostics.NeedsPublication(instance_id)) {
        return;
    }
    js_publish_region_diagnostics(reinterpret_cast<const u8*>(diagnostics.Regions().data()),
                                  reinterpret_cast<const u8*>(diagnostics.SelectedDescriptors().data()),
                                  diagnostics.EmittedBlocks().data(), diagnostics.EntryReachableBlocks().data(),
                                  diagnostics.ReturnSites().data(),
                                  reinterpret_cast<const u8*>(diagnostics.LiveRegionIds().data()),
                                  diagnostics.Regions().size(), instance_id);
    diagnostics.MarkPublished(instance_id);
}

}  // namespace Dynarmic::Backend::Wasm
#endif
