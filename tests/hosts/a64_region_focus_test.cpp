// Generated-Wasm correctness tests for the private A64 region focus. The
// focused fixture deliberately makes required blocks displace ordinary blocks
// at the 64-block cap; diagnostics are compile-shape evidence only.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/icl/interval_set.hpp>
#include <emscripten.h>

#include "dynarmic/backend/wasm/a64_address_space.h"
#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/a64_region_focus.h"
#include "dynarmic/backend/wasm/compile_stats.h"
#include "dynarmic/backend/wasm/core.h"
#include "dynarmic/backend/wasm/profiler.h"
#include "dynarmic/backend/wasm/region_diagnostics.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;

namespace {

constexpr u64 kEntry = 0x1000;
constexpr u64 kOptional = 0x1004;
constexpr size_t kOptionalBlocks = 80;
constexpr u64 kOptionalEnd = kOptional + kOptionalBlocks * 4;
constexpr u64 kCond = 0x1800;
constexpr u64 kCondExit = 0x1804;
constexpr u64 kCondBrk = 0x1808;
constexpr u64 kCall = 0x1900;
constexpr u64 kReturn = 0x1904;
constexpr u64 kReturnBranch = 0x1908;
constexpr u64 kReturnBrk = 0x190c;
constexpr size_t kRequiredBlocks = 40;
constexpr u64 kRequired = 0x1200;
constexpr u64 kCallee = 0x1a00;
constexpr u64 kFastCallee = 0x9000;
constexpr u64 kFocusedBlockCap = 64;
constexpr u64 kFocusedOptionalBlocks = kFocusedBlockCap - (kRequiredBlocks + 6);

constexpr u32 kBrk = 0xd4200000;
constexpr u32 kRet = 0xd65f03c0;

static_assert(kRequired + (kRequiredBlocks - 1) * 4 - kEntry < 4096);
static_assert(kCallee - kEntry < 4096);
static_assert(kFocusedOptionalBlocks == 18);

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL a64 region focus: %s\n", message);
        std::exit(1);
    }
}

constexpr u32 Branch(u64 from, u64 to) {
    return 0x14000000u | (static_cast<u32>((to - from) >> 2) & 0x03ffffffu);
}
constexpr u32 BranchLink(u64 from, u64 to) {
    return 0x94000000u | (static_cast<u32>((to - from) >> 2) & 0x03ffffffu);
}
constexpr u32 Cbz(u64 from, u64 to, u32 reg) {
    return 0xb4000000u | ((static_cast<u32>((to - from) >> 2) & 0x7ffffu) << 5) | reg;
}
constexpr u32 MovX(u32 dst, u32 immediate) {
    return 0xd2800000u | ((immediate & 0xffffu) << 5) | dst;
}

IR::LocationDescriptor Location(u64 pc, bool single_step = false) {
    return A64::LocationDescriptor{pc, FP::FPCR{}, single_step};
}
IR::LocationDescriptor FpcrLocation(u64 pc) {
    return A64::LocationDescriptor{pc, FP::FPCR{1u << 24}, false};
}
std::unordered_map<u64, u32> MakeCode() {
    std::unordered_map<u64, u32> code;
    code[kEntry] = Cbz(kEntry, kRequired, 1);
    for (size_t i = 0; i < kOptionalBlocks; ++i) {
        const u64 pc = kOptional + i * 4;
        const u64 next = i + 1 == kOptionalBlocks ? kOptionalEnd : pc + 4;
        code[pc] = Branch(pc, next);
    }
    code[kOptionalEnd] = MovX(5, 0x55);
    code[kOptionalEnd + 4] = kBrk;
    for (size_t i = 0; i < kRequiredBlocks; ++i) {
        const u64 pc = kRequired + i * 4;
        code[pc] = Branch(pc, i + 1 == kRequiredBlocks ? kCond : pc + 4);
    }
    code[kCond] = Cbz(kCond, kCall, 0);
    code[kCondExit] = MovX(5, 0x44);
    code[kCondBrk] = kBrk;
    code[kCall] = BranchLink(kCall, kCallee);
    code[kReturn] = MovX(5, 0x22);
    code[kReturnBranch] = Cbz(kReturnBranch, kCond, 2);
    code[kReturnBrk] = kBrk;
    code[kCallee] = MovX(6, 0x33);
    code[kCallee + 4] = kRet;
    code[kFastCallee] = kRet;
    return code;
}

class Callbacks final : public A64::UserCallbacks {
public:
    Callbacks() : code(MakeCode()) {}

