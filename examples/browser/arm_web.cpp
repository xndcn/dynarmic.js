// Browser host for the dynarmic WASM backend: a minimal ARM machine (16 MiB
// of flat RAM, SVC-based console) with either an A32 or an A64 CPU, exposed
// through a small C API for JS.
//
//   arm_init(mode, arch)     create the JIT. mode 0: memory callbacks,
//                            1: inline page-table access, 2: flat window
//                            (bounds check + direct load). arch 0: A32, 1: A64
//   arm_ram() / arm_ram_size()   guest RAM in linear memory (load programs here)
//   arm_run(pc)              run from pc until the guest halts; returns HaltReason
//   arm_reg(i)               low 32 bits of R<i> / X<i>
//   arm_console() / _len() / _clear()   text the guest printed via SVC
//   arm_marks() / arm_mark_count()      timestamps (ms) of the guest's SVC #3
//
// Guest "semihosting" (svc immediate): 0 = exit, 1 = putchar(r0 / x0),
// 2 = print r0 as 8 hex digits + newline, 3 = record a timestamp,
// 4 = r0 / x0 := host time in microseconds (wrapping u32).

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <emscripten.h>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

#ifndef DYNARMIC_JS_EXAMPLE_RAM_SIZE
#define DYNARMIC_JS_EXAMPLE_RAM_SIZE (1 << 24)
#endif
constexpr size_t kRamSize = DYNARMIC_JS_EXAMPLE_RAM_SIZE;
constexpr size_t kConsoleSize = 1 << 16;
constexpr size_t kMaxMarks = 64;
// A64 page table: 32-bit address space, 4 KiB pages.
constexpr size_t kA64PageTableBits = 32;

std::array<u8, kRamSize> g_ram{};
std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> g_page_table{};
std::vector<void*> g_page_table64;
std::array<char, kConsoleSize> g_console{};
u32 g_console_len = 0;
std::array<double, kMaxMarks> g_marks{};
u32 g_mark_count = 0;

std::unique_ptr<A32::Jit> g_jit32;
std::unique_ptr<A64::Jit> g_jit64;

void HaltAll(HaltReason reason = HaltReason::UserDefined1);

bool InRamRange(u64 vaddr, size_t size) {
    return size <= kRamSize && vaddr <= kRamSize - size;
}

template<typename T>
T ReadMem(u64 vaddr) {
    T value{};
    if (!InRamRange(vaddr, sizeof(T))) {
        HaltAll(HaltReason::UserDefined2);
        return value;
    }
    std::memcpy(&value, g_ram.data() + static_cast<size_t>(vaddr), sizeof(T));
    return value;
}

template<typename T>
void WriteMem(u64 vaddr, T value) {
    if (!InRamRange(vaddr, sizeof(T))) {
        HaltAll(HaltReason::UserDefined2);
        return;
    }
    std::memcpy(g_ram.data() + static_cast<size_t>(vaddr), &value, sizeof(T));
}

void ConsolePut(char c) {
    if (g_console_len < kConsoleSize) {
        g_console[g_console_len++] = c;
    }
}

// UserDefined1 is semihost exit; UserDefined2 is an unhandled guest fault.
void HaltAll(HaltReason reason) {
    if (g_jit32) {
        g_jit32->HaltExecution(reason);
    }
    if (g_jit64) {
        g_jit64->HaltExecution(reason);
    }
}

// Semihosting call `swi` with argument `arg` (r0 / x0); returns the new value
// of r0 / x0.
u32 Semihost(u32 swi, u32 arg) {
    switch (swi) {
    case 1:
        ConsolePut(static_cast<char>(arg & 0xFF));
        break;
    case 2:
        for (int shift = 28; shift >= 0; shift -= 4) {
            ConsolePut("0123456789abcdef"[(arg >> shift) & 0xF]);
        }
        ConsolePut('\n');
        break;
    case 3:
        if (g_mark_count < kMaxMarks) {
            g_marks[g_mark_count++] = emscripten_get_now();
        }
        break;
    case 4:
        return static_cast<u32>(static_cast<u64>(emscripten_get_now() * 1000.0));
    default:
        HaltAll();  // svc 0 (and anything unknown) = exit
        break;
    }
    return arg;
}

