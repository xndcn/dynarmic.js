// End-to-end ownership and dispatch tests for A64 fallback return-site aliases.
// The guest has an irreducible continuation region and a distant `bl`/`ret`
// callee; BRK, rather than SVC, is the RSB-neutral observation point.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <unordered_map>

#include <boost/icl/interval_set.hpp>
#include <emscripten.h>

#include "dynarmic/backend/wasm/a64_address_space.h"
#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/compile_stats.h"
#include "dynarmic/backend/wasm/core.h"
#include "dynarmic/backend/wasm/profiler.h"
#include "dynarmic/backend/wasm/region_diagnostics.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;

namespace {

constexpr u64 kPredecessor = 0x1ff8;
// Entering at the BL alone would be structured: PushRSB is a selector edge,
// not a planner edge. This predecessor gives the planner a real path to the
// irreducible continuation graph as well as the fall-through path to the BL.
constexpr u64 kEntry = 0x1ffc;
constexpr u64 kCallSite = 0x2000;
constexpr u64 kContinuation = kCallSite + 4;
constexpr u64 kA = 0x2008;
constexpr u64 kACond = 0x200c;
constexpr u64 kExitA = 0x2010;
constexpr u64 kB = 0x2014;
constexpr u64 kBCond = 0x2018;
constexpr u64 kExitB = 0x201c;
constexpr u64 kUnrelated = 0x3000;
constexpr u64 kExternal = 0x4000;
static_assert(kExternal - kCallSite > 4096);
constexpr u64 kInternalEntry = 0x5000;
constexpr u64 kInternalContinuation = kInternalEntry + 4;
constexpr u64 kInternalCallee = kInternalEntry + 8;
constexpr u64 kHighCallSite = 0xfffffffffff00000ULL;
constexpr u64 kHighContinuation = kHighCallSite + 4;
constexpr u64 kHighExternal = kHighCallSite + 0x2000;
static_assert((kHighCallSite & (u64{1} << 55)) != 0);

constexpr u32 kBrk = 0xd4200000;
constexpr u32 kRet = 0xd65f03c0;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL a64 region alias: %s\n", message);
        std::exit(1);
    }
}

constexpr u32 Branch(u64 from, u64 to) {
    return 0x14000000u | (static_cast<u32>((to - from) >> 2) & 0x03ffffffu);
}
constexpr u32 BranchLink(u64 from, u64 to) {
    return 0x94000000u | (static_cast<u32>((to - from) >> 2) & 0x03ffffffu);
}
constexpr u32 BranchCond(u64 from, u64 to, u32 condition) {
    return 0x54000000u | ((static_cast<u32>((to - from) >> 2) & 0x7ffffu) << 5) | condition;
}
constexpr u32 Cbz(u64 from, u64 to, u32 reg) {
    return 0xb4000000u | ((static_cast<u32>((to - from) >> 2) & 0x7ffffu) << 5) | reg;
}
constexpr u32 Subs(u32 dst, u32 src, u32 immediate) {
    return 0xf1000000u | ((immediate & 0xfffu) << 10) | (src << 5) | dst;
}

IR::LocationDescriptor Location(u64 pc, bool single_step = false) {
    return A64::LocationDescriptor{pc, FP::FPCR{}, single_step};
}
u32 FunctionIndex(BlockFunc fn) {
    return static_cast<u32>(reinterpret_cast<uptr>(fn));
}

EM_JS(int, table_entry_live, (uptr slot), {
    return typeof wasmTable.get(slot) === 'function';
});
EM_JS(void, check_table_free_unique, (), {
    const seen = new Set();
    for (const slot of freeTableIndexes) {
        if (seen.has(slot)) throw new Error('duplicate generated free table slot');
        if (wasmTable.get(slot) !== null) throw new Error('live generated entry in free list');
        seen.add(slot);
    }
});

