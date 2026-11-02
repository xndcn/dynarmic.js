// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// WASM backend emitted-code ABI.
//
// Every compiled guest basic block is a single-function WASM module of type:
//
//     (func $f (param $state i32) (param $ectx i32) (result i32))
//
//   state: linear-memory address of the A32JitState / A64JitState struct
//   ectx:  linear-memory address of the ExecutionContext struct
//   result: halt-reason bits; 0 means "return to dispatch normally"
//
// The block's exported entry funcref is registered in the main module's
// indirect function table; its table index is its C function pointer value:
//
//     using BlockFunc = u32 (*)(uptr state, uptr ectx);
//
// Generated modules import the main module's linear memory ("e", "m") and
// devirtualized host-callback thunks ("e", <name below>).

#pragma once

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/wasm_binary.h"
#include "dynarmic/ir/type.h"

namespace Dynarmic::Backend::Wasm {

// Import module name for all imports of generated modules (kept to one
// character to minimize module size, following v86).
inline constexpr const char* kImportModule = "e";
// Import name of the shared linear memory.
inline constexpr const char* kMemoryImport = "m";
// Import name of the main module's indirect function table (used for direct
// block-to-block chaining via return_call_indirect).
inline constexpr const char* kTableImport = "t";

// Import names of host-callback thunks.
inline constexpr const char* kImportReadMemory8 = "read8";
inline constexpr const char* kImportReadMemory16 = "read16";
inline constexpr const char* kImportReadMemory32 = "read32";
inline constexpr const char* kImportReadMemory64 = "read64";
inline constexpr const char* kImportWriteMemory8 = "write8";
inline constexpr const char* kImportWriteMemory16 = "write16";
inline constexpr const char* kImportWriteMemory32 = "write32";
inline constexpr const char* kImportWriteMemory64 = "write64";
inline constexpr const char* kImportExclusiveReadMemory8 = "xread8";
inline constexpr const char* kImportExclusiveReadMemory16 = "xread16";
inline constexpr const char* kImportExclusiveReadMemory32 = "xread32";
inline constexpr const char* kImportExclusiveReadMemory64 = "xread64";
inline constexpr const char* kImportExclusiveWriteMemory8 = "xwrite8";
inline constexpr const char* kImportExclusiveWriteMemory16 = "xwrite16";
inline constexpr const char* kImportExclusiveWriteMemory32 = "xwrite32";
inline constexpr const char* kImportExclusiveWriteMemory64 = "xwrite64";
inline constexpr const char* kImportClearExclusive = "xclear";
inline constexpr const char* kImportCallSupervisor = "svc";
inline constexpr const char* kImportExceptionRaised = "raise";
inline constexpr const char* kImportInterpreterFallback = "interp";
inline constexpr const char* kImportDataSynchronizationBarrier = "dsb";
inline constexpr const char* kImportInstructionCacheInvalidate = "icache";
// (state, ectx, fn, user_arg, arg0, arg1) -> u64: indirect coprocessor
// callback call (fn is the host function pointer as a table index).
inline constexpr const char* kImportCoprocCallback = "coproc";
// (fpsr_addr, ectx, value, fpcr, rounding) -> result: half-precision conversions
// via FP::FPConvert (h2d returns u64, d2h takes u64).
inline constexpr const char* kImportFpHalfToSingle = "fp_h2s";
inline constexpr const char* kImportFpSingleToHalf = "fp_s2h";
inline constexpr const char* kImportFpHalfToDouble = "fp_h2d";
inline constexpr const char* kImportFpDoubleToHalf = "fp_d2h";
// (state, ectx, value) -> u32: per-lane NEON reciprocal estimate LUTs
// (VRECPE/VRSQRTE .u32), via Common::RecipEstimate/RecipSqrtEstimate.
inline constexpr const char* kImportRecipEstimate = "recip_est";
inline constexpr const char* kImportRecipSqrtEstimate = "rsqrt_est";
// (fpsr_addr, ectx, value[, operand2], fpcr) -> u32: per-lane NEON f32 reciprocal
// estimate / step ops (VRECPE/VRSQRTE/VRECPS/VRSQRTS .f32), via the reference
// FP:: implementation; exception bits raised are ORed into *fpsr_addr.
// (scratch_addr, ectx, op): AES / SHA256 helper operating on the state's
// crypto scratch area (inputs at v128 slots 0..2, result in slot 3). op is a
// CryptoOp.
inline constexpr const char* kImportCrypto = "crypto";
enum class CryptoOp : u32 {
    AesEncryptRound,
    AesDecryptRound,
    AesMixColumns,
    AesInverseMixColumns,
    Sha256HashPart1,
    Sha256HashPart2,
    Sha256MessageSchedule0,
    Sha256MessageSchedule1,
};

// A64 host-callback thunks (thunks_a64.cpp). Virtual addresses are i64.
//   a64_read{8,16,32,64}:   (state, ectx, vaddr) -> value (i32, or i64 for 64)
//   a64_write{8,16,32,64}:  (state, ectx, vaddr, value) -> ()
//   a64_xread* / a64_xwrite*: exclusive variants; xwrite returns 0 on success
//   *128 variants pass the value through A64JitState::mem_scratch.
inline constexpr const char* kImportA64Read[5] = {"a64_read8", "a64_read16", "a64_read32", "a64_read64", "a64_read128"};
inline constexpr const char* kImportA64Write[5] = {"a64_write8", "a64_write16", "a64_write32", "a64_write64", "a64_write128"};
inline constexpr const char* kImportA64ExclusiveRead[5] = {"a64_xread8", "a64_xread16", "a64_xread32", "a64_xread64", "a64_xread128"};
inline constexpr const char* kImportA64ExclusiveWrite[5] = {"a64_xwrite8", "a64_xwrite16", "a64_xwrite32", "a64_xwrite64", "a64_xwrite128"};
inline constexpr const char* kImportA64ClearExclusive = "a64_xclear";
// (state, ectx, imm32)
inline constexpr const char* kImportA64CallSupervisor = "a64_svc";
// (state, ectx, pc, exception)
inline constexpr const char* kImportA64ExceptionRaised = "a64_raise";
// (state, ectx, pc, num_instructions) -> halt bits
inline constexpr const char* kImportA64InterpreterFallback = "a64_interp";
// (state, ectx, op, value): DataCacheOperationRaised / InstructionCacheOperationRaised
inline constexpr const char* kImportA64DataCacheOperation = "a64_dc";
inline constexpr const char* kImportA64InstructionCacheOperation = "a64_ic";
// (state, ectx)
inline constexpr const char* kImportA64InstructionSynchronizationBarrier = "a64_isb";
// (state, ectx) -> u64
inline constexpr const char* kImportA64GetCNTPCT = "a64_cntpct";

// (fpsr_addr, ectx, op, imm, fpcr, a, b, c) -> u64: one scalar operation of
// the reference FP:: implementation (soft-float, exact ARM semantics incl.
// exception bits, ORed into *fpsr_addr). Used for rare operations (estimates,
// Newton-Raphson steps, FRECPX, all half-precision arithmetic). op is
// FpRefOp | (element bits << 8); operands are zero-extended bit patterns.
inline constexpr const char* kImportFpRef = "fp_ref";
enum class FpRefOp : u32 {
    MulAdd,          // a + b * c
    MulSub,          // a - b * c
    RecipEstimate,   // a
    RSqrtEstimate,   // a
    RecipExponent,   // a
    RecipStepFused,  // a, b
    RSqrtStepFused,  // a, b
    RoundInt,        // a; imm = rounding | exact << 8
    ToFixed,         // a; imm = fbits | rounding << 8 | ibits << 16 | unsigned << 24
    CompareEqual,    // a, b -> all-ones / 0 of the element size
};
// (a, b, c) -> f64: fused a * b + c (correctly rounded, via the C library).
inline constexpr const char* kImportFpFma64 = "fp_fma64";

inline constexpr const char* kImportFpVRecipEstimate32 = "fp_vrecpe32";
inline constexpr const char* kImportFpVRSqrtEstimate32 = "fp_vrsqrte32";
inline constexpr const char* kImportFpVRecipStep32 = "fp_vrecps32";
inline constexpr const char* kImportFpVRSqrtStep32 = "fp_vrsqrts32";

// Mutable state shared between the dispatcher and JIT blocks, exchanged
// through linear memory (pointed to by the ectx block parameter).
inline constexpr u32 kRsbSize = 8;  // power of two
inline constexpr u32 kRsbMask = kRsbSize - 1;

// Optional board-observable state for cycle-counted region execution. The
// dispatcher owns this POD in shared linear memory for the JIT's lifetime.
// Any nonzero flags value forces the next in-region edge back to the native
// dispatcher. instruction_hooks uses bit 0 for "slot filter may contain hooks"
// and bits 1/2 for global code/block hooks; hook_slots is a 65536-byte filter
// indexed by (pc >> 1) & 0xffff. The filter is conservative: a hit only asks
// the native boundary to perform its exact hook checks.
struct BoundaryState {
    volatile u32 flags;
    volatile u32 instruction_hooks;
    u32 hook_slots;
};
inline constexpr u32 kBoundaryDisableRegions = 1u << 0;
inline constexpr u32 kBoundaryInstructionHookMask = (1u << 1) | (1u << 2);
static_assert(sizeof(BoundaryState) == 12);

// Optional native observer of a completed boundary-dispatch entry. Runs after
// AddTicks, with registers materialized, before the next allowance/IRQ query.
// It may redirect PC or request halt/cache invalidation. It must not reenter
// Run/Step or destroy the JIT; invalidated modules remain owned until Run exits.
// No generated-code or JavaScript call is added on the disabled path.
struct BoundaryObserver {
    void (*after_ticks)(void*) = nullptr;
    void* user = nullptr;
};

struct alignas(8) ExecutionContext {
    volatile u32 halt_reason;
    u32 check_bit;  // unused: blocks keep the check bit in a local
    // Set by a block that hit an unlinked LinkBlock/LinkBlockFast edge: the linear-memory
    // address of that edge's link slot. The dispatcher patches the slot with
    // the next block's table index, then clears this back to 0.
    u32 link_slot_addr;
    // Return stack buffer top: index of the next push slot, in [0, kRsbSize).
    u32 rsb_top;
    // Set by a block whose fast-dispatch lookup missed: the address of the
    // FastDispatchEntry to fill. The dispatcher stores the current location
    // and its block's table index there, then clears this back to 0.
    u32 fast_dispatch_entry;
    u32 pad;
    s64 cycles_remaining;
    s64 cycles_to_run;

