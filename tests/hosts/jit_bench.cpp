// Emscripten host benchmark: memory-heavy A32 loop through the WASM backend.
//
//   0x1000: mov  r4, #2000          ; outer iterations
//   0x1004: mov  r0, #0             ; sum        ; outer:
//   0x1008: mov  r1, #0x4000        ; buf ptr
//   0x100C: mov  r2, #256           ; inner iterations
//   0x1010: ldr  r3, [r1], #4                    ; inner:
//   0x1014: add  r0, r0, r3
//   0x1018: str  r0, [r1, #1024]
//   0x101C: subs r2, r2, #1
//   0x1020: bne  inner
//   0x1024: subs r4, r4, #1
//   0x1028: bne  outer
//   0x102C: svc  #0
//
// usage: node jit_bench.js [0|1]   (0 = thunk memory, 1 = fastmem page table)

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

constexpr std::array<u32, 13> kCode = {
    0xE3A04E7D, 0xE3A00000, 0xE3A01901, 0xE3A02C01,
    0xE5913000, 0xE2811004, 0xE0800003, 0xE5810400,
    0xE2522001, 0x1AFFFFF9, 0xE2544001, 0x1AFFFFF4,
    0xEF000000,
};
constexpr A32::VAddr kCodeBase = 0x1000;
constexpr A32::VAddr kBufBase = 0x4000;
constexpr size_t kRamSize = 1 << 20;
// The outer back edge targets mov r0,#0 (0x1004), so resetting the sum is
// executed on every outer iteration as well as resetting the pointer/counter.
constexpr u64 kGuestInstructions = 1 + 2000ULL * (3 + 256ULL * 6 + 2) + 1;

std::array<u8, kRamSize> g_ram{};
std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> g_page_table{};

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
    for (u32 i = 0; i < 256; ++i) WriteMem<u32>(kBufBase + 1024 + (i + 1) * 4, 0xdeadbeef);

    benchmark_begin();
    const double t0 = benchmark_now();
    const HaltReason hr = jit.Run();
    const double t1 = benchmark_now();

    const u32 expected = 255u * 256u / 2u;  // sum(0..255); r0 is reset each outer pass
    bool ok = hr == HaltReason::UserDefined1 && jit.Regs()[0] == expected;
    for (u32 i = 0; i < 256; ++i) {
        ok = ok && ReadMem<u32>(kBufBase + i * 4) == i &&
             ReadMem<u32>(kBufBase + 1024 + (i + 1) * 4) == i * (i + 1) / 2;
    }
    if (!ok) {
        std::printf("  WRONG RESULT: hr=%u r0=%u r1=%x r2=%u r3=%u r4=%u pc=%x (expected %u)\n",
                    static_cast<u32>(hr), jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                    jit.Regs()[3], jit.Regs()[4], jit.Regs()[15], expected);
    }
    if (ok) benchmark_sample(t1 - t0, static_cast<double>(kGuestInstructions), cold, run_index);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::memcpy(g_ram.data() + kCodeBase, kCode.data(), sizeof(kCode));
    for (u32 i = 0; i < 256; i++) {
        WriteMem<u32>(kBufBase + i * 4, i);
    }
    for (size_t i = 0; i < kRamSize / 4096; i++) {
        g_page_table[i] = g_ram.data() + i * 4096;
    }

    static BenchCallbacks callbacks;
    static ExclusiveMonitor monitor{1};

    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.processor_id = 0;
    conf.enable_cycle_counting = false;

    const int mode = argc > 1 ? std::atoi(argv[1]) : 0;
    if (mode < 0 || mode > 2) return 2;
    if (mode) conf.page_table = &g_page_table;
    if (mode == 2) conf.fastmem_pointer = reinterpret_cast<uptr>(g_ram.data());

    A32::Jit jit{conf};
    if (!RunOnce(jit, true, 0)) return 1;
    // Allow the engine to optimize the same modules; preserve the JIT cache.
    for (int i = 0; i < 8; ++i) {
        if (!RunOnce(jit, false, i + 1)) return 1;
    }
    std::puts("PASS");
    return 0;
}
