// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <mcl/stdint.hpp>

namespace Dynarmic::IR {
class Inst;
}

namespace Dynarmic::Backend::Wasm {
struct EmitContext;

// Shared scalar/SIMD FMINNM/FMAXNM lowering for 32- and 64-bit elements.
// Implemented in emit_wasm_vector_fp.cpp.
void EmitFPMinMaxNumericOp(EmitContext& ctx, IR::Inst* inst, u32 esize, bool vector, bool is_max);
}
