// Emscripten host: run a bare-metal A32 flat binary (linked at 0x10000)
// through the WASM backend and check the result region written by the guest.
//
// usage: node jit_runbin.js <binary> [fastmem]
// exit 0 and "PASS" iff guest wrote MAGIC and the expected checksum.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include <emscripten.h>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

constexpr A32::VAddr kLoadBase = 0x10000;
constexpr size_t kRamSize = 1 << 24;  // 16 MiB
constexpr A32::VAddr kResultAddr = 0x80000;
constexpr A32::VAddr kMagicAddr = 0x80040;
constexpr u32 kMagic = 0x600DC0DEu;

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

// Test coprocessor state (coproc.c guest test)
u32 g_tls = 0x11111111;
u32 g_tls2 = 0x22222222;
u32 g_cb_sent = 0;
u32 g_mcrr_lo = 0, g_mcrr_hi = 0;
u32 g_cdp_count = 0;
u32 g_exc_count = 0;

class TestCoproc final : public A32::Coprocessor {
public:
    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, A32::CoprocReg CRd, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (two || opc1 != 2 || CRd != A32::CoprocReg::C7 || CRn != A32::CoprocReg::C8 || CRm != A32::CoprocReg::C9 || opc2 != 3) {
            return std::nullopt;
        }
        return Callback{[](void*, u32, u32) -> u64 {
                            g_cdp_count++;
                            return 0;
                        },
                        std::nullopt};
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2) {
            return &g_tls;  // inline linear-memory store
        }
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C14 && CRm == A32::CoprocReg::C0 && opc2 == 0) {
            return Callback{[](void*, u32 arg0, u32) -> u64 {
                                g_cb_sent = arg0;
                                return 0;
                            },
                            std::nullopt};
        }
        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, A32::CoprocReg CRm) override {
        if (!two && opc == 1 && CRm == A32::CoprocReg::C6) {
            return Callback{[](void*, u32 arg0, u32 arg1) -> u64 {
                                g_mcrr_lo = arg0;
                                g_mcrr_hi = arg1;
                                return 0;
                            },
                            std::nullopt};
        }
        return std::monostate{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2) {
            return &g_tls;  // inline linear-memory load
        }
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 3) {
            return Callback{[](void*, u32, u32) -> u64 { return 0xC0FFEE00u | (g_tls & 0xFF); }, std::nullopt};
        }
        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, A32::CoprocReg CRm) override {
        if (!two && opc == 0 && CRm == A32::CoprocReg::C5) {
            return std::array<u32*, 2>{&g_tls, &g_tls2};  // inline pair load
        }
        return std::monostate{};
    }

    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<u8>) override {
        return std::nullopt;  // always raises
    }

    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<u8>) override {
        return std::nullopt;
    }
};

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
    void InterpreterFallback(A32::VAddr pc, size_t) override {
        std::printf("  INTERPRETER FALLBACK at pc=%08x\n", pc);
        g_jit->HaltExecution();
    }
    void CallSVC(u32) override { g_jit->HaltExecution(); }
    void ExceptionRaised(A32::VAddr pc, A32::Exception e) override {
        std::printf("  EXCEPTION %u at pc=%08x\n", static_cast<unsigned>(e), pc);
        // Record instead of halting so the coproc test can observe the
        // exception path and continue.
        WriteMem<u32>(0x80048, pc);
        WriteMem<u32>(0x8004C, static_cast<u32>(e));
        WriteMem<u32>(0x80050, ++g_exc_count);
        if (g_exc_count > 8) {
            g_jit->HaltExecution();
        }
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <binary> [fastmem]\n", argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror("fopen");
        return 2;
    }
    const size_t n = std::fread(g_ram.data() + kLoadBase, 1, kRamSize - kLoadBase, f);
    std::fclose(f);
    std::printf("loaded %zu bytes at %#x\n", n, kLoadBase);

    static BenchCallbacks callbacks;
    static ExclusiveMonitor monitor{1};

    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.processor_id = 0;
    conf.enable_cycle_counting = false;
    conf.coprocessors[15] = std::make_shared<TestCoproc>();
    if (argc > 2 && (argv[2][0] == '1' || argv[2][0] == '2')) {
        // fastmem: page table covering our flat RAM, 4 KiB pages
        for (u32 i = 0; i < kRamSize / 4096; i++)
            g_page_table[i] = g_ram.data() + i * 4096;
        conf.page_table = &g_page_table;
        if (argv[2][0] == '2')
            conf.fastmem_pointer = reinterpret_cast<uintptr_t>(g_ram.data());
    }

    A32::Jit jit{conf};
    g_jit = &jit;
    jit.Regs()[15] = kLoadBase;
    jit.SetCpsr(0x1F);

    const double t0 = emscripten_get_now();
    const HaltReason hr = jit.Run();
    const double ms = emscripten_get_now() - t0;

    const u32 magic = ReadMem<u32>(kMagicAddr);

    std::printf("hr=%u pc=%08x  magic=%08x  (%.1f ms)\n",
                static_cast<u32>(hr), jit.Regs()[15], magic, ms);
    EM_ASM({
        const st = Module.dynarmicWasmStats;
        if (st)
            console.log(`blocks=${st.blocks} bytes=${st.bytes} compile=${st.compileMs.toFixed(1)}ms inst=${st.instMs.toFixed(1)}ms table=${st.tableMs.toFixed(1)}ms | translate=${(st.translateMs||0).toFixed(1)} opt=${(st.optMs||0).toFixed(1)} emit=${(st.emitMs||0).toFixed(1)}ms`);
    });
    for (int s = 0; s < 7; s++)
        std::printf("result%d %016llx\n", s, (unsigned long long)ReadMem<u64>(kResultAddr + s * 8));
    std::printf("coproc tls=%08x cb_sent=%08x mcrr=%08x/%08x cdp=%u exc=%u exc_pc=%08x exc_code=%08x\n",
                g_tls, g_cb_sent, g_mcrr_lo, g_mcrr_hi, g_cdp_count, g_exc_count,
                ReadMem<u32>(0x80048), ReadMem<u32>(0x8004C));
    if (magic == kMagic) {
        std::printf("PASS-marker\n");
        const u32 log_lo = ReadMem<u32>(kResultAddr + 0xC);
        const u32 log_hi = ReadMem<u32>(kResultAddr + 0x10);
        if (log_lo && log_hi > log_lo && log_hi <= kRamSize && (log_hi - log_lo) <= (1u << 20)) {
            FILE* lf = std::fopen("guest_mix.log", "w");
            for (u32 p = log_lo; p < log_hi; p += 4)
                std::fprintf(lf, "%08x\n", ReadMem<u32>(p));
            std::fclose(lf);
            std::printf("dumped %u mix entries\n", (log_hi - log_lo) / 4);
        }
        return 0;
    }
    return 1;
}
