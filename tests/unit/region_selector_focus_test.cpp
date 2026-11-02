// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dynarmic/backend/wasm/region_selector.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;
using D = IR::LocationDescriptor;

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error{message};
}

template<typename Exception, typename Callback>
void Throws(Callback callback, const std::string& diagnostic) {
    try {
        callback();
    } catch (const Exception& error) {
        Require(std::string{error.what()}.find(diagnostic) != std::string::npos,
                "wrong diagnostic: " + std::string{error.what()});
        return;
    }
    throw std::runtime_error{"expected exception containing: " + diagnostic};
}

IR::Terminal Link(u64 next) { return IR::Term::LinkBlock{D{next}}; }
IR::Terminal Fast(u64 next) { return IR::Term::LinkBlockFast{D{next}}; }
IR::Terminal Branch(IR::Terminal then_, IR::Terminal else_) {
    return IR::Term::If{IR::Cond::EQ, std::move(then_), std::move(else_)};
}

struct Node {
    u64 pc = 0;
    size_t ir = 0;
    bool compatible = true;
    IR::Terminal terminal = IR::Term::ReturnToDispatch{};
    std::optional<D> condition_failed;
    std::vector<D> push_rsb;
};

struct SyntheticGraph {
    std::map<u64, Node> nodes;
    mutable std::map<u64, size_t> generated;
    mutable std::vector<u64> generation_order;

    SyntheticGraph& Add(u64 descriptor, IR::Terminal terminal = IR::Term::ReturnToDispatch{},
                        u64 pc = 0, size_t ir = 0) {
        nodes[descriptor] = Node{.pc = pc, .ir = ir, .terminal = std::move(terminal)};
        return *this;
    }

    IR::Block Generate(D location) const {
        generated[location.Value()]++;
        generation_order.push_back(location.Value());
        const auto& node = nodes.at(location.Value());
        IR::Block block{location};
        if (node.condition_failed) block.SetConditionFailedLocation(*node.condition_failed);
        for (size_t i = 0; i < node.ir; ++i) block.AppendNewInst(IR::Opcode::Breakpoint, {});
        for (D next : node.push_rsb) {
            block.AppendNewInst(IR::Opcode::PushRSB, {IR::Value{next.Value()}});
        }
        block.SetTerminal(node.terminal);
        return block;
    }

    u64 PC(D location) const { return nodes.at(location.Value()).pc; }
    RegionGuestRange Range(const IR::Block& block) const {
        const u64 pc = PC(block.Location());
        return {pc, pc + 3};
    }
    bool Compatible(D, D target) const {
        const auto it = nodes.find(target.Value());
        return it != nodes.end() && it->second.compatible;
    }
};

RegionSelectionResult Select(const SyntheticGraph& graph, D entry,
                             std::optional<RegionSelectionFocus> focus = std::nullopt,
                             RegionSelectionLimits limits = {}, bool chain = true) {
    return RegionSelector::Select(
        entry, [&](D location) { return graph.Generate(location); },
        [&](const IR::Block& block) { return graph.Range(block); },
        [&](D location) { return graph.PC(location); },
        [&](D from, D to) { return graph.Compatible(from, to); }, chain, limits, focus);
}

RegionSelectionFocus Focus(u64 entry, u64 exit, u64 target, std::vector<u64> returns = {}) {
    RegionSelectionFocus focus{D{entry}, D{exit}, D{target}, {}};
    for (u64 value : returns) focus.return_blocks.emplace_back(value);
    return focus;
}

std::vector<u64> Descriptors(const RegionSelectionResult& result) {
    std::vector<u64> values;
    for (const auto& block : result.blocks) values.push_back(block.Location().Value());
    return values;
}

void OncePerGeneratedBlock(const SyntheticGraph& graph) {
    for (const auto& [descriptor, count] : graph.generated) {
        Require(count == 1, "descriptor generated more than once: " + std::to_string(descriptor));
    }
}