class Callbacks32 final : public A32::UserCallbacks {
public:
    std::optional<u32> MemoryReadCode(A32::VAddr vaddr) override {
        if (!InRamRange(vaddr, sizeof(u32))) return std::nullopt;
        return ReadMem<u32>(vaddr);
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
    void InterpreterFallback(A32::VAddr, size_t) override { HaltAll(HaltReason::UserDefined2); }
    void CallSVC(u32 swi) override { g_jit32->Regs()[0] = Semihost(swi, g_jit32->Regs()[0]); }
    void ExceptionRaised(A32::VAddr, A32::Exception) override { HaltAll(HaltReason::UserDefined2); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
};

class Callbacks64 final : public A64::UserCallbacks {
public:
    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        if (!InRamRange(vaddr, sizeof(u32))) return std::nullopt;
        return ReadMem<u32>(vaddr);
    }
    u8 MemoryRead8(u64 vaddr) override { return ReadMem<u8>(vaddr); }
    u16 MemoryRead16(u64 vaddr) override { return ReadMem<u16>(vaddr); }
    u32 MemoryRead32(u64 vaddr) override { return ReadMem<u32>(vaddr); }
    u64 MemoryRead64(u64 vaddr) override { return ReadMem<u64>(vaddr); }
    A64::Vector MemoryRead128(u64 vaddr) override { return ReadMem<A64::Vector>(vaddr); }
    void MemoryWrite8(u64 vaddr, u8 value) override { WriteMem(vaddr, value); }
    void MemoryWrite16(u64 vaddr, u16 value) override { WriteMem(vaddr, value); }
    void MemoryWrite32(u64 vaddr, u32 value) override { WriteMem(vaddr, value); }
    void MemoryWrite64(u64 vaddr, u64 value) override { WriteMem(vaddr, value); }
    void MemoryWrite128(u64 vaddr, A64::Vector value) override { WriteMem(vaddr, value); }

    void InterpreterFallback(u64, size_t) override { HaltAll(HaltReason::UserDefined2); }
    void CallSVC(u32 swi) override {
        const u64 x0 = g_jit64->GetRegister(0);
        const u32 result = Semihost(swi, static_cast<u32>(x0));
        if (result != static_cast<u32>(x0)) {
            g_jit64->SetRegister(0, result);
        }
    }
    void ExceptionRaised(u64, A64::Exception) override { HaltAll(HaltReason::UserDefined2); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
    u64 GetCNTPCT() override { return static_cast<u64>(emscripten_get_now() * 1000.0); }
};

void InitA32(int mode) {
    static Callbacks32 callbacks;
    static ExclusiveMonitor monitor{1};
    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.enable_cycle_counting = false;
    if (mode) {
        for (u32 i = 0; i < kRamSize / 4096; i++) {
            g_page_table[i] = g_ram.data() + i * 4096;
        }
        conf.page_table = &g_page_table;
        if (mode == 2) {
            conf.fastmem_pointer = reinterpret_cast<uintptr_t>(g_ram.data());
        }
    }
    g_jit32 = std::make_unique<A32::Jit>(conf);
}

void InitA64(int mode) {
    static Callbacks64 callbacks;
    static ExclusiveMonitor monitor{1};
    A64::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.enable_cycle_counting = false;
    if (mode) {
        g_page_table64.assign(size_t{1} << (kA64PageTableBits - 12), nullptr);
        for (size_t i = 0; i < kRamSize / 4096; i++) {
            g_page_table64[i] = g_ram.data() + i * 4096;
        }
        conf.page_table = g_page_table64.data();
        conf.page_table_address_space_bits = kA64PageTableBits;
        conf.silently_mirror_page_table = false;
        if (mode == 2) {
            conf.fastmem_pointer = reinterpret_cast<uintptr_t>(g_ram.data());
        }
    }
    g_jit64 = std::make_unique<A64::Jit>(conf);
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int arm_init(int mode, int arch) {
    if (mode < 0 || mode > 2 || arch < 0 || arch > 1) return -1;
    // This demo runs one guest at a time. Release its old code cache before
    // constructing a fresh CPU for the next request.
    g_jit32.reset();
    g_jit64.reset();
    if (arch == 1) {
        InitA64(mode);
    } else {
        InitA32(mode);
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE u8* arm_ram() { return g_ram.data(); }
EMSCRIPTEN_KEEPALIVE u32 arm_ram_size() { return static_cast<u32>(kRamSize); }

EMSCRIPTEN_KEEPALIVE int arm_run(u32 pc) {
    g_mark_count = 0;
    if ((!g_jit32 && !g_jit64) || !InRamRange(pc, sizeof(u32)) || (pc & 3) != 0) {
        return static_cast<int>(HaltReason::UserDefined2);
    }
    if (g_jit64) {
        g_jit64->SetPC(pc);
        g_jit64->SetSP(0);
        return static_cast<int>(g_jit64->Run());
    }
    g_jit32->Regs()[15] = pc;
    g_jit32->SetCpsr(0x1F);  // system mode, ARM state
    return static_cast<int>(g_jit32->Run());
}

EMSCRIPTEN_KEEPALIVE u32 arm_reg(int index) {
    if (index < 0) return 0;
    if (g_jit64) {
        return index < 31 ? static_cast<u32>(g_jit64->GetRegister(static_cast<size_t>(index))) : 0;
    }
    return g_jit32 && index < 16 ? g_jit32->Regs()[index] : 0;
}

EMSCRIPTEN_KEEPALIVE const char* arm_console() { return g_console.data(); }
EMSCRIPTEN_KEEPALIVE u32 arm_console_len() { return g_console_len; }
EMSCRIPTEN_KEEPALIVE void arm_console_clear() { g_console_len = 0; }
EMSCRIPTEN_KEEPALIVE const double* arm_marks() { return g_marks.data(); }
EMSCRIPTEN_KEEPALIVE u32 arm_mark_count() { return g_mark_count; }

}  // extern "C"