class Callbacks final : public A64::UserCallbacks {
public:
    Callbacks()
            : code{
                  {kPredecessor, Branch(kPredecessor, kEntry)},
                  {kEntry, Cbz(kEntry, kContinuation, 2)},
                  {kCallSite, BranchLink(kCallSite, kExternal)},
                  {kContinuation, Cbz(kContinuation, kB, 1)},
                  {kA, Subs(0, 0, 1)}, {kACond, BranchCond(kACond, kB, 1)}, {kExitA, kBrk},
                  {kB, Subs(0, 0, 1)}, {kBCond, BranchCond(kBCond, kA, 1)}, {kExitB, kBrk},
                  {kUnrelated, kBrk}, {kExternal, kRet},
                  {kInternalEntry, BranchLink(kInternalEntry, kInternalCallee)},
                  {kInternalContinuation, kBrk}, {kInternalCallee, kRet},
                  {kHighCallSite, BranchLink(kHighCallSite, kHighExternal)},
                  {kHighContinuation, kBrk}, {kHighExternal, kRet},
              } {}

    std::unordered_map<u64, u32> code;
    ExecutionContext* ectx = nullptr;
    u64 observed_pc = 0;
    unsigned exceptions = 0;

    std::optional<u32> MemoryReadCode(u64 address) override {
        const auto it = code.find(address);
        if (it == code.end()) return std::nullopt;
        return it->second;
    }
    u8 MemoryRead8(u64) override { return 0; }
    u16 MemoryRead16(u64) override { return 0; }
    u32 MemoryRead32(u64 address) override { return MemoryReadCode(address).value_or(0); }
    u64 MemoryRead64(u64) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, u16) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64, size_t) override { Require(false, "unexpected interpreter fallback"); }
    void CallSVC(u32) override { Require(false, "fixture must not use SVC"); }
    void ExceptionRaised(u64 pc, A64::Exception exception) override {
        Require(exception == A64::Exception::Breakpoint, "unexpected guest exception");
        ++exceptions;
        observed_pc = pc;
        ectx->halt_reason = static_cast<u32>(HaltReason::UserDefined4);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 10000; }
    u64 GetCNTPCT() override { return 0; }
};

A64::UserConfig ConfigFor(Callbacks& callbacks) {
    A64::UserConfig conf;
    conf.enable_cycle_counting = false;
    conf.callbacks = &callbacks;
    return conf;
}

struct Harness {
    Callbacks callbacks;
    A64::UserConfig conf;
    ProfileCounters profile;
    CompileStats stats;
    RegionDiagnostics diagnostics;
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    u64 instance_id = AllocateProfileInstanceId();
#endif
    A64AddressSpace space;
    A64JitState state{};
    ExecutionContext ectx{};
    Core<A64::UserConfig, A64JitState> core;

    explicit Harness(bool enable_aliases)
            : conf(ConfigFor(callbacks)),
              space(conf, &profile, &stats, &diagnostics, enable_aliases), core(conf, &profile) {
        callbacks.ectx = &ectx;
        ectx.callback_config = &conf;
    }
};

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
struct ExpectedTransferRegion {
    u64 region_id;
    u64 caller_descriptor;
    u64 caller_pc;
    u64 entry_count;
    u64 exit_count;
};
struct ExpectedTransferExit {
    u64 block_descriptor;
    u64 kind;
    u64 static_target;
    u64 count;
    u64 dynamic_descriptor;
    u64 dynamic_pc;
};

ExpectedTransferRegion Caller(u64 id, u64 pc, u64 entries, u64 exits) {
    return {id, pc, pc, entries, exits};
}
ExpectedTransferExit Exit(u64 block, RegionTransferKind kind, u64 static_target, u64 count,
                          u64 dynamic = 0, u64 dynamic_pc = 0) {
    return {block, static_cast<u64>(kind), static_target, count, dynamic, dynamic_pc};
}
ExpectedTransferExit Direct(u64 count) {
    return Exit(kCallSite, RegionTransferKind::ExternalDirectCall, kExternal, count,
                count ? kExternal : 0, count ? kExternal : 0);
}
ExpectedTransferExit Rsb(u64 count) {
    return Exit(kExternal, RegionTransferKind::RsbReturn, 0, count,
                count ? kContinuation : 0, count ? kContinuation : 0);
}
ExpectedTransferExit Other(u64 count) {
    return Exit(kExternal, RegionTransferKind::OtherTailTransfer, 0, count,
                count ? kContinuation : 0, count ? kContinuation : 0);
}

