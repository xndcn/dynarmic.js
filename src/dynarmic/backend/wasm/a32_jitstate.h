/* This file is part of the dynarmic project.
 * Copyright (c) 2024 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>

#include <mcl/stdint.hpp>

#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Wasm {

struct A32JitState {
    u32 cpsr_nzcv = 0;
    u32 cpsr_q = 0;
    u32 cpsr_jaifm = 0;
    u32 cpsr_ge = 0;

    u32 fpsr = 0;  // FPSR part of FPSCR: NZCV (31..28), QC (27), cumulative exception bits

    std::array<u32, 16> regs{};

    u32 upper_location_descriptor;

    alignas(16) std::array<u32, 64> ext_regs{};

    u32 exclusive_state = 0;

    // Operand/result staging for the crypto thunk: three v128 inputs followed
    // by one v128 result (AES / SHA256 helpers).
    alignas(16) std::array<u32, 16> crypto_scratch{};

    u32 Cpsr() const;
    void SetCpsr(u32 cpsr);

    u32 Fpscr() const;
    void SetFpscr(u32 fpscr);

    IR::LocationDescriptor GetLocationDescriptor() const {
        return IR::LocationDescriptor{regs[15] | (static_cast<u64>(upper_location_descriptor) << 32)};
    }
};

}  // namespace Dynarmic::Backend::Wasm
