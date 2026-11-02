// Minimal native x86-64 dynarmic host for the pinned A64 CoreMark guest.
// Memory mode: 0 = callbacks, 1 = page table, 2 = flat-window fastmem.
// Guest ABI: svc #0 exits, svc #1 writes one byte, svc #4 returns wrapping
// CLOCK_MONOTONIC-equivalent microseconds in x0.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {
constexpr size_t kRamSize = 16 * 1024 * 1024;
constexpr u64 kGuestAddress = 0x10000;
constexpr size_t kPageSize = 4096;

std::array<u8, kRamSize> ram{};
std::vector<void*> page_table;
std::unique_ptr<A64::Jit> jit;
const auto clock_origin = std::chrono::steady_clock::now();

u64 ClockMicroseconds() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - clock_origin).count());
}

template<typename T>
T ReadMemory(u64 address) {
    T value;
    std::memcpy(&value, ram.data() + (address % kRamSize), sizeof(value));
    return value;
}

template<typename T>
void WriteMemory(u64 address, T value) {
    std::memcpy(ram.data() + (address % kRamSize), &value, sizeof(value));
}

class Callbacks final : public A64::UserCallbacks {
public:
    u8 MemoryRead8(u64 address) override { return ReadMemory<u8>(address); }
    u16 MemoryRead16(u64 address) override { return ReadMemory<u16>(address); }
    u32 MemoryRead32(u64 address) override { return ReadMemory<u32>(address); }
    u64 MemoryRead64(u64 address) override { return ReadMemory<u64>(address); }
    A64::Vector MemoryRead128(u64 address) override {
        return {ReadMemory<u64>(address), ReadMemory<u64>(address + 8)};
    }
    void MemoryWrite8(u64 address, u8 value) override { WriteMemory(address, value); }
    void MemoryWrite16(u64 address, u16 value) override { WriteMemory(address, value); }
    void MemoryWrite32(u64 address, u32 value) override { WriteMemory(address, value); }
    void MemoryWrite64(u64 address, u64 value) override { WriteMemory(address, value); }
    void MemoryWrite128(u64 address, A64::Vector value) override {
        WriteMemory(address, value[0]);
        WriteMemory(address + 8, value[1]);
    }

    void InterpreterFallback(u64, size_t) override { jit->HaltExecution(HaltReason::UserDefined2); }
    void ExceptionRaised(u64, A64::Exception) override { jit->HaltExecution(HaltReason::UserDefined2); }
    void CallSVC(u32 immediate) override {
        switch (immediate) {
        case 0:
            jit->HaltExecution(HaltReason::UserDefined1);
            return;
        case 1:
            std::fputc(static_cast<unsigned char>(jit->GetRegister(0)), stdout);
            return;
        case 4:
            jit->SetRegister(0, static_cast<u32>(ClockMicroseconds()));
            return;
        default:
            std::fprintf(stderr, "unsupported guest SVC: %u\n", immediate);
            jit->HaltExecution(HaltReason::UserDefined2);
            return;
        }
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1ULL << 20; }
    u64 GetCNTPCT() override { return ClockMicroseconds(); }
};

bool LoadGuest(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::fprintf(stderr, "cannot open guest: %s\n", path);
        return false;
    }
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<u64>(size) > kRamSize - kGuestAddress) {
        std::fprintf(stderr, "guest does not fit in RAM: %s\n", path);
        return false;
    }
    input.seekg(0);
    input.read(reinterpret_cast<char*>(ram.data() + kGuestAddress), size);
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        std::fprintf(stderr, "cannot read complete guest: %s\n", path);
        return false;
    }
    std::fprintf(stderr, "DYNARMIC_GUEST_BYTES=%lld\n", static_cast<long long>(size));
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s GUEST.bin [MEMORY_MODE=2]\n", argv[0]);
        return 64;
    }
    const int mode = argc == 3 ? std::atoi(argv[2]) : 2;
    if (mode < 0 || mode > 2 || (argc == 3 && std::to_string(mode) != argv[2])) {
        std::fprintf(stderr, "invalid memory mode; expected 0, 1 or 2\n");
        return 64;
    }
    if (!LoadGuest(argv[1])) return 65;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Callbacks callbacks;
    ExclusiveMonitor monitor{1};
    A64::UserConfig config;
    config.callbacks = &callbacks;
    config.global_monitor = &monitor;
    config.enable_cycle_counting = false;
    if (mode != 0) {
        page_table.assign(size_t{1} << 20, nullptr); // 32-bit address space, 4 KiB pages.
        for (size_t index = 0; index < kRamSize / kPageSize; ++index) {
            page_table[index] = ram.data() + index * kPageSize;
        }
        config.page_table = page_table.data();
        config.page_table_address_space_bits = 32;
        config.silently_mirror_page_table = false;
        if (mode == 2) config.fastmem_pointer = reinterpret_cast<uintptr_t>(ram.data());
    }
    jit = std::make_unique<A64::Jit>(config);
    jit->SetPC(kGuestAddress);
    jit->SetSP(0);
    std::fprintf(stderr, "DYNARMIC_MEMORY_MODE=%d\n", mode);
    const auto run_start = std::chrono::steady_clock::now();
    const HaltReason reason = jit->Run();
    const double run_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - run_start).count();
    std::fprintf(stderr, "DYNARMIC_RUN_MS=%.6f\nDYNARMIC_HALT_REASON=0x%08x\n",
                 run_ms, static_cast<unsigned>(reason));
    if (reason != HaltReason::UserDefined1) {
        std::fprintf(stderr, "guest did not exit normally\n");
        return 70;
    }
    return 0;
}