EM_JS(void, check_transfers, (u64 id, const u8* expected_regions, const u8* expected_exits,
                              int region_count, int exit_count), {
    const require = (condition, message) => {
        if (!condition) throw new Error('A64 transfer diagnostics: ' + message);
    };
    const hex = offset => {
        const index = offset >>> 2;
        return '0x' + HEAPU32[index + 1].toString(16).padStart(8, '0') +
            HEAPU32[index].toString(16).padStart(8, '0');
    };
    const small = offset => {
        const index = offset >>> 2;
        require(HEAPU32[index + 1] === 0, 'unsafe expected numeric field');
        return HEAPU32[index];
    };
    const stats = Module.dynarmicWasmStats;
    const diagnostic = stats.getInstance(String(id))?.regionTransferDiagnostics;
    require(diagnostic && diagnostic.schema === 'dynarmic.js/region-transfer-diagnostics-v1', 'schema or publication');
    require(stats.regionTransferDiagnostics === undefined, 'unexpected legacy mirror');
    require(Object.isFrozen(diagnostic) && Object.isFrozen(diagnostic.regions), 'mutable root');
    require(diagnostic.regions.length === region_count, 'region count');
    const kinds = ['external-direct-call', 'rsb-return', 'other-tail-transfer'];
    let expected_exit = 0;
    for (let i = 0; i < region_count; ++i) {
        const expected = expected_regions + i * 40;
        const region = diagnostic.regions[i];
        require(region.regionId === small(expected) && region.frontend === 'a64', 'region identity');
        require(region.callerEntryDescriptor === hex(expected + 8) && region.callerEntryPc === hex(expected + 16),
            'caller identity');
        require(region.entryCount === hex(expected + 24), 'entry count at region ' + i);
        require(region.exits.length === small(expected + 32), 'exit count at region ' + i);
        require(Object.isFrozen(region) && Object.isFrozen(region.exits), 'mutable region at ' + i);
        for (let j = 0; j < region.exits.length; ++j, ++expected_exit) {
            require(expected_exit < exit_count, 'unexpected exit');
            const wanted = expected_exits + expected_exit * 48;
            const exit = region.exits[j];
            const kind = small(wanted + 8);
            require(exit.exitSite === j && exit.exitBlockDescriptor === hex(wanted), 'exit identity');
            require(exit.kind === kinds[kind], 'exit kind');
            require(exit.staticTargetDescriptor === (kind === 0 ? hex(wanted + 16) : null), 'static target');
            require(exit.count === hex(wanted + 24), 'taken count at exit ' + expected_exit);
            require(exit.lastDynamicTargetDescriptor === hex(wanted + 32) &&
                exit.lastDynamicTargetPc === hex(wanted + 40), 'last dynamic target');
            require(Object.isFrozen(exit), 'mutable exit');
        }
    }
    require(expected_exit === exit_count, 'missing expected exits');
});

EM_JS(void, hold_transfers, (u64 id), {
    const key = String(id);
    const value = Module.dynarmicWasmStats.getInstance(key).regionTransferDiagnostics;
    const holds = Module.regionAliasTransferHolds || (Module.regionAliasTransferHolds = {});
    holds[key] = {value, json: JSON.stringify(value)};
});

EM_JS(void, check_held_transfers, (u64 id, int same_leaf), {
    const key = String(id);
    const held = Module.regionAliasTransferHolds?.[key];
    const current = Module.dynarmicWasmStats.getInstance(key)?.regionTransferDiagnostics;
    if (!held || JSON.stringify(held.value) !== held.json ||
        (current === held.value) !== (same_leaf !== 0))
        throw new Error('A64 transfer diagnostics: historical snapshot changed or was reused');
});

void Publish(const Harness& h) {
    PublishStats(&h.profile, h.stats, h.instance_id);
    PublishRegionDiagnostics(h.diagnostics, h.instance_id);
    PublishRegionTransferDiagnostics(h.diagnostics, h.instance_id);
}

void CheckTransfers(const Harness& h, std::initializer_list<ExpectedTransferRegion> regions,
                    std::initializer_list<ExpectedTransferExit> exits) {
    check_transfers(h.instance_id, reinterpret_cast<const u8*>(regions.begin()),
                    reinterpret_cast<const u8*>(exits.begin()),
                    static_cast<int>(regions.size()), static_cast<int>(exits.size()));
}