struct Limits : RegionSelectionLimits {
    Limits(size_t blocks = 64, size_t ir = 2000)
            : RegionSelectionLimits{blocks, ir, std::numeric_limits<u64>::max()} {}
};

void NoFocusIsOrdinaryAndNulloptEquivalent() {
    SyntheticGraph graph;
    graph.Add(100, Branch(Link(300), Link(200)), 100).Add(200, Link(400), 200)
        .Add(300, {}, 300).Add(400, {}, 400);
    const auto ordinary = RegionSelector::Select(
        D{100}, [&](D d) { return graph.Generate(d); },
        [&](const IR::Block& b) { return graph.Range(b); }, [&](D d) { return graph.PC(d); },
        [&](D a, D b) { return graph.Compatible(a, b); }, true, Limits{3, 2000});
    const auto explicit_nullopt = Select(graph, D{100}, std::nullopt, Limits{3, 2000});
    Require(Descriptors(ordinary) == std::vector<u64>({100, 200, 300}), "ordinary dispatch changed");
    Require(Descriptors(explicit_nullopt) == Descriptors(ordinary), "explicit nullopt differs");
    Require(ordinary.entry_index == explicit_nullopt.entry_index && ordinary.stats.candidates == 3 &&
                explicit_nullopt.stats.candidates == 3 &&
                ordinary.stats.rejected_block_budget == explicit_nullopt.stats.rejected_block_budget,
            "ordinary counters or entry index changed");
}

void MinimalMultiReturnClosure() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(120), 110).Add(120, Link(200), 120)
        .Add(200, Branch(Link(300), Link(310)), 200).Add(300, {}, 300).Add(310, {}, 310);
    const auto result = Select(graph, D{100}, Focus(100, 120, 200, {300, 310}));
    Require(Descriptors(result) == std::vector<u64>({100, 110, 120, 200, 300, 310}),
            "multi-return required closure is incomplete");
    Require(result.stats.candidates == 0, "required blocks counted as optional candidates");
    OncePerGeneratedBlock(graph);
}

void EqualLengthPathUsesSuccessorOrder() {
    SyntheticGraph graph;
    graph.Add(100, Branch(Link(110), Link(120)), 100).Add(110, Link(130), 110)
        .Add(120, Link(130), 120).Add(130, Link(200), 130).Add(200, {}, 200);
    const auto result = Select(graph, D{100}, Focus(100, 130, 200), Limits{4});
    Require(Descriptors(result) == std::vector<u64>({100, 110, 130, 200}),
            "BFS tie did not use production successor order");
}

void CompleteSuccessorOrderAndNestedFast() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100)
        .Add(110, Link(900), 110).Add(200, {}, 200).Add(300, {}, 300)
        .Add(400, {}, 400).Add(500, {}, 500).Add(600, {}, 600).Add(900, {}, 900);
    graph.nodes[100].terminal = Branch(IR::Term::CheckHalt{Fast(300)}, Link(200));
    graph.nodes[100].condition_failed = D{400};
    graph.nodes[100].push_rsb = {D{500}, D{600}};
    // The exit is reachable through a required predecessor; all entry successors retain FIFO order.
    graph.nodes[200].terminal = Link(110);
    const auto result = Select(graph, D{100}, Focus(100, 110, 900), Limits{6});
    Require(graph.generated.size() == 8, "condition/direct/PushRSB discovery order is incomplete");
    Require(graph.generation_order == std::vector<u64>({100, 400, 300, 200, 500, 600, 110, 900}),
            "production successor order changed during focused discovery");
    Require(Descriptors(result) == std::vector<u64>({100, 110, 200, 300, 400, 900}),
            "optional fill did not retain discovery FIFO order");
    Require(result.stats.candidates == 4 && result.stats.rejected_block_budget == 2,
            "focused optional counters are wrong");
    OncePerGeneratedBlock(graph);
}

