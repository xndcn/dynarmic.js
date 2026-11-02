// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <array>

#include <mcl/stdint.hpp>

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Wasm {

// Guest state of an A64 Jit, addressed directly by generated code.
struct A64JitState {
    u32 cpsr_nzcv = 0;  // PSTATE.NZCV in ARM order (bits 31..28), the IR's NZCV format
    u32 fpsr = 0;       // FPSR as the guest sees it (cumulative exception bits, QC)
    u32 fpcr = 0;
    u32 exclusive_state = 0;

    u64 pc = 0;
    u64 sp = 0;
    std::array<u64, 31> reg{};

    alignas(16) std::array<u64, 64> vec{};  // V0..V31, two u64 halves each

    // Staging for the crypto thunk (three v128 inputs, one v128 result) and
    // for 128-bit memory callbacks (one v128).
    alignas(16) std::array<u32, 16> crypto_scratch{};
    alignas(16) std::array<u64, 2> mem_scratch{};

    IR::LocationDescriptor GetLocationDescriptor() const {
        const u64 fpcr_u64 = static_cast<u64>(fpcr & A64::LocationDescriptor::fpcr_mask) << A64::LocationDescriptor::fpcr_shift;
        const u64 pc_u64 = pc & A64::LocationDescriptor::pc_mask;
        return IR::LocationDescriptor{pc_u64 | fpcr_u64};
    }
};

}  // namespace Dynarmic::Backend::Wasm
