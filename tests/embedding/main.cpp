#include <cstdio>
#include <dynarmic/interface/A32/a32.h>

class Callbacks final : public Dynarmic::A32::UserCallbacks {
public:
    Dynarmic::A32::Jit* jit = nullptr;
    std::uint32_t svc = 0;
    std::uint8_t MemoryRead8(std::uint32_t) override { return 0; }
    std::uint16_t MemoryRead16(std::uint32_t) override { return 0; }
    std::uint32_t MemoryRead32(std::uint32_t address) override {
        return address == 0x1000 ? 0xe3a0002a : 0xef000007; // mov r0, #42; svc #7
    }
    std::uint64_t MemoryRead64(std::uint32_t) override { return 0; }
    void MemoryWrite8(std::uint32_t, std::uint8_t) override {}
    void MemoryWrite16(std::uint32_t, std::uint16_t) override {}
    void MemoryWrite32(std::uint32_t, std::uint32_t) override {}
    void MemoryWrite64(std::uint32_t, std::uint64_t) override {}
    void InterpreterFallback(std::uint32_t, std::size_t) override { jit->HaltExecution(); }
    void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override { jit->HaltExecution(); }
    void CallSVC(std::uint32_t value) override { svc = value; jit->HaltExecution(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 100; }
};

int main() {
    Callbacks callbacks;
    Dynarmic::A32::UserConfig config{};
    config.callbacks = &callbacks;
    Dynarmic::A32::Jit jit(config);
    callbacks.jit = &jit;
    jit.Regs()[15] = 0x1000;
    jit.SetCpsr(0x1f);
    jit.Run();
    if (jit.Regs()[0] != 42 || callbacks.svc != 7) return 1;
    std::puts("PASS external CMake embedding");
}
