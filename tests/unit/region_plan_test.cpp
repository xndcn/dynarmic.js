// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
#include "dynarmic/backend/wasm/region_plan.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <vector>

using namespace Dynarmic::Backend::Wasm;
using Node = std::uint32_t;

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1); // Keep checks enabled in Release builds.
    }
}

// Independent oracle: a dominates b iff removing a makes b unreachable.
std::vector<bool> Reachable(const RegionGraph& graph, Node entry, Node removed) {
    std::vector<bool> seen(graph.size());
    std::vector<Node> work{entry};
    while (!work.empty()) {
        const Node v = work.back();
        work.pop_back();
        if (v == removed || seen[v]) continue;
        seen[v] = true;
        for (Node w : graph[v]) work.push_back(w);
    }
    return seen;
}

void Check(const RegionGraph& graph, Node entry) {
    const Node n = graph.size();
    const auto reachable = Reachable(graph, entry, n);
    std::vector<std::vector<bool>> dominates(n);
    for (Node v = 0; v < n; ++v) {
        dominates[v] = Reachable(graph, entry, v);
        for (Node w = 0; w < n; ++w) dominates[v][w] = reachable[w] && !dominates[v][w];
    }
    // Removing edges to dominators must leave a DAG for reducible flow.
    std::vector<unsigned> indegree(n);
    unsigned reachable_count = 0;
    for (Node v = 0; v < n; ++v) {
        if (!reachable[v]) continue;
        ++reachable_count;
        for (Node w : graph[v]) if (!dominates[w][v]) ++indegree[w];
    }
    std::vector<Node> work;
    for (Node v = 0; v < n; ++v) if (reachable[v] && !indegree[v]) work.push_back(v);
    unsigned removed = 0;
    while (!work.empty()) {
        const Node v = work.back(); work.pop_back(); ++removed;
        for (Node w : graph[v]) if (!dominates[w][v] && --indegree[w] == 0) work.push_back(w);
    }
    const auto plan = PlanRegion(graph, entry);
    Require(plan.has_value() == (removed == reachable_count), "reducibility oracle mismatch");
    if (!plan) return;

    // Simulate actual nested block/loop scopes: every edge must have an open
    // label or loop at its source, each reachable node must emit exactly once.
    std::vector<bool> labels(n), loops(n), emitted(n);
    std::function<void(Node)> tree;
    std::function<void(Node, size_t)> within = [&](Node v, size_t i) {
        if (i == plan->children[v].size()) {
            Require(!emitted[v], "duplicate tree node");
            emitted[v] = true;
            for (Node w : graph[v]) Require(labels[w] || loops[w], "edge has no open Wasm target");
            return;
        }
        const Node w = plan->children[v][i];
        Require(w < n && dominates[v][w], "tree parent does not dominate child");
        labels[w] = true;
        within(v, i + 1);
        labels[w] = false;
        tree(w);
    };
    tree = [&](Node v) {
        loops[v] = plan->header[v];
        within(v, 0);
        loops[v] = false;
    };
    tree(entry);
    Require(emitted == reachable, "layout reachability mismatch");
}

int main() {
    Require(!PlanRegion({}, 0), "empty graph accepted");
    Require(!PlanRegion({{}}, 1), "invalid entry accepted");
    Require(!PlanRegion({{1}}, 0), "invalid edge accepted");
    Check({{1, 1}, {2}, {1}, {}}, 0); // duplicates and unreachable node
    Check({{}, {2, 3}, {3}, {2}}, 1); // nonzero entry, irreducible loop
    Check({{1}, {2, 3}, {1}, {0}}, 3); // nested loops, nonzero entry
    // Exhaust every directed four-node graph, including self edges and dead
    // components. Small enough to run on every change, no probabilistic seed.
    for (unsigned mask = 0; mask < (1u << 16); ++mask) {
        RegionGraph graph(4);
        for (Node v = 0; v < 4; ++v) {
            for (Node w = 0; w < 4; ++w) if (mask & (1u << (4 * v + w))) graph[v].push_back(w);
        }
        Check(graph, 0);
    }
    std::cout << "PASS region planner: 65536 graphs and boundary cases\n";
}