#if defined(DYNARMIC_WASM_PROFILE)
EM_JS(void, check_transfer_conservation, (u64 id, int expected_dispatch), {
    const snapshot = Module.dynarmicWasmStats.getInstance(String(id));
    const regions = snapshot.regionTransferDiagnostics.regions;
    let entries = 0n, exits = 0n;
    for (const region of regions) {
        entries += BigInt(region.entryCount);
        for (const exit of region.exits) exits += BigInt(exit.count);
    }
    if (snapshot.profile.dispatchCount !== expected_dispatch ||
        entries !== BigInt(expected_dispatch) + exits)
        throw new Error('A64 transfer diagnostics: Run conservation failed');
});

void CheckConservation(const Harness& h, u64 dispatch_count) {
    Require(h.profile.dispatch_count == dispatch_count, "wrong Run-only dispatch fixture");
    check_transfer_conservation(h.instance_id, static_cast<int>(dispatch_count));
}
#endif
#else
void Publish(const Harness&) {}
#endif

void Prepare(Harness& h, u64 pc, u64 x0 = 2, u64 x1 = 1) {
    h.state = {};
    h.state.pc = pc;
    h.state.reg[0] = x0;
    h.state.reg[1] = x1;
    h.state.reg[2] = 1;  // Region entry falls through to the BL; zero bypasses it.
    h.state.reg[30] = kContinuation;
    h.ectx.halt_reason = 0;
    h.ectx.link_slot_addr = 0;
    h.ectx.fast_dispatch_entry = 0;
    h.callbacks.observed_pc = 0;
    h.callbacks.exceptions = 0;
}

void Run(Harness& h, u64 pc, u64 expected_exit, u64 x0 = 2, u64 x1 = 1) {
    Prepare(h, pc, x0, x1);
    const HaltReason reason = h.core.Run(h.space, h.state, h.ectx);
    Publish(h);
    Require(reason == HaltReason::UserDefined4, "guest did not stop at BRK");
    Require(h.callbacks.exceptions == 1 && h.callbacks.observed_pc == expected_exit,
            "wrong guest path or observation count");
}

HaltReason Step(Harness& h, u64 pc) {
    Prepare(h, pc);
    const HaltReason reason = h.core.Step(h.space, h.state, h.ectx, Location(pc, true));
    Publish(h);
    return reason;
}

BlockFunc Compile(Harness& h, u64 pc) {
    BlockFunc fn = h.space.GetOrEmit(Location(pc));
    Require(fn != nullptr, "compilation returned no entry");
    return fn;
}

void Invalidate(Harness& h, u64 pc) {
    boost::icl::interval_set<u64> ranges;
    ranges.insert(boost::icl::discrete_interval<u64>::closed(pc, pc + 3));
    h.space.InvalidateRanges(ranges);
}

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
void CheckFallbackDiagnostics(const Harness& h) {
    constexpr std::array expected{kEntry, kCallSite, kContinuation, kA, kExitA, kB, kExitB};
    const auto& regions = h.diagnostics.Regions();
    Require(!regions.empty(), "missing fallback diagnostic");
    const auto& region = regions.back();
    Require(region.entry_pc == kEntry && region.entry_descriptor == kEntry &&
                region.layout == static_cast<u64>(RegionDiagnosticLayout::Fallback),
            "actual selector did not produce the expected fallback");
    Require(region.block_count == expected.size(), "wrong fallback block count");
    size_t continuation_index = expected.size();
    size_t return_sites = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        const size_t at = region.block_offset + i;
        Require(h.diagnostics.SelectedDescriptors()[at] == expected[i], "wrong selected block or external callee selected");
        Require(h.diagnostics.EmittedBlocks()[at] && h.diagnostics.EntryReachableBlocks()[at],
                "fallback block was not emitted and entry-reachable");
        return_sites += h.diagnostics.ReturnSites()[at];
        if (expected[i] == kContinuation) continuation_index = i;
    }
    Require(return_sites == 1 && continuation_index != expected.size() &&
                continuation_index != region.entry_index && h.diagnostics.ReturnSites()[region.block_offset + continuation_index],
            "continuation is not an emitted interior return site");
}
#else
void CheckFallbackDiagnostics(const Harness&) {}
#endif

