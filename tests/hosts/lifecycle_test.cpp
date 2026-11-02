// Lifecycle contract test for the WASM backend. It covers externally queued
// halts, Step() progression, cache clears while idle, and a cache clear plus a
// user halt requested from inside a callback for both frontends.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL lifecycle: %s\n", message);
        std::exit(1);
    }
}

class Callbacks32 final : public A32::UserCallbacks {
public:
    A32::Jit* jit = nullptr;
    std::optional<u32> MemoryReadCode(u32 address) override { return MemoryRead32(address); }
    u8 MemoryRead8(u32) override { return 0; }
    u16 MemoryRead16(u32) override { return 0; }
    u32 MemoryRead32(u32 address) override {
        if (address == 0x1000) return 0xe3a00001; // mov r0,#1
        if (address == 0x1004) return 0xe2800001; // add r0,r0,#1
        return 0xef000000; // svc #0
    }
    u64 MemoryRead64(u32) override { return 0; }
    void MemoryWrite8(u32, u8) override {}
    void MemoryWrite16(u32, u16) override {}
    void MemoryWrite32(u32, u32) override {}
    void MemoryWrite64(u32, u64) override {}
    void InterpreterFallback(u32, size_t) override { jit->HaltExecution(HaltReason::UserDefined3); }
    void CallSVC(u32) override {
        jit->ClearCache();
        jit->HaltExecution(HaltReason::UserDefined2);
    }
    void ExceptionRaised(u32, A32::Exception) override { jit->HaltExecution(HaltReason::UserDefined4); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 10000; }
};

class Callbacks64 final : public A64::UserCallbacks {
public:
    A64::Jit* jit = nullptr;
    u8 MemoryRead8(u64) override { return 0; }
    u16 MemoryRead16(u64) override { return 0; }
    u32 MemoryRead32(u64 address) override {
        if (address == 0x1000) return 0xd2800020; // mov x0,#1
        if (address == 0x1004) return 0x91000400; // add x0,x0,#1
        return 0xd4000001; // svc #0
    }
    u64 MemoryRead64(u64) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, u16) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64, size_t) override { jit->HaltExecution(HaltReason::UserDefined3); }
    void CallSVC(u32) override {
        jit->ClearCache();
        jit->HaltExecution(HaltReason::UserDefined2);
    }
    void ExceptionRaised(u64, A64::Exception) override { jit->HaltExecution(HaltReason::UserDefined4); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 10000; }
    u64 GetCNTPCT() override { return 0; }
};

void TestA32() {
    Callbacks32 callbacks;
    ExclusiveMonitor monitor{1};
    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.enable_cycle_counting = false;
    A32::Jit jit{conf};
    callbacks.jit = &jit;
    jit.Regs()[15] = 0x1000;
    jit.SetCpsr(0x1f);

    jit.HaltExecution(HaltReason::UserDefined1);
    Require(jit.Run() == HaltReason::UserDefined1 && jit.Regs()[0] == 0, "A32 external halt ran guest");
    Require(jit.Step() == HaltReason::UserDefined1 && jit.Regs()[0] == 0, "A32 Step ignored external halt");
    jit.ClearHalt(HaltReason::UserDefined1);
    Require(Has(jit.Step(), HaltReason::Step) && jit.Regs()[0] == 1 && jit.Regs()[15] == 0x1004, "A32 first step");
    jit.ClearCache();
    Require(Has(jit.Step(), HaltReason::Step) && jit.Regs()[0] == 2 && jit.Regs()[15] == 0x1008, "A32 step after idle clear");
    const HaltReason hr = jit.Step();
    Require(Has(hr, HaltReason::Step) && Has(hr, HaltReason::UserDefined2), "A32 callback halt/cache clear composition");
    jit.ClearHalt(HaltReason::UserDefined2);
    jit.Reset();
    Require(jit.Regs()[0] == 0 && jit.Regs()[15] == 0, "A32 reset state");
}

void TestA64() {
    Callbacks64 callbacks;
    ExclusiveMonitor monitor{1};
    A64::UserConfig conf;
    conf.callbacks = &callbacks;
    conf.global_monitor = &monitor;
    conf.enable_cycle_counting = false;
    A64::Jit jit{conf};
    callbacks.jit = &jit;
    jit.SetPC(0x1000);

    jit.HaltExecution(HaltReason::UserDefined1);
    Require(jit.Run() == HaltReason::UserDefined1 && jit.GetRegister(0) == 0, "A64 external halt ran guest");
    Require(jit.Step() == HaltReason::UserDefined1 && jit.GetRegister(0) == 0, "A64 Step ignored external halt");
    jit.ClearHalt(HaltReason::UserDefined1);
    Require(Has(jit.Step(), HaltReason::Step) && jit.GetRegister(0) == 1 && jit.GetPC() == 0x1004, "A64 first step");
    jit.ClearCache();
    Require(Has(jit.Step(), HaltReason::Step) && jit.GetRegister(0) == 2 && jit.GetPC() == 0x1008, "A64 step after idle clear");
    const HaltReason hr = jit.Step();
    Require(Has(hr, HaltReason::Step) && Has(hr, HaltReason::UserDefined2), "A64 callback halt/cache clear composition");
    jit.ClearHalt(HaltReason::UserDefined2);
    jit.Reset();
    Require(jit.GetRegister(0) == 0 && jit.GetPC() == 0, "A64 reset state");
}

}  // namespace

int main() {
    TestA32();
    TestA64();
    std::puts("PASS lifecycle A32/A64");
}
