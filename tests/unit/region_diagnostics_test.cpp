// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include "dynarmic/backend/wasm/region_diagnostics.h"
#include "dynarmic/backend/wasm/wasm_binary.h"

using namespace Dynarmic::Backend::Wasm;

namespace {

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestAppendOnlyHistory() {
    RegionDiagnostics history;
    Require(history.Regions().empty() && history.SelectedDescriptors().empty(), "new history must be empty");
    constexpr u64 entry = 0xf123456789abcdefULL;
    constexpr u64 shared = 0x8000000000001000ULL;
    const u64 first = history.Record(Frontend::A64, entry, 0xfffffffffffffff0ULL, 1,
                                     1000, 800, RegionDiagnosticLayout::Fallback,
                                     {{shared, entry}, {1, 1}, {1, 1}, {0, 1}});
    const u64 second = history.Record(Frontend::A32, shared, 0x1000, 0,
                                      500, 400, RegionDiagnosticLayout::Single,
                                      {{shared}, {1}, {1}, {0}}, {first});
    Require(first == 1 && second == 2, "region IDs must be contiguous and start at one");
    const auto& regions = history.Regions();
    Require(regions.size() == 2, "two compilations must be retained");
    Require(regions[0].frontend == static_cast<u64>(Frontend::A64) &&
            regions[1].frontend == static_cast<u64>(Frontend::A32), "frontend identity must be preserved");
    Require(regions[0].entry_descriptor == entry && regions[0].entry_pc == 0xfffffffffffffff0ULL,
            "full 64-bit entry identity must be preserved");
    Require(regions[0].entry_index == 1 && regions[0].module_bytes == 1000 &&
            regions[0].entry_body_bytes == 800, "entry and byte metadata must be preserved");
    Require(regions[0].block_offset == 0 && regions[0].block_count == 2 &&
            regions[1].block_offset == 2 && regions[1].block_count == 1,
            "flattened block offsets must address the selected descriptors");
    Require(history.SelectedDescriptors() == std::vector<u64>({shared, entry, shared}),
            "selection order and duplicates must be retained");
    Require(history.EmittedBlocks() == std::vector<u8>({1, 1, 1}) &&
            history.EntryReachableBlocks() == std::vector<u8>({1, 1, 1}) &&
            history.ReturnSites() == std::vector<u8>({0, 1, 0}),
            "per-selected-block facts must use the same flattened offsets");
    Require(regions[0].layout == static_cast<u64>(RegionDiagnosticLayout::Fallback) &&
            regions[1].layout == static_cast<u64>(RegionDiagnosticLayout::Single),
            "actual region layouts must be retained");
    Require(regions[0].live_offset == 0 && regions[0].live_count == 0 &&
            regions[1].live_offset == 0 && regions[1].live_count == 1 &&
            history.LiveRegionIds() == std::vector<u64>({first}),
            "pre-compile live generations must be retained by exact ID");

    RegionDiagnostics other;
    other.Record(Frontend::A64, entry, 0x1000, 0, 100, 80, RegionDiagnosticLayout::Single,
                 {{entry}, {1}, {1}, {0}});
    Require(other.Regions().size() == 1 && other.Regions()[0].region_id == 1,
            "recorder owners must have independent histories and IDs");
}

void TestTransferHistory() {
    RegionDiagnostics history;
    auto transfers = std::make_unique<RegionTransferRecord>();
    RegionTransferRecord* const stable = transfers.get();
    transfers->entry_count = 7;
    RegionTransferExit& direct = transfers->AddExit(
        0x1000, RegionTransferKind::ExternalDirectCall, 0x8000000000002000ULL);
    direct.count = 0x20000000000001ULL;
    direct.last_dynamic_target_descriptor = 0x8000000000002000ULL;
    direct.last_dynamic_target_pc = 0x2000;
    RegionTransferExit& indirect = transfers->AddExit(0x1004, RegionTransferKind::OtherTailTransfer);
    indirect.count = 3;
    indirect.last_dynamic_target_descriptor = 0xf123456789abcdefULL;
    indirect.last_dynamic_target_pc = 0xfffffffffffffff0ULL;

    const u64 first = history.Record(Frontend::A64, 0x1000, 0x1000, 0, 100, 80,
                                     RegionDiagnosticLayout::Fallback,
                                     {{0x1000, 0x1004}, {1, 1}, {1, 1}, {0, 0}}, {}, std::move(transfers));
    Require(first == 1 && history.TransferRegions().size() == 1 &&
            history.TransferRegions()[0].get() == stable && stable->region_id == first,
            "transfer history must preserve stable records and assign their generation ID");
    Require(history.TransferRegions()[0]->exits.size() == 2 &&
            history.TransferRegions()[0]->exits[0].count == 0x20000000000001ULL &&
            history.TransferRegions()[0]->exits[1].static_target_descriptor == 0,
            "transfer history must retain exact counts and nullable static targets");

    auto next = std::make_unique<RegionTransferRecord>();
    next->AddExit(0x1000, RegionTransferKind::RsbReturn);
    const u64 second = history.Record(Frontend::A64, 0x1000, 0x1000, 0, 100, 80,
                                      RegionDiagnosticLayout::Single,
                                      {{0x1000}, {1}, {1}, {0}}, {}, std::move(next));
    Require(second == 2 && history.TransferRegions().size() == 2 &&
            history.TransferRegions()[0]->entry_count == 7 &&
            history.TransferRegions()[0]->exits[0].count == 0x20000000000001ULL &&
            history.TransferRegions()[1]->entry_count == 0 &&
            history.TransferRegions()[1]->exits[0].count == 0,
            "recompilation must append fresh zeroed counters without rewriting history");
}

void TestPublicationOwnership() {
    RegionDiagnostics first;
    RegionDiagnostics second;
    Require(first.NeedsPublication(1) && second.NeedsPublication(2), "new histories need initial publication");
    first.MarkPublished(1);
    Require(!first.NeedsPublication(1) && second.NeedsPublication(2) && first.NeedsPublication(2),
            "publication state must belong to its recorder and instance");
    second.MarkPublished(2);
    first.Record(Frontend::A64, 0x1000, 0x1000, 0, 100, 80, RegionDiagnosticLayout::Single,
                 {{0x1000}, {1}, {1}, {0}});
    Require(first.NeedsPublication(1) && !second.NeedsPublication(2),
            "appending one owner must not invalidate another owner's publication");
    first.MarkPublished(1);
    Require(!first.NeedsPublication(1) && !second.NeedsPublication(2),
            "alternating unchanged owners must remain published");
}

void TestSerializedBodySize() {
    ModuleBuilder measured;
    const u32 type = measured.AddType({{}, {ValType::I32}});
    const u32 fn = measured.AddFunction(type);
    FuncBody body;
    body.I32Const(0);
    const std::vector<u8> serialized = body.Serialize();
    const size_t body_bytes = measured.SetCode(fn, body);
    Require(body_bytes == serialized.size(), "SetCode must return the exact stored function-body size");
    measured.AddExport("f", ExternalKind::Function, fn);
    const std::vector<u8> module = measured.Finalize();
    Require(body_bytes > 0 && body_bytes < module.size(), "entry body must fit within its module");

    ModuleBuilder ordinary;
    ordinary.AddType({{}, {ValType::I32}});
    const u32 ordinary_fn = ordinary.AddFunction(0);
    ordinary.SetCode(ordinary_fn, body);
    ordinary.AddExport("f", ExternalKind::Function, ordinary_fn);
    Require(ordinary.Finalize() == module, "body-size observation must not change emitted module bytes");
}

}  // namespace

int main() {
    TestAppendOnlyHistory();
    TestTransferHistory();
    TestPublicationOwnership();
    TestSerializedBodySize();
    std::cout << "PASS region diagnostics history and body sizes\n";
}