void CyclesAndDuplicateEdgesGenerateOnce() {
    SyntheticGraph graph;
    graph.Add(100, Branch(Link(110), Link(110)), 100)
        .Add(110, Branch(Link(100), Link(200)), 110).Add(200, Link(110), 200);
    const auto result = Select(graph, D{100}, Focus(100, 110, 200));
    Require(Descriptors(result) == std::vector<u64>({100, 110, 200}), "cycle deduplication failed");
    Require(graph.generated.size() == 3, "cycle generated an unexpected descriptor");
    OncePerGeneratedBlock(graph);
}

void DuplicateReturnsAreDeduplicated() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(200), 110)
        .Add(200, Branch(Link(300), Link(300)), 200).Add(300, {}, 300);
    const auto result = Select(graph, D{100}, Focus(100, 110, 200, {300, 300, 300}));
    Require(Descriptors(result) == std::vector<u64>({100, 110, 200, 300}), "duplicate return retained");
    OncePerGeneratedBlock(graph);
}

void DescriptorIdentityIncludesMoreThanPC() {
    constexpr u64 a = 0x0100000000000100, b = 0x0200000000000100;
    SyntheticGraph graph;
    graph.Add(a, Link(a), 0x500).Add(b, Fast(b), 0x500);
    const auto result = Select(graph, D{a}, Focus(a, a, a));
    Require(Descriptors(result) == std::vector<u64>({a}), "full identity changed self selection");
    // Discover the same-PC alias through a non-terminal edge. The requested
    // direct target therefore reaches the final terminal-edge identity check.
    graph.nodes[a].condition_failed = D{b};
    Throws<std::runtime_error>([&] { Select(graph, D{a}, Focus(a, a, b)); }, "direct terminal edge");
}

void FalseConditionFailedEdgeRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, {}, 110).Add(200, {}, 200);
    graph.nodes[110].condition_failed = D{200};
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200)); },
                               "direct terminal edge");
}

void FalsePushRSBEdgeRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, {}, 110).Add(200, {}, 200);
    graph.nodes[110].push_rsb = {D{200}};
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200)); },
                               "direct terminal edge");
}

void ReachableDescendantIsNotDirectEdge() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(150), 110).Add(150, Link(200), 150)
        .Add(200, {}, 200);
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200)); },
                               "direct terminal edge");
}

void LinkBlockFastDirectEdgeAccepted() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Fast(200), 110).Add(200, {}, 200);
    const auto result = Select(graph, D{100}, Focus(100, 110, 200));
    Require(Descriptors(result) == std::vector<u64>({100, 110, 200}), "fast direct edge rejected");
}

void ExactEntryMismatchRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(100), 100);
    Throws<std::invalid_argument>([&] { Select(graph, D{100}, Focus(101, 100, 100)); },
                                  "focus.entry");
}

void RequiredBlockExactBoundaryAndOverflow() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(200), 110).Add(200, {}, 200);
    Require(Select(graph, D{100}, Focus(100, 110, 200), Limits{3}).blocks.size() == 3,
            "exact required block boundary rejected");
    Throws<std::length_error>([&] { Select(graph, D{100}, Focus(100, 110, 200), Limits{2}); },
                              "required block budget");
}

void RequiredIRExactBoundaryAndOverflow() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100, 2).Add(110, Link(200), 110, 3).Add(200, {}, 200, 4);
    Require(Select(graph, D{100}, Focus(100, 110, 200), Limits{3, 9}).blocks.size() == 3,
            "exact required IR boundary rejected");
    Throws<std::length_error>([&] { Select(graph, D{100}, Focus(100, 110, 200), Limits{3, 8}); },
                              "required IR budget");
}

void EntryIRParticipatesInRequiredBudget() {
    SyntheticGraph graph;
    graph.Add(100, Link(100), 100, 1);
    Throws<std::length_error>([&] { Select(graph, D{100}, Focus(100, 100, 100), Limits{1, 0}); },
                              "required IR budget");
}

void UnreachableRequiredExitRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(100), 100).Add(110, Link(200), 110).Add(200, {}, 200);
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200)); },
                               "entry to exit_block");
}

void UnreachableRequiredReturnRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(200), 110).Add(200, {}, 200).Add(300, {}, 300);
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200, {300})); },
                               "direct_target to return");
}

void IncompatibleRequiredNodeRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(200), 110).Add(200, {}, 200);
    graph.nodes[110].compatible = false;
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200)); },
                               "entry to exit_block");
}

void DistantRequiredNodeRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 0).Add(110, Link(200), 100).Add(200, {}, 200);
    auto limits = Limits{};
    limits.max_guest_distance = 50;
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200), limits); },
                               "entry to exit_block");
}

void DiscoverySafetyBoundAllowsExactBoundary() {
    SyntheticGraph graph;
    graph.Add(1000, Branch(Link(1001), Link(2000)), 0)
        .Add(1001, Link(1002), 1).Add(1002, {}, 2);
    for (u64 i = 0; i < 1021; ++i) {
        graph.Add(2000 + i, i == 1020 ? IR::Terminal{} : Link(2001 + i), 3 + i);
    }
    const auto result = Select(graph, D{1000}, Focus(1000, 1001, 1002));
    Require(graph.generated.size() == 1024 && result.blocks.size() == 64,
            "exact 1024-block discovery boundary was rejected");
    OncePerGeneratedBlock(graph);
}

void DiscoverySafetyBoundThrows() {
    SyntheticGraph graph;
    for (u64 i = 0; i < 1025; ++i) graph.Add(1000 + i, Link(1001 + i), i);
    graph.Add(2025, {}, 1025);
    Throws<std::length_error>([&] { Select(graph, D{1000}, Focus(1000, 2024, 2025)); },
                              "1024");
}

void DiscoveryIgnoresOrdinaryBlockCap() {
    SyntheticGraph graph;
    graph.Add(100, Branch(Link(110), Link(300)), 100).Add(110, Link(200), 110).Add(200, {}, 200);
    for (u64 i = 0; i < 70; ++i) graph.Add(300 + i, i == 69 ? IR::Terminal{} : Link(301 + i), 300 + i);
    const auto result = Select(graph, D{100}, Focus(100, 110, 200));
    Require(graph.generated.size() == 73 && result.blocks.size() == 64,
            "ordinary block cap limited focused discovery");
}

void ChainingDisabledFailsInsteadOfOrdinaryFallback() {
    SyntheticGraph graph;
    graph.Add(100, Link(100), 100);
    Require(Select(graph, D{100}, std::nullopt, Limits{}, false).blocks.size() == 1,
            "ordinary chaining-disabled behavior changed");
    Throws<std::invalid_argument>([&] { Select(graph, D{100}, Focus(100, 100, 100), Limits{}, false); },
                                  "chain_link_blocks");
}

void OneBlockBudgetFailureAndSelfEdgeSuccess() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(200), 110).Add(200, {}, 200);
    Throws<std::length_error>([&] { Select(graph, D{100}, Focus(100, 110, 200), Limits{1}); },
                              "required block budget");
    SyntheticGraph self;
    self.Add(100, Link(100), 100);
    const auto result = Select(self, D{100}, Focus(100, 100, 100), Limits{1});
    Require(result.blocks.size() == 1 && result.entry_index == 0, "one-block direct self edge failed");
}

void SelfIncompatibleEntryRejected() {
    SyntheticGraph graph;
    graph.Add(100, Link(100), 100);
    graph.nodes[100].compatible = false;
    Throws<std::invalid_argument>([&] { Select(graph, D{100}, Focus(100, 100, 100)); },
                                  "entry compatibility");
}

