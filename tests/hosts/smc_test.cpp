// Per-range invalidation test: patch a hot function, invalidate only it, and
// check that (a) the new code runs, (b) chained callers don't jump to the
// stale block, (c) unrelated blocks are not recompiled.
#include <array>
#include <cstdio>
#include <cstring>
#include <emscripten.h>
#include <mcl/stdint.hpp>
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
using namespace Dynarmic;
namespace {
constexpr size_t kRam = 1 << 20;
std::array<u8, kRam> g_ram{};
std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> g_pt{};
A32::Jit* g_jit;
template<typename T> T Rd(A32::VAddr a) { T v; std::memcpy(&v, &g_ram[a % kRam], sizeof(T)); return v; }
template<typename T> void Wr(A32::VAddr a, T v) { std::memcpy(&g_ram[a % kRam], &v, sizeof(T)); }
class CB final : public A32::UserCallbacks {
public:
    std::optional<u32> MemoryReadCode(A32::VAddr a) override { return Rd<u32>(a); }
    u8 MemoryRead8(A32::VAddr a) override { return Rd<u8>(a); }
    u16 MemoryRead16(A32::VAddr a) override { return Rd<u16>(a); }
    u32 MemoryRead32(A32::VAddr a) override { return Rd<u32>(a); }
    u64 MemoryRead64(A32::VAddr a) override { return Rd<u64>(a); }
    void MemoryWrite8(A32::VAddr a, u8 v) override { Wr(a, v); }
    void MemoryWrite16(A32::VAddr a, u16 v) override { Wr(a, v); }
    void MemoryWrite32(A32::VAddr a, u32 v) override { Wr(a, v); }
    void MemoryWrite64(A32::VAddr a, u64 v) override { Wr(a, v); }
    void InterpreterFallback(A32::VAddr, size_t) override { g_jit->HaltExecution(); }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(A32::VAddr, A32::Exception) override { g_jit->HaltExecution(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
};
double Blocks() { return EM_ASM_DOUBLE({ const s = Module.dynarmicWasmStats; return s ? s.blocks : 0; }); }
}  // namespace

int main(int argc, char** argv) {
    FILE* f = std::fopen(argv[1], "rb");
    const size_t n = std::fread(&g_ram[0x10000], 1, kRam - 0x10000, f);
    std::fclose(f);
    const u32 func = 0x1001c;  // mov r0,#1 ; bx lr
    static CB cb;
    static ExclusiveMonitor mon{1};
    A32::UserConfig conf;
    conf.callbacks = &cb;
    conf.global_monitor = &mon;
    conf.enable_cycle_counting = false;
    for (u32 i = 0; i < kRam / 4096; i++) g_pt[i] = g_ram.data() + i * 4096;
    conf.page_table = &g_pt;
    A32::Jit jit{conf};
    g_jit = &jit;
    int fail = 0;
    for (int round = 0; round < 3; round++) {
        jit.Regs()[15] = 0x10000;
        jit.SetCpsr(0x1F);
        const double b0 = Blocks();
        jit.Run();
        const double b1 = Blocks();
        const u32 r5 = jit.Regs()[5];
        const u32 expect = 100 * (round + 1);  // func returns round+1
        std::printf("round %d: r5=%u (expect %u) new blocks=%g\n", round, r5, expect, b1 - b0);
        if (r5 != expect) fail = 1;
        if (round > 0 && b1 - b0 > 3) { std::printf("  too many recompiles\n"); fail = 1; }
        // patch `mov r0,#imm` and invalidate only that function
        Wr<u32>(func, 0xe3a00000u | (round + 2));
        jit.InvalidateCacheRange(func, 8);
        jit.ClearHalt(HaltReason::UserDefined1);
    }
    // Single-step through one loop iteration.
    jit.Regs()[15] = 0x10000;
    jit.SetCpsr(0x1F);
    const u32 expect_pc[] = {0x10004, 0x10008, 0x1001c, 0x10020, 0x1000c, 0x10010, 0x10014, 0x10008, 0x1001c};
    for (u32 pc : expect_pc) {
        const HaltReason hr = jit.Step();
        const bool ok = (static_cast<u32>(hr) & static_cast<u32>(HaltReason::Step)) && jit.Regs()[15] == pc;
        std::printf("step -> pc=%08x (expect %08x) hr=%x %s\n", jit.Regs()[15], pc, static_cast<u32>(hr), ok ? "ok" : "BAD");
        if (!ok) fail = 1;
    }
    std::printf("after steps r4=%u (expect 99) r5=%u\n", jit.Regs()[4], jit.Regs()[5]);
    if (jit.Regs()[4] != 99) fail = 1;
    std::printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