    // Return stack buffer for bl/bx-lr speculation: (location descriptor,
    // block table index) pairs. index == 0 means "not yet linked"; the
    // dispatcher patches it lazily via link_slot_addr on first use.
    struct RsbEntry {
        u64 descriptor;
        u32 index;
        u32 pad;
    };
    RsbEntry rsb[kRsbSize];

    // Per-instance callback configuration. Appended to preserve generated-code offsets.
    const void* callback_config = nullptr;
};

// Fast dispatch table (FastDispatchHint terminals, RSB misses): direct-mapped
// by guest location, owned by the address space, wiped on any invalidation.
inline constexpr u32 kFastDispatchEntries = 4096;  // power of two
struct FastDispatchEntry {
    u64 descriptor;
    u32 index;  // block table index; 0 = empty
    u32 pad;
};
static_assert(sizeof(FastDispatchEntry) == 16);
// Slot of a location in the table, from the low 32 bits of its descriptor
// (the emitted lookup computes the same).
inline constexpr u32 FastDispatchSlot(u64 descriptor) {
    const u32 lo = static_cast<u32>(descriptor);
    return ((lo >> 2) ^ (lo >> 14)) & (kFastDispatchEntries - 1);
}

// Local indices of the block function parameters.
inline constexpr u32 kParamState = 0;
inline constexpr u32 kParamEctx = 1;
inline constexpr u32 kParamCount = 2;

// IR type -> WASM value type mapping.
inline ValType ValTypeOf(IR::Type type) {
    switch (type) {
    case IR::Type::U1:
    case IR::Type::U8:
    case IR::Type::U16:
    case IR::Type::U32:
    case IR::Type::NZCVFlags:
        return ValType::I32;
    case IR::Type::U64:
        return ValType::I64;
    case IR::Type::U128:
        return ValType::V128;
    default:
        UNREACHABLE();
    }
}

}  // namespace Dynarmic::Backend::Wasm
