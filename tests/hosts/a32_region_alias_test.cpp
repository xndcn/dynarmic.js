// A32 shared-region correctness and a bounded resume/throughput benchmark.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <boost/icl/interval_set.hpp>
#include <emscripten.h>

#include "dynarmic/backend/wasm/a32_address_space.h"
#include "dynarmic/backend/wasm/a32_interface.h"
#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/core.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;
namespace {
constexpr u32 base = 0x1000;
constexpr u32 ram_size = 1 << 20;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL A32 aliases: %s\n", message); std::exit(1); }
}
struct Callbacks final : A32::UserCallbacks {
    std::vector<u8> ram = std::vector<u8>(ram_size);
    ExecutionContext* context = nullptr;
    bool split = false;
    u64 ticks = 0, budget = 1000000000;
    template<typename T> T Read(u32 address) {
        Require(address <= ram_size - sizeof(T), "read outside fixture");
        T value; std::memcpy(&value, ram.data() + address, sizeof(T)); return value;
    }
    template<typename T> void Write(u32 address, T value) {
        Require(address <= ram_size - sizeof(T), "write outside fixture");
        std::memcpy(ram.data() + address, &value, sizeof(T));
    }
    std::optional<u32> MemoryReadCode(u32 address) override { return Read<u32>(address); }
    bool PreCodeReadHook(bool, u32 pc, A32::IREmitter& ir) override {
        if (split && pc != A32::LocationDescriptor{ir.block.Location()}.PC()) {
            ir.SetTerm(IR::Term::LinkBlock{ir.current_location}); return false;
        }
        return true;
    }
    u8 MemoryRead8(u32 a) override { return Read<u8>(a); }
    u16 MemoryRead16(u32 a) override { return Read<u16>(a); }
    u32 MemoryRead32(u32 a) override { return Read<u32>(a); }
    u64 MemoryRead64(u32 a) override { return Read<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { Write(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { Write(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { Write(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { Write(a, v); }
    void InterpreterFallback(u32, size_t) override { Require(false, "interpreter fallback"); }
    void ExceptionRaised(u32, A32::Exception) override { Require(false, "guest exception"); }
    void CallSVC(u32) override { context->halt_reason = static_cast<u32>(HaltReason::UserDefined1); }
    void AddTicks(u64 n) override { ticks += n; }
    u64 GetTicksRemaining() override {
        if (ticks >= budget) context->halt_reason = static_cast<u32>(HaltReason::UserDefined2);
        return ticks >= budget ? 0 : budget - ticks;
    }
};
struct Harness {
    Callbacks callbacks;
    std::unique_ptr<std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> pages;
    A32::UserConfig conf;
    BoundaryState boundary{};
    CompileStats stats;
    A32JitState state{};
    ExecutionContext context{};
    std::unique_ptr<A32AddressSpace> space;
    std::unique_ptr<Core<A32::UserConfig, A32JitState>> core;
    Harness(bool aliases = true, int mode = 0, bool cycles = true, bool board = true) {
        conf.callbacks = &callbacks;
        conf.enable_cycle_counting = cycles;
        if (aliases) conf.optimizations |= OptimizationFlag::WasmRegionMultiEntry;
        if (mode) {
            pages = std::make_unique<std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
            for (u32 p = 0; p < ram_size / 4096; ++p) (*pages)[p] = callbacks.ram.data() + p * 4096;
            conf.page_table = pages.get();
            if (mode == 2) conf.fastmem_pointer = reinterpret_cast<uptr>(callbacks.ram.data());
        }
        space = std::make_unique<A32AddressSpace>(conf, nullptr, &stats, nullptr,
                                                 board ? &boundary : nullptr);
        core = std::make_unique<Core<A32::UserConfig, A32JitState>>(conf, nullptr, board && cycles);
        context.callback_config = &conf;
        callbacks.context = &context;
        state.SetCpsr(0x1f);
    }
    void Run(u32 pc, u64 budget = 1000000000) {
        state.regs[15] = pc;
        context.halt_reason = 0;
        callbacks.ticks = 0;
        callbacks.budget = budget;
        core->Run(*space, state, context);
    }
    void Chain(bool thumb, u32 count = 32) {
        const u32 stride = thumb ? 4 : 8;
        for (u32 i = 0; i < count; ++i) {
            if (thumb) { // adds r0,#1; b next
                callbacks.Write<u16>(base + i * stride, 0x3001);
                callbacks.Write<u16>(base + i * stride + 2, 0xe7ff);
            } else { // add r0,r0,#1; b next
                callbacks.Write<u32>(base + i * stride, 0xe2800001);
                callbacks.Write<u32>(base + i * stride + 4, 0xeaffffff);
            }
        }
        if (thumb) callbacks.Write<u16>(base + count * stride, 0xdf00);
        else callbacks.Write<u32>(base + count * stride, 0xef000000);
        state.SetCpsr(thumb ? 0x3f : 0x1f);
    }
    void Invalidate(u32 pc, u32 size) {
        boost::icl::interval_set<u64> ranges;
        ranges.add(boost::icl::discrete_interval<u64>::closed(pc, pc + size - 1));
        space->InvalidateRanges(ranges);
    }
};
void CheckChain(bool thumb, int mode, bool boundaries) {
    Harness h(true, mode);
    h.Chain(thumb, 96); // Crosses the 64-block cap and exercises patched chains.
    h.boundary.flags = boundaries ? 2 : 0;
    h.Run(base);
    Require(h.state.regs[0] == 96 && h.callbacks.ticks == 193, "initial chain result/ticks");
    const auto compiled = h.stats.blocks;
    const u32 stride = thumb ? 4 : 8;
    for (u32 i = 0; i < 96; ++i) {
        h.state.regs[0] = 0;
        h.Run(base + i * stride);
        Require(h.state.regs[0] == 96 - i, "interior result");
        Require(h.callbacks.ticks == 2 * (96 - i) + 1, "interior ticks");
        Require(h.stats.blocks == compiled, "interior recompiled");
    }
    const u32 changed = base + 70 * stride;
    if (thumb) h.callbacks.Write<u16>(changed, 0x3003);
    else h.callbacks.Write<u32>(changed, 0xe2800003);
    h.Invalidate(changed, thumb ? 2 : 4);
    h.state.regs[0] = 0; h.Run(base);
    Require(h.state.regs[0] == 98 && h.stats.blocks > compiled, "invalidate and relink");
    h.space->ClearCache();
    h.state.regs[0] = 0; h.Run(changed);
    Require(h.state.regs[0] == 28, "clear and recompile interior");
}
void CheckDescriptor() {
    Harness h;
    h.Chain(false);
    h.Run(base);
    h.state.regs[15] = base + 8;
    const auto loc = h.state.GetLocationDescriptor();
    const auto fn = h.space->Get(loc);
    Require(fn != nullptr, "missing descriptor alias");
    // A stale function pointer must reject same-PC Thumb, E, IT and FPSCR variants.
    for (u32 bit : {1u, 2u, 0x100u, 1u << 22}) {
        h.state.upper_location_descriptor = bit;
        h.state.regs[0] = 123;
        h.context.cycles_remaining = 100;
        Require(fn(reinterpret_cast<uptr>(&h.state), reinterpret_cast<uptr>(&h.context)) == 0,
                "descriptor mismatch return");
        Require(h.state.regs[0] == 123 && h.state.regs[15] == base + 8 && h.context.cycles_remaining == 100,
                "descriptor mismatch executed guest code");
    }
    h.state.SetCpsr(0x1f); h.state.SetFpscr(1u << 22); h.state.regs[0] = 0;
    const auto before = h.stats.blocks;
    h.Run(base + 8);
    Require(h.state.regs[0] == 31 && h.stats.blocks > before, "FPSCR variant");
    h.state.SetFpscr(0); h.state.regs[0] = 0;
    const auto after = h.stats.blocks; h.Run(base + 8);
    Require(h.state.regs[0] == 31 && h.stats.blocks == after, "FPSCR original reuse");
}
void CheckIT(u32 input) {
    Harness h;
    h.callbacks.split = true;
    // cmp r1,#0; ite eq; addeq r0,#1; addne r0,#2; adds r2,#1; svc #0
    constexpr std::array<u16, 6> code{0x2900, 0xbf0c, 0x3001, 0x3002, 0x3201, 0xdf00};
    std::memcpy(h.callbacks.ram.data() + base, code.data(), sizeof(code));
    h.state.SetCpsr(0x3f); h.state.regs[1] = input;
    h.Run(base, 2);
    Require(h.state.regs[15] == base + 4 && (h.state.Cpsr() & 0x0600fc00) != 0, "IT boundary");
    const auto before = h.stats.blocks;
    h.Run(base + 4);
    Require(h.state.regs[0] == (input ? 2u : 1u) && h.state.regs[2] == 1, "IT resumed result");
    Require(h.stats.blocks == before && (h.state.Cpsr() & 0x0600fc00) == 0, "IT alias/exit state");
}
void CheckEndian(int mode) {
    Harness h(true, mode);
    h.callbacks.Write<u32>(base, 0xeaffffff); // b next
    h.callbacks.Write<u32>(base + 4, 0xe5910000); // ldr r0,[r1]
    h.callbacks.Write<u32>(base + 8, 0xef000000);
    h.callbacks.Write<u32>(0x8000, 0x12345678);
    for (bool big : {false, true, false}) {
        h.state.SetCpsr(big ? 0x21f : 0x1f); h.state.regs[1] = 0x8000;
        h.Run(base);
        const auto before = h.stats.blocks;
        h.Run(base + 4);
        Require(h.state.regs[0] == (big ? 0x78563412u : 0x12345678u), "endian load");
        Require(h.stats.blocks == before, "endian interior recompiled");
    }
}
void CheckOptInAndStep() {
    A32::UserConfig defaults;
    Require(!defaults.HasOptimization(OptimizationFlag::WasmRegionMultiEntry), "sharing enabled by default");
    defaults.optimizations |= OptimizationFlag::WasmRegionMultiEntry | OptimizationFlag::Unsafe_UnfuseFMA;
    Require(defaults.HasOptimization(OptimizationFlag::WasmRegionMultiEntry) &&
                !defaults.HasOptimization(OptimizationFlag::Unsafe_UnfuseFMA), "safe opt-in requires unsafe optimizations");
    defaults.optimizations = all_safe_optimizations;
    Require(defaults.HasOptimization(OptimizationFlag::WasmRegionMultiEntry), "all safe flags omit region sharing");
    defaults.optimizations = no_optimizations;
    Require(!defaults.HasOptimization(OptimizationFlag::WasmRegionMultiEntry), "no optimizations must disable sharing");
    for (const auto [aliases, cycles, board] : {std::array{false,true,true}, std::array{true,false,true}, std::array{true,true,false}}) {
        Harness h(aliases, 0, cycles, board); h.Chain(false);
        h.space->GetOrEmit(IR::LocationDescriptor{base});
        Require(h.space->Get(IR::LocationDescriptor{base + 8}) == nullptr, "sharing enabled without opt-in prerequisites");
    }
    Harness h; h.Chain(false);
    const auto stepped = A32::LocationDescriptor{IR::LocationDescriptor{base}}.SetSingleStepping(true);
    h.space->GetOrEmit(stepped);
    Require(h.space->Get(IR::LocationDescriptor{base + 8}) == nullptr, "step published aliases");
    // Also exercise construction through the embedder API and a public Step.
    auto jit = CreateA32Jit(h.conf, &h.boundary);
    jit->SetCpsr(0x1f); jit->Regs()[15] = base;
    jit->Step();
    Require(jit->Regs()[0] == 1 && jit->Regs()[15] == base + 4, "public Step with sharing");
}
void CheckDisabledHookFilter() {
    Harness h; h.Chain(false, 8);
    std::vector<u8> filter(65536, 6);
    h.boundary.hook_slots = static_cast<u32>(reinterpret_cast<uptr>(filter.data()));
    u32 observed = 0;
    h.core = std::make_unique<Core<A32::UserConfig, A32JitState>>(h.conf, nullptr, true,
        BoundaryObserver{[](void* p) { ++*static_cast<u32*>(p); }, &observed});
    h.Run(base);
    Require(observed == 1 && h.state.regs[0] == 8 && h.callbacks.ticks == 17,
            "disabled filter must not force native boundaries");
}
void CheckBackwardAndReturn() {
    const auto branch = [](u32 from, u32 to, bool link = false) {
        return (link ? 0xeb000000u : 0xea000000u) | (((to - from - 8) >> 2) & 0xffffff);
    };
    Harness h;
    h.callbacks.Write<u32>(base, 0xe2800004);
    h.callbacks.Write<u32>(base + 4, 0xef000000);
    h.callbacks.Write<u32>(base + 8, 0xe2800002);
    h.callbacks.Write<u32>(base + 12, branch(base + 12, base));
    h.callbacks.Write<u32>(base + 16, 0xe2800001);
    h.callbacks.Write<u32>(base + 20, branch(base + 20, base + 8));
    h.Run(base + 16);
    Require(h.state.regs[0] == 7, "nonzero entry index");
    const auto before = h.stats.blocks;
    h.state.regs[0] = 0; h.Run(base + 8);
    Require(h.state.regs[0] == 6 && h.stats.blocks == before, "backward interior entry");
    h.space->ClearCache();
    h.callbacks.Write<u32>(base, branch(base, 0x8000, true));
    h.callbacks.Write<u32>(base + 4, 0xe2800007);
    h.callbacks.Write<u32>(base + 8, 0xef000000);
    h.callbacks.Write<u32>(0x8000, 0xe3a03009);
    h.callbacks.Write<u32>(0x8004, 0xe12fff1e);
    for (int i = 0; i < 2; ++i) {
        h.state.regs[0] = 0; h.Run(base);
        Require(h.state.regs[0] == 7 && h.state.regs[3] == 9, "external call/return");
        Require(h.space->Get(IR::LocationDescriptor{base}) == h.space->Get(IR::LocationDescriptor{base + 4}),
                "return continuation must share the caller allocation");
    }
}
void Benchmark(bool aliases, bool thumb, bool boundaries) {
    Harness h(aliases);
    constexpr u32 count = 48;
    h.Chain(thumb, count); h.boundary.flags = boundaries ? 2 : 0;
    for (u32 sample = 0; sample < 9; ++sample) {
        const auto before = h.stats;
        const double start = emscripten_get_now();
        const u32 repeats = sample == 0 ? 1 : 1000;
        for (u32 r = 0; r < repeats; ++r) for (u32 i = 0; i < count; ++i) {
            h.state.regs[0] = 0;
            h.Run(base + i * (thumb ? 4 : 8));
            Require(h.state.regs[0] == count - i && h.callbacks.ticks == 2 * (count - i) + 1,
                    "benchmark result/ticks");
        }
        std::printf("REGION_SAMPLE {\"sample\":%u,\"ms\":%.9f,\"blocks\":%.0f,\"bytes\":%.0f,\"instructions\":%u}\n",
                    sample, emscripten_get_now() - start, h.stats.blocks - before.blocks,
                    h.stats.bytes - before.bytes, repeats * count * (count + 2));
        Require(sample == 0 || h.stats.blocks == before.blocks, "warm compilation");
    }
}
void CheckRegisterExits() {
    Harness h, reference(false, 0, false, false);
    std::vector<u32> code;
    for (u32 repeat = 0; repeat < 8; ++repeat) {
        for (u32 reg = 0; reg < 13; ++reg)
            code.push_back(0xe2800000 | reg << 16 | reg << 12 | (reg + 1));
        code.insert(code.end(), {0xe150000e, 0x1a000000, 0xe2822005}); // cmp r0,lr; bne skip; add r2,#5
    }
    const u32 branch_pc = base + static_cast<u32>(code.size()) * 4;
    code.push_back(0xea000000 | (((base - branch_pc - 8) >> 2) & 0xffffff));
    for (Harness* backend : {&h, &reference}) {
        std::memcpy(backend->callbacks.ram.data() + base, code.data(), code.size() * 4);
        backend->state.regs[14] = 3;
    }
    auto step = CreateA32Jit(reference.conf, nullptr);
    step->SetCpsr(h.state.Cpsr()); step->Regs() = h.state.regs; step->Regs()[15] = base;
    for (const u64 budget : {1, 17, 33, 63, 7, 111, 513}) {
        h.Run(h.state.regs[15] ? h.state.regs[15] : base, budget);
        Require(h.callbacks.ticks >= budget, "register-exit budget not charged");
        for (u64 i = 0; i < h.callbacks.ticks; ++i) step->Step();
        Require(h.state.regs == step->Regs() && h.state.Cpsr() == step->Cpsr(),
                "shared exit differs from instruction-by-instruction registers/flags");
    }
}
void CheckNestedCalls() {
    // push {r4,lr}; mov r4,r1; bl inner; add r0,r4; pop {r4,pc};
    // inner: push {lr}; blx r3; adds r0,#1; pop {pc}
    constexpr std::array<u32, 9> caller{0xe92d4010,0xe1a04001,0xeb000001,0xe0800004,
        0xe8bd8010,0xe52de004,0xe12fff33,0xe2900001,0xe49df004};
    for (const bool board : {false, true}) {
        Harness h(true, 0, true, board);
        std::memcpy(h.callbacks.ram.data() + base, caller.data(), sizeof(caller));
        h.callbacks.Write<u32>(0x8000, 0xe2900002); // adds r0,#2
        h.callbacks.Write<u32>(0x8004, 0xe12fff1e); // bx lr
        h.callbacks.Write<u32>(0xf000, 0xef000000);
        h.context.rsb_top = 7;
        for (auto& entry : h.context.rsb) { entry.descriptor = 0xdeadbeefdeadbeefULL; entry.index = 0; }
        for (u32 i = 0; i < 40; ++i) {
            h.state.regs[0] = i; h.state.regs[1] = 7; h.state.regs[3] = 0x8000;
            h.state.regs[4] = 0x12345678; h.state.regs[13] = 0xe000; h.state.regs[14] = 0xf000;
            h.Run(base);
            Require(h.state.regs[0] == i + 10 && h.state.regs[4] == 0x12345678 && h.state.regs[13] == 0xe000,
                    "nested BL/BLX register/stack result");
            if (board) {
                Require(h.context.rsb_top == 7, "board call/return changed RSB top");
                for (const auto& entry : h.context.rsb)
                    Require(entry.descriptor == 0xdeadbeefdeadbeefULL && entry.index == 0, "board call wrote unused RSB");
            }
        }
    }
}
// Literal load and constant store survive frontend optimization because the
// callbacks do not mark the literal page read-only. Run through the real JIT.
void BenchmarkLiteral(int mode) {
    Harness h(true, mode);
    constexpr std::array<u32, 7> code{
        0xe59f20f8, 0xe0800002, 0xe3a01901, 0xe5810000,
        0xe2544001, 0x1afffff9, 0xef000000,
    };
    constexpr u32 repeats = 1 << 18;
    constexpr u32 instructions = repeats * 6 + 1;
    std::memcpy(h.callbacks.ram.data() + base, code.data(), sizeof(code));
    h.callbacks.Write<u32>(base + 0x100, 3);
    for (u32 sample = 0; sample < 9; ++sample) {
        h.state.regs[0] = 0;
        h.state.regs[4] = repeats;
        const auto before = h.stats;
        const double start = emscripten_get_now();
        h.Run(base);
        const double ms = emscripten_get_now() - start;
        Require(h.state.regs[0] == repeats * 3 && h.callbacks.Read<u32>(0x4000) == repeats * 3 &&
                    h.callbacks.ticks == instructions, "literal result/store/ticks");
        std::printf("REGION_SAMPLE {\"sample\":%u,\"ms\":%.9f,\"blocks\":%.0f,\"bytes\":%.0f,\"instructions\":%u}\n",
                    sample, ms, h.stats.blocks - before.blocks, h.stats.bytes - before.bytes, instructions);
        Require(sample == 0 || h.stats.blocks == before.blocks, "warm literal compilation");
    }
}
void BenchmarkLoop(bool aliases, int mode) {
    Harness h(aliases, mode);
    // Same independently checked nested memory workload as jit_bench, with
    // cycle-counted board boundaries enabled for this opt-in comparison.
    constexpr std::array<u32, 13> code{
        0xe3a04e7d, 0xe3a00000, 0xe3a01901, 0xe3a02c01,
        0xe5913000, 0xe2811004, 0xe0800003, 0xe5810400,
        0xe2522001, 0x1afffff9, 0xe2544001, 0x1afffff4, 0xef000000,
    };
    constexpr u32 instructions = 1 + 2000 * (3 + 256 * 6 + 2) + 1;
    std::memcpy(h.callbacks.ram.data() + base, code.data(), sizeof(code));
    for (u32 i = 0; i < 256; ++i) h.callbacks.Write<u32>(0x4000 + i * 4, i);
    for (u32 sample = 0; sample < 9; ++sample) {
        const auto before = h.stats;
        const double start = emscripten_get_now();
        h.Run(base);
        const double ms = emscripten_get_now() - start;
        Require(h.state.regs[0] == 255 * 256 / 2 && h.callbacks.ticks == instructions, "loop sum/ticks");
        for (u32 i = 0; i < 256; ++i) {
            Require(h.callbacks.Read<u32>(0x4000 + i * 4) == i &&
                    h.callbacks.Read<u32>(0x4404 + i * 4) == i * (i + 1) / 2, "loop memory");
        }
        std::printf("REGION_SAMPLE {\"sample\":%u,\"ms\":%.9f,\"blocks\":%.0f,\"bytes\":%.0f,\"instructions\":%u}\n",
                    sample, ms, h.stats.blocks - before.blocks, h.stats.bytes - before.bytes, instructions);
        Require(sample == 0 || h.stats.blocks == before.blocks, "warm loop compilation");
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "literal") == 0) { BenchmarkLiteral(std::atoi(argv[2])); return 0; }
    if (argc == 5) { BenchmarkLoop(std::atoi(argv[1]), std::atoi(argv[2])); return 0; }
    if (argc == 4) { Benchmark(std::atoi(argv[1]), std::atoi(argv[2]), std::atoi(argv[3])); return 0; }
    for (int mode : {0,1,2}) {
        for (bool thumb : {false,true}) for (bool boundaries : {false,true}) CheckChain(thumb, mode, boundaries);
        CheckEndian(mode);
    }
    CheckDescriptor(); CheckIT(0); CheckIT(1); CheckOptInAndStep(); CheckBackwardAndReturn(); CheckDisabledHookFilter();
    CheckRegisterExits(); CheckNestedCalls();
    std::puts("PASS A32 shared entries: ARM/Thumb, memory modes, boundaries, IT/FPSCR/endian, stale selectors, invalidation, links, opt-in and Step");
}
