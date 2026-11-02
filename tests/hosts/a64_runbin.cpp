// Emscripten host: run a bare-metal A64 flat binary (linked at 0x10000)
// through the WASM backend and check the result region written by the guest
// (same protocol as jit_runbin: checksums at 0x80000, magic at 0x80040).
//
// usage: node a64_runbin.js <binary> [mode]
//   mode 0: memory callbacks only, 1: page table, 2: page table + flat window
// exit 0 and "PASS-marker" iff the guest wrote the magic value.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <emscripten.h>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

constexpr u64 kLoadBase = 0x10000;
constexpr size_t kRamSize = 1 << 24;  // 16 MiB
constexpr u64 kResultAddr = 0x80000;
constexpr u64 kMagicAddr = 0x80040;
constexpr u32 kMagic = 0x600DC0DEu;
constexpr size_t kPageTableBits = 32;

std::array<u8, kRamSize> g_ram{};
std::vector<void*> g_page_table;

A64::Jit* g_jit = nullptr;
u64 g_tpidr = 0;

template<typename T>
T ReadMem(u64 vaddr) {
    T value;
    std::memcpy(&value, g_ram.data() + (vaddr % kRamSize), sizeof(T));
    return value;
}

template<typename T>
void WriteMem(u64 vaddr, T value) {
    std::memcpy(g_ram.data() + (vaddr % kRamSize), &value, sizeof(T));
}

class Callbacks final : public A64::UserCallbacks {
public:
    u8 MemoryRead8(u64 vaddr) override { return ReadMem<u8>(vaddr); }
    u16 MemoryRead16(u64 vaddr) override { return ReadMem<u16>(vaddr); }
    u32 MemoryRead32(u64 vaddr) override { return ReadMem<u32>(vaddr); }
    u64 MemoryRead64(u64 vaddr) override { return ReadMem<u64>(vaddr); }
    A64::Vector MemoryRead128(u64 vaddr) override { return {ReadMem<u64>(vaddr), ReadMem<u64>(vaddr + 8)}; }
    void MemoryWrite8(u64 vaddr, u8 value) override { WriteMem(vaddr, value); }
    void MemoryWrite16(u64 vaddr, u16 value) override { WriteMem(vaddr, value); }
    void MemoryWrite32(u64 vaddr, u32 value) override { WriteMem(vaddr, value); }
    void MemoryWrite64(u64 vaddr, u64 value) override { WriteMem(vaddr, value); }
    void MemoryWrite128(u64 vaddr, A64::Vector value) override {
        WriteMem(vaddr, value[0]);
        WriteMem(vaddr + 8, value[1]);
    }
    bool MemoryWriteExclusive8(u64 vaddr, u8 value, u8) override { return WriteMem(vaddr, value), true; }
    bool MemoryWriteExclusive16(u64 vaddr, u16 value, u16) override { return WriteMem(vaddr, value), true; }
    bool MemoryWriteExclusive32(u64 vaddr, u32 value, u32) override { return WriteMem(vaddr, value), true; }
    bool MemoryWriteExclusive64(u64 vaddr, u64 value, u64) override { return WriteMem(vaddr, value), true; }
    bool MemoryWriteExclusive128(u64 vaddr, A64::Vector value, A64::Vector) override {
        return MemoryWrite128(vaddr, value), true;
    }

    void InterpreterFallback(u64 pc, size_t) override {
        std::printf("  INTERPRETER FALLBACK at pc=%016llx\n", static_cast<unsigned long long>(pc));
        g_jit->HaltExecution();
    }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(u64 pc, A64::Exception e) override {
        std::printf("  EXCEPTION %u at pc=%016llx\n", static_cast<unsigned>(e), static_cast<unsigned long long>(pc));
        g_jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
    u64 GetCNTPCT() override { return 0; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <binary> [mode]\n", argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror("fopen");
        return 2;
    }
    const size_t n = std::fread(g_ram.data() + kLoadBase, 1, kRamSize - kLoadBase, f);
    std::fclose(f);
    std::printf("loaded %zu bytes at %#llx\n", n, static_cast<unsigned long long>(kLoadBase));

    static Callbacks callbacks;
    static ExclusiveMonitor monitor{1};

    A64::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.processor_id = 0;
    conf.enable_cycle_counting = false;
    conf.tpidr_el0 = &g_tpidr;
    const char mode = argc > 2 ? argv[2][0] : '0';
    if (mode == '1' || mode == '2') {
        g_page_table.assign(size_t{1} << (kPageTableBits - 12), nullptr);
        for (size_t i = 0; i < kRamSize / 4096; i++) {
            g_page_table[i] = g_ram.data() + i * 4096;
        }
        conf.page_table = g_page_table.data();
        conf.page_table_address_space_bits = kPageTableBits;
        conf.silently_mirror_page_table = false;
        if (mode == '2') {
            conf.fastmem_pointer = reinterpret_cast<uintptr_t>(g_ram.data());
        }
    }

    A64::Jit jit{conf};
    g_jit = &jit;
    jit.SetPC(kLoadBase);

    const double t0 = emscripten_get_now();
    const HaltReason hr = jit.Run();
    const double ms = emscripten_get_now() - t0;

    const u32 magic = ReadMem<u32>(kMagicAddr);
    std::printf("hr=%u pc=%016llx  magic=%08x  (%.1f ms)\n",
                static_cast<u32>(hr), static_cast<unsigned long long>(jit.GetPC()), magic, ms);
    EM_ASM({
        const st = Module.dynarmicWasmStats;
        if (st)
            console.log(`blocks=${st.blocks} bytes=${st.bytes} compile=${st.compileMs.toFixed(1)}ms inst=${st.instMs.toFixed(1)}ms table=${st.tableMs.toFixed(1)}ms | translate=${(st.translateMs||0).toFixed(1)} opt=${(st.optMs||0).toFixed(1)} emit=${(st.emitMs||0).toFixed(1)}ms`);
    });
    for (int s = 0; s < 7; s++) {
        std::printf("result%d %016llx\n", s, static_cast<unsigned long long>(ReadMem<u64>(kResultAddr + s * 8)));
    }
    if (magic == kMagic) {
        std::printf("PASS-marker\n");
        return 0;
    }
    return 1;
}
