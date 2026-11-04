// A64 all-entry regions: public opt-in, budgets, aliases and bounded benchmarks.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <boost/icl/interval_set.hpp>
#include <emscripten.h>
#include "dynarmic/backend/wasm/a64_address_space.h"
#include "dynarmic/backend/wasm/a64_interface.h"
#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/core.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;
namespace {
constexpr u64 base = 0x1000;
constexpr u64 ram_size = 1 << 20;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL A64 sharing: %s\n", message); std::exit(1); }
}
u32 Branch(u64 from, u64 to, bool link = false) {
    return (link ? 0x94000000u : 0x14000000u) | (static_cast<u32>((to - from) >> 2) & 0x3ffffff);
}
struct Callbacks final : A64::UserCallbacks {
    std::vector<u8> ram = std::vector<u8>(ram_size);
    u64 code_base = base, ticks = 0, budget = 1000000000, svc_ticks = 0;
    ExecutionContext* context = nullptr;
    A64::Jit* public_jit = nullptr;
    bool commit_svc = false;
    template<class T> T Read(u64 a) {
        Require(a <= ram_size - sizeof(T), "fixture read range");
        T v; std::memcpy(&v, ram.data() + a, sizeof(v)); return v;
    }
    template<class T> void Write(u64 a, T v) {
        Require(a <= ram_size - sizeof(T), "fixture write range");
        std::memcpy(ram.data() + a, &v, sizeof(v));
    }
    void Put(u64 pc, u32 word) { Write<u32>(pc >= code_base && pc - code_base < 4096 ? base + pc - code_base : pc, word); }
    std::optional<u32> MemoryReadCode(u64 pc) override {
        const u64 a = pc >= code_base && pc - code_base < 4096 ? base + pc - code_base : pc;
        if (a > ram_size - 4) return std::nullopt;
        return Read<u32>(a);
    }
    void Halt(HaltReason reason) {
        if (public_jit) public_jit->HaltExecution(reason);
        else context->halt_reason |= static_cast<u32>(reason);
    }
    u8 MemoryRead8(u64 a) override { return Read<u8>(a); }
    u16 MemoryRead16(u64 a) override { return Read<u16>(a); }
    u32 MemoryRead32(u64 a) override { return Read<u32>(a); }
    u64 MemoryRead64(u64 a) override { return Read<u64>(a); }
    A64::Vector MemoryRead128(u64 a) override { return Read<A64::Vector>(a); }
    void MemoryWrite8(u64 a, u8 v) override { Write(a, v); }
    void MemoryWrite16(u64 a, u16 v) override { Write(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { Write(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { Write(a, v); }
    void MemoryWrite128(u64 a, A64::Vector v) override { Write(a, v); }
    void InterpreterFallback(u64, size_t) override { Require(false, "interpreter fallback"); }
    void ExceptionRaised(u64, A64::Exception) override { Require(false, "guest exception"); }
    void CallSVC(u32) override {
        if (commit_svc) { CommitA64ExecutionTicks(*public_jit); CommitA64ExecutionTicks(*public_jit); }
        svc_ticks = ticks; Halt(HaltReason::UserDefined1);
    }
    void AddTicks(u64 n) override { ticks += n; }
    u64 GetTicksRemaining() override {
        if (ticks >= budget) { Halt(HaltReason::UserDefined2); return 0; }
        return budget - ticks;
    }
    u64 GetCNTPCT() override { return 0; }
};
struct Harness {
    Callbacks cb;
    A64::UserConfig conf;
    BoundaryState boundary{};
    CompileStats stats;
    A64JitState state{};
    ExecutionContext context{};
    std::vector<void*> pages;
    std::unique_ptr<A64AddressSpace> space;
    std::unique_ptr<Core<A64::UserConfig, A64JitState>> core;
    u32 observations = 0;
    std::vector<u64> observed_pcs;
    // 0: ordinary public default; 1: regions only; 2: regions + all-entry sharing.
    explicit Harness(int policy = 2, int memory = 0, bool cycles = true, bool observe = false) {
        conf.callbacks = &cb; conf.enable_cycle_counting = cycles;
        if (policy == 2) conf.optimizations |= OptimizationFlag::WasmRegionMultiEntry;
        if (memory) {
            pages.resize(1 << (32 - 12));
            for (u32 p = 0; p < ram_size / 4096; ++p) pages[p] = cb.ram.data() + p * 4096;
            conf.page_table = pages.data(); conf.page_table_address_space_bits = 32;
            if (memory == 2) conf.fastmem_pointer = reinterpret_cast<uptr>(cb.ram.data());
        }
        space = std::make_unique<A64AddressSpace>(conf, nullptr, &stats, nullptr, false, policy ? &boundary : nullptr);
        BoundaryObserver observer{[](void* p) {
            auto& h = *static_cast<Harness*>(p); ++h.observations; h.observed_pcs.push_back(h.state.pc);
        }, this};
        core = std::make_unique<Core<A64::UserConfig, A64JitState>>(conf, nullptr, policy && cycles,
                                                               observe ? observer : BoundaryObserver{});
        cb.context = &context; context.callback_config = &conf;
    }
    HaltReason Run(u64 pc, u64 budget = 1000000000) {
        state.pc = pc; context.halt_reason = 0; cb.ticks = 0; cb.budget = budget;
        observations = 0; observed_pcs.clear();
        return core->Run(*space, state, context);
    }
    u64 Ticks() const { // Ordinary dispatch can leave a final, uncommitted allowance at SVC.
        return cb.ticks + static_cast<u64>(context.cycles_to_run - context.cycles_remaining);
    }
    void Chain(u32 count = 48) {
        for (u32 i = 0; i < count; ++i) {
            cb.Put(cb.code_base + i * 8, 0x91000400); // add x0,x0,#1
            cb.Put(cb.code_base + i * 8 + 4, 0x14000001); // b next
        }
        cb.Put(cb.code_base + count * 8, 0xd4000001);
    }
    void Invalidate(u64 pc, u64 size = 4) {
        boost::icl::interval_set<u64> ranges;
        ranges.add(boost::icl::discrete_interval<u64>::closed(pc, pc + size - 1));
        space->InvalidateRanges(ranges);
    }
};
void CheckChain(int memory, bool boundaries, u64 start) {
    Harness h(2, memory); h.cb.code_base = start; h.Chain(96);
    h.boundary.flags = boundaries ? 2 : 0;
    Require(h.Run(start) == HaltReason::UserDefined1 && h.state.reg[0] == 96 && h.Ticks() == 193, "chain result/ticks");
    const auto compiled = h.stats.blocks;
    for (u32 i = 0; i < 96; ++i) {
        h.state.reg[0] = 0; h.Run(start + i * 8);
        Require(h.state.reg[0] == 96 - i && h.Ticks() == 2 * (96 - i) + 1 && h.stats.blocks == compiled,
                "interior result/ticks/reuse");
    }
    h.cb.Put(start + 70 * 8, 0x91000c00); h.Invalidate(start + 70 * 8);
    h.state.reg[0] = 0; h.Run(start);
    Require(h.state.reg[0] == 98 && h.stats.blocks > compiled, "invalidate and relink");
    h.space->ClearCache(); h.state.reg[0] = 0; h.Run(start + 70 * 8);
    Require(h.state.reg[0] == 28, "clear and compile interior");
}
void CheckDescriptorAndFpcr() {
    Harness h; h.Chain(); h.Run(base);
    h.state.pc = base + 8;
    const auto fn = h.space->Get(h.state.GetLocationDescriptor());
    Require(fn != nullptr, "missing alias");
    for (u64 pc : {base + 1, base + 4, base + 8 + (1ULL << 32)}) {
        h.state.pc = pc; h.context.cycles_remaining = 100; const auto saved = h.state;
        Require(fn(reinterpret_cast<uptr>(&h.state), reinterpret_cast<uptr>(&h.context)) == 0 &&
                    std::memcmp(&saved, &h.state, sizeof(saved)) == 0 && h.context.cycles_remaining == 100,
                "unknown full PC must not execute");
    }
    h.state.pc = base + 8;
    for (u32 mode : {1u << 22, 1u << 24, 1u << 26}) {
        h.state.fpcr = mode; const auto saved = h.state;
        Require(fn(reinterpret_cast<uptr>(&h.state), reinterpret_cast<uptr>(&h.context)) == 0 &&
                    std::memcmp(&saved, &h.state, sizeof(saved)) == 0 && h.context.cycles_remaining == 100,
                "same-PC FPCR mismatch must not execute");
    }
    // FPCR.AHP puts a mode bit at descriptor bit 63; keep its aliases distinct
    // and searchable even though signed descriptor order would differ.
    h.state.fpcr = 1u << 26; h.state.reg[0] = 0; h.Run(base + 8);
    const auto modes = h.stats.blocks;
    h.state.reg[0] = 0; h.Run(base + 16);
    Require(h.state.reg[0] == 46 && h.stats.blocks == modes, "high-bit FPCR descriptor aliases");
    Harness fp;
    fp.cb.Put(base, 0x14000001); fp.cb.Put(base + 4, 0x1e27c020); // frinti s0,s1
    fp.cb.Put(base + 8, 0xd4000001);
    for (u32 mode : {0u, 1u << 22, 0u}) {
        fp.state.fpcr = mode; fp.state.vec[2] = 0x3fa00000; // 1.25f
        fp.Run(base);
        const auto before = fp.stats.blocks;
        fp.Run(base + 4);
        Require(fp.state.vec[0] == (mode ? 0x40000000u : 0x3f800000u) && fp.stats.blocks == before,
                "FPCR rounding mode and alias reuse");
    }
    Require(fp.stats.blocks == 2, "FPCR variants must have distinct owners");
}
void CheckBudgetsAndHooks() {
    Harness h(2, 0, true, true); h.Chain(8);
    std::vector<u8> filter(65536, 6);
    h.boundary.hook_slots = static_cast<u32>(reinterpret_cast<uptr>(filter.data()));
    h.Run(base);
    Require(h.observations == 1 && h.Ticks() == 17, "disabled hook filter must not force boundaries");
    h.boundary.instruction_hooks = 1;
    std::fill(filter.begin(), filter.end(), 0);
    filter[((base + 3 * 8) >> 1) & 0xffff] = 2;
    h.state.reg[0] = 0; h.Run(base);
    Require(h.observations == 2 && h.observed_pcs[0] == base + 3 * 8 && h.state.reg[0] == 8, "static slot hook boundary");
    h.boundary.instruction_hooks = 2;
    h.state.reg[0] = 0; h.Run(base);
    Require(h.observations == 9 && h.Ticks() == 17, "global hook boundaries");
    h.boundary.instruction_hooks = 0; h.state.reg[0] = 0;
    Require(h.Run(base, 4) == HaltReason::UserDefined2 && h.Ticks() == 4 && h.state.pc == base + 16 && h.state.reg[0] == 2,
            "budget boundary PC and exact ticks");
    const auto compiled = h.stats.blocks;
    h.Run(h.state.pc);
    Require(h.state.reg[0] == 8 && h.Ticks() == 13 && h.stats.blocks == compiled, "budget resume alias");
    h.boundary.flags = kBoundaryDisableRegions; h.space->ClearCache(); h.state.reg[0] = 0; h.Run(base);
    Require(h.state.reg[0] == 8 && h.observations == 9, "disable regions at runtime");
}
void CheckReturnAndPruning() {
    Harness h(2, 0, true, true); h.Chain(96);
    h.Run(base + 48 * 8);
    const auto owner = h.space->Get(IR::LocationDescriptor{base + 48 * 8});
    h.state.reg[0] = 0; h.Run(base);
    Require(h.state.reg[0] == 96 && h.stats.blocks == 2 &&
                h.space->Get(IR::LocationDescriptor{base + 48 * 8}) == owner, "cached target pruning");
    h.space->ClearCache();
    h.cb.Put(base, Branch(base, 0x8000, true));
    h.cb.Put(base + 4, 0x91001c00); // add x0,x0,#7
    h.cb.Put(base + 8, 0xd4000001);
    h.cb.Put(0x8000, 0xd2800123); h.cb.Put(0x8004, 0xd65f03c0); // mov x3,#9; ret
    for (int i = 0; i < 3; ++i) {
        h.state.reg[0] = 0; h.Run(base);
        Require(h.state.reg[0] == 7 && h.state.reg[3] == 9 && h.Ticks() == 5, "external call/return");
        Require(h.space->Get(IR::LocationDescriptor{base}) == h.space->Get(IR::LocationDescriptor{base + 4}),
                "caller and return continuation must share an owner");
    }
    std::vector<u8> filter(65536, 0);
    filter[((base + 4) >> 1) & 0xffff] = 2;
    h.boundary.hook_slots = static_cast<u32>(reinterpret_cast<uptr>(filter.data()));
    h.boundary.instruction_hooks = 1;
    h.state.reg[0] = 0; h.Run(base);
    Require(std::find(h.observed_pcs.begin(), h.observed_pcs.end(), base + 4) != h.observed_pcs.end() &&
                h.state.reg[0] == 7 && h.Ticks() == 5, "dynamic return hook guard");
}
void CheckBackwardEntries() {
    Harness h;
    h.cb.Put(base, 0x91001000); h.cb.Put(base + 4, 0xd4000001);
    h.cb.Put(base + 8, 0x91000800); h.cb.Put(base + 12, Branch(base + 12, base));
    h.cb.Put(base + 16, 0x91000400); h.cb.Put(base + 20, Branch(base + 20, base + 8));
    h.Run(base + 16);
    Require(h.state.reg[0] == 7 && h.Ticks() == 6, "nonzero original entry index");
    const auto compiled = h.stats.blocks;
    h.state.reg[0] = 0; h.Run(base + 8);
    Require(h.state.reg[0] == 6 && h.Ticks() == 4 && h.stats.blocks == compiled, "backward alias");
    h.space->ClearCache();
    h.cb.Put(base, 0xd2800000); h.cb.Put(base + 4, Branch(base + 4, base + 16));
    h.cb.Put(base + 16, 0x91000400); h.cb.Put(base + 20, Branch(base + 20, base + 32));
    h.cb.Put(base + 32, 0xf1001c1f); h.cb.Put(base + 36, 0x54ffff61); h.cb.Put(base + 40, 0xd4000001);
    u64 ticks = 0, pc = base;
    do { h.Run(pc, 3); ticks += h.Ticks(); pc = h.state.pc; }
    while (h.context.halt_reason != static_cast<u32>(HaltReason::UserDefined1) && ticks < 100);
    Require(h.state.reg[0] == 7 && ticks == 31, "multi-block loop budget resumes");
}
void CheckPublicAndPrerequisites() {
    for (const auto [policy, cycles] : {std::array{1,1}, std::array{2,0}}) {
        Harness h(policy, 0, cycles); h.Chain(); h.space->GetOrEmit(IR::LocationDescriptor{base});
        Require(h.space->Get(IR::LocationDescriptor{base + 8}) == nullptr, "prerequisite gating");
    }
    Harness isolated; isolated.Chain();
    A64AddressSpace unbound(isolated.conf);
    unbound.GetOrEmit(IR::LocationDescriptor{base});
    Require(unbound.Get(IR::LocationDescriptor{base + 8}) == nullptr, "flag alone must not enable sharing without boundary");
    isolated.space->GetOrEmit(A64::LocationDescriptor{IR::LocationDescriptor{base}}.SetSingleStepping(true));
    Require(isolated.space->Get(IR::LocationDescriptor{base + 8}) == nullptr, "Step must not publish aliases");
    Harness h; h.cb.Put(base, 0x14000001); h.cb.Put(base + 4, 0x91000400); h.cb.Put(base + 8, 0xd4000001);
    u32 observed = 0;
    auto jit = CreateA64Jit(h.conf, &h.boundary, {[](void* p) { ++*static_cast<u32*>(p); }, &observed});
    h.cb.public_jit = jit.get(); h.cb.commit_svc = true;
    jit->SetPC(base); jit->Run();
    Require(jit->GetRegister(0) == 1 && h.cb.svc_ticks == 1 && h.cb.ticks == 3 && observed == 1,
            "public factory, callback tick commit and observer ordering");
    h.cb.commit_svc = false; jit->ClearHalt(); jit->SetRegister(0, 0); jit->SetPC(base + 4); jit->Step();
    Require(jit->GetRegister(0) == 1 && jit->GetPC() == base + 8, "public Step");
}
void Print(u32 sample, double ms, const CompileStats& before, const Harness& h, u64 instructions) {
    Require(sample == 0 || h.stats.blocks == before.blocks, "warm compilation");
    std::printf("A64_REGION_SAMPLE {\"sample\":%u,\"ms\":%.9f,\"blocks\":%.0f,\"bytes\":%.0f,\"instructions\":%llu}\n",
                sample, ms, h.stats.blocks - before.blocks, h.stats.bytes - before.bytes, static_cast<unsigned long long>(instructions));
}
void Benchmark(int policy, const char* kind, int memory) {
    Harness h(policy, memory);
    const bool chain = std::strcmp(kind, "chain") == 0 || std::strcmp(kind, "boundaries") == 0;
    const bool mem = std::strcmp(kind, "memory") == 0;
    const bool neon = std::strcmp(kind, "neon") == 0;
    const bool scalar = std::strcmp(kind, "scalar") == 0;
    Require(chain || mem || neon || scalar, "unknown workload");
    constexpr u32 count = 48;
    u64 instructions = 0;
    if (chain) { h.Chain(count); h.boundary.flags = std::strcmp(kind, "boundaries") == 0 ? 2 : 0; }
    if (mem) {
        constexpr std::array<u32, 12> code{0xd280fa04,0xd2800000,0xd2880001,0xd2802002,0xf8408423,0x8b030000,
            0xf9040020,0xf1000442,0x54ffff81,0xf1000484,0x54ffff01,0xd4000001};
        for (u32 i = 0; i < code.size(); ++i) h.cb.Put(base + i * 4, code[i]);
        for (u32 i = 0; i < 256; ++i) h.cb.Write<u64>(0x4000 + i * 8, i);
        instructions = 2 + 2000ULL * (2 + 256 * 5 + 2) + 1;
    }
    if (scalar || neon) {
        const std::vector<u32> code = neon
            ? std::vector<u32>{0xd2a00204,0x4f000420,0x4f000441,0x4ea18400,0x4ea18400,0xf1000484,0x54ffffa1,0x4eb1b800,0x0e043c00,0xd4000001}
            : std::vector<u32>{0xd2a00204,0x52800020,0x52800041,0x0b010000,0x0b010000,0xf1000484,0x54ffffa1,0xd4000001};
        for (u32 i = 0; i < code.size(); ++i) h.cb.Put(base + i * 4, code[i]);
        instructions = 3 + 4 * (1ULL << 20) + (neon ? 3 : 1);
    }
    const auto run = [&](u32 repeats) {
        if (chain) {
            for (u32 r = 0; r < repeats; ++r) for (u32 i = 0; i < count; ++i) {
                h.state.reg[0] = 0; h.Run(base + i * 8);
                Require(h.state.reg[0] == count - i && h.Ticks() == 2 * (count - i) + 1, "benchmark chain result/ticks");
            }
        } else {
            h.Run(base);
            Require(h.Ticks() == instructions, "benchmark loop ticks");
            if (mem) {
                Require(h.state.reg[0] == 2000ULL * 255 * 256 / 2, "memory loop sum");
                for (u32 i = 0; i < 256; ++i) Require(h.cb.Read<u64>(0x4000 + i * 8) == i &&
                    h.cb.Read<u64>(0x4808 + i * 8) == 1999ULL * 255 * 256 / 2 + u64{i} * (i + 1) / 2, "memory loop output");
            } else Require(h.state.reg[0] == (neon ? 4 : 1) * (1 + 4 * (1ULL << 20)), "scalar/NEON output");
        }
    };
    const char* warmup = std::getenv("DYNARMIC_REGION_WARMUP");
    const int warmups = warmup ? std::atoi(warmup) : 0;
    Require(warmups >= 0 && warmups <= 100, "warmup count");
    for (u32 sample = 0; sample < 9; ++sample) {
        const auto before = h.stats;
        if (sample == 1) for (int i = 0; i < warmups; ++i) run(chain ? 1000 : 1);
        const u32 repeats = sample == 0 ? 1 : 1000;
        const double start = emscripten_get_now(); run(repeats);
        Print(sample, emscripten_get_now() - start, before, h, chain ? u64{repeats} * count * (count + 2) : instructions);
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 4) { Benchmark(std::atoi(argv[1]), argv[2], std::atoi(argv[3])); return 0; }
    for (int memory : {0,1,2}) for (bool boundaries : {false,true}) CheckChain(memory, boundaries, base);
    CheckChain(0, false, 0x100001000ULL); CheckChain(0, false, 0xfffffffffff00000ULL);
    CheckDescriptorAndFpcr(); CheckBudgetsAndHooks(); CheckReturnAndPruning(); CheckBackwardEntries(); CheckPublicAndPrerequisites();
    std::puts("PASS A64 region sharing: aliases, full PC/FPCR, budgets/hooks, invalidation, call/return, public API");
}
