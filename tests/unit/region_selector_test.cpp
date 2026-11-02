// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <cstdlib>
#include <iostream>
#include <unordered_map>

#include "dynarmic/backend/wasm/region_selector.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;

static_assert(RegionSelectionLimits{}.max_blocks == 64);

namespace {

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct SyntheticGraph {
    std::unordered_map<u64, std::vector<u64>> successors;
    std::unordered_map<u64, bool> compatible;

    IR::Block Block(u64 location) const {
        IR::Block block{IR::LocationDescriptor{location}};
        const auto it = successors.find(location);
        if (it == successors.end() || it->second.empty()) {
            block.SetTerminal(IR::Term::ReturnToDispatch{});
        } else {
            block.SetTerminal(IR::Term::LinkBlock{IR::LocationDescriptor{it->second.front()}});
        }
        return block;
    }
};

}  // namespace

int main() {
    SyntheticGraph graph{
        .successors = {{100, {300, 150}}, {300, {}}, {150, {200}}, {200, {}}, {500, {}}},
        .compatible = {{150, true}, {200, false}, {300, true}, {500, true}},
    };
    auto selected = RegionSelector::Select(
        IR::LocationDescriptor{100},
        [&](IR::LocationDescriptor location) { return graph.Block(location.Value()); },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [&](IR::LocationDescriptor, IR::LocationDescriptor target) {
            const auto it = graph.compatible.find(target.Value());
            return it == graph.compatible.end() || it->second;
        },
        true,
        RegionSelectionLimits{.max_blocks = 8, .max_instructions = 100, .max_guest_distance = 200});
    Require(selected.blocks.size() == 2, "compatibility must filter a successor");
    Require(selected.blocks[0].Location().Value() == 100 && selected.blocks[1].Location().Value() == 300,
            "selection must be ordered by guest address");
    Require(selected.entry_index == 0, "entry index must follow sorted region");

    SyntheticGraph filtered{{{100, {150}}}, {{150, false}}};
    selected = RegionSelector::Select(
        IR::LocationDescriptor{100},
        [&](IR::LocationDescriptor location) { return filtered.Block(location.Value()); },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [&](IR::LocationDescriptor, IR::LocationDescriptor target) {
            const auto it = filtered.compatible.find(target.Value());
            return it == filtered.compatible.end() || it->second;
        },
        true);
    Require(selected.blocks.size() == 1 && selected.stats.candidates == 1 &&
                selected.stats.rejected_incompatible == 1,
            "compatibility rejection statistics must be recorded");

    filtered.compatible[150] = true;
    selected = RegionSelector::Select(
        IR::LocationDescriptor{100},
        [&](IR::LocationDescriptor location) { return filtered.Block(location.Value()); },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [](IR::LocationDescriptor, IR::LocationDescriptor) { return true; },
        true, RegionSelectionLimits{.max_blocks = 8, .max_instructions = 100, .max_guest_distance = 10});
    Require(selected.stats.candidates == 1 && selected.stats.rejected_distance == 1,
            "distance rejection statistics must be recorded");

    selected = RegionSelector::Select(
        IR::LocationDescriptor{100},
        [&](IR::LocationDescriptor location) { return filtered.Block(location.Value()); },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [](IR::LocationDescriptor, IR::LocationDescriptor) { return true; },
        true, RegionSelectionLimits{.max_blocks = 1, .max_instructions = 100, .max_guest_distance = 4096});
    Require(selected.blocks.size() == 1 && selected.stats.rejected_block_budget == 1,
            "block budget rejection statistics must be recorded");

    selected = RegionSelector::Select(
        IR::LocationDescriptor{100},
        [&](IR::LocationDescriptor location) {
            auto block = filtered.Block(location.Value());
            block.AppendNewInst(IR::Opcode::Breakpoint, {});
            return block;
        },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [](IR::LocationDescriptor, IR::LocationDescriptor) { return true; },
        true, RegionSelectionLimits{.max_blocks = 8, .max_instructions = 1, .max_guest_distance = 4096});
    Require(selected.blocks.size() == 1 && selected.stats.rejected_ir_budget == 1,
            "IR budget rejection statistics must be recorded");

    selected = RegionSelector::Select(
        IR::LocationDescriptor{100},
        [&](IR::LocationDescriptor location) { return graph.Block(location.Value()); },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [](IR::LocationDescriptor, IR::LocationDescriptor) { return true; },
        true,
        RegionSelectionLimits{.max_blocks = 2, .max_instructions = 100, .max_guest_distance = 50});
    Require(selected.blocks.size() == 1, "distance budget must reject far successors");
    Require(selected.entry_index == 0, "single-block region entry must be zero");

    selected = RegionSelector::Select(
        IR::LocationDescriptor{100},
        [&](IR::LocationDescriptor location) { return graph.Block(location.Value()); },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [](IR::LocationDescriptor, IR::LocationDescriptor) { return true; },
        true,
        RegionSelectionLimits{.max_blocks = 1, .max_instructions = 0, .max_guest_distance = 4096});
    Require(selected.blocks.size() == 1, "entry block must survive zero extension budgets");
    Require(selected.stats.candidates == 1 && selected.stats.rejected_block_budget == 1,
            "zero extension budget must record the blocked candidate");
    SyntheticGraph full_identity;
    constexpr u64 d100 = 0x0300000000000100ULL;
    constexpr u64 d200 = 0x0100000000000200ULL;
    constexpr u64 d300 = 0x0200000000000300ULL;
    full_identity.successors = {{d200, {d300}}, {d300, {d100}}, {d100, {}}};
    const auto identity_pc = [](u64 descriptor) {
        if (descriptor == d100) return u64{0x100};
        if (descriptor == d200) return u64{0x200};
        return u64{0x300};
    };
    selected = RegionSelector::Select(
        IR::LocationDescriptor{d200},
        [&](IR::LocationDescriptor location) { return full_identity.Block(location.Value()); },
        [&](const IR::Block& block) {
            const u64 pc = identity_pc(block.Location().Value());
            return RegionGuestRange{pc, pc + 3};
        },
        [&](IR::LocationDescriptor location) { return identity_pc(location.Value()); },
        [&](IR::LocationDescriptor, IR::LocationDescriptor) { return true; }, true);
    Require(selected.blocks.size() == 3 && selected.blocks[0].Location().Value() == d100 &&
                selected.blocks[1].Location().Value() == d200 && selected.blocks[2].Location().Value() == d300,
            "guest ranges, not raw descriptors, must determine emission order");
    Require(selected.entry_index == 1 && selected.blocks[selected.entry_index].Location().Value() == d200,
            "entry identity must survive guest-range sorting");

    SyntheticGraph capped;
    for (u64 pc = 0x1000; pc <= 0x1100; pc += 4) {
        capped.successors[pc] = pc == 0x1100 ? std::vector<u64>{} : std::vector<u64>{pc + 4};
    }
    selected = RegionSelector::Select(
        IR::LocationDescriptor{0x1000},
        [&](IR::LocationDescriptor location) { return capped.Block(location.Value()); },
        [](const IR::Block& block) {
            const u64 pc = block.Location().Value();
            return RegionGuestRange{pc, pc + 3};
        },
        [](IR::LocationDescriptor location) { return location.Value(); },
        [](IR::LocationDescriptor, IR::LocationDescriptor) { return true; }, true);
    Require(selected.blocks.size() == 64, "the production default must stop at 64 blocks");
    Require(selected.blocks.back().Location().Value() == 0x10fc,
            "the 64-block region must retain the expected last guest block");
    Require(selected.stats.candidates == 64 && selected.stats.rejected_block_budget == 1 &&
                selected.stats.rejected_ir_budget == 0,
            "the 64-block boundary must reject exactly the next candidate");
    std::cout << "PASS region selector budgets and ordering\n";
}
