// Emscripten cache-pressure benchmark for the WASM backend.
//
// Each cycle patches one instruction in a tiny A32 guest, invalidates only
// that instruction's range, and runs the guest again. The workload therefore
// measures deliberate code-cache destruction and reconstruction rather than
// pretending that a recompiling run is a warm-cache sample.
//
// usage: node jit_cachebench.js [0|1|2]

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
constexpr size_t kRamSize = 1 << 20;
constexpr int kCycles = 8;
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

// Keep the JS bridge local to this workload. The regular benchmark protocol
// intentionally rejects samples that compile during a warm run.
EM_JS(void, cache_benchmark_begin, (), {
    Module.cacheBenchmarkBefore = {...(Module.dynarmicWasmStats || {})};
    if (Module.cacheBenchmarkBefore.profile) {
        Module.cacheBenchmarkBefore.profile = {...Module.cacheBenchmarkBefore.profile};
    }
});

EM_JS(void, cache_benchmark_sample, (double ms, int cycle, int value), {
    const before = Module.cacheBenchmarkBefore || {};
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
    console.log('CACHE_BENCHMARK ' + JSON.stringify({cycle, value, ms, compile, profile}));
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
    if (mode < 0 || mode > 2) return 2;

    static BenchCallbacks callbacks;
    static ExclusiveMonitor monitor{1};
    for (size_t i = 0; i < g_ram.size() / 4096; ++i) {
        g_page_table[i] = g_ram.data() + i * 4096;
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
    WriteMem<u32>(kCodeBase + 4, 0xef000000u); // svc #0
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        // mov r0, #(cycle + 1); svc #0
        WriteMem<u32>(kCodeBase, 0xe3a00000u | static_cast<u32>(cycle + 1));
        if (cycle != 0) {
            jit.InvalidateCacheRange(kCodeBase, sizeof(u32));
        }
        jit.ClearHalt(HaltReason::UserDefined1);
        jit.Reset();
        jit.Regs()[15] = kCodeBase;
        jit.SetCpsr(0x1F);

        cache_benchmark_begin();
        const double start = emscripten_get_now();
        const HaltReason reason = jit.Run();
        const double elapsed = emscripten_get_now() - start;
        const int value = static_cast<int>(jit.Regs()[0]);
        if (reason != HaltReason::UserDefined1 || value != cycle + 1) {
            std::printf("WRONG RESULT cycle=%d reason=%u value=%d expected=%d\n",
                        cycle, static_cast<u32>(reason), value, cycle + 1);
            return 1;
        }
        cache_benchmark_sample(elapsed, cycle, value);
    }
    std::puts("PASS cache pressure");
    return 0;
}
