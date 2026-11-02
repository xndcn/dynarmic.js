// Emscripten host for differential fuzzing of the A64 JIT (tests/a64/fuzz.py).
//
// usage: node a64_fuzz.js <cases.bin> <results.bin> [mode]
//   mode 0: memory callbacks only, 1: page table, 2: page table + flat window
//
// cases.bin is a sequence of records (little-endian):
//   u32 num_words; u32 words[num_words]   code, placed at kCodeBase, followed
//                                          by an implicit `svc #0`
//   u64 x[31]; u64 sp; u32 nzcv; u32 fpcr; u32 fpsr; u32 pad
//   u64 v[64]                              V0..V31, low half first
//   u8 mem[kDataSize]                      contents of [kDataBase, +kDataSize)
// results.bin gets, per case:
//   u32 status (0 = ran to the svc, 1 = unsupported / exception / fallback)
//   u32 pad; u64 x[31]; u64 sp; u32 nzcv; u32 fpsr; u64 v[64]; u8 mem[kDataSize]

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

constexpr u64 kCodeBase = 0x1000;
constexpr u64 kDataBase = 0x100000;
constexpr size_t kDataSize = 0x1000;
constexpr size_t kRamSize = 1 << 22;  // 4 MiB, covers code and data
constexpr u32 kSvc0 = 0xD4000001;
constexpr size_t kPageTableBits = 32;

std::array<u8, kRamSize> g_ram{};
std::vector<void*> g_page_table;
A64::Jit* g_jit = nullptr;
bool g_failed = false;
u64 g_tpidr = 0;

template<typename T>
T ReadMem(u64 vaddr) {
    T value{};
    if (vaddr + sizeof(T) <= kRamSize) {
        std::memcpy(&value, g_ram.data() + vaddr, sizeof(T));
    }
    return value;
}

template<typename T>
void WriteMem(u64 vaddr, T value) {
    if (vaddr + sizeof(T) <= kRamSize) {
        std::memcpy(g_ram.data() + vaddr, &value, sizeof(T));
    }
}

class Callbacks final : public A64::UserCallbacks {
public:
    // Code outside the code page raises (a sequence that runs off its end
    // stops instead of executing zeros).
    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        if (vaddr < kCodeBase || vaddr >= kCodeBase + 0x1000) {
            return std::nullopt;
        }
        return ReadMem<u32>(vaddr);
    }
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

    void InterpreterFallback(u64, size_t) override {
        g_failed = true;
        g_jit->HaltExecution();
    }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(u64, A64::Exception) override {
        g_failed = true;
        g_jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
    u64 GetCNTPCT() override { return 0; }
};

struct CaseState {
    std::array<u64, 31> x;
    u64 sp;
    u32 nzcv, fpcr, fpsr, pad;
    std::array<u64, 64> v;
};
static_assert(sizeof(CaseState) == 31 * 8 + 8 + 16 + 64 * 8);

struct ResultState {
    u32 status, pad;
    std::array<u64, 31> x;
    u64 sp;
    u32 nzcv, fpsr;
    std::array<u64, 64> v;
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
    A64::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.enable_cycle_counting = false;
    conf.tpidr_el0 = &g_tpidr;
    const char mode = argc > 3 ? argv[3][0] : '0';
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

    size_t cases = 0;
    u32 num_words;
    while (std::fread(&num_words, 4, 1, in) == 1) {
        std::vector<u32> words(num_words);
        CaseState st;
        std::vector<u8> mem(kDataSize);
        if (std::fread(words.data(), 4, num_words, in) != num_words || std::fread(&st, sizeof(st), 1, in) != 1 ||
            std::fread(mem.data(), 1, kDataSize, in) != kDataSize) {
            std::fprintf(stderr, "truncated case %zu\n", cases);
            return 2;
        }

        // Unicorn maps the code page fresh for every case: clear it entirely
        // (an earlier case may have stored into it).
        std::memset(g_ram.data() + kCodeBase, 0, 0x1000);
        std::memcpy(g_ram.data() + kCodeBase, words.data(), 4 * num_words);
        WriteMem<u32>(kCodeBase + 4 * num_words, kSvc0);
        std::memcpy(g_ram.data() + kDataBase, mem.data(), kDataSize);
        // A store into the code page is self-modifying code, which Unicorn
        // picks up and dynarmic (without an invalidation) does not: such a
        // case counts as unsupported.
        const std::vector<u8> code_page(g_ram.begin() + kCodeBase, g_ram.begin() + kCodeBase + 0x1000);
        jit.ClearCache();
        jit.ClearExclusiveState();

        jit.SetRegisters(st.x);
        jit.SetSP(st.sp);
        jit.SetPC(kCodeBase);
        jit.SetPstate(st.nzcv);
        jit.SetFpcr(st.fpcr);
        jit.SetFpsr(st.fpsr);
        std::array<A64::Vector, 32> vecs;
        for (size_t i = 0; i < 32; i++) {
            vecs[i] = {st.v[2 * i], st.v[2 * i + 1]};
        }
        jit.SetVectors(vecs);

        g_failed = false;
        jit.Run();
        jit.ClearHalt(HaltReason::UserDefined1);

        ResultState r{};
        const bool smc = std::memcmp(code_page.data(), g_ram.data() + kCodeBase, code_page.size()) != 0;
        r.status = (g_failed || smc || jit.GetPC() != kCodeBase + 4 * num_words + 4) ? 1 : 0;
        r.x = jit.GetRegisters();
        r.sp = jit.GetSP();
        r.nzcv = jit.GetPstate();
        r.fpsr = jit.GetFpsr();
        const auto final_vecs = jit.GetVectors();
        for (size_t i = 0; i < 32; i++) {
            r.v[2 * i] = final_vecs[i][0];
            r.v[2 * i + 1] = final_vecs[i][1];
        }
        std::fwrite(&r, sizeof(r), 1, out);
        std::fwrite(g_ram.data() + kDataBase, 1, kDataSize, out);
        cases++;
    }
    std::fclose(in);
    std::fclose(out);
    std::printf("ran %zu cases\n", cases);
    return 0;
}
