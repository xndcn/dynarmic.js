// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Host-callback thunks imported by generated A64 block modules. They
// devirtualize A64::UserCallbacks using the calling JIT execution context.
// Virtual addresses are u64 (wasm i64 parameters); 128-bit values travel
// through A64JitState::mem_scratch.

#include <cstring>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/wasm_runtime.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::Wasm {
namespace {

const A64::UserConfig* ConfigOf(uptr ectx) {
    return static_cast<const A64::UserConfig*>(reinterpret_cast<const ExecutionContext*>(ectx)->callback_config);
}

A64JitState* StateOf(uptr state) {
    return reinterpret_cast<A64JitState*>(state);
}

ExecutionContext* EctxOf(uptr ectx) {
    return reinterpret_cast<ExecutionContext*>(ectx);
}

A64::Vector LoadScratch(uptr state) {
    A64::Vector v;
    std::memcpy(v.data(), StateOf(state)->mem_scratch.data(), sizeof(v));
    return v;
}

void StoreScratch(uptr state, const A64::Vector& v) {
    std::memcpy(StateOf(state)->mem_scratch.data(), v.data(), sizeof(v));
}

template<typename T, typename Read>
T ExclusiveRead(uptr state, uptr ectx, u64 vaddr, Read read) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    StateOf(state)->exclusive_state = 1;
    return ConfigOf(ectx)->global_monitor->ReadAndMark<T>(ConfigOf(ectx)->processor_id, vaddr, read);
}

// Returns 0 on success, 1 on failure (ARM STXR status).
template<typename T, typename Write>
u32 ExclusiveWrite(uptr state, uptr ectx, u64 vaddr, Write write) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    A64JitState* jit_state = StateOf(state);
    if (!jit_state->exclusive_state) {
        return 1;
    }
    jit_state->exclusive_state = 0;
    return ConfigOf(ectx)->global_monitor->DoExclusiveOperation<T>(ConfigOf(ectx)->processor_id, vaddr, write) ? 0 : 1;
}

}  // anonymous namespace


}  // namespace Dynarmic::Backend::Wasm

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;

