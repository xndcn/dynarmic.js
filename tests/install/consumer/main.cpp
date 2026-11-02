#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>

// Native builds cannot execute generated Wasm. Still instantiate both frontends
// and exercise lifecycle APIs so this test resolves real symbols from the
// installed archive (a header-only config check would not test static linkage).
class Callbacks final : public Dynarmic::A32::UserCallbacks, public Dynarmic::A64::UserCallbacks {
public:
    std::uint8_t MemoryRead8(std::uint32_t) override { return 0; }
    std::uint16_t MemoryRead16(std::uint32_t) override { return 0; }
    std::uint32_t MemoryRead32(std::uint32_t) override { return 0; }
    std::uint64_t MemoryRead64(std::uint32_t) override { return 0; }
    void MemoryWrite8(std::uint32_t, std::uint8_t) override {}
    void MemoryWrite16(std::uint32_t, std::uint16_t) override {}
    void MemoryWrite32(std::uint32_t, std::uint32_t) override {}
    void MemoryWrite64(std::uint32_t, std::uint64_t) override {}
    void InterpreterFallback(std::uint32_t, std::size_t) override {}
    void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override {}

    std::uint8_t MemoryRead8(std::uint64_t) override { return 0; }
    std::uint16_t MemoryRead16(std::uint64_t) override { return 0; }
    std::uint32_t MemoryRead32(std::uint64_t) override { return 0; }
    std::uint64_t MemoryRead64(std::uint64_t) override { return 0; }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { return {}; }
    void MemoryWrite8(std::uint64_t, std::uint8_t) override {}
    void MemoryWrite16(std::uint64_t, std::uint16_t) override {}
    void MemoryWrite32(std::uint64_t, std::uint32_t) override {}
    void MemoryWrite64(std::uint64_t, std::uint64_t) override {}
    void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override {}
    void InterpreterFallback(std::uint64_t, std::size_t) override {}
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override {}
    std::uint64_t GetCNTPCT() override { return 0; }

    void CallSVC(std::uint32_t) override {}
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 0; }
};

int main() {
    Callbacks callbacks;
    Dynarmic::A32::UserConfig a32{};
    Dynarmic::A64::UserConfig a64{};
    a32.callbacks = &callbacks;
    a64.callbacks = &callbacks;
    Dynarmic::A32::Jit jit32(a32);
    Dynarmic::A64::Jit jit64(a64);
    jit32.Regs()[0] = 42;
    jit64.SetRegister(0, 42);
    if (jit32.Regs()[0] != 42 || jit64.GetRegister(0) != 42) return 1;
    jit32.ClearCache();
    jit64.ClearCache();
    jit32.Reset();
    jit64.Reset();
    return (jit32.Regs()[0] == 0 && jit64.GetRegister(0) == 0) ? 0 : 1;
}
