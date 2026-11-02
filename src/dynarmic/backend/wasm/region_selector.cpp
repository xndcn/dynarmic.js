// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/region_selector.h"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <boost/variant.hpp>

#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::Wasm {
namespace {

using Descriptor = IR::LocationDescriptor;
using GenerateBlock = RegionSelector::GenerateBlock;
using RangeOf = RegionSelector::RangeOf;
using GuestPC = RegionSelector::GuestPC;
using Compatible = RegionSelector::Compatible;

constexpr size_t focused_discovery_safety_limit = 1024;

struct SuccessorCollector {
    std::vector<Descriptor>& out;

    void operator()(const IR::Term::LinkBlock& term) { out.push_back(term.next); }
    void operator()(const IR::Term::LinkBlockFast& term) { out.push_back(term.next); }
    void operator()(const IR::Term::If& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckBit& term) {
        boost::apply_visitor(*this, term.then_);
        boost::apply_visitor(*this, term.else_);
    }
    void operator()(const IR::Term::CheckHalt& term) { boost::apply_visitor(*this, term.else_); }
    template<typename T>
    void operator()(const T&) {}
};

std::vector<Descriptor> DirectSuccessors(const IR::Block& block) {
    std::vector<Descriptor> out;
    SuccessorCollector collector{out};
    boost::apply_visitor(collector, block.GetTerminal());
    return out;
}

std::vector<Descriptor> Successors(const IR::Block& block) {
    std::vector<Descriptor> out;
    if (block.HasConditionFailedLocation()) out.push_back(block.ConditionFailedLocation());
    SuccessorCollector collector{out};
    boost::apply_visitor(collector, block.GetTerminal());
    for (const auto& inst : block) {
        if (inst.GetOpcode() == IR::Opcode::PushRSB) out.push_back(Descriptor{inst.GetArg(0).GetU64()});
    }
    return out;
}

RegionSelectionResult SelectOrdinary(Descriptor entry, GenerateBlock generate_block, RangeOf range_of,
                                     GuestPC guest_pc, Compatible compatible, bool chain_link_blocks,
                                     RegionSelectionLimits limits) {
    RegionSelectionResult result;
    result.blocks.push_back(generate_block(entry));
    if (!chain_link_blocks || !compatible(entry, entry)) return result;

    size_t instructions = result.blocks.front().size();
    std::unordered_set<u64> seen{entry.Value()};
    std::deque<Descriptor> work;
    for (const auto& next : Successors(result.blocks.front())) work.push_back(next);

    while (!work.empty()) {
        const Descriptor next = work.front();
        work.pop_front();
        result.stats.candidates++;
        if (!seen.insert(next.Value()).second) continue;
        if (!compatible(entry, next)) {
            result.stats.rejected_incompatible++;
            continue;
        }
        if (result.blocks.size() >= limits.max_blocks) {
            result.stats.rejected_block_budget++;
            continue;
        }
        const u64 entry_pc = range_of(result.blocks.front()).start;
        const u64 next_pc = guest_pc(next);
        const u64 distance = next_pc > entry_pc ? next_pc - entry_pc : entry_pc - next_pc;
        if (distance > limits.max_guest_distance) {
            result.stats.rejected_distance++;
            continue;
        }
        IR::Block block = generate_block(next);
        if (instructions + block.size() > limits.max_instructions) {
            result.stats.rejected_ir_budget++;
            continue;
        }
        instructions += block.size();
        for (const auto& successor : Successors(block)) work.push_back(successor);
        result.blocks.push_back(std::move(block));
    }
    if (result.blocks.size() == 1) return result;

    std::vector<size_t> order(result.blocks.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return range_of(result.blocks[a]).start < range_of(result.blocks[b]).start;
    });
    std::vector<IR::Block> sorted;
    sorted.reserve(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        if (order[i] == 0) result.entry_index = i;
        sorted.push_back(std::move(result.blocks[order[i]]));
    }
    result.blocks = std::move(sorted);
    return result;
}

struct DiscoveredGraph {
    std::vector<IR::Block> blocks;
    std::unordered_map<u64, size_t> indices;

    bool Contains(Descriptor location) const { return indices.contains(location.Value()); }
    size_t Index(Descriptor location) const { return indices.at(location.Value()); }
};

