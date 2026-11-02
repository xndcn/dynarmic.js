// Emscripten host benchmark: Thumb-1 memory loop through the WASM backend.
// It is paired with jit_bench.cpp to expose the A32 frontend's Thumb path.
//
// usage: node jit_thumb_bench.js [0|1|2]

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

// 0x1000: movs r4,#200; movs r0,#0; outer:
// 0x1004: movs r1,#64; movs r2,#32; inner:
// 0x1008: ldr r3,[r1]; adds r0,r0,r3; str r0,[r1,#4]
// 0x1010: subs r2,#1; bne inner; subs r4,#1; bne outer; svc #0
constexpr std::array<u16, 12> kCode = {
    0x24c8, 0x2000, 0x2140, 0x2220, 0x680b, 0x18c0,
    0x6048, 0x3a01, 0xd1fa, 0x3c01, 0xd1f6, 0xdf00,
};
constexpr A32::VAddr kCodeBase = 0x1000;
constexpr A32::VAddr kBufBase = 0x40;
constexpr size_t kRamSize = 1 << 20;
constexpr u64 kGuestInstructions = 2 + 200ULL * (2 + 32ULL * 5 + 2) + 1;

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
    std::optional<u32> MemoryReadCode(A32::VAddr vaddr) override { return MemoryRead32(vaddr); }
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
    jit.SetCpsr(0x3F); // user mode, Thumb state
    WriteMem<u32>(kBufBase + 4, 0xdeadbeef);

    benchmark_begin();
    const double t0 = benchmark_now();
    const HaltReason hr = jit.Run();
    const double t1 = benchmark_now();

    constexpr u32 expected = 200u * 32u * 64u;
    const bool ok = hr == HaltReason::UserDefined1 && jit.Regs()[0] == expected &&
                    ReadMem<u32>(kBufBase) == 64 && ReadMem<u32>(kBufBase + 4) == expected;
    if (!ok) {
        std::printf("  WRONG RESULT: hr=%u r0=%u pc=%x (expected %u)\n",
                    static_cast<u32>(hr), jit.Regs()[0], jit.Regs()[15], expected);
    }
    if (ok) benchmark_sample(t1 - t0, static_cast<double>(kGuestInstructions), cold, run_index);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::memcpy(g_ram.data() + kCodeBase, kCode.data(), sizeof(kCode));
    WriteMem<u32>(kBufBase, 64);
    for (size_t i = 0; i < kRamSize / 4096; i++) g_page_table[i] = g_ram.data() + i * 4096;

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
    for (int i = 0; i < 8; ++i) {
        if (!RunOnce(jit, false, i + 1)) return 1;
    }
    std::puts("PASS");
    return 0;
}