extern "C" {

u32 dynarmic_wasm_a64_read8(uptr, uptr ectx, u64 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead8(vaddr);
}

u32 dynarmic_wasm_a64_read16(uptr, uptr ectx, u64 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead16(vaddr);
}

u32 dynarmic_wasm_a64_read32(uptr, uptr ectx, u64 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead32(vaddr);
}

u64 dynarmic_wasm_a64_read64(uptr, uptr ectx, u64 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead64(vaddr);
}

void dynarmic_wasm_a64_read128(uptr state, uptr ectx, u64 vaddr) {
    StoreScratch(state, ConfigOf(ectx)->callbacks->MemoryRead128(vaddr));
}

void dynarmic_wasm_a64_write8(uptr, uptr ectx, u64 vaddr, u32 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite8(vaddr, static_cast<u8>(value));
}

void dynarmic_wasm_a64_write16(uptr, uptr ectx, u64 vaddr, u32 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite16(vaddr, static_cast<u16>(value));
}

void dynarmic_wasm_a64_write32(uptr, uptr ectx, u64 vaddr, u32 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite32(vaddr, value);
}

void dynarmic_wasm_a64_write64(uptr, uptr ectx, u64 vaddr, u64 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite64(vaddr, value);
}

void dynarmic_wasm_a64_write128(uptr state, uptr ectx, u64 vaddr) {
    ConfigOf(ectx)->callbacks->MemoryWrite128(vaddr, LoadScratch(state));
}

u32 dynarmic_wasm_a64_xread8(uptr state, uptr ectx, u64 vaddr) {
    return ExclusiveRead<u8>(state, ectx, vaddr, [&] { return ConfigOf(ectx)->callbacks->MemoryRead8(vaddr); });
}

u32 dynarmic_wasm_a64_xread16(uptr state, uptr ectx, u64 vaddr) {
    return ExclusiveRead<u16>(state, ectx, vaddr, [&] { return ConfigOf(ectx)->callbacks->MemoryRead16(vaddr); });
}

u32 dynarmic_wasm_a64_xread32(uptr state, uptr ectx, u64 vaddr) {
    return ExclusiveRead<u32>(state, ectx, vaddr, [&] { return ConfigOf(ectx)->callbacks->MemoryRead32(vaddr); });
}

u64 dynarmic_wasm_a64_xread64(uptr state, uptr ectx, u64 vaddr) {
    return ExclusiveRead<u64>(state, ectx, vaddr, [&] { return ConfigOf(ectx)->callbacks->MemoryRead64(vaddr); });
}

void dynarmic_wasm_a64_xread128(uptr state, uptr ectx, u64 vaddr) {
    StoreScratch(state, ExclusiveRead<A64::Vector>(state, ectx, vaddr, [&] { return ConfigOf(ectx)->callbacks->MemoryRead128(vaddr); }));
}

u32 dynarmic_wasm_a64_xwrite8(uptr state, uptr ectx, u64 vaddr, u32 value) {
    return ExclusiveWrite<u8>(state, ectx, vaddr, [&](u8 expected) {
        return ConfigOf(ectx)->callbacks->MemoryWriteExclusive8(vaddr, static_cast<u8>(value), expected);
    });
}

u32 dynarmic_wasm_a64_xwrite16(uptr state, uptr ectx, u64 vaddr, u32 value) {
    return ExclusiveWrite<u16>(state, ectx, vaddr, [&](u16 expected) {
        return ConfigOf(ectx)->callbacks->MemoryWriteExclusive16(vaddr, static_cast<u16>(value), expected);
    });
}

u32 dynarmic_wasm_a64_xwrite32(uptr state, uptr ectx, u64 vaddr, u32 value) {
    return ExclusiveWrite<u32>(state, ectx, vaddr, [&](u32 expected) {
        return ConfigOf(ectx)->callbacks->MemoryWriteExclusive32(vaddr, value, expected);
    });
}

u32 dynarmic_wasm_a64_xwrite64(uptr state, uptr ectx, u64 vaddr, u64 value) {
    return ExclusiveWrite<u64>(state, ectx, vaddr, [&](u64 expected) {
        return ConfigOf(ectx)->callbacks->MemoryWriteExclusive64(vaddr, value, expected);
    });
}

u32 dynarmic_wasm_a64_xwrite128(uptr state, uptr ectx, u64 vaddr) {
    const A64::Vector value = LoadScratch(state);
    return ExclusiveWrite<A64::Vector>(state, ectx, vaddr, [&](A64::Vector expected) {
        return ConfigOf(ectx)->callbacks->MemoryWriteExclusive128(vaddr, value, expected);
    });
}

void dynarmic_wasm_a64_xclear(uptr state, uptr) {
    StateOf(state)->exclusive_state = 0;
}

void dynarmic_wasm_a64_svc(uptr, uptr ectx, u32 imm) {
    ConfigOf(ectx)->callbacks->CallSVC(imm);
}

void dynarmic_wasm_a64_raise(uptr, uptr ectx, u64 pc, u64 exception) {
    ConfigOf(ectx)->callbacks->ExceptionRaised(pc, static_cast<A64::Exception>(exception));
}

u32 dynarmic_wasm_a64_interp(uptr state, uptr ectx, u64 pc, u32 num_instructions) {
    StateOf(state)->pc = pc;
    ConfigOf(ectx)->callbacks->InterpreterFallback(pc, num_instructions);
    return EctxOf(ectx)->halt_reason;
}

void dynarmic_wasm_a64_dc(uptr, uptr ectx, u64 op, u64 value) {
    ConfigOf(ectx)->callbacks->DataCacheOperationRaised(static_cast<A64::DataCacheOperation>(op), value);
}

void dynarmic_wasm_a64_ic(uptr, uptr ectx, u64 op, u64 value) {
    ConfigOf(ectx)->callbacks->InstructionCacheOperationRaised(static_cast<A64::InstructionCacheOperation>(op), value);
}

void dynarmic_wasm_a64_isb(uptr, uptr ectx) {
    ConfigOf(ectx)->callbacks->InstructionSynchronizationBarrierRaised();
}

// CNTPCT may depend on elapsed ticks: account for the cycles run so far
// (like the x64 backend's UpdateTicks) before asking the host.
u64 dynarmic_wasm_a64_cntpct(uptr, uptr ectx) {
    if (ConfigOf(ectx)->enable_cycle_counting && !ConfigOf(ectx)->wall_clock_cntpct) {
        ExecutionContext* const e = EctxOf(ectx);
        ConfigOf(ectx)->callbacks->AddTicks(static_cast<u64>(e->cycles_to_run - e->cycles_remaining));
        e->cycles_to_run = static_cast<s64>(ConfigOf(ectx)->callbacks->GetTicksRemaining());
        e->cycles_remaining = e->cycles_to_run;
    }
    return ConfigOf(ectx)->callbacks->GetCNTPCT();
}

}  // extern "C"
