// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
#include "dynarmic/backend/wasm/region_plan.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Dynarmic::Backend::Wasm {
using u32 = std::uint32_t;

std::optional<RegionPlan> PlanRegion(const RegionGraph& succ, u32 entry) {
    if (entry >= succ.size()) {
        return std::nullopt;
    }
    for (const auto& edges : succ) {
        for (u32 target : edges) {
            if (target >= succ.size()) {
                return std::nullopt;
            }
        }
    }
    const u32 n = static_cast<u32>(succ.size());
    RegionPlan plan;
    // Reverse postorder from the entry (iterative DFS).
    constexpr u32 kNone = ~u32{0};
    std::vector<u32> order(n, kNone);  // RPO number
    std::vector<u32> postorder;
    {
        std::vector<bool> visited(n, false);
        std::vector<std::pair<u32, size_t>> stack{{entry, 0}};
        visited[entry] = true;
        while (!stack.empty()) {
            auto& [v, next] = stack.back();
            if (next < succ[v].size()) {
                const u32 w = succ[v][next++];
                if (!visited[w]) {
                    visited[w] = true;
                    stack.emplace_back(w, 0);
                }
                continue;
            }
            postorder.push_back(v);
            stack.pop_back();
        }
    }
    std::vector<u32> rpo(postorder.rbegin(), postorder.rend());
    for (u32 k = 0; k < rpo.size(); k++) {
        order[rpo[k]] = k;
    }
    std::vector<std::vector<u32>> pred(n);
    for (const u32 v : rpo) {
        for (const u32 w : succ[v]) {
            pred[w].push_back(v);
        }
    }

    // Dominators (Cooper, Harvey, Kennedy).
    std::vector<u32> idom(n, kNone);
    idom[entry] = entry;
    auto intersect = [&](u32 a, u32 b) {
        while (a != b) {
            while (order[a] > order[b]) {
                a = idom[a];
            }
            while (order[b] > order[a]) {
                b = idom[b];
            }
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const u32 v : rpo) {
            if (v == entry) {
                continue;
            }
            u32 new_idom = kNone;
            for (const u32 p : pred[v]) {
                if (idom[p] != kNone) {
                    new_idom = new_idom == kNone ? p : intersect(p, new_idom);
                }
            }
            if (idom[v] != new_idom) {
                idom[v] = new_idom;
                changed = true;
            }
        }
    }
    auto dominates = [&](u32 a, u32 b) {
        for (;;) {
            if (a == b) {
                return true;
            }
            if (b == entry) {
                return false;
            }
            b = idom[b];
        }
    };

    // Reducible iff every retreating edge targets a dominator of its source.
    plan.header.assign(n, false);
    for (const u32 v : rpo) {
        for (const u32 w : succ[v]) {
            if (order[w] <= order[v]) {
                if (!dominates(w, v)) {
                    return std::nullopt;
                }
                plan.header[w] = true;
            }
        }
    }

    plan.children.assign(n, {});
    for (const u32 v : rpo) {
        if (v != entry) {
            plan.children[idom[v]].push_back(v);
        }
    }
    for (auto& c : plan.children) {
        std::sort(c.begin(), c.end(), [&](u32 a, u32 b) { return order[a] > order[b]; });
    }
    return plan;
}

}  // namespace Dynarmic::Backend::Wasm
