// Private ABI for the experimental JavaScript package. Each factory owns one
// Emscripten module and one CPU. No JavaScript callback runs on the JIT stack.
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <emscripten.h>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace {
using namespace Dynarmic;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum Event : u32 { Halted, Svc, Budget, MemoryFault, UnsupportedInstruction, Exception, Step };
constexpr u32 kWrite = 0x100;
constexpr u32 kExecute = 0x200;
constexpr HaltReason kOwnedHalts = HaltReason::UserDefined1 | HaltReason::UserDefined2 |
    HaltReason::UserDefined3 | HaltReason::UserDefined4 | HaltReason::MemoryAbort | HaltReason::Step;

struct Machine;

class Callbacks32 final : public A32::UserCallbacks {
public:
    explicit Callbacks32(Machine& machine) : machine(machine) {}
    std::optional<u32> MemoryReadCode(u32 address) override;
    u8 MemoryRead8(u32 address) override;
    u16 MemoryRead16(u32 address) override;
    u32 MemoryRead32(u32 address) override;
    u64 MemoryRead64(u32 address) override;
    void MemoryWrite8(u32 address, u8 value) override;
    void MemoryWrite16(u32 address, u16 value) override;
    void MemoryWrite32(u32 address, u32 value) override;
    void MemoryWrite64(u32 address, u64 value) override;
    bool MemoryWriteExclusive8(u32 address, u8 value, u8 expected) override;
    bool MemoryWriteExclusive16(u32 address, u16 value, u16 expected) override;
    bool MemoryWriteExclusive32(u32 address, u32 value, u32 expected) override;
    bool MemoryWriteExclusive64(u32 address, u64 value, u64 expected) override;
    void InterpreterFallback(u32 pc, size_t instructions) override;
    void CallSVC(u32 immediate) override;
    void ExceptionRaised(u32 pc, A32::Exception exception) override;
    void AddTicks(u64 ticks) override;
    u64 GetTicksRemaining() override;
private:
    Machine& machine;
};

class Callbacks64 final : public A64::UserCallbacks {
public:
    explicit Callbacks64(Machine& machine) : machine(machine) {}
    std::optional<u32> MemoryReadCode(u64 address) override;
    u8 MemoryRead8(u64 address) override;
    u16 MemoryRead16(u64 address) override;
    u32 MemoryRead32(u64 address) override;
    u64 MemoryRead64(u64 address) override;
    A64::Vector MemoryRead128(u64 address) override;
    void MemoryWrite8(u64 address, u8 value) override;
    void MemoryWrite16(u64 address, u16 value) override;
    void MemoryWrite32(u64 address, u32 value) override;
    void MemoryWrite64(u64 address, u64 value) override;
    void MemoryWrite128(u64 address, A64::Vector value) override;
    bool MemoryWriteExclusive8(u64 address, u8 value, u8 expected) override;
    bool MemoryWriteExclusive16(u64 address, u16 value, u16 expected) override;
    bool MemoryWriteExclusive32(u64 address, u32 value, u32 expected) override;
    bool MemoryWriteExclusive64(u64 address, u64 value, u64 expected) override;
    bool MemoryWriteExclusive128(u64 address, A64::Vector value, A64::Vector expected) override;
    void InterpreterFallback(u64 pc, size_t instructions) override;
    void CallSVC(u32 immediate) override;
    void ExceptionRaised(u64 pc, A64::Exception exception) override;
    void AddTicks(u64 ticks) override;
    u64 GetTicksRemaining() override;
    u64 GetCNTPCT() override;
private:
    Machine& machine;
};

struct Machine {
    explicit Machine(u32 architecture, u32 size) : arch(architecture), ram(size) {}

    void Initialize() {
        if (arch == 0) {
            pages32 = std::make_unique<std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
            for (size_t page = 0; page < ram.size() / 4096; ++page) (*pages32)[page] = ram.data() + page * 4096;
            A32::UserConfig config;
            config.callbacks = &callbacks32;
            config.global_monitor = &monitor;
            config.enable_cycle_counting = true;
            config.page_table = pages32.get();
            config.fastmem_pointer = reinterpret_cast<uintptr_t>(ram.data());
            jit32 = std::make_unique<A32::Jit>(config);
            jit32->SetCpsr(0x1f); // Initial system mode, ARM state; callers may select Thumb.
        } else {
            pages64.resize(size_t{1} << (32 - 12), nullptr);
            for (size_t page = 0; page < ram.size() / 4096; ++page) pages64[page] = ram.data() + page * 4096;
            A64::UserConfig config;
            config.callbacks = &callbacks64;
            config.global_monitor = &monitor;
            config.enable_cycle_counting = true;
            config.page_table = pages64.data();
            config.page_table_address_space_bits = 32;
            config.silently_mirror_page_table = false;
            config.fastmem_pointer = reinterpret_cast<uintptr_t>(ram.data());
            jit64 = std::make_unique<A64::Jit>(config);
        }
    }

    bool Contains(u64 address, size_t size) const {
        return size <= ram.size() && address <= ram.size() - size;
    }

    u64 PC() const { return jit32 ? jit32->Regs()[15] : jit64 ? jit64->GetPC() : 0; }

    void Stop(Event kind, u32 detail, u64 address) {
        // Keep the first cause if a later instruction or tick callback also
        // requests a stop before the current translated block returns.
        if (event == Halted) { event = kind; event_detail = detail; event_address = address; }
        const HaltReason reason = kind == Svc ? HaltReason::UserDefined1 :
            kind == Budget ? HaltReason::UserDefined2 : kind == MemoryFault ? HaltReason::MemoryAbort :
            kind == UnsupportedInstruction ? HaltReason::UserDefined3 : HaltReason::UserDefined4;
        if (jit32) jit32->HaltExecution(reason);
        if (jit64) jit64->HaltExecution(reason);
    }

    std::optional<u32> Code(u64 address) const {
        // Translation can inspect a branch which will never execute. Returning
        // nullopt emits NoExecuteFault; halting here would stop valid paths.
        if (!Contains(address, sizeof(u32))) return std::nullopt;
        u32 value;
        std::memcpy(&value, ram.data() + static_cast<size_t>(address), sizeof(value));
        return value;
    }

    template<class T> T Read(u64 address) {
        T value{};
        if (!Contains(address, sizeof(T))) { Stop(MemoryFault, sizeof(T), address); return value; }
        std::memcpy(&value, ram.data() + static_cast<size_t>(address), sizeof(value));
        return value;
    }

    template<class T> void Write(u64 address, T value) {
        if (!Contains(address, sizeof(T))) { Stop(MemoryFault, kWrite | sizeof(T), address); return; }
        std::memcpy(ram.data() + static_cast<size_t>(address), &value, sizeof(value));
    }

    template<class T> bool WriteExclusive(u64 address, T value, T expected) {
        if (!Contains(address, sizeof(T))) { Stop(MemoryFault, kWrite | sizeof(T), address); return false; }
        T current;
        std::memcpy(&current, ram.data() + static_cast<size_t>(address), sizeof(current));
        if (current != expected) return false;
        std::memcpy(ram.data() + static_cast<size_t>(address), &value, sizeof(value));
        return true;
    }

    void AddTicks(u64 ticks) {
        if (ticks >= remaining) { remaining = 0; Stop(Budget, 0, PC()); }
        else remaining -= ticks;
    }
    u64 TicksRemaining() {
        if (!remaining) Stop(Budget, 0, PC());
        return remaining;
    }
    void ClearExclusive() {
        monitor.Clear();
        if (jit32) jit32->ClearExclusiveState();
        if (jit64) jit64->ClearExclusiveState();
    }

    u32 arch;
    std::vector<u8> ram;
    std::unique_ptr<std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> pages32;
    std::vector<void*> pages64;
    ExclusiveMonitor monitor{1};
    Callbacks32 callbacks32{*this};
    Callbacks64 callbacks64{*this};
    Event event = Halted;
    u32 event_detail = 0;
    u64 event_address = 0;
    u64 remaining = 0;
    bool running = false;
    // Destroy JITs (and their emitted modules) before callbacks, maps and RAM.
    std::unique_ptr<A32::Jit> jit32;
    std::unique_ptr<A64::Jit> jit64;
};

std::optional<u32> Callbacks32::MemoryReadCode(u32 address) { return machine.Code(address); }
std::optional<u32> Callbacks64::MemoryReadCode(u64 address) { return machine.Code(address); }
#define MEMORY_CALLBACKS(Class, Address, Bits, Type) \
    Type Class::MemoryRead##Bits(Address address) { return machine.Read<Type>(address); } \
    void Class::MemoryWrite##Bits(Address address, Type value) { machine.Write(address, value); } \
    bool Class::MemoryWriteExclusive##Bits(Address address, Type value, Type expected) { return machine.WriteExclusive(address, value, expected); }
MEMORY_CALLBACKS(Callbacks32, u32, 8, u8)
MEMORY_CALLBACKS(Callbacks32, u32, 16, u16)
MEMORY_CALLBACKS(Callbacks32, u32, 32, u32)
MEMORY_CALLBACKS(Callbacks32, u32, 64, u64)
MEMORY_CALLBACKS(Callbacks64, u64, 8, u8)
MEMORY_CALLBACKS(Callbacks64, u64, 16, u16)
MEMORY_CALLBACKS(Callbacks64, u64, 32, u32)
MEMORY_CALLBACKS(Callbacks64, u64, 64, u64)
MEMORY_CALLBACKS(Callbacks64, u64, 128, A64::Vector)
#undef MEMORY_CALLBACKS
void Callbacks32::InterpreterFallback(u32 pc, size_t count) { machine.Stop(UnsupportedInstruction, static_cast<u32>(count), pc); }
void Callbacks64::InterpreterFallback(u64 pc, size_t count) { machine.Stop(UnsupportedInstruction, static_cast<u32>(count), pc); }
void Callbacks32::CallSVC(u32 immediate) { machine.Stop(Svc, immediate, machine.PC()); }
void Callbacks64::CallSVC(u32 immediate) { machine.Stop(Svc, immediate, machine.PC()); }
void Callbacks32::ExceptionRaised(u32 pc, A32::Exception exception) {
    if (exception == A32::Exception::NoExecuteFault) machine.Stop(MemoryFault, kExecute | 4, pc);
    else machine.Stop(Exception, static_cast<u32>(exception), pc);
}
void Callbacks64::ExceptionRaised(u64 pc, A64::Exception exception) {
    if (exception == A64::Exception::NoExecuteFault) machine.Stop(MemoryFault, kExecute | 4, pc);
    else machine.Stop(Exception, static_cast<u32>(exception), pc);
}
void Callbacks32::AddTicks(u64 ticks) { machine.AddTicks(ticks); }
void Callbacks64::AddTicks(u64 ticks) { machine.AddTicks(ticks); }
u64 Callbacks32::GetTicksRemaining() { return machine.TicksRemaining(); }
u64 Callbacks64::GetTicksRemaining() { return machine.TicksRemaining(); }
u64 Callbacks64::GetCNTPCT() { return static_cast<u64>(emscripten_get_now() * 1000.0); }

std::unique_ptr<Machine> machine;
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE int dj_create(u32 arch, u32 ram_bytes) {
    if (machine || arch > 1 || ram_bytes < 65536 || ram_bytes > 268435456 || ram_bytes % 4096) return 1;
    auto next = std::make_unique<Machine>(arch, ram_bytes);
    next->Initialize();
    machine = std::move(next);
    return 0;
}
EMSCRIPTEN_KEEPALIVE void dj_destroy() { if (!machine || !machine->running) machine.reset(); }
EMSCRIPTEN_KEEPALIVE u32 dj_ram() { return machine ? static_cast<u32>(reinterpret_cast<uintptr_t>(machine->ram.data())) : 0; }

EMSCRIPTEN_KEEPALIVE u32 dj_run(u32 max_ticks, u32 step) {
    if (!machine || machine->running || step > 1) return static_cast<u32>(HaltReason::UserDefined4);
    auto& cpu = *machine;
    cpu.event = Halted; cpu.event_detail = 0; cpu.event_address = 0; cpu.remaining = max_ticks;
    if (cpu.jit32) cpu.jit32->ClearHalt(kOwnedHalts);
    if (cpu.jit64) cpu.jit64->ClearHalt(kOwnedHalts);
    if (!step && !max_ticks) {
        cpu.Stop(Budget, 0, cpu.PC());
        return static_cast<u32>(HaltReason::UserDefined2);
    }
    // Validate instruction alignment independently of memory fetch callbacks.
    const u64 alignment = cpu.jit32 && (cpu.jit32->Cpsr() & 0x20) ? 2 : 4;
    if (cpu.PC() % alignment || !cpu.Contains(cpu.PC(), alignment)) {
        cpu.Stop(MemoryFault, kExecute | static_cast<u32>(alignment), cpu.PC());
        return static_cast<u32>(HaltReason::MemoryAbort);
    }
    cpu.running = true;
    const HaltReason reason = cpu.jit32 ? (step ? cpu.jit32->Step() : cpu.jit32->Run()) :
                                         (step ? cpu.jit64->Step() : cpu.jit64->Run());
    cpu.running = false;
    if (cpu.event == Halted && step) { cpu.event = Step; cpu.event_address = cpu.PC(); }
    return static_cast<u32>(reason);
}

EMSCRIPTEN_KEEPALIVE u32 dj_event_kind() { return machine ? machine->event : Halted; }
EMSCRIPTEN_KEEPALIVE u32 dj_event_detail() { return machine ? machine->event_detail : 0; }
EMSCRIPTEN_KEEPALIVE u64 dj_event_address() { return machine ? machine->event_address : 0; }
EMSCRIPTEN_KEEPALIVE u64 dj_get_reg(u32 index) {
    if (!machine) return 0;
    if (machine->jit32) return index < 16 ? machine->jit32->Regs()[index] : 0;
    return index < 31 ? machine->jit64->GetRegister(index) : 0;
}
EMSCRIPTEN_KEEPALIVE void dj_set_reg(u32 index, u64 value) {
    if (!machine || machine->running) return;
    if (machine->jit32) { if (index < 16) machine->jit32->Regs()[index] = static_cast<u32>(value); }
    else if (index < 31) machine->jit64->SetRegister(index, value);
}
EMSCRIPTEN_KEEPALIVE u64 dj_get_pc() { return machine ? machine->PC() : 0; }
EMSCRIPTEN_KEEPALIVE void dj_set_pc(u64 value) {
    if (!machine || machine->running) return;
    if (machine->jit32) machine->jit32->Regs()[15] = static_cast<u32>(value);
    else machine->jit64->SetPC(value);
}
EMSCRIPTEN_KEEPALIVE u64 dj_get_sp() {
    if (!machine) return 0;
    return machine->jit32 ? machine->jit32->Regs()[13] : machine->jit64->GetSP();
}
EMSCRIPTEN_KEEPALIVE void dj_set_sp(u64 value) {
    if (!machine || machine->running) return;
    if (machine->jit32) machine->jit32->Regs()[13] = static_cast<u32>(value);
    else machine->jit64->SetSP(value);
}
EMSCRIPTEN_KEEPALIVE u32 dj_get_status() {
    if (!machine) return 0;
    return machine->jit32 ? machine->jit32->Cpsr() : machine->jit64->GetPstate();
}
EMSCRIPTEN_KEEPALIVE void dj_set_status(u32 value) {
    if (!machine || machine->running) return;
    if (machine->jit32) machine->jit32->SetCpsr(value);
    else machine->jit64->SetPstate(value);
}
EMSCRIPTEN_KEEPALIVE void dj_invalidate(u64 address, u32 size) {
    if (!machine || machine->running || !size) return;
    machine->ClearExclusive();
    if (machine->jit32) {
        if (address <= UINT32_MAX) machine->jit32->InvalidateCacheRange(static_cast<u32>(address), size);
    } else machine->jit64->InvalidateCacheRange(address, size);
}
EMSCRIPTEN_KEEPALIVE void dj_clear_cache() {
    if (!machine || machine->running) return;
    if (machine->jit32) machine->jit32->ClearCache();
    else machine->jit64->ClearCache();
}
} // extern "C"