void TestDefaultPublicationDisabled() {
    Callbacks callbacks;
    const A64::UserConfig conf = ConfigFor(callbacks);
    A64AddressSpace space{conf};
    const BlockFunc owner = space.GetOrEmit(Location(kEntry));
    Require(owner != nullptr, "default owner compilation returned no entry");
    Require(space.Get(Location(kContinuation)) == nullptr,
            "default A64 configuration published an experimental fallback alias");
    const BlockFunc continuation = space.GetOrEmit(Location(kContinuation));
    Require(continuation != nullptr && continuation != owner,
            "default continuation did not compile as an independent entry");
}

void TestOwnerAliasAndColdWarmCall() {
    Harness h{true};
    const BlockFunc owner = Compile(h, kEntry);
    const BlockFunc alias = h.space.Get(Location(kContinuation));
    Require(h.stats.blocks == 1 && alias == owner, "owner and alias must share one compilation");
    CheckFallbackDiagnostics(h);

    Run(h, kEntry, kExitB, 2, 1);
    Require(h.stats.blocks == 2, "cold call should compile only the external callee additionally");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    CheckTransfers(h, {Caller(1, kEntry, 2, 1), Caller(2, kExternal, 1, 2)},
                   {Direct(0), Rsb(0), Other(0)});
#if defined(DYNARMIC_WASM_PROFILE)
    CheckConservation(h, 3);
#endif
#endif
    Require(h.space.Get(Location(kEntry)) == owner && h.space.Get(Location(kContinuation)) == owner,
            "cold dispatch changed owner or alias");
    Require(h.ectx.rsb_top == 0 && h.ectx.rsb[0].descriptor == kContinuation &&
                h.ectx.rsb[0].index == FunctionIndex(owner),
            "cold external return did not lazily link exactly one RSB pop to the alias");
#if defined(DYNARMIC_WASM_PROFILE)
    Require(h.profile.link_patches == 2, "cold direct link and lazy RSB link were not both exercised");
#endif

    Run(h, kEntry, kExitA, 2, 0);
    Require(h.stats.blocks == 2 && h.ectx.rsb_top == 0 && h.ectx.rsb[0].index == FunctionIndex(owner),
            "warm linked call/return recompiled or performed extra RSB pops");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    CheckTransfers(h, {Caller(1, kEntry, 4, 1), Caller(2, kExternal, 2, 2)},
                   {Direct(1), Rsb(1), Other(0)});
#if defined(DYNARMIC_WASM_PROFILE)
    CheckConservation(h, 4);
#endif
#endif
#if defined(DYNARMIC_WASM_PROFILE)
    Require(h.profile.link_patches == 2, "warm direct link or RSB return missed its linked target");
#endif
}

void TestFastDispatchColdWarm() {
    Harness h{true};
    const BlockFunc owner = Compile(h, kEntry);
    h.ectx.rsb_top = 0;  // Force an RSB mismatch so RET uses fast dispatch.
    Run(h, kExternal, kExitB);
    Require(h.stats.blocks == 2 && h.space.Get(Location(kContinuation)) == owner,
            "cold fast-dispatch return did not resolve the existing alias");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    CheckTransfers(h, {Caller(1, kEntry, 1, 1), Caller(2, kExternal, 1, 2)},
                   {Direct(0), Rsb(0), Other(0)});
#endif
    Require(h.ectx.rsb_top == kRsbMask, "RSB-neutral cold fast-dispatch setup did not perform one empty pop");
#if defined(DYNARMIC_WASM_PROFILE)
    Require(h.profile.fast_dispatch_misses == 1, "cold fast-dispatch miss was not exercised");
#endif
    h.ectx.rsb_top = 0;
    Run(h, kExternal, kExitA, 2, 0);
    Require(h.stats.blocks == 2 && h.ectx.rsb_top == kRsbMask,
            "warm fast-dispatch return recompiled or changed pop behavior");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    CheckTransfers(h, {Caller(1, kEntry, 2, 1), Caller(2, kExternal, 2, 2)},
                   {Direct(0), Rsb(0), Other(1)});
#endif
#if defined(DYNARMIC_WASM_PROFILE)
    Require(h.profile.fast_dispatch_misses == 1, "warm fast-dispatch lookup missed");
#endif
}

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
void TestHaltPendingTransfersExcluded() {
    Harness h{true};
    const BlockFunc owner = Compile(h, kEntry);
    Run(h, kEntry, kExitB);
    Run(h, kEntry, kExitA, 2, 0);
    const BlockFunc external = h.space.Get(Location(kExternal));
    Require(external != nullptr, "missing halt fixture callee");

    Prepare(h, kEntry);
    h.ectx.halt_reason = static_cast<u32>(HaltReason::UserDefined4);
    Require(owner(reinterpret_cast<uptr>(&h.state), reinterpret_cast<uptr>(&h.ectx)) == 0 &&
                h.state.pc == kExternal, "halt-pending direct edge did not return to dispatch");
    Prepare(h, kExternal);
    h.ectx.halt_reason = static_cast<u32>(HaltReason::UserDefined4);
    Require(external(reinterpret_cast<uptr>(&h.state), reinterpret_cast<uptr>(&h.ectx)) == 0 &&
                h.state.pc == kContinuation, "halt-pending RSB edge did not return to dispatch");
    h.ectx.rsb_top = 0;
    Prepare(h, kExternal);
    h.ectx.halt_reason = static_cast<u32>(HaltReason::UserDefined4);
    Require(external(reinterpret_cast<uptr>(&h.state), reinterpret_cast<uptr>(&h.ectx)) == 0 &&
                h.state.pc == kContinuation, "halt-pending dispatch edge did not return");
    Publish(h);
    CheckTransfers(h, {Caller(1, kEntry, 5, 1), Caller(2, kExternal, 4, 2)},
                   {Direct(1), Rsb(1), Other(0)});
}

