// Emscripten host benchmark: AArch64 memory loop through the WASM backend.
//
// The guest resets its input pointer and loop counter for each outer pass,
// accumulates 256 64-bit values, and stores the running sum. This mirrors the
// A32 memory benchmark so memory modes and instruction accounting are paired.
//
// usage: node jit_a64_bench.js [0|1|2]

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "benchmark.h"

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

// 0x1000: mov x4,#2000; mov x0,#0; outer:
// 0x1008: mov x1,#0x4000; mov x2,#256; inner:
// 0x1010: ldr x3,[x1],#8; add x0,x0,x3; str x0,[x1,#2048]
// 0x101c: subs x2,x2,#1; b.ne inner; subs x4,x4,#1; b.ne outer; svc #0
constexpr std::array<u32, 12> kCode = {
    0xd280fa04, 0xd2800000, 0xd2880001, 0xd2802002,
    0xf8408423, 0x8b030000, 0xf9040020, 0xf1000442,
    0x54ffff81, 0xf1000484, 0x54ffff01, 0xd4000001,
};
constexpr A64::VAddr kCodeBase = 0x1000;
constexpr A64::VAddr kBufBase = 0x4000;
constexpr size_t kRamSize = 1 << 20;
// Each outer iteration resets x1/x2, executes the 256-iteration inner loop,
// then decrements x4 and branches back to the outer setup.
constexpr u64 kGuestInstructions = 2 + 2000ULL * (2 + 256ULL * 5 + 2) + 1;

std::array<u8, kRamSize> g_ram{};
// A64's 32-bit page table has one entry per 4 KiB page.
std::array<void*, size_t{1} << (32 - 12)> g_page_table{};
A64::Jit* g_jit = nullptr;

template<typename T>
T ReadMem(A64::VAddr vaddr) {
    T value;
    std::memcpy(&value, g_ram.data() + (vaddr % kRamSize), sizeof(T));
    return value;
}

template<typename T>
void WriteMem(A64::VAddr vaddr, T value) {
    std::memcpy(g_ram.data() + (vaddr % kRamSize), &value, sizeof(T));
}

class BenchCallbacks final : public A64::UserCallbacks {
public:
    u8 MemoryRead8(A64::VAddr vaddr) override { return ReadMem<u8>(vaddr); }
    u16 MemoryRead16(A64::VAddr vaddr) override { return ReadMem<u16>(vaddr); }
    u32 MemoryRead32(A64::VAddr vaddr) override { return ReadMem<u32>(vaddr); }
    u64 MemoryRead64(A64::VAddr vaddr) override { return ReadMem<u64>(vaddr); }
    A64::Vector MemoryRead128(A64::VAddr vaddr) override { return {ReadMem<u64>(vaddr), ReadMem<u64>(vaddr + 8)}; }
    void MemoryWrite8(A64::VAddr vaddr, u8 value) override { WriteMem(vaddr, value); }
    void MemoryWrite16(A64::VAddr vaddr, u16 value) override { WriteMem(vaddr, value); }
    void MemoryWrite32(A64::VAddr vaddr, u32 value) override { WriteMem(vaddr, value); }
    void MemoryWrite64(A64::VAddr vaddr, u64 value) override { WriteMem(vaddr, value); }
    void MemoryWrite128(A64::VAddr vaddr, A64::Vector value) override {
        WriteMem(vaddr, value[0]);
        WriteMem(vaddr + 8, value[1]);
    }
    void InterpreterFallback(A64::VAddr, size_t) override { std::abort(); }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(A64::VAddr, A64::Exception) override { std::abort(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
    u64 GetCNTPCT() override { return 0; }
};

bool RunOnce(A64::Jit& jit, bool cold, int run_index) {
    jit.Reset();
    jit.ClearHalt();
    g_jit = &jit;
    jit.SetPC(kCodeBase);
    for (u32 i = 0; i < 256; ++i) WriteMem<u64>(kBufBase + 2048 + (i + 1) * 8, 0xdeadbeefdeadbeefULL);

    benchmark_begin();
    const double t0 = benchmark_now();
    const HaltReason hr = jit.Run();
    const double t1 = benchmark_now();

    constexpr u64 expected = 2000ULL * (255ULL * 256ULL / 2);
    bool ok = hr == HaltReason::UserDefined1 && jit.GetRegister(0) == expected;
    for (u64 i = 0; i < 256; ++i) {
        const u64 stored = 1999ULL * (255ULL * 256ULL / 2) + i * (i + 1) / 2;
        ok = ok && ReadMem<u64>(kBufBase + i * 8) == i &&
             ReadMem<u64>(kBufBase + 2048 + (i + 1) * 8) == stored;
    }
    if (!ok) {
        std::printf("  WRONG RESULT: hr=%u x0=%llx pc=%llx (expected %llx)\n",
                    static_cast<u32>(hr), static_cast<unsigned long long>(jit.GetRegister(0)),
                    static_cast<unsigned long long>(jit.GetPC()), static_cast<unsigned long long>(expected));
    }
    if (ok) benchmark_sample(t1 - t0, static_cast<double>(kGuestInstructions), cold, run_index);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::memcpy(g_ram.data() + kCodeBase, kCode.data(), sizeof(kCode));
    for (u32 i = 0; i < 256; i++) WriteMem<u64>(kBufBase + i * 8, i);
    for (size_t i = 0; i < g_ram.size() / 4096; i++) g_page_table[i] = g_ram.data() + i * 4096;

    static BenchCallbacks callbacks;
    static ExclusiveMonitor monitor{1};
    A64::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.processor_id = 0;
    conf.enable_cycle_counting = false;

    const int mode = argc > 1 ? std::atoi(argv[1]) : 0;
    if (mode < 0 || mode > 2) return 2;
    if (mode) {
        conf.page_table = g_page_table.data();
        conf.page_table_address_space_bits = 32;
    }
    if (mode == 2) conf.fastmem_pointer = reinterpret_cast<uptr>(g_ram.data());

    A64::Jit jit{conf};
    if (!RunOnce(jit, true, 0)) return 1;
    for (int i = 0; i < 8; ++i) {
        if (!RunOnce(jit, false, i + 1)) return 1;
    }
    std::puts("PASS");
    return 0;
}
