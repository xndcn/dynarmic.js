// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include <mcl/stdint.hpp>

#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Wasm {

struct RegionGuestRange {
    u64 start = 0;
    u64 end = 0;
};

struct RegionSelectionLimits {
    size_t max_blocks = 64;
    size_t max_instructions = 2000;
    u64 max_guest_distance = 4096;
};

struct RegionSelectionStats {
    u64 candidates = 0;
    u64 rejected_incompatible = 0;
    u64 rejected_distance = 0;
    u64 rejected_block_budget = 0;
    u64 rejected_ir_budget = 0;
};

struct RegionSelectionFocus {
    IR::LocationDescriptor entry;
    IR::LocationDescriptor exit_block;
    IR::LocationDescriptor direct_target;
    std::vector<IR::LocationDescriptor> return_blocks;
};

struct RegionSelectionResult {
    std::vector<IR::Block> blocks;
    size_t entry_index = 0;
    RegionSelectionStats stats{};
};

// Selects a bounded, local-flow region. This class owns no cache state and
// does not emit code; all frontend and IR policy enters through callbacks.
class RegionSelector final {
public:
    using GenerateBlock = std::function<IR::Block(IR::LocationDescriptor)>;
    using RangeOf = std::function<RegionGuestRange(const IR::Block&)>;
    using GuestPC = std::function<u64(IR::LocationDescriptor)>;
    using Compatible = std::function<bool(IR::LocationDescriptor, IR::LocationDescriptor)>;

    static RegionSelectionResult Select(IR::LocationDescriptor entry,
                                        GenerateBlock generate_block,
                                        RangeOf range_of,
                                        GuestPC guest_pc,
                                        Compatible compatible,
                                        bool chain_link_blocks,
                                        RegionSelectionLimits limits = {},
                                        std::optional<RegionSelectionFocus> focus = std::nullopt);
};

}  // namespace Dynarmic::Backend::Wasm
