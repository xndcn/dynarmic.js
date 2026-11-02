#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <emscripten.h>

#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/a32_interface.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;
namespace {

int PublishedInstanceCount() {
    return EM_ASM_INT({
        return Object.keys(Module.dynarmicWasmStats?.instances || {}).length;
    });
}

int ActiveInstanceAvailable() {
    return EM_ASM_INT({
        const s = Module.dynarmicWasmStats;
        return s && typeof s.getInstance === 'function' && s.getInstance(s.activeInstance) ? 1 : 0;
    });
}

void CheckFastDispatchMisses(u64 expected) {
#if defined(DYNARMIC_WASM_PROFILE)
    const int actual = EM_ASM_INT({
        return Module.dynarmicWasmStats?.profile?.fastDispatchMisses || 0;
    });
    if (actual != static_cast<int>(expected)) std::abort();
#else
    (void)expected;
#endif
}

class Callbacks32 final : public A32::UserCallbacks {
public:
    A32::Jit* jit = nullptr;
    u32 result = 0;
    u64 ticks = 0;
    u64 polls = 0;
    std::optional<u32> MemoryReadCode(u32 address) override { return MemoryRead32(address); }
    u8 MemoryRead8(u32) override { return 0; }
    u16 MemoryRead16(u32) override { return 0; }
    u32 MemoryRead32(u32 address) override {
        if (address == 0x1000) return 0xe3a00001;  // mov r0, #1
        if (address == 0x1004) return 0xea000000;  // b 0x100c
        if (address == 0x100c) return 0xe3a01002;  // mov r1, #2
        return 0xef000007;                         // svc #7
    }
    u64 MemoryRead64(u32) override { return 0; }
    void MemoryWrite8(u32, u8) override {}
    void MemoryWrite16(u32, u16) override {}
    void MemoryWrite32(u32, u32) override {}
    void MemoryWrite64(u32, u64) override {}
    void InterpreterFallback(u32, size_t) override { jit->HaltExecution(); }
    void CallSVC(u32 imm) override { result = imm; jit->HaltExecution(); }
    void ExceptionRaised(u32, A32::Exception) override { jit->HaltExecution(); }
    void AddTicks(u64 elapsed) override { ticks += elapsed; }
    u64 GetTicksRemaining() override { ++polls; return 10000; }
};

class IndirectCallbacks32 final : public A32::UserCallbacks {
public:
    A32::Jit* jit = nullptr;
    u32 result = 0;
    u64 ticks = 0;
    u64 polls = 0;
    u64 allowance = 10000;
    bool return_edge = false;
    u32 target_mov = 0xe3a02002;  // mov r2, #2
    std::vector<u64> slices;
    std::optional<u32> MemoryReadCode(u32 address) override { return MemoryRead32(address); }
    u8 MemoryRead8(u32) override { return 0; }
    u16 MemoryRead16(u32) override { return 0; }
    u32 MemoryRead32(u32 address) override {
        if (address == 0x1000) return 0xe3a00001;  // mov r0, #1
        if (address == 0x1004) return return_edge ? 0xe3a0ea02 : 0xe3a01a02;  // mov lr/r1, #0x2000
        if (address == 0x1008) return return_edge ? 0xe12fff1e : 0xe12fff11;  // bx lr/r1
        if (address == 0x2000) return target_mov;
        return 0xef000007;                         // svc #7
    }
    u64 MemoryRead64(u32) override { return 0; }
    void MemoryWrite8(u32, u8) override {}
    void MemoryWrite16(u32, u16) override {}
    void MemoryWrite32(u32, u32) override {}
    void MemoryWrite64(u32, u64) override {}
    void InterpreterFallback(u32, size_t) override { jit->HaltExecution(); }
    void CallSVC(u32 imm) override { result = imm; jit->HaltExecution(); }
    void ExceptionRaised(u32, A32::Exception) override { jit->HaltExecution(); }
    void AddTicks(u64 elapsed) override {
        ticks += elapsed;
        slices.push_back(elapsed);
    }
    u64 GetTicksRemaining() override {
        ++polls;
        return allowance;
    }
};

class Callbacks64 final : public A64::UserCallbacks {
public:
    A64::Jit* jit = nullptr;
    u32 result = 0;
    u8 MemoryRead8(u64) override { return 0; }
    u16 MemoryRead16(u64) override { return 0; }
    u32 MemoryRead32(u64 address) override { return address == 0x1000 ? 0xd2800020 : 0xd40000e1; }
    u64 MemoryRead64(u64) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, u16) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64, size_t) override { jit->HaltExecution(); }
    void CallSVC(u32 imm) override { result = imm; jit->HaltExecution(); }
    void ExceptionRaised(u64, A64::Exception) override { jit->HaltExecution(); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 10000; }
    u64 GetCNTPCT() override { return 0; }
};

