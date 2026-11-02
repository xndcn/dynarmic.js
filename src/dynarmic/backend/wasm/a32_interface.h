// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <memory>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/interface/A32/a32.h"

namespace Dynarmic::Backend::Wasm {

// Constructs an A32 JIT with optional board boundary state in shared linear
// memory. Ordinary A32::Jit construction remains independent of this policy.
// specialize_boundary_hooks is an explicit whole-cache invalidation contract:
// the caller must quiesce execution, clear the entire JIT after every hook-state
// or slot mutation, and serialize mutation with compilation/publication.
// Commit completed cycle-counted work from a callback in this active Run.
// Generated code must materialize its budget before calling (host-call IR does).
// Does not refill the allowance or sample IRQs; cannot be called while idle.
void CommitA32ExecutionTicks(A32::Jit& jit);

std::unique_ptr<A32::Jit> CreateA32Jit(A32::UserConfig conf, const BoundaryState* boundary_state,
                                       bool specialize_boundary_hooks = false,
                                       BoundaryObserver observer = {});

}  // namespace Dynarmic::Backend::Wasm
