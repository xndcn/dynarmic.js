// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Structured region layout (after Ramsey, "Beyond Relooper", ICFP 2022):
// when the region's in-region control flow is reducible, its blocks are
// emitted as nested wasm `block`s and `loop`s following the dominator tree,
// so every in-region edge is a direct `br` -- forward to an enclosing block
// label, backward to an enclosing loop header -- instead of going through the
// br_table dispatch loop of EmitBeginRegion. Irreducible regions keep the
// dispatch loop.
//
// doTree(x)        = x is a loop header ? loop { nodeWithin(x, children) }
//                                       : nodeWithin(x, children)
// nodeWithin(x, y:ys) = block { nodeWithin(x, ys) } doTree(y)
// nodeWithin(x, [])   = code of x
// with x's dominator-tree children ordered by decreasing reverse-postorder
// number, so a child's label encloses the code of every block that can
// branch forward to it.

#include <functional>
#include <vector>

#include <boost/variant.hpp>

#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/region_plan.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::Wasm {

namespace {

// In-region successors of one block: link targets of its terminal tree,
// its cond-fail location, and the region's return sites when it returns
// (EmitRegionReturn). Self edges are left out: they are the block's own
// nested loop (EmitBeginRegionSelfLoop).
struct EdgeCollector {
    const EmitContext& ctx;
    u32 self;
    std::vector<u32>& out;

    void Add(const IR::LocationDescriptor& next) {
        const auto it = ctx.region.index_of.find(next.Value());
        if (it != ctx.region.index_of.end() && it->second != self) {
            out.push_back(it->second);
        }
    }
    void AddReturnSites() {
        for (const u32 site : ctx.region.return_sites) {
            if (site != self) {
                out.push_back(site);
            }
        }
    }

    void operator()(const IR::Term::LinkBlock& term) { Add(term.next); }
    void operator()(const IR::Term::LinkBlockFast& term) { Add(term.next); }
    void operator()(const IR::Term::PopRSBHint&) { AddReturnSites(); }
    void operator()(const IR::Term::FastDispatchHint&) { AddReturnSites(); }
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

struct Emitter {
    EmitContext& ctx;
    const RegionPlan& plan;
    const std::function<void(u32)>& emit_block;

    void DoTree(u32 x) {
        auto& region = ctx.region;
        if (plan.header[x]) {
            ctx.code.Loop();
            region.open_loop_depth[x] = ctx.code.Depth();
            NodeWithin(x, 0);
            region.open_loop_depth[x] = 0;
            ctx.code.End();
        } else {
            NodeWithin(x, 0);
        }
    }

    void NodeWithin(u32 x, size_t i) {
        const auto& ys = plan.children[x];
        if (i == ys.size()) {
            emit_block(x);
            return;
        }
        auto& region = ctx.region;
        const u32 y = ys[i];
        ctx.code.Block();
        region.open_label_depth[y] = ctx.code.Depth();
        NodeWithin(x, i + 1);
        region.open_label_depth[y] = 0;
        ctx.code.End();
        DoTree(y);
    }
};

std::vector<u8> FindEntryReachable(const RegionGraph& successors, u32 entry) {
    std::vector<u8> reachable(successors.size(), 0);
    std::vector<u32> pending{entry};
    reachable[entry] = 1;
    while (!pending.empty()) {
        const u32 current = pending.back();
        pending.pop_back();
        for (const u32 next : successors[current]) {
            if (reachable[next] == 0) {
                reachable[next] = 1;
                pending.push_back(next);
            }
        }
    }
    return reachable;
}

}  // anonymous namespace

bool EmitStructuredRegion(EmitContext& ctx, const std::function<void(u32)>& emit_block,
                          std::vector<u8>* entry_reachable) {
    auto& region = ctx.region;
    const bool multi_entry = ctx.emit_conf.enable_region_multi_entry &&
                             !IsSingleStepping(ctx, region.blocks[region.entry]->Location());
    // Multi-entry regions need labels for every block. Retain reachability
    // diagnostics when requested, without paying for graph planning otherwise.
    if (multi_entry && entry_reachable == nullptr) return false;
    const u32 n = static_cast<u32>(region.blocks.size());
    RegionGraph successors(n);
    for (u32 i = 0; i < n; ++i) {
        EdgeCollector collector{ctx, i, successors[i]};
        if (region.blocks[i]->HasConditionFailedLocation()) {
            collector.Add(region.blocks[i]->ConditionFailedLocation());
        }
        boost::apply_visitor(collector, region.blocks[i]->GetTerminal());
    }
    if (entry_reachable != nullptr) {
        *entry_reachable = FindEntryReachable(successors, region.entry);
    }
    if (multi_entry) return false;
    const auto plan = PlanRegion(successors, region.entry);
    if (!plan) {
        return false;
    }
    region.structured = true;
    region.open_label_depth.assign(n, 0);
    region.open_loop_depth.assign(n, 0);
    EmitBeginStructuredRegion(ctx);
    Emitter{ctx, *plan, emit_block}.DoTree(region.entry);
    EmitEndStructuredRegion(ctx);
    return true;
}

}  // namespace Dynarmic::Backend::Wasm
