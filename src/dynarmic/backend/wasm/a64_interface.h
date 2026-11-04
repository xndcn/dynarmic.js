// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <memory>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/interface/A64/a64.h"

namespace Dynarmic::Backend::Wasm {

// Commit completed work from a callback in this JIT's active Run, without
// refilling the allowance. Generated code materializes its budget before calls.
void CommitA64ExecutionTicks(A64::Jit& jit);

// The boundary and observer must outlive the JIT. Opt into all region entries
// through conf.optimizations (WasmRegionMultiEntry); cycle counting is required.
std::unique_ptr<A64::Jit> CreateA64Jit(A64::UserConfig conf, const BoundaryState* boundary_state,
                                    BoundaryObserver observer = {});

}  // namespace Dynarmic::Backend::Wasm