void RequiredFirstDisplacesOrdinaryOptionalBlock() {
    SyntheticGraph graph;
    graph.Add(100, Branch(Link(120), Link(110)), 100).Add(110, Link(200), 110)
        .Add(120, {}, 120).Add(200, {}, 200);
    const auto ordinary = Select(graph, D{100}, std::nullopt, Limits{3});
    const auto focused = Select(graph, D{100}, Focus(100, 110, 200), Limits{3});
    Require(Descriptors(ordinary) == std::vector<u64>({100, 110, 120}), "ordinary fixture changed");
    Require(Descriptors(focused) == std::vector<u64>({100, 110, 200}),
            "required target did not displace optional block");
}

void NonselectedAlternativeRemainsExternal() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Branch(Link(200), Link(300)), 110)
        .Add(200, {}, 200).Add(300, {}, 300);
    const auto result = Select(graph, D{100}, Focus(100, 110, 200), Limits{3});
    Require(Descriptors(result) == std::vector<u64>({100, 110, 200}),
            "nonselected direct alternative was internalized");
}

void ExpensiveOptionalParentCannotAdmitDescendant() {
    SyntheticGraph graph;
    graph.Add(100, Branch(Link(110), Link(300)), 100).Add(110, Link(200), 110)
        .Add(200, {}, 200).Add(300, Link(400), 300, 4).Add(400, {}, 400, 1)
        .Add(500, {}, 500, 1);
    graph.nodes[100].terminal = Branch(Link(110), Branch(Link(300), Link(500)));
    const auto result = Select(graph, D{100}, Focus(100, 110, 200), Limits{4, 1});
    Require(Descriptors(result) == std::vector<u64>({100, 110, 200, 500}),
            "cheap independent block was not admitted or rejected parent admitted descendant");
    Require(result.stats.candidates == 3 && result.stats.rejected_ir_budget == 1,
            "optional parent/descendant counters wrong");
}

void OptionalBlockCapCountsOnlyOptionalCandidates() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Branch(Link(200), Branch(Link(300), Link(400))), 110)
        .Add(200, {}, 200).Add(300, {}, 300).Add(400, {}, 400);
    const auto result = Select(graph, D{100}, Focus(100, 110, 200), Limits{4});
    Require(result.stats.candidates == 2 && result.stats.rejected_block_budget == 1 &&
                result.stats.rejected_ir_budget == 0,
            "optional cap counters wrong");
}

void SortingEntryIndexAndDirectMembershipProof() {
    SyntheticGraph graph;
    graph.Add(100, Link(300), 300).Add(200, Fast(200), 200).Add(300, Link(200), 100);
    const auto result = Select(graph, D{100}, Focus(100, 300, 200));
    Require(Descriptors(result) == std::vector<u64>({300, 200, 100}) && result.entry_index == 2,
            "focused sorting or nonzero entry index wrong");
    Require(result.blocks[result.entry_index].Location() == D{100}, "entry index identity wrong");
}

void FocusedGuestRangeTieBreakIsDeterministic() {
    constexpr u64 high = 0x0200000000000300, low = 0x0100000000000300;
    SyntheticGraph graph;
    graph.Add(100, Link(200), 300).Add(200, Branch(Link(high), Link(low)), 100)
        .Add(high, {}, 200).Add(low, {}, 200);
    const auto result = Select(graph, D{100}, Focus(100, 200, high), Limits{4});
    Require(Descriptors(result) == std::vector<u64>({200, low, high, 100}) && result.entry_index == 3,
            "focused guest-range tie-break is not descriptor deterministic");
}

void FocusedCountersExcludeDiscoveryRejections() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 0).Add(110, Link(200), 10).Add(200, {}, 20)
        .Add(300, {}, 30).Add(400, {}, 40, 2).Add(500, {}, 1000).Add(600, {}, 50)
        .Add(700, {}, 60);
    graph.nodes[110].terminal = Branch(Link(200), Branch(Link(400), Branch(Link(300), Branch(Link(700), Link(500)))));
    graph.nodes[100].terminal = Branch(Link(110), Link(600));
    graph.nodes[600].compatible = false;
    auto limits = Limits{4, 1};
    limits.max_guest_distance = 100;
    const auto result = Select(graph, D{100}, Focus(100, 110, 200), limits);
    Require(result.stats.candidates == 3 && result.stats.rejected_block_budget == 1 &&
                result.stats.rejected_ir_budget == 1 && result.stats.rejected_incompatible == 0 &&
                result.stats.rejected_distance == 0,
            "focused statistics include discovery or miscount optional admission");
}