void TestInternalEdgesAndReturnExcluded() {
    Harness h{true};
    Run(h, kInternalEntry, kInternalContinuation);
    Require(h.stats.blocks == 1, "internal call fixture compiled an external region");
    CheckTransfers(h, {Caller(1, kInternalEntry, 1, 2)},
                   {Exit(kInternalCallee, RegionTransferKind::RsbReturn, 0, 0),
                    Exit(kInternalCallee, RegionTransferKind::OtherTailTransfer, 0, 0)});
}

void TestHighPcDynamicTargets() {
    Harness h{true};
    Run(h, kHighCallSite, kHighContinuation);
    Run(h, kHighCallSite, kHighContinuation);
    Require(h.stats.blocks == 3, "high-PC fixture did not use its separate continuation");
    const u64 caller = Location(kHighCallSite).Value();
    const u64 callee = Location(kHighExternal).Value();
    const u64 continuation = Location(kHighContinuation).Value();
    CheckTransfers(h,
        {{1, caller, kHighCallSite, 2, 1}, {2, callee, kHighExternal, 2, 2},
         {3, continuation, kHighContinuation, 2, 0}},
        {Exit(caller, RegionTransferKind::ExternalDirectCall, callee, 1, callee, kHighExternal),
         Exit(callee, RegionTransferKind::RsbReturn, 0, 1, continuation, kHighContinuation),
         Exit(callee, RegionTransferKind::OtherTailTransfer, 0, 0)});
}

void TestTransferSnapshotsPerJit() {
    Harness first{true}, second{true};
    Require(first.instance_id != second.instance_id, "JITs share a diagnostic instance");
    Run(first, kEntry, kExitB);
    CheckTransfers(first, {Caller(1, kEntry, 2, 1), Caller(2, kExternal, 1, 2)},
                   {Direct(0), Rsb(0), Other(0)});
    hold_transfers(first.instance_id);
    Run(second, kEntry, kExitB);
    CheckTransfers(second, {Caller(1, kEntry, 2, 1), Caller(2, kExternal, 1, 2)},
                   {Direct(0), Rsb(0), Other(0)});
    check_held_transfers(first.instance_id, 1);
    Run(second, kEntry, kExitA, 2, 0);
    CheckTransfers(second, {Caller(1, kEntry, 4, 1), Caller(2, kExternal, 2, 2)},
                   {Direct(1), Rsb(1), Other(0)});
    hold_transfers(second.instance_id);
    Run(first, kEntry, kExitA, 2, 0);
    CheckTransfers(first, {Caller(1, kEntry, 4, 1), Caller(2, kExternal, 2, 2)},
                   {Direct(1), Rsb(1), Other(0)});
    check_held_transfers(first.instance_id, 0);
    check_held_transfers(second.instance_id, 1);
}
#endif