void TestA32() {
    // mov r0,#1; svc #7
    constexpr std::array<u32, 2> code{0xe3a00001, 0xef000007};
    Callbacks32 cb_a, cb_b;
    A32::UserConfig conf;
    conf.enable_cycle_counting = false;
    conf.callbacks = &cb_a;
    auto a = std::make_unique<A32::Jit>(conf);
    cb_a.jit = a.get();
    a->Regs()[15] = 0x1000;
    a->SetCpsr(0x1f);
    // Separate callback and JIT remain live at the same time.
    conf.callbacks = &cb_b;
    auto b = std::make_unique<A32::Jit>(conf);
    cb_b.jit = b.get();
    b->Regs()[15] = 0x1000;
    b->SetCpsr(0x1f);
    // Empty invalidations are no-ops; oversized ranges saturate instead of
    // wrapping around the frontend address space.
    a->InvalidateCacheRange(0x1000, 0);
    b->InvalidateCacheRange(0xFFFFFFF0u, 32);
    a->InvalidateCacheRange(0, static_cast<size_t>(-1));
    a->Run(); b->Run();
    if (cb_a.result != 7 || cb_b.result != 7) std::abort();
    if (PublishedInstanceCount() < 2) std::abort();
    if (!ActiveInstanceAvailable()) std::abort();
    std::puts("instance A32 constructed");
}

void TestA32BoundaryTicks() {
    Callbacks32 callbacks;
    A32::UserConfig conf;
    conf.enable_cycle_counting = true;
    conf.callbacks = &callbacks;
    Backend::Wasm::BoundaryState boundary{};
    auto jit = Backend::Wasm::CreateA32Jit(conf, &boundary);
    callbacks.jit = jit.get();
    jit->Regs()[15] = 0x1000;
    jit->SetCpsr(0x1f);
    jit->Run();
    if (callbacks.result != 7 || callbacks.ticks != 4) std::abort();
    std::puts("instance A32 boundary ticks committed");
}

void TestA32BoundaryIndirectDispatch(bool specialize, bool return_edge = false) {
    IndirectCallbacks32 callbacks;
    callbacks.return_edge = return_edge;
    A32::UserConfig conf;
    conf.enable_cycle_counting = true;
    conf.callbacks = &callbacks;
    Backend::Wasm::BoundaryState boundary{};
    auto jit = Backend::Wasm::CreateA32Jit(conf, &boundary, specialize);
    callbacks.jit = jit.get();
    const std::vector<u64> cold_slices{3, 2};
    const std::vector<u64> warm_slices{5};
    const auto reset = [&] {
        jit->ClearHalt();
        jit->Regs()[0] = 0;
        jit->Regs()[1] = 0;
        jit->Regs()[2] = 0;
        jit->Regs()[15] = 0x1000;
        jit->SetCpsr(0x1f);
        callbacks.result = 0;
        callbacks.ticks = 0;
        callbacks.polls = 0;
        callbacks.slices.clear();
    };

    reset();
    jit->Run();
    CheckFastDispatchMisses(1);
    if (callbacks.result != 7 || jit->Regs()[0] != 1 || (return_edge ? jit->Regs()[14] : jit->Regs()[1]) != 0x2000 ||
        jit->Regs()[2] != 2 || callbacks.ticks != 5 || callbacks.slices != cold_slices || callbacks.polls != 2) {
        std::abort();
    }

    // A warm indirect edge completes both original blocks in one native entry.
    reset();
    jit->Run();
    CheckFastDispatchMisses(1);
    if (callbacks.result != 7 || callbacks.ticks != 5 || callbacks.slices != warm_slices || callbacks.polls != 1) {
        std::abort();
    }

    // The target may not be entered after the source consumes the allowance.
    callbacks.allowance = 3;
    reset();
    jit->Run();
    CheckFastDispatchMisses(1);
    if (callbacks.result != 7 || callbacks.ticks != 5 || callbacks.slices != cold_slices || callbacks.polls != 2) {
        std::abort();
    }

    // Range invalidation drops the old table index and forces one cold refill.
    callbacks.allowance = 10000;
    callbacks.target_mov = 0xe3a02003;  // mov r2, #3
    jit->InvalidateCacheRange(0x2000, 4);
    reset();
    jit->Run();
    CheckFastDispatchMisses(2);
    if (callbacks.result != 7 || jit->Regs()[2] != 3 || callbacks.slices != cold_slices || callbacks.polls != 2) {
        std::abort();
    }
    reset();
    jit->Run();
    CheckFastDispatchMisses(2);
    if (callbacks.result != 7 || jit->Regs()[2] != 3 || callbacks.slices != warm_slices || callbacks.polls != 1) {
        std::abort();
    }
    std::puts("instance A32 boundary indirect dispatch guarded");
}