void ImpossibleFocusDoesNotFallBackToOrdinarySuccess() {
    SyntheticGraph graph;
    graph.Add(100, Link(110), 100).Add(110, Link(200), 110).Add(200, {}, 200);
    Require(Select(graph, D{100}).blocks.size() == 3, "ordinary representative must succeed");
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 110, 200, {999})); },
                               "direct_target to return");
    Throws<std::runtime_error>([&] { Select(graph, D{100}, Focus(100, 200, 200)); },
                               "direct terminal edge");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"no-focus ordinary/nullopt", NoFocusIsOrdinaryAndNulloptEquivalent},
        {"minimal multi-return", MinimalMultiReturnClosure},
        {"shortest-path tie", EqualLengthPathUsesSuccessorOrder},
        {"successor order/nested fast", CompleteSuccessorOrderAndNestedFast},
        {"cycles/duplicate edges", CyclesAndDuplicateEdgesGenerateOnce},
        {"duplicate returns", DuplicateReturnsAreDeduplicated},
        {"full descriptor identity", DescriptorIdentityIncludesMoreThanPC},
        {"false condition-failed edge", FalseConditionFailedEdgeRejected},
        {"false PushRSB edge", FalsePushRSBEdgeRejected},
        {"false descendant edge", ReachableDescendantIsNotDirectEdge},
        {"LinkBlockFast acceptance", LinkBlockFastDirectEdgeAccepted},
        {"entry mismatch", ExactEntryMismatchRejected},
        {"required block boundary", RequiredBlockExactBoundaryAndOverflow},
        {"required IR boundary", RequiredIRExactBoundaryAndOverflow},
        {"entry IR budget", EntryIRParticipatesInRequiredBudget},
        {"unreachable exit", UnreachableRequiredExitRejected},
        {"unreachable return", UnreachableRequiredReturnRejected},
        {"incompatible required node", IncompatibleRequiredNodeRejected},
        {"distant required node", DistantRequiredNodeRejected},
        {"discovery exact safety boundary", DiscoverySafetyBoundAllowsExactBoundary},
        {"discovery safety bound", DiscoverySafetyBoundThrows},
        {"discovery ignores ordinary cap", DiscoveryIgnoresOrdinaryBlockCap},
        {"chaining disabled", ChainingDisabledFailsInsteadOfOrdinaryFallback},
        {"one-block failure/success", OneBlockBudgetFailureAndSelfEdgeSuccess},
        {"self-incompatible entry", SelfIncompatibleEntryRejected},
        {"required-first displacement", RequiredFirstDisplacesOrdinaryOptionalBlock},
        {"nonselected alternative external", NonselectedAlternativeRemainsExternal},
        {"optional parent/descendant", ExpensiveOptionalParentCannotAdmitDescendant},
        {"optional cap counters", OptionalBlockCapCountsOnlyOptionalCandidates},
        {"sorting/entry/direct proof", SortingEntryIndexAndDirectMembershipProof},
        {"guest-range deterministic tie", FocusedGuestRangeTieBreakIsDeterministic},
        {"focused counters", FocusedCountersExcludeDiscoveryRejections},
        {"no ordinary fallback", ImpossibleFocusDoesNotFallBackToOrdinarySuccess},
    };
    size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << "/" << tests.size() << " focused selector tests failed\n";
        return 1;
    }
    std::cout << "PASS " << tests.size() << " focused selector tests\n";
    return 0;
}
