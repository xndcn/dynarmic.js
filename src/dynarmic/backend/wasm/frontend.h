// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

namespace Dynarmic::Backend::Wasm {

// Guest architecture of the block being compiled. Selects the state struct
// (A32JitState / A64JitState), how location descriptors are written back,
// and which context opcodes are legal.
enum class Frontend {
    A32,
    A64,
};

}  // namespace Dynarmic::Backend::Wasm
