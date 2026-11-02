// Emscripten host benchmark: paired AArch64 scalar and NEON arithmetic loops.
// Both guests perform two additions per loop iteration; the NEON case updates
// four 32-bit lanes in one instruction and the scalar case updates one lane.
// No guest memory is accessed, so this workload isolates SIMD lowering and
// register/flag loop handling. The argument selects `neon` or `scalar`.

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
constexpr A64::VAddr kCodeBase = 0x1000;
constexpr u64 kIterations = 1ULL << 20;
constexpr u64 kGuestInstructionsScalar = 3 + 4 * kIterations + 1;
constexpr u64 kGuestInstructionsNeon = 3 + 4 * kIterations + 3;
constexpr u32 kExpectedScalar = 1 + 4 * static_cast<u32>(kIterations);
constexpr u32 kExpectedNeon = 4 * kExpectedScalar;
constexpr std::array<u32, 8> kScalar = {
    0xd2a00204, 0x52800020, 0x52800041, 0x0b010000,
    0x0b010000, 0xf1000484, 0x54ffffa1, 0xd4000001,
};
constexpr std::array<u32, 10> kNeon = {
    0xd2a00204, 0x4f000420, 0x4f000441, 0x4ea18400,
    0x4ea18400, 0xf1000484, 0x54ffffa1, 0x4eb1b800,
    0x0e043c00, 0xd4000001,
};
std::array<u8, 64 * 1024> g_ram{};
A64::Jit* g_jit = nullptr;

template<typename T> T ReadMem(A64::VAddr address) {
    T value;
    std::memcpy(&value, g_ram.data() + (address % g_ram.size()), sizeof(T));
    return value;
}

class Callbacks final : public A64::UserCallbacks {
public:
    u8 MemoryRead8(u64 a) override { return ReadMem<u8>(a); }
    u16 MemoryRead16(u64 a) override { return ReadMem<u16>(a); }
    u32 MemoryRead32(u64 a) override { return ReadMem<u32>(a); }
    u64 MemoryRead64(u64 a) override { return ReadMem<u64>(a); }
    A64::Vector MemoryRead128(u64 a) override { return {ReadMem<u64>(a), ReadMem<u64>(a + 8)}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, u16) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64, size_t) override { std::abort(); }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(u64, A64::Exception) override { std::abort(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
    u64 GetCNTPCT() override { return 0; }
};

bool RunOnce(A64::Jit& jit, bool neon, bool cold, int run_index) {
    jit.Reset(); jit.ClearHalt(); g_jit = &jit; jit.SetPC(kCodeBase);
    benchmark_begin();
    const double t0 = benchmark_now();
    const HaltReason hr = jit.Run();
    const double t1 = benchmark_now();
    const u32 expected = neon ? kExpectedNeon : kExpectedScalar;
    const bool ok = hr == HaltReason::UserDefined1 && jit.GetRegister(0) == expected;
    if (!ok) std::printf("WRONG RESULT kind=%s hr=%u x0=%llx expected=%x\n", neon ? "neon" : "scalar",
                         static_cast<u32>(hr), static_cast<unsigned long long>(jit.GetRegister(0)), expected);
    if (ok) benchmark_sample(t1 - t0, static_cast<double>(neon ? kGuestInstructionsNeon : kGuestInstructionsScalar), cold, run_index);
    return ok;
}
}  // namespace

int main(int argc, char** argv) {
    const bool neon = argc > 1 && std::strcmp(argv[1], "neon") == 0;
    const bool scalar = argc > 1 && std::strcmp(argv[1], "scalar") == 0;
    if (!neon && !scalar) return 2;
    if (neon) {
        std::memcpy(g_ram.data() + kCodeBase, kNeon.data(), kNeon.size() * sizeof(u32));
    } else {
        std::memcpy(g_ram.data() + kCodeBase, kScalar.data(), kScalar.size() * sizeof(u32));
    }
    static Callbacks callbacks;
    static ExclusiveMonitor monitor{1};
    A64::UserConfig conf;
    conf.callbacks = &callbacks; conf.global_monitor = &monitor; conf.enable_cycle_counting = false;
    A64::Jit jit{conf};
    if (!RunOnce(jit, neon, true, 0)) return 1;
    for (int i = 0; i < 8; ++i) if (!RunOnce(jit, neon, false, i + 1)) return 1;
    std::puts("PASS");
}
