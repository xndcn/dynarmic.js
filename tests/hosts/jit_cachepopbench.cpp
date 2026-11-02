// Emscripten cache-population benchmark for the WASM backend.
//
// Compile a configurable number of independent A32 entry blocks and keep all
// of them live in one JIT. This measures cache population and retained module
// bytes separately from invalidation pressure.
//
// usage: node jit_cachepopbench.js [0|1|2] [population]

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include <emscripten.h>
#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

constexpr A32::VAddr kCodeBase = 0x1000;
constexpr u32 kEntryStride = 0x10;
constexpr size_t kRamSize = 1 << 20;
constexpr int kMaxPopulation = 4096;
std::array<u8, kRamSize> g_ram{};
std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> g_page_table{};
A32::Jit* g_jit = nullptr;

template<typename T>
T ReadMem(A32::VAddr address) {
    T value;
    std::memcpy(&value, g_ram.data() + (address % kRamSize), sizeof(T));
    return value;
}

template<typename T>
void WriteMem(A32::VAddr address, T value) {
    std::memcpy(g_ram.data() + (address % kRamSize), &value, sizeof(T));
}

EM_JS(void, cache_population_begin, (), {
    Module.cachePopulationBefore = {...(Module.dynarmicWasmStats || {})};
    if (Module.cachePopulationBefore.profile) {
        Module.cachePopulationBefore.profile = {...Module.cachePopulationBefore.profile};
    }
});

EM_JS(void, cache_population_sample, (double ms, int population, int warm), {
    const before = Module.cachePopulationBefore || {};
    const after = Module.dynarmicWasmStats || {};
    const compile = {};
    for (const key of ['blocks', 'bytes', 'translateMs', 'optMs', 'emitMs',
                       'compileMs', 'instMs', 'tableMs']) {
        compile[key] = (after[key] || 0) - (before[key] || 0);
    }
    let profile = null;
    if (after.profile) {
        profile = {schema: after.profile.schema, enabled: true};
        for (const [key, current] of Object.entries(after.profile)) {
            if (typeof current === 'number') profile[key] = current - (before.profile?.[key] || 0);
        }
    }
    console.log('CACHE_POPULATION ' + JSON.stringify({phase: warm ? 'warm' : 'cold', population, ms, compile, profile}));
});

class BenchCallbacks final : public A32::UserCallbacks {
public:
    std::optional<u32> MemoryReadCode(A32::VAddr address) override { return MemoryRead32(address); }
    u8 MemoryRead8(A32::VAddr address) override { return ReadMem<u8>(address); }
    u16 MemoryRead16(A32::VAddr address) override { return ReadMem<u16>(address); }
    u32 MemoryRead32(A32::VAddr address) override { return ReadMem<u32>(address); }
    u64 MemoryRead64(A32::VAddr address) override { return ReadMem<u64>(address); }
    void MemoryWrite8(A32::VAddr address, u8 value) override { WriteMem(address, value); }
    void MemoryWrite16(A32::VAddr address, u16 value) override { WriteMem(address, value); }
    void MemoryWrite32(A32::VAddr address, u32 value) override { WriteMem(address, value); }
    void MemoryWrite64(A32::VAddr address, u64 value) override { WriteMem(address, value); }
    bool IsReadOnlyMemory(A32::VAddr) override { return false; }
    void InterpreterFallback(A32::VAddr, size_t) override { std::abort(); }
    void CallSVC(u32) override { g_jit->HaltExecution(HaltReason::UserDefined1); }
    void ExceptionRaised(A32::VAddr, A32::Exception) override { std::abort(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
};

}  // namespace

int main(int argc, char** argv) {
    const int mode = argc > 1 ? std::atoi(argv[1]) : 0;
    const int population = argc > 2 ? std::atoi(argv[2]) : kMaxPopulation;
    if (mode < 0 || mode > 2 || population < 1 || population > kMaxPopulation) return 2;

    static BenchCallbacks callbacks;
    static ExclusiveMonitor monitor{1};
    for (size_t i = 0; i < g_ram.size() / 4096; ++i) {
        g_page_table[i] = g_ram.data() + i * 4096;
    }
    for (int i = 0; i < population; ++i) {
        const A32::VAddr pc = kCodeBase + static_cast<u32>(i) * kEntryStride;
        // mov r0, #(i % 255 + 1); svc #0. Each entry has a distinct location and
        // immediate, preventing accidental cache aliasing.
        WriteMem<u32>(pc, 0xe3a00000u | static_cast<u32>(i % 255 + 1));
        WriteMem<u32>(pc + 4, 0xef000000u);
        // SVC has a fallthrough successor even when this host halts. Bound
        // speculative region discovery to this entry instead of translating
        // zero-filled padding and neighbouring entries.
        WriteMem<u32>(pc + 8, 0xeafffffcu); // b entry
    }

    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.processor_id = 0;
    conf.enable_cycle_counting = false;
    if (mode != 0) conf.page_table = &g_page_table;
    if (mode == 2) conf.fastmem_pointer = reinterpret_cast<uptr>(g_ram.data());

    A32::Jit jit{conf};
    g_jit = &jit;
    for (int warm = 0; warm < 2; ++warm) {
        cache_population_begin();
        const double start = emscripten_get_now();
        for (int i = 0; i < population; ++i) {
            jit.ClearHalt();
            jit.Reset();
            jit.Regs()[15] = kCodeBase + static_cast<u32>(i) * kEntryStride;
            jit.SetCpsr(0x1F);
            const HaltReason reason = jit.Run();
            if (reason != HaltReason::UserDefined1 || jit.Regs()[0] != static_cast<u32>(i % 255 + 1)) {
                std::printf("WRONG RESULT entry=%d reason=%u value=%u expected=%d\n", i,
                            static_cast<u32>(reason), jit.Regs()[0], i % 255 + 1);
                return 1;
            }
        }
        cache_population_sample(emscripten_get_now() - start, population, warm);
    }
    std::puts("PASS cache population");
    return 0;
}