void TestA32BoundaryObserver() {
    IndirectCallbacks32 callbacks;
    A32::UserConfig conf;
    conf.enable_cycle_counting = true;
    conf.callbacks = &callbacks;
    Backend::Wasm::BoundaryState boundary{};
    struct Observation {
        IndirectCallbacks32* callbacks;
        unsigned calls = 0;
    } observation{&callbacks};
    const Backend::Wasm::BoundaryObserver observer{
        [](void* user) {
            auto& state = *static_cast<Observation*>(user);
            auto& cb = *state.callbacks;
            ++state.calls;
            // The first cold source must be charged before host observation.
            if (state.calls == 1) {
                if (cb.ticks != 3 || cb.jit->Regs()[15] != 0x2000) std::abort();
                Backend::Wasm::CommitA32ExecutionTicks(*cb.jit);
                if (cb.ticks != 3) std::abort();
                cb.jit->Regs()[15] = 0x3000; // redirect to SVC
            }
        }, &observation};
    auto jit = Backend::Wasm::CreateA32Jit(conf, &boundary, false, observer);
    callbacks.jit = jit.get();
    jit->Regs()[15] = 0x1000;
    jit->SetCpsr(0x1f);
    jit->Run();
    if (callbacks.result != 7 || callbacks.ticks != 4 || observation.calls != 2) std::abort();
    jit->ClearHalt();
    // Observer installation is per instance; ordinary construction is empty.
    IndirectCallbacks32 other_callbacks;
    conf.callbacks = &other_callbacks;
    auto other = Backend::Wasm::CreateA32Jit(conf, &boundary);
    other_callbacks.jit = other.get();
    other->Regs()[15] = 0x1000;
    other->SetCpsr(0x1f);
    other->Run();
    if (other_callbacks.ticks != 5 || observation.calls != 2) std::abort();
    std::puts("instance A32 post-tick observer ordered and isolated");
}

void TestA32BoundaryHookInvalidation(bool specialize) {
    Callbacks32 callbacks;
    A32::UserConfig conf;
    conf.enable_cycle_counting = true;
    conf.callbacks = &callbacks;
    Backend::Wasm::BoundaryState boundary{};
    const auto slots = std::make_unique<std::array<u8, 65536>>();
    boundary.hook_slots = static_cast<u32>(reinterpret_cast<uptr>(slots->data()));
    auto jit = Backend::Wasm::CreateA32Jit(conf, &boundary, specialize);
    callbacks.jit = jit.get();
    jit->Regs()[15] = 0x1000;
    jit->SetCpsr(0x1f);
    jit->Run();
    if (callbacks.result != 7 || jit->Regs()[0] != 1 || jit->Regs()[1] != 2) std::abort();
    const u64 unhooked_polls = callbacks.polls;

    // A dynamic guard observes this mutation without invalidation. The opt-in
    // path instead requires whole-cache invalidation before taking a new snapshot.
    boundary.instruction_hooks = 1;
    (*slots)[(0x100c >> 1) & 0xffff] = 1 << 1;
    if (specialize) jit->ClearCache();
    jit->ClearHalt();
    jit->Regs()[15] = 0x1000;
    jit->Run();
    if (callbacks.result != 7 || jit->Regs()[1] != 2 || callbacks.polls <= unhooked_polls) std::abort();}

void TestA64() {
    Callbacks64 cb_a, cb_b;
    A64::UserConfig conf;
    conf.enable_cycle_counting = false;
    conf.callbacks = &cb_a;
    auto a = std::make_unique<A64::Jit>(conf);
    cb_a.jit = a.get();
    conf.callbacks = &cb_b;
    auto b = std::make_unique<A64::Jit>(conf);
    cb_b.jit = b.get();
    a->SetPC(0x1000); b->SetPC(0x1000);
    a->InvalidateCacheRange(0x1000, 0);
    b->InvalidateCacheRange(~u64{0} - 8, 32);
    a->InvalidateCacheRange(0, static_cast<size_t>(-1));
    a->Run(); b->Run();
    if (cb_a.result != 7 || cb_b.result != 7) std::abort();
    if (PublishedInstanceCount() < 4) std::abort();
    if (!ActiveInstanceAvailable()) std::abort();
    std::puts("instance A64 constructed");
}

}  // namespace

int main() {
    TestA32();
    TestA32BoundaryTicks();
    TestA32BoundaryObserver();
    TestA32BoundaryIndirectDispatch(false);
    TestA32BoundaryIndirectDispatch(true);
    TestA32BoundaryIndirectDispatch(false, true);
    TestA32BoundaryIndirectDispatch(true, true);
    TestA32BoundaryHookInvalidation(false);
    TestA32BoundaryHookInvalidation(true);
    TestA64();
    std::puts("PASS instance isolation");
}
