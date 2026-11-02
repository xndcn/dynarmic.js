// Emscripten host demo: run an A32 program through the full
// Dynarmic::A32::Jit interface with the WASM backend.
//
//   0x1000: mov  r0, #5
//   0x1004: mov  r1, #1
//   0x1008: add  r1, r1, r0        ; loop:
//   0x100C: subs r0, r0, #1
//   0x1010: bne  loop
//   0x1014: svc  #0                ; -> CallSVC -> HaltExecution
//
// Expected after Run(): r0=0, r1=16, Z=1.

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>

#include <emscripten.h>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

constexpr std::array<u32, 6> kCode = {
    0xE3A00005,
    0xE3A01001,
    0xE0811000,
    0xE2500001,
    0x1AFFFFFC,
    0xEF000000,
};
constexpr A32::VAddr kCodeBase = 0x1000;
constexpr size_t kRamSize = 1 << 20;  // 1 MiB guest RAM

// Guest RAM lives in the host module's linear memory.
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

class DemoCallbacks final : public A32::UserCallbacks {
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
    void InterpreterFallback(A32::VAddr, size_t) override {}
    void CallSVC(u32) override {
        std::printf("guest svc: halting\n");
        g_jit->HaltExecution();
    }
    void ExceptionRaised(A32::VAddr, A32::Exception) override {}
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
};

}  // namespace

int main() {
    std::memcpy(g_ram.data() + kCodeBase, kCode.data(), sizeof(kCode));

    static DemoCallbacks callbacks;
    static ExclusiveMonitor monitor{1};
    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.processor_id = 0;

    A32::Jit jit{conf};
    g_jit = &jit;

    jit.Regs()[15] = kCodeBase;
    jit.SetCpsr(0x1F);  // system mode, ARM state

    const HaltReason hr = jit.Run();

    const u32 cpsr = jit.Cpsr();
    std::printf("halt_reason=%u r0=%u r1=%u pc=%08x nzcv=%08x\n",
                static_cast<u32>(hr), jit.Regs()[0], jit.Regs()[1],
                jit.Regs()[15], cpsr & 0xF0000000);

    const bool ok = jit.Regs()[0] == 0 && jit.Regs()[1] == 16 && (cpsr & 0x40000000) != 0;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