void TestConflictingRegionsPreserveDescriptors() {
    Harness existing_owner{true};
    const BlockFunc continuation = Compile(existing_owner, kContinuation);
    Compile(existing_owner, kEntry);
    Require(existing_owner.stats.blocks == 2 && existing_owner.space.Get(Location(kContinuation)) == continuation,
            "fallback compilation overwrote a pre-existing continuation owner");

    Harness existing_alias{true};
    const BlockFunc first = Compile(existing_alias, kEntry);
    Compile(existing_alias, kPredecessor);
    Require(existing_alias.stats.blocks == 2 && existing_alias.space.Get(Location(kContinuation)) == first &&
                existing_alias.space.Get(Location(kEntry)) == first,
            "later conflicting region replaced an existing alias");
}

void TestInvalidationAndMutation() {
    Harness h{true};
    const BlockFunc unrelated = Compile(h, kUnrelated);
    Compile(h, kEntry);
    Invalidate(h, kContinuation);
    Require(h.space.Get(Location(kEntry)) == nullptr && h.space.Get(Location(kContinuation)) == nullptr &&
                h.space.Get(Location(kUnrelated)) == unrelated,
            "alias-range invalidation did not drop all aliases or dropped an unrelated owner");

    h.callbacks.code[kA] = Subs(7, 0, 1);
    const BlockFunc recompiled = Compile(h, kEntry);
    Require(h.stats.blocks == 3 && h.space.Get(Location(kContinuation)) == recompiled,
            "alias was not restored by the single recompilation");
    Run(h, kEntry, kExitA, 1, 1);
    Require(h.state.reg[0] == 1 && h.state.reg[7] == 0, "guest mutation did not take effect after alias invalidation");

    h.callbacks.code[kA] = Subs(0, 0, 1);
    Invalidate(h, kEntry);
    Require(h.space.Get(Location(kEntry)) == nullptr && h.space.Get(Location(kContinuation)) == nullptr &&
                h.space.Get(Location(kUnrelated)) == unrelated,
            "owner-range invalidation did not drop every alias or preserve unrelated code");
    Compile(h, kEntry);
    Run(h, kEntry, kExitA, 1, 1);
    Require(h.state.reg[0] == 0, "owner-range recompilation retained stale guest code");
}

void TestWarmTransferInvalidation() {
    Harness h{true};
    Compile(h, kEntry);
    Run(h, kEntry, kExitB);
    Run(h, kEntry, kExitA, 2, 0);
    Require(h.stats.blocks == 2 && h.ectx.rsb[0].index != 0,
            "warm direct-link/RSB setup was not exercised");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    CheckTransfers(h, {Caller(1, kEntry, 4, 1), Caller(2, kExternal, 2, 2)},
                   {Direct(1), Rsb(1), Other(0)});
    hold_transfers(h.instance_id);
#endif

    h.callbacks.code[kA] = Subs(7, 0, 1);
    Invalidate(h, kContinuation);
    Require(h.ectx.rsb[0].index == 0, "invalidation retained a patched RSB target");
    Run(h, kEntry, kExitA, 1, 1);
    Require(h.stats.blocks == 3 && h.state.reg[0] == 1 && h.state.reg[7] == 0,
            "recompiled direct-link/RSB path executed stale code or recompiled the callee");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    CheckTransfers(h,
        {Caller(1, kEntry, 4, 1), Caller(2, kExternal, 3, 2), Caller(3, kEntry, 2, 1)},
        {Direct(1), Rsb(1), Other(0), Direct(0)});
#endif

    Run(h, kEntry, kExitA, 1, 1);
    Invalidate(h, kEntry);
    h.callbacks.code[kA] = Subs(0, 0, 1);
    h.ectx.rsb_top = 0;  // Force RET through fast dispatch rather than the RSB.
    Run(h, kExternal, kExitA, 1, 1);
    Require(h.stats.blocks == 4 && h.state.reg[0] == 0,
            "invalidated fast-dispatch path executed a stale generation");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    CheckTransfers(h,
        {Caller(1, kEntry, 4, 1), Caller(2, kExternal, 5, 2),
         Caller(3, kEntry, 4, 1), Caller(4, kContinuation, 1, 0)},
        {Direct(1), Rsb(2), Other(0), Direct(1)});
    check_held_transfers(h.instance_id, 0);
#endif
#if defined(DYNARMIC_WASM_PROFILE)
    Require(h.profile.fast_dispatch_misses == 1,
            "fast dispatch was not cleared before recompilation");
#endif
}

