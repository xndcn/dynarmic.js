// Emscripten host benchmark: call/return-heavy A32 loop through the WASM
// backend (exercises PushRSB / PopRSBHint speculation).
//
//   0x1000: mov  r4, #0x100000    ; 1M iterations
//   0x1004: mov  r0, #0           ; sum
//   0x1008: bl   func             ; loop:
//   0x100C: subs r4, r4, #1
//   0x1010: bne  loop
//   0x1014: svc  #0
//   0x1018: add  r0, r0, #1       ; func:
//   0x101C: bx   lr
//
// usage: node jit_callbench.js

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "benchmark.h"

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"


using namespace Dynarmic;

namespace {

constexpr std::array<u32, 8> kCode = {
    0xE3A04601,  // mov r4, #0x100000
    0xE3A00000,  // mov r0, #0
    0xEB000002,  // bl func (0x1018)
    0xE2544001,  // subs r4, r4, #1
    0x1AFFFFFC,  // bne 0x1008
    0xEF000000,  // svc #0
    0xE2800001,  // add r0, r0, #1
    0xE12FFF1E,  // bx lr
};
constexpr A32::VAddr kCodeBase = 0x1000;
constexpr size_t kRamSize = 1 << 20;
constexpr u64 kGuestInstructions = 0x100000ULL * 5 + 3;

std::array<u8, kRamSize> g_ram{};

A32::Jit* g_jit = nullptr;

template<typename T>
T ReadMem(A32::VAddr vaddr) {
    T value;
    std::memcpy(&value, g_ram.data() + (vaddr % kRamSize), sizeof(T));
    return value;
}

template<typename T>
void WriteMem(A32::VAddr vaddr, T value) {
    std::memcpy(g_ram.data() + (vaddr % kRamSize), &value, sizeof(T));
}

class BenchCallbacks final : public A32::UserCallbacks {
public:
    std::optional<u32> MemoryReadCode(A32::VAddr vaddr) override {
        return MemoryRead32(vaddr);
    }
    u8 MemoryRead8(A32::VAddr vaddr) override { return ReadMem<u8>(vaddr); }
    u16 MemoryRead16(A32::VAddr vaddr) override { return ReadMem<u16>(vaddr); }
    u32 MemoryRead32(A32::VAddr vaddr) override { return ReadMem<u32>(vaddr); }
    u64 MemoryRead64(A32::VAddr vaddr) override { return ReadMem<u64>(vaddr); }
    void MemoryWrite8(A32::VAddr vaddr, u8 value) override { WriteMem(vaddr, value); }
    void MemoryWrite16(A32::VAddr vaddr, u16 value) override { WriteMem(vaddr, value); }
    void MemoryWrite32(A32::VAddr vaddr, u32 value) override { WriteMem(vaddr, value); }
    void MemoryWrite64(A32::VAddr vaddr, u64 value) override { WriteMem(vaddr, value); }

    bool IsReadOnlyMemory(A32::VAddr) override { return false; }
    void InterpreterFallback(A32::VAddr, size_t) override { std::abort(); }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(A32::VAddr, A32::Exception) override { std::abort(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
};

bool RunOnce(A32::Jit& jit, bool cold, int run_index) {
    jit.Reset();
    jit.ClearHalt();
    g_jit = &jit;
    jit.Regs()[15] = kCodeBase;
    jit.SetCpsr(0x1F);

    benchmark_begin();
    const double t0 = benchmark_now();
    const HaltReason hr = jit.Run();
    const double t1 = benchmark_now();

    const u32 expected = 0x100000;
    const bool ok = hr == HaltReason::UserDefined1 && jit.Regs()[0] == expected;
    if (!ok) {
        std::printf("  WRONG RESULT: hr=%u r0=%u lr=%x pc=%x (expected %u)\n",
                    static_cast<u32>(hr), jit.Regs()[0], jit.Regs()[14], jit.Regs()[15], expected);
    }
    if (ok) benchmark_sample(t1 - t0, static_cast<double>(kGuestInstructions), cold, run_index);
    return ok;
}

}  // namespace

int main() {
    std::memcpy(g_ram.data() + kCodeBase, kCode.data(), sizeof(kCode));

    static BenchCallbacks callbacks;
    static ExclusiveMonitor monitor{1};

    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.processor_id = 0;
    conf.enable_cycle_counting = false;

    A32::Jit jit{conf};
    if (!RunOnce(jit, true, 0)) return 1;
    // Allow the engine to optimize the same modules; preserve the JIT cache.
    for (int i = 0; i < 8; ++i) {
        if (!RunOnce(jit, false, i + 1)) return 1;
    }
    std::puts("PASS");
    return 0;
}