DiscoveredGraph DiscoverFocused(Descriptor entry, GenerateBlock generate_block, RangeOf range_of,
                                GuestPC guest_pc, Compatible compatible, u64 max_guest_distance) {
    DiscoveredGraph graph;
    graph.blocks.push_back(generate_block(entry));
    graph.indices.emplace(entry.Value(), 0);
    std::unordered_set<u64> seen{entry.Value()};
    std::deque<Descriptor> work;
    for (const Descriptor next : Successors(graph.blocks.front())) work.push_back(next);
    const u64 entry_pc = range_of(graph.blocks.front()).start;

    while (!work.empty()) {
        const Descriptor next = work.front();
        work.pop_front();
        if (!seen.insert(next.Value()).second) continue;
        if (!compatible(entry, next)) continue;
        const u64 next_pc = guest_pc(next);
        const u64 distance = next_pc > entry_pc ? next_pc - entry_pc : entry_pc - next_pc;
        if (distance > max_guest_distance) continue;
        if (graph.blocks.size() >= focused_discovery_safety_limit) {
            throw std::length_error{"focused discovery exceeded 1024 accepted-block safety bound"};
        }
        IR::Block block = generate_block(next);
        for (const Descriptor successor : Successors(block)) work.push_back(successor);
        graph.indices.emplace(next.Value(), graph.blocks.size());
        graph.blocks.push_back(std::move(block));
    }
    return graph;
}

std::vector<size_t> BreadthFirstTree(const DiscoveredGraph& graph, size_t start) {
    const size_t missing = graph.blocks.size();
    std::vector<size_t> predecessor(missing, missing);
    std::deque<size_t> work{start};
    predecessor[start] = start;
    while (!work.empty()) {
        const size_t current = work.front();
        work.pop_front();
        for (const Descriptor next : Successors(graph.blocks[current])) {
            const auto it = graph.indices.find(next.Value());
            if (it == graph.indices.end() || predecessor[it->second] != missing) continue;
            predecessor[it->second] = current;
            work.push_back(it->second);
        }
    }
    return predecessor;
}

void RequirePath(const DiscoveredGraph& graph, size_t start, Descriptor goal,
                 const std::vector<size_t>& predecessor, std::unordered_set<u64>& required,
                 const char* diagnostic) {
    const auto it = graph.indices.find(goal.Value());
    const size_t missing = graph.blocks.size();
    if (it == graph.indices.end() || predecessor[it->second] == missing) {
        throw std::runtime_error{diagnostic};
    }
    size_t current = it->second;
    while (true) {
        required.insert(graph.blocks[current].Location().Value());
        if (current == start) break;
        current = predecessor[current];
    }
}

std::unordered_set<u64> RequiredFocusedClosure(const DiscoveredGraph& graph,
                                               const RegionSelectionFocus& focus) {
    std::unordered_set<u64> required;
    const size_t entry = graph.Index(focus.entry);
    const auto entry_tree = BreadthFirstTree(graph, entry);
    RequirePath(graph, entry, focus.exit_block, entry_tree, required,
                "focused required entry to exit_block path is missing");
    if (!graph.Contains(focus.direct_target)) {
        throw std::runtime_error{"focused required direct terminal edge exit_block->direct_target is absent"};
    }
    const size_t target = graph.Index(focus.direct_target);
    const auto target_tree = BreadthFirstTree(graph, target);
    required.insert(focus.direct_target.Value());
    for (const Descriptor return_block : focus.return_blocks) {
        RequirePath(graph, target, return_block, target_tree, required,
                    "focused required direct_target to return block path is missing");
    }
    const auto& direct = DirectSuccessors(graph.blocks[graph.Index(focus.exit_block)]);
    if (std::find(direct.begin(), direct.end(), focus.direct_target) == direct.end()) {
        throw std::runtime_error{"focused required direct terminal edge exit_block->direct_target is absent"};
    }
    return required;
}

size_t RequiredInstructions(const DiscoveredGraph& graph, const std::unordered_set<u64>& required,
                            size_t max_instructions) {
    size_t instructions = 0;
    for (const auto& block : graph.blocks) {
        if (!required.contains(block.Location().Value())) continue;
        if (block.size() > max_instructions - instructions) {
            throw std::length_error{"focused required IR budget exceeded"};
        }
        instructions += block.size();
    }
    return instructions;
}

std::vector<std::vector<size_t>> Predecessors(const DiscoveredGraph& graph) {
    std::vector<std::vector<size_t>> predecessors(graph.blocks.size());
    for (size_t current = 0; current < graph.blocks.size(); ++current) {
        for (const Descriptor next : Successors(graph.blocks[current])) {
            const auto it = graph.indices.find(next.Value());
            if (it != graph.indices.end()) predecessors[it->second].push_back(current);
        }
    }
    return predecessors;
}