void TestStepDescriptorSeparation() {
    Harness h{true};
    const BlockFunc owner = Compile(h, kEntry);
    const HaltReason reason = Step(h, kContinuation);
    Require((static_cast<u32>(reason) & static_cast<u32>(HaltReason::Step)) != 0 && h.state.pc == kA,
            "single-step continuation did not execute exactly its own instruction");
    Require(h.stats.blocks == 2 && h.space.Get(Location(kContinuation)) == owner &&
                h.space.Get(Location(kContinuation, true)) != nullptr,
            "step descriptor collided with or replaced the normal alias");
}

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
void TestStepHasNoTransfers() {
    Harness h{true};
    const HaltReason call = Step(h, kCallSite);
    Require((static_cast<u32>(call) & static_cast<u32>(HaltReason::Step)) != 0 &&
                h.state.pc == kExternal, "single-step BL did not stop at its target");
    const HaltReason ret = Step(h, kExternal);
    Require((static_cast<u32>(ret) & static_cast<u32>(HaltReason::Step)) != 0 &&
                h.state.pc == kContinuation, "single-step RET did not stop at its target");
    CheckTransfers(h,
        {{1, Location(kCallSite, true).Value(), kCallSite, 1, 0},
         {2, Location(kExternal, true).Value(), kExternal, 1, 0}}, {});
}
#endif

void TestClearAndDestructionRelease() {
    Harness clear{true};
    const BlockFunc cleared = Compile(clear, kEntry);
    clear.space.ClearCache();
    Require(!table_entry_live(reinterpret_cast<uptr>(cleared)), "clear left the shared generated entry live");
    Require(clear.space.Get(Location(kEntry)) == nullptr && clear.space.Get(Location(kContinuation)) == nullptr,
            "clear retained owner or alias lookup");
    const BlockFunc first_after_clear = Compile(clear, kEntry);
    const BlockFunc second_after_clear = Compile(clear, kUnrelated);
    Require(first_after_clear != second_after_clear &&
                table_entry_live(reinterpret_cast<uptr>(first_after_clear)) &&
                table_entry_live(reinterpret_cast<uptr>(second_after_clear)),
            "clear duplicated a free table slot or released a repopulated entry");

    BlockFunc destroyed = nullptr;
    {
        Harness scoped{true};
        destroyed = Compile(scoped, kEntry);
        Require(table_entry_live(reinterpret_cast<uptr>(destroyed)), "new generated entry is not live");
    }
    Require(!table_entry_live(reinterpret_cast<uptr>(destroyed)), "destruction left the shared generated entry live");

    Harness repopulated{true};
    const BlockFunc first = Compile(repopulated, kEntry);
    const BlockFunc second = Compile(repopulated, kUnrelated);
    Require(first != second && table_entry_live(reinterpret_cast<uptr>(first)) &&
                table_entry_live(reinterpret_cast<uptr>(second)),
            "destruction duplicated a free table slot or released a repopulated entry");
}

}  // namespace

int main() {
    check_table_free_unique();
    TestDefaultPublicationDisabled();
    check_table_free_unique();
    TestOwnerAliasAndColdWarmCall();
    check_table_free_unique();
    TestFastDispatchColdWarm();
    check_table_free_unique();
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    TestHaltPendingTransfersExcluded();
    check_table_free_unique();
    TestInternalEdgesAndReturnExcluded();
    check_table_free_unique();
    TestHighPcDynamicTargets();
    check_table_free_unique();
    TestTransferSnapshotsPerJit();
    check_table_free_unique();
#endif
    TestConflictingRegionsPreserveDescriptors();
    check_table_free_unique();
    TestInvalidationAndMutation();
    check_table_free_unique();
    TestWarmTransferInvalidation();
    check_table_free_unique();
    TestStepDescriptorSeparation();
    check_table_free_unique();
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    TestStepHasNoTransfers();
    check_table_free_unique();
#endif
    TestClearAndDestructionRelease();
    check_table_free_unique();
    std::puts("PASS A64 default alias opt-out and opt-in dispatch, invalidation and lifetime");
}