    std::unordered_map<u64, u32> code;
    ExecutionContext* ectx = nullptr;
    A64::Jit* jit = nullptr;
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
    void CallSVC(u32) override { Require(false, "unexpected SVC"); }
    void ExceptionRaised(u64 pc, A64::Exception exception) override {
        Require(exception == A64::Exception::Breakpoint, "unexpected guest exception");
        ++exceptions;
        observed_pc = pc;
        if (ectx) {
            ectx->halt_reason = static_cast<u32>(HaltReason::UserDefined4);
        } else {
            jit->HaltExecution(HaltReason::UserDefined4);
        }
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
    A64AddressSpace space;
    A64JitState state{};
    ExecutionContext ectx{};
    Core<A64::UserConfig, A64JitState> core;

    Harness()
            : conf(ConfigFor(callbacks)), space(conf, &profile, &stats, &diagnostics, true), core(conf, &profile) {
        callbacks.ectx = &ectx;
        ectx.callback_config = &conf;
    }
};

void Prepare(Harness& h, u64 pc, u64 x0, u64 x1, u32 fpcr = 0) {
    h.state = {};
    h.state.pc = pc;
    h.state.reg[0] = x0;
    h.state.reg[1] = x1;
    h.state.reg[30] = kReturn;
    h.state.reg[2] = 1;
    h.state.fpcr = fpcr;
    h.ectx.halt_reason = 0;
    h.ectx.link_slot_addr = 0;
    h.ectx.fast_dispatch_entry = 0;
    h.callbacks.observed_pc = 0;
    h.callbacks.exceptions = 0;
}

void Run(Harness& h, u64 pc, u64 expected_brk, u64 expected_x5, u64 x0 = 0, u64 x1 = 0,
         u32 fpcr = 0, u64 expected_x6 = 0x33) {
    Prepare(h, pc, x0, x1, fpcr);
    const HaltReason reason = h.core.Run(h.space, h.state, h.ectx);
    Require(reason == HaltReason::UserDefined4, "guest did not stop at BRK");
    Require(h.callbacks.exceptions == 1 && h.callbacks.observed_pc == expected_brk,
            "wrong guest path or observation count");
    Require(h.state.reg[5] == expected_x5 && h.state.reg[6] == expected_x6,
            "wrong guest execution result");
}

BlockFunc Compile(Harness& h, IR::LocationDescriptor descriptor) {
    BlockFunc fn = h.space.GetOrEmit(descriptor);
    Require(fn != nullptr, "compilation returned no entry");
    return fn;
}

void Invalidate(Harness& h, u64 pc, size_t length = 4) {
    boost::icl::interval_set<u64> ranges;
    ranges.insert(boost::icl::discrete_interval<u64>::closed(pc, pc + length - 1));
    h.space.InvalidateRanges(ranges);
}

std::optional<RegionSelectionFocus> CompiledFocus() {
#if defined(DYNARMIC_JS_A64_REGION_FOCUS)
    return ParseA64RegionFocus(DYNARMIC_JS_A64_REGION_FOCUS);
#else
    return std::nullopt;
#endif
}

bool HasFixtureFocus(const std::optional<RegionSelectionFocus>& focus) {
    return focus && focus->entry == Location(kEntry) && focus->exit_block == Location(kCond) &&
           focus->direct_target == Location(kCall) && focus->return_blocks.size() == 3 &&
           focus->return_blocks[0] == Location(kReturn) && focus->return_blocks[1] == Location(kReturnBrk) &&
           focus->return_blocks[2] == Location(kCallee);
}

bool HasStaleFixtureFocus(const std::optional<RegionSelectionFocus>& focus) {
    return focus && focus->entry == Location(kEntry) && !HasFixtureFocus(focus);
}

#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
std::vector<u64> Descriptors(const Harness& h, size_t region_index) {
    const auto& region = h.diagnostics.Regions().at(region_index);
    const auto begin = h.diagnostics.SelectedDescriptors().begin() + region.block_offset;
    return {begin, begin + region.block_count};
}

std::vector<u64> ExpectedFocusedDescriptors() {
    std::vector<u64> expected{kEntry, kCond, kCall, kReturn, kReturnBrk, kCallee};
    for (size_t i = 0; i < kFocusedOptionalBlocks; ++i) expected.push_back(kOptional + i * 4);
    for (size_t i = 0; i < kRequiredBlocks; ++i) expected.push_back(kRequired + i * 4);
    std::sort(expected.begin(), expected.end());
    return expected;
}

void CheckFocusedRegion(const Harness& h) {
    Require(h.diagnostics.Regions().size() == 1, "focused compilation did not record exactly one region");
    const auto& region = h.diagnostics.Regions()[0];
    Require(region.entry_descriptor == Location(kEntry).Value() && region.block_count == kFocusedBlockCap,
            "wrong focused entry or ordinary cap");
    const auto descriptors = Descriptors(h, 0);
    if (descriptors != ExpectedFocusedDescriptors()) {
        const auto expected = ExpectedFocusedDescriptors();
        for (size_t i = 0; i < std::max(descriptors.size(), expected.size()); ++i) {
            const u64 actual_value = i < descriptors.size() ? descriptors[i] : 0;
            const u64 expected_value = i < expected.size() ? expected[i] : 0;
            if (actual_value != expected_value) {
                std::fprintf(stderr, "focused descriptor[%zu] actual 0x%llx expected 0x%llx\n", i,
                             static_cast<unsigned long long>(actual_value),
                             static_cast<unsigned long long>(expected_value));
            }
        }
    }
    Require(descriptors == ExpectedFocusedDescriptors(),
            "generated Wasm does not contain the exact focused/displaced descriptors");
    Require(std::find(descriptors.begin(), descriptors.end(), Location(kCondExit).Value()) == descriptors.end(),
            "required blocks did not displace the conditional alternative");
    for (size_t i = 0; i < region.block_count; ++i) {
        const size_t at = region.block_offset + i;
        if (!h.diagnostics.EmittedBlocks()[at] || !h.diagnostics.EntryReachableBlocks()[at]) {
            std::fprintf(stderr, "unemitted focused descriptor 0x%llx layout %llu emitted %u reachable %u\n",
                         static_cast<unsigned long long>(h.diagnostics.SelectedDescriptors()[at]),
                         static_cast<unsigned long long>(region.layout), h.diagnostics.EmittedBlocks()[at],
                         h.diagnostics.EntryReachableBlocks()[at]);
        }
        Require(h.diagnostics.EmittedBlocks()[at] && h.diagnostics.EntryReachableBlocks()[at],
                "focused descriptor was selected but not emitted and entry-reachable");
        if (h.diagnostics.SelectedDescriptors()[at] == Location(kReturn).Value()) {
            Require(h.diagnostics.ReturnSites()[at], "focused BL continuation is not an emitted return site");
        }
    }
}

void CheckOrdinaryRegion(const Harness& h, size_t region_index = 0) {
    const auto descriptors = Descriptors(h, region_index);
    Require(descriptors.size() == kFocusedBlockCap, "ordinary control did not use the ordinary region cap");
    Require(std::find(descriptors.begin(), descriptors.end(), Location(kCall).Value()) == descriptors.end() &&
                std::find(descriptors.begin(), descriptors.end(), Location(kReturn).Value()) == descriptors.end(),
            "ordinary control silently selected the deep focused call/return blocks");
}

const RegionTransferExit& TransferExit(const Harness& h, u64 block, RegionTransferKind kind, u64 target) {
    const auto& transfers = h.diagnostics.TransferRegions().at(0);
    Require(transfers != nullptr, "missing focused transfer record");
    for (const auto& exit : transfers->exits) {
        if (exit.exit_block_descriptor == block && exit.kind == static_cast<u64>(kind) &&
            exit.static_target_descriptor == target) {
            return exit;
        }
    }
    Require(false, "newly external focused edge was not recorded");
    std::abort();
}
#else
void CheckFocusedRegion(const Harness&) {}
void CheckOrdinaryRegion(const Harness&, size_t = 0) {}
#endif

void TestFocusedColdWarmConditionalAndDisplacement() {
    Harness h;
    const BlockFunc owner = Compile(h, Location(kEntry));
    CheckFocusedRegion(h);
    Require(owner != nullptr && h.space.Get(Location(kReturn)) == nullptr,
            "structured focused return acquired an unexpected standalone alias");
    Require(h.space.Get(Location(kCall)) == nullptr, "direct_target acquired an implicit standalone alias");

    Run(h, kEntry, kReturnBrk, 0x22);
    Require(h.stats.blocks == 1, "cold focused BL/PushRSB/RET left its required generated-Wasm region");
    Require(h.ectx.rsb_top == 0 && h.ectx.rsb[0].descriptor == Location(kReturn).Value(),
            "focused BL/RET did not push and pop the required return descriptor");
    Run(h, kEntry, kReturnBrk, 0x22);
    Require(h.stats.blocks == 1, "warm focused BL/RET recompiled generated Wasm");

    Run(h, kEntry, kCondBrk, 0x44, 1, 0, 0, 0);
    Require(h.stats.blocks == 2, "displaced conditional alternative did not compile as an external block");
    Run(h, kEntry, kCondBrk, 0x44, 1, 0, 0, 0);
    Run(h, kEntry, kReturnBrk, 0x22);
    Require(h.stats.blocks == 2, "warm direct conditional edges recompiled generated Wasm");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    const auto& displaced = TransferExit(h, kCond, RegionTransferKind::ExternalDirectCall, Location(kCondExit).Value());
    Require(displaced.count == 1, "newly external conditional transfer count is wrong");
#endif

    Run(h, kEntry, kOptionalEnd + 4, 0x55, 0, 1, 0, 0);
    Require(h.stats.blocks > 2, "displaced optional path did not continue through external generated Wasm");
}

void TestFullDescriptorIdentityAndStandaloneTarget() {
    Harness h;
    const BlockFunc owner = Compile(h, Location(kEntry));
    CheckFocusedRegion(h);
    const BlockFunc standalone = Compile(h, Location(kCall));
    Require(standalone != owner && h.stats.blocks == 2,
            "explicit standalone direct_target compilation did not remain independent");
    Run(h, kCall, kReturnBrk, 0x22);
    Require(h.stats.blocks == 2 && h.state.reg[6] == 0x33,
            "standalone direct_target BL/RET execution failed");

    Harness modes;
    const BlockFunc normal = Compile(modes, Location(kEntry));
    const BlockFunc fpcr = Compile(modes, FpcrLocation(kEntry));
    Require(fpcr != normal && modes.stats.blocks == 2, "same-PC/different-FPCR entry reused focused Wasm");
    CheckOrdinaryRegion(modes, 1);
    Run(modes, kEntry, kReturnBrk, 0x22, 0, 0, 1u << 24);

    Harness step;
    const BlockFunc focused = Compile(step, Location(kEntry));
    Prepare(step, kEntry, 0, 0);
    const HaltReason reason = step.core.Step(step.space, step.state, step.ectx, Location(kEntry, true));
    Require((static_cast<u32>(reason) & static_cast<u32>(HaltReason::Step)) != 0 && step.state.pc == kRequired,
            "single-step entry did not execute exactly its own instruction");
    Require(step.stats.blocks == 2 && step.space.Get(Location(kEntry, true)) != focused,
            "single-step descriptor matched or replaced focused Wasm");
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    Require(step.diagnostics.Regions().back().block_count == 1, "single-step descriptor used region focus");
#endif
}

void TestInvalidationAndDependentDispatchState() {
    Harness h;
    Compile(h, Location(kEntry));
    Run(h, kEntry, kReturnBrk, 0x22);
    Run(h, kEntry, kReturnBrk, 0x22);
    Run(h, kEntry, kCondBrk, 0x44, 1, 0, 0, 0);
    Run(h, kEntry, kCondBrk, 0x44, 1, 0, 0, 0);
    Require(h.stats.blocks == 2 && h.ectx.rsb[0].descriptor == Location(kReturn).Value(),
            "warm focused link/RSB fixture was not established");

    h.callbacks.code[kCondExit] = MovX(5, 0x88);
    Invalidate(h, kCondExit);
    Require(h.space.Get(Location(kEntry)) == nullptr && h.space.Get(Location(kCondExit)) == nullptr,
            "overlapping external-edge invalidation retained the focused caller or its target");
    Run(h, kEntry, kCondBrk, 0x88, 1, 0, 0, 0);
    Require(h.stats.blocks == 4, "dependent direct link executed stale displaced code");

    h.callbacks.code[kReturn] = MovX(5, 0x77);
    Invalidate(h, kReturn);
    Require(h.space.Get(Location(kEntry)) == nullptr && h.space.Get(Location(kReturn)) == nullptr,
            "focused invalidation retained its generated-Wasm owner");
    Run(h, kEntry, kReturnBrk, 0x77);
    Run(h, kEntry, kReturnBrk, 0x77);
    Require(h.stats.blocks == 5, "focused recompilation did not execute mutated return code");

    Invalidate(h, kReturn);
    h.callbacks.code[kReturn] = MovX(5, 0x22);
    h.ectx.rsb_top = 0;
    Run(h, kFastCallee, kReturnBrk, 0x22, 0, 0, 0, 0);
    Require(h.stats.blocks == 7, "invalidated fast-dispatch return executed a stale generation");
#if defined(DYNARMIC_WASM_PROFILE)
    Require(h.profile.fast_dispatch_misses == 1, "fast-dispatch state was not cleared before recompilation");
#endif
}

void TestPublicJitIsolation() {
    Callbacks first_callbacks, second_callbacks;
    A64::UserConfig first_conf = ConfigFor(first_callbacks);
    A64::UserConfig second_conf = ConfigFor(second_callbacks);
    A64::Jit first{first_conf}, second{second_conf};
    first_callbacks.jit = &first;
    second_callbacks.jit = &second;

    const auto run = [](A64::Jit& jit, Callbacks& callbacks, u64 expected_x6) {
        jit.ClearHalt(HaltReason::UserDefined4);
        jit.SetPC(kEntry);
        jit.SetRegister(0, 0);
        jit.SetRegister(1, 0);
        jit.SetRegister(30, kReturn);
        jit.SetRegister(2, 1);
        callbacks.observed_pc = 0;
        callbacks.exceptions = 0;
        Require(jit.Run() == HaltReason::UserDefined4 && callbacks.observed_pc == kReturnBrk &&
                    callbacks.exceptions == 1 && jit.GetRegister(5) == 0x22 && jit.GetRegister(6) == expected_x6,
                "public JIT focused execution or instance result is wrong");
    };
    run(first, first_callbacks, 0x33);
    run(second, second_callbacks, 0x33);
    second_callbacks.code[kCallee] = MovX(6, 0x66);
    second.InvalidateCacheRange(kCallee, 8);
    run(first, first_callbacks, 0x33);
    run(second, second_callbacks, 0x66);
    run(first, first_callbacks, 0x33);
}

void TestEmptyOrNonmatchingControl() {
    Harness h;
    Compile(h, Location(kEntry));
    CheckOrdinaryRegion(h);
    Run(h, kEntry, kReturnBrk, 0x22);
    Run(h, kEntry, kCondBrk, 0x44, 1, 0, 0, 0);
    Run(h, kEntry, kOptionalEnd + 4, 0x55, 0, 1, 0, 0);
}

EM_JS(int, live_table_entries, (), {
    let count = 0;
    for (let i = 0; i < wasmTable.length; ++i) if (wasmTable.get(i) !== null) ++count;
    return count;
});

EM_JS(int, published_instance_count, (), {
    return Object.keys(Module.dynarmicWasmStats?.instances || {}).length;
});

void TestStaleFocusFailsWithoutPublication() {
    const int before = live_table_entries();
    const int published_before = published_instance_count();
    Harness h;
    bool threw = false;
    try {
        h.space.GetOrEmit(Location(kEntry));
    } catch (const std::exception& error) {
        threw = std::string{error.what()}.find("direct_target to return") != std::string::npos;
    } catch (...) {
        Require(false, "stale focus threw a non-standard exception");
    }
    Require(threw, "stale focus did not fail explicitly with its required-path diagnostic");
    Require(h.space.Get(Location(kEntry)) == nullptr && h.stats.blocks == 0 &&
                h.diagnostics.Regions().empty() && live_table_entries() == before,
            "stale focus published cache/module/table/diagnostic state or fell back to ordinary Wasm");

    Callbacks callbacks;
    A64::UserConfig conf = ConfigFor(callbacks);
    A64::Jit jit{conf};
    callbacks.jit = &jit;
    jit.SetPC(kEntry);
    bool public_threw = false;
    try {
        jit.Run();
    } catch (const std::exception& error) {
        public_threw = std::string{error.what()}.find("direct_target to return") != std::string::npos;
    }
    Require(public_threw && live_table_entries() == before && published_instance_count() == published_before,
            "public stale-focus Run published stats/diagnostics/table state or used ordinary fallback");
}

}  // namespace

int main() {
    const auto focus = CompiledFocus();
    if (HasStaleFixtureFocus(focus)) {
        TestStaleFocusFailsWithoutPublication();
        std::puts("PASS A64 stale focus fails without generated-Wasm publication");
        return 0;
    }
    if (HasFixtureFocus(focus)) {
        TestFocusedColdWarmConditionalAndDisplacement();
        TestFullDescriptorIdentityAndStandaloneTarget();
        TestInvalidationAndDependentDispatchState();
        TestPublicJitIsolation();
        std::puts("PASS A64 focused generated-Wasm descriptors, dispatch, identity and isolation");
        return 0;
    }
#if defined(DYNARMIC_JS_A64_REGION_FOCUS)
    Require(focus && focus->entry != Location(kEntry), "test build has an unrecognized fixture focus");
#endif
    TestEmptyOrNonmatchingControl();
    TestPublicJitIsolation();
    std::puts("PASS A64 empty/nonmatching focus ordinary generated-Wasm control");
    return 0;
}
