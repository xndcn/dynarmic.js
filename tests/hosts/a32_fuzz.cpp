// Emscripten host for differential fuzzing of the A32 JIT (tests/a32/fuzz.py).
//
// usage: node a32_fuzz.js <cases.bin> <results.bin> [mode]
//   mode 0: memory callbacks only, 1: page table, 2: page table + flat window
//
// cases.bin is a sequence of records (little-endian):
//   u32 thumb; u32 num_bytes; u8 code[num_bytes] (padded to 4)   placed at
//       kCodeBase and followed by an implicit `svc #0` (ARM or Thumb)
//   u32 r[15] (R0..R14); u32 cpsr; u32 fpscr; u32 pad
//   u32 ext[64]                            S0..S63 / D0..D31
//   u8 mem[kDataSize]                      contents of [kDataBase, +kDataSize)
// results.bin gets, per case:
//   u32 status (0 = ran to the svc, 1 = unsupported / exception / fallback)
//   u32 r[16]; u32 cpsr; u32 fpscr; u32 ext[64]; u8 mem[kDataSize]

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

constexpr u32 kCodeBase = 0x1000;
constexpr u32 kDataBase = 0x100000;
constexpr size_t kDataSize = 0x1000;
constexpr size_t kRamSize = 1 << 22;  // 4 MiB, covers code and data

std::array<u8, kRamSize> g_ram{};
std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> g_page_table{};
A32::Jit* g_jit = nullptr;
bool g_failed = false;

template<typename T>
T ReadMem(u32 vaddr) {
    T value{};
    if (vaddr + sizeof(T) <= kRamSize) {
        std::memcpy(&value, g_ram.data() + vaddr, sizeof(T));
    }
    return value;
}

template<typename T>
void WriteMem(u32 vaddr, T value) {
    if (vaddr + sizeof(T) <= kRamSize) {
        std::memcpy(g_ram.data() + vaddr, &value, sizeof(T));
    }
}

class Callbacks final : public A32::UserCallbacks {
public:
    // Code outside the code page raises (a sequence that runs off its end
    // stops instead of executing zeros).
    std::optional<u32> MemoryReadCode(u32 vaddr) override {
        if (vaddr < kCodeBase || vaddr >= kCodeBase + 0x1000) {
            return std::nullopt;
        }
        return ReadMem<u32>(vaddr);
    }
    u8 MemoryRead8(u32 vaddr) override { return ReadMem<u8>(vaddr); }
    u16 MemoryRead16(u32 vaddr) override { return ReadMem<u16>(vaddr); }
    u32 MemoryRead32(u32 vaddr) override { return ReadMem<u32>(vaddr); }
    u64 MemoryRead64(u32 vaddr) override { return ReadMem<u64>(vaddr); }
    void MemoryWrite8(u32 vaddr, u8 value) override { WriteMem(vaddr, value); }
    void MemoryWrite16(u32 vaddr, u16 value) override { WriteMem(vaddr, value); }
    void MemoryWrite32(u32 vaddr, u32 value) override { WriteMem(vaddr, value); }
    void MemoryWrite64(u32 vaddr, u64 value) override { WriteMem(vaddr, value); }
    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8) override { return WriteMem(vaddr, value), true; }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16) override { return WriteMem(vaddr, value), true; }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32) override { return WriteMem(vaddr, value), true; }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64) override { return WriteMem(vaddr, value), true; }

    void InterpreterFallback(u32, size_t) override {
        g_failed = true;
        g_jit->HaltExecution();
    }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(u32, A32::Exception) override {
        g_failed = true;
        g_jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
};

struct CaseState {
    std::array<u32, 15> r;
    u32 cpsr, fpscr, pad;
    std::array<u32, 64> ext;
};

struct ResultState {
    u32 status;
    std::array<u32, 16> r;
    u32 cpsr, fpscr;
    std::array<u32, 64> ext;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <cases.bin> <results.bin> [mode]\n", argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    FILE* out = std::fopen(argv[2], "wb");
    if (!in || !out) {
        std::perror("fopen");
        return 2;
    }

    static Callbacks callbacks;
    static ExclusiveMonitor monitor{1};
    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.enable_cycle_counting = false;
    conf.arch_version = A32::ArchVersion::v8;
    const char mode = argc > 3 ? argv[3][0] : '0';
    if (mode == '1' || mode == '2') {
        for (size_t i = 0; i < kRamSize / 4096; i++) {
            g_page_table[i] = g_ram.data() + i * 4096;
        }
        conf.page_table = &g_page_table;
        if (mode == '2') {
            conf.fastmem_pointer = reinterpret_cast<uintptr_t>(g_ram.data());
        }
    }

    A32::Jit jit{conf};
    g_jit = &jit;

    size_t cases = 0;
    u32 header[2];
    while (std::fread(header, 4, 2, in) == 2) {
        const bool thumb = header[0] != 0;
        const u32 num_bytes = header[1];
        std::vector<u8> code((num_bytes + 3) & ~3u);
        CaseState st;
        std::vector<u8> mem(kDataSize);
        if (std::fread(code.data(), 1, code.size(), in) != code.size() || std::fread(&st, sizeof(st), 1, in) != 1 ||
            std::fread(mem.data(), 1, kDataSize, in) != kDataSize) {
            std::fprintf(stderr, "truncated case %zu\n", cases);
            return 2;
        }

        // Unicorn maps the code page fresh for every case: clear it entirely.
        std::memset(g_ram.data() + kCodeBase, 0, 0x1000);
        std::memcpy(g_ram.data() + kCodeBase, code.data(), num_bytes);
        const u32 end = kCodeBase + num_bytes;
        if (thumb) {
            WriteMem<u16>(end, 0xDF00);  // svc #0
        } else {
            WriteMem<u32>(end, 0xEF000000);  // svc #0
        }
        std::memcpy(g_ram.data() + kDataBase, mem.data(), kDataSize);
        // A store into the code page is self-modifying code, which Unicorn
        // picks up and dynarmic (without an invalidation) does not: such a
        // case counts as unsupported.
        const std::vector<u8> code_page(g_ram.begin() + kCodeBase, g_ram.begin() + kCodeBase + 0x1000);
        jit.ClearCache();
        jit.ClearExclusiveState();

        for (size_t i = 0; i < 15; i++) {
            jit.Regs()[i] = st.r[i];
        }
        jit.Regs()[15] = kCodeBase;
        jit.SetCpsr(st.cpsr);
        jit.SetFpscr(st.fpscr);
        std::memcpy(jit.ExtRegs().data(), st.ext.data(), sizeof(st.ext));

        g_failed = false;
        jit.Run();
        jit.ClearHalt(HaltReason::UserDefined1);

        ResultState r{};
        const bool smc = std::memcmp(code_page.data(), g_ram.data() + kCodeBase, code_page.size()) != 0;
        r.status = (g_failed || smc || jit.Regs()[15] != end + (thumb ? 2 : 4)) ? 1 : 0;
        for (size_t i = 0; i < 16; i++) {
            r.r[i] = jit.Regs()[i];
        }
        r.cpsr = jit.Cpsr();
        r.fpscr = jit.Fpscr();
        std::memcpy(r.ext.data(), jit.ExtRegs().data(), sizeof(r.ext));
        std::fwrite(&r, sizeof(r), 1, out);
        std::fwrite(g_ram.data() + kDataBase, 1, kDataSize, out);
        cases++;
    }
    std::fclose(in);
    std::fclose(out);
    std::printf("ran %zu cases\n", cases);
    return 0;
}