void VerifyFocusedSelection(const DiscoveredGraph& graph, const std::vector<bool>& selected,
                            const RegionSelectionFocus& focus, size_t selected_count) {
    std::vector<bool> reachable(graph.blocks.size(), false);
    std::deque<size_t> work{graph.Index(focus.entry)};
    reachable[work.front()] = true;
    size_t reachable_count = 0;
    while (!work.empty()) {
        const size_t current = work.front();
        work.pop_front();
        ++reachable_count;
        for (const Descriptor next : Successors(graph.blocks[current])) {
            const auto it = graph.indices.find(next.Value());
            if (it == graph.indices.end() || !selected[it->second] || reachable[it->second]) continue;
            reachable[it->second] = true;
            work.push_back(it->second);
        }
    }
    if (reachable_count != selected_count || !selected[graph.Index(focus.exit_block)] ||
        !selected[graph.Index(focus.direct_target)]) {
        throw std::runtime_error{"focused selected graph is not entirely entry-reachable"};
    }
    for (const Descriptor return_block : focus.return_blocks) {
        if (!graph.Contains(return_block) || !selected[graph.Index(return_block)]) {
            throw std::runtime_error{"focused selected graph lost a required return block"};
        }
    }
}

RegionSelectionResult SortFocused(DiscoveredGraph graph, const std::vector<bool>& selected,
                                  Descriptor entry, RangeOf range_of, RegionSelectionStats stats) {
    std::vector<size_t> order;
    for (size_t i = 0; i < selected.size(); ++i) {
        if (selected[i]) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto range_a = range_of(graph.blocks[a]);
        const auto range_b = range_of(graph.blocks[b]);
        if (range_a.start != range_b.start) return range_a.start < range_b.start;
        if (range_a.end != range_b.end) return range_a.end < range_b.end;
        return graph.blocks[a].Location().Value() < graph.blocks[b].Location().Value();
    });
    RegionSelectionResult result;
    result.stats = stats;
    result.blocks.reserve(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        if (graph.blocks[order[i]].Location() == entry) result.entry_index = i;
        result.blocks.push_back(std::move(graph.blocks[order[i]]));
    }
    return result;
}

RegionSelectionResult SelectFocused(Descriptor entry, GenerateBlock generate_block, RangeOf range_of,
                                    GuestPC guest_pc, Compatible compatible, bool chain_link_blocks,
                                    RegionSelectionLimits limits, const RegionSelectionFocus& focus) {
    if (entry != focus.entry) {
        throw std::invalid_argument{"focused selection requires focus.entry to equal selection entry"};
    }
    if (!chain_link_blocks) throw std::invalid_argument{"focused selection requires chain_link_blocks=true"};
    if (!compatible(entry, entry)) throw std::invalid_argument{"focused selection failed entry compatibility"};

    DiscoveredGraph graph = DiscoverFocused(entry, generate_block, range_of, guest_pc, compatible,
                                            limits.max_guest_distance);
    const auto required = RequiredFocusedClosure(graph, focus);
    if (required.size() > limits.max_blocks) {
        throw std::length_error{"focused required block budget exceeded"};
    }
    size_t instructions = RequiredInstructions(graph, required, limits.max_instructions);

    // Focused statistics describe optional-fill admission only. Discovery and
    // required-closure failures throw; required blocks are never candidates.
    RegionSelectionStats stats;
    std::vector<bool> selected(graph.blocks.size(), false);
    size_t selected_count = 0;
    for (size_t i = 0; i < graph.blocks.size(); ++i) {
        if (required.contains(graph.blocks[i].Location().Value())) {
            selected[i] = true;
            ++selected_count;
        }
    }
    const auto predecessors = Predecessors(graph);
    for (size_t i = 0; i < graph.blocks.size(); ++i) {
        if (selected[i]) continue;
        ++stats.candidates;
        const bool connected = std::any_of(predecessors[i].begin(), predecessors[i].end(),
                                           [&](size_t parent) { return selected[parent]; });
        if (!connected) continue;
        if (selected_count >= limits.max_blocks) {
            ++stats.rejected_block_budget;
            continue;
        }
        if (graph.blocks[i].size() > limits.max_instructions - instructions) {
            ++stats.rejected_ir_budget;
            continue;
        }
        instructions += graph.blocks[i].size();
        selected[i] = true;
        ++selected_count;
    }
    VerifyFocusedSelection(graph, selected, focus, selected_count);
    return SortFocused(std::move(graph), selected, entry, range_of, stats);
}

}  // namespace

RegionSelectionResult RegionSelector::Select(Descriptor entry, GenerateBlock generate_block, RangeOf range_of,
                                             GuestPC guest_pc, Compatible compatible, bool chain_link_blocks,
                                             RegionSelectionLimits limits,
                                             std::optional<RegionSelectionFocus> focus) {
    if (!focus) {
        return SelectOrdinary(entry, std::move(generate_block), std::move(range_of), std::move(guest_pc),
                              std::move(compatible), chain_link_blocks, limits);
    }
    return SelectFocused(entry, std::move(generate_block), std::move(range_of), std::move(guest_pc),
                         std::move(compatible), chain_link_blocks, limits, *focus);
}

}  // namespace Dynarmic::Backend::Wasm
