// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace Dynarmic::Backend::Wasm {

// Pure control-flow input: dense node indices, directed edges, explicit entry.
// Self edges are supported; emission may omit them when handled by local loops.
using RegionGraph = std::vector<std::vector<std::uint32_t>>;
struct RegionPlan {
    std::vector<std::vector<std::uint32_t>> children; // dominator tree, decreasing RPO
    std::vector<bool> header;                       // target of a back edge
};

// Only entry-reachable nodes appear in the tree. Returns no plan for invalid
// indices or irreducible flow, allowing the emitter's dispatch-loop fallback.
std::optional<RegionPlan> PlanRegion(const RegionGraph& successors, std::uint32_t entry);

}  // namespace Dynarmic::Backend::Wasm
