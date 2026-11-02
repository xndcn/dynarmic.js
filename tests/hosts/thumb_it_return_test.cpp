// Reduced from QuickJS/newlib strlen: revisit one ARM BLX call site with the
// Thumb IT condition first true, then false. The callee must restore SP exactly.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>
#include <mcl/stdint.hpp>
#include "dynarmic/interface/A32/a32.h"

using namespace Dynarmic;
namespace {
constexpr u32 kRamSize = 1 << 24;
constexpr u32 kStack = 0xff0000;
constexpr std::array<u32, 22> code{
    0xe3a0d8ff, 0xe3a04044, 0xe3a05055, 0xe3a00000, 0xe3a06000, 0xe3a03cff,
    // loop: mov r2,r6; blx tail; mov r0,sp; svc #2; ++r6; loop until r6==3
    0xe1a02006, 0xfa000006, 0xe1a0000d, 0xef000002, 0xe2866001, 0xe3560003,
    0x1afffff8, 0xe3a00000, 0xef000000,
    // Thumb tail: strd r4,r5,[sp,#-8]!; cmp r2,#0; itt eq; add r0,#4;
    // mov r2,r3; rev r2,r2; clz r2,r2; ldrd r4,r5,[sp],#8;
    // add.w r0,r0,r2,lsr #3; bx lr
    0x4502e96d, 0xbf042a00, 0x461a3004, 0xfab2ba12, 0xe8fdf282,
    0xeb004502, 0x477000d2,
};
class Callbacks final : public A32::UserCallbacks {
public:
    std::vector<u8> ram = std::vector<u8>(kRamSize);
    A32::Jit* jit = nullptr;
    unsigned observations = 0;
    bool fault = false;
    template<class T> T Read(u32 address) {
        T value{};
        if (address > kRamSize - sizeof(T)) { fault = true; jit->HaltExecution(); }
        else std::memcpy(&value, ram.data() + address, sizeof(T));
        return value;
    }
    template<class T> void Write(u32 address, T value) {
        if (address > kRamSize - sizeof(T)) { fault = true; jit->HaltExecution(); }
        else std::memcpy(ram.data() + address, &value, sizeof(T));
    }
    std::optional<u32> MemoryReadCode(u32 a) override {
        if (a > kRamSize - sizeof(u32)) return std::nullopt;
        return Read<u32>(a);
    }
    u8 MemoryRead8(u32 a) override { return Read<u8>(a); }
    u16 MemoryRead16(u32 a) override { return Read<u16>(a); }
    u32 MemoryRead32(u32 a) override { return Read<u32>(a); }
    u64 MemoryRead64(u32 a) override { return Read<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { Write(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { Write(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { Write(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { Write(a, v); }
    void CallSVC(u32 imm) override {
        if (imm == 2) {
            ++observations;
            if (jit->Regs()[0] != kStack || jit->Regs()[13] != kStack ||
                jit->Regs()[4] != 0x44 || jit->Regs()[5] != 0x55) {
                std::fprintf(stderr, "Thumb IT return %u: SP=%08x r4=%08x r5=%08x\n",
                             observations, jit->Regs()[13], jit->Regs()[4], jit->Regs()[5]);
                fault = true;
                jit->HaltExecution();
            }
        } else {
            if (imm != 0) fault = true;
            jit->HaltExecution();
        }
    }
    void InterpreterFallback(u32 pc, size_t) override { std::fprintf(stderr, "fallback %08x\n", pc); fault = true; jit->HaltExecution(); }
    void ExceptionRaised(u32 pc, A32::Exception e) override { std::fprintf(stderr, "exception %08x %d\n", pc, static_cast<int>(e)); fault = true; jit->HaltExecution(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 100000; }
};
void Check(int mode, bool cycles, bool optimize) {
    Callbacks cb;
    std::memcpy(cb.ram.data() + 0x10000, code.data(), sizeof(code));
    auto table = std::make_unique<std::array<u8*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
    A32::UserConfig conf;
    conf.callbacks = &cb;
    conf.enable_cycle_counting = cycles;
    if (!optimize) conf.optimizations = no_optimizations;
    if (mode != 0) {
        for (u32 page = 0; page < kRamSize / 4096; ++page) (*table)[page] = cb.ram.data() + page * 4096;
        conf.page_table = table.get();
        if (mode == 2) conf.fastmem_pointer = reinterpret_cast<uintptr_t>(cb.ram.data());
    }
    A32::Jit jit(conf);
    cb.jit = &jit;
    jit.Regs()[15] = 0x10000;
    jit.SetCpsr(0x1f);
    jit.Run();
    if (cb.fault || cb.observations != 3 || jit.Regs()[0] != 0) {
        std::fprintf(stderr, "FAIL Thumb IT return mode=%d cycles=%d optimize=%d fault=%d observations=%u PC=%08x SP=%08x r0=%08x\n", mode, cycles, optimize, cb.fault, cb.observations, jit.Regs()[15], jit.Regs()[13], jit.Regs()[0]);
        std::exit(1);
    }
}
void CheckDescriptor(const char* name, std::initializer_list<u16> instructions,
                     bool thumb, bool cycles, bool optimize) {
    Callbacks cb;
    std::memcpy(cb.ram.data() + 0x10000, instructions.begin(), instructions.size() * sizeof(u16));
    A32::UserConfig conf;
    conf.callbacks = &cb;
    conf.enable_cycle_counting = cycles;
    if (!optimize) conf.optimizations = no_optimizations;
    A32::Jit jit(conf);
    cb.jit = &jit;
    jit.Regs()[15] = 0x10000;
    jit.SetCpsr(thumb ? 0x3f : 0x1f);
    jit.Run();
    constexpr u32 kITMask = 0x0600fc00;
    if (cb.fault || jit.Regs()[0] != 1 || (jit.Cpsr() & kITMask) != 0) {
        std::fprintf(stderr, "FAIL %s cycles=%d optimize=%d fault=%d PC=%08x r0=%08x CPSR=%08x\n",
                     name, cycles, optimize, cb.fault, jit.Regs()[15], jit.Regs()[0], jit.Cpsr());
        std::exit(1);
    }
}
} // namespace
int main() {
    for (int mode : {0, 1, 2}) for (bool cycles : {false, true}) for (bool optimize : {false, true}) Check(mode, cycles, optimize);
    for (bool cycles : {false, true}) for (bool optimize : {false, true}) {
        // movs r0,#0; itt eq; addeq r0,#1; svceq #0.
        // SVC must expose the completed ITSTATE to its callback/host.
        CheckDescriptor("Thumb IT supervisor descriptor", {0x2000, 0xbf04, 0x3001, 0xdf00},
                        true, cycles, optimize);
        // movs r0,#0; movs r1,#0; it eq; tbbeq [pc,r1]; table byte 1;
        // target: movs r0,#1; beq bad; svc #0; bad: movs r0,#2; svc #0.
        // A leaked IT bit suppresses MOVS's flag update and incorrectly takes BEQ.
        CheckDescriptor("Thumb IT table-branch descriptor",
                        {0x2000, 0x2100, 0xbf08, 0xe8df, 0xf001, 0x0001,
                         0x2001, 0xd000, 0xdf00, 0x2002, 0xdf00},
                        true, cycles, optimize);
        // ARM: ldr r0,=0x1000e; bx r0; nop; mov r0,#1; svc #0.
        // BX into ARM clears both low address bits, so execution starts at 0x1000c.
        CheckDescriptor("ARM BX target alignment",
                        {0x000c, 0xe59f, 0xff10, 0xe12f, 0xf000, 0xe320,
                         0x0001, 0xe3a0, 0x0000, 0xef00, 0x000e, 0x0001},
                        false, cycles, optimize);
    }
    std::puts("PASS Thumb IT return stack and callee-saved registers (12 configurations)");
    std::puts("PASS Thumb IT dynamic-exit descriptors and ARM BX alignment (12 configurations)");
}
