// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// WASM backend runtime: instantiation of emitted block modules and the host
// callback thunks the modules import.
//
// Block modules cannot be instantiated from C++ — a WASM engine is required.
// Under Emscripten the InstantiateBlock implementation (wasm_runtime_emscripten.cpp)
// forwards to JS, which compiles the module, instantiates it against the main
// module's linear memory and the thunks below, and registers the exported
// fresh entry function in the shared Emscripten indirect function table. The returned
// table index doubles as a C function pointer (v86-style).

#pragma once

#include <cstddef>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/config.h"

namespace Dynarmic::Backend::Wasm {

struct CompileStats;

// Signature of a compiled block entry point (see abi.h). Under Emscripten
// this is a function pointer whose value is a WASM table index.
using BlockFunc = u32 (*)(uptr state, uptr ectx);

// Compiles and instantiates one emitted block module, which must define its
// own f(i32, i32) -> i32 entry (not re-export an imported function). Returns
// its table index under Emscripten. Engine failures propagate to the host.
BlockFunc InstantiateBlock(const u8* bytes, size_t size, CompileStats* stats = nullptr);

// Releases the table slot of a function returned by InstantiateBlock.
void ReleaseBlock(BlockFunc fn);

// Debug aid: whether newly emitted modules carry function names (see
// EmitConfig::function_names). Toggled by dynarmic_wasm_debug_names().
bool DebugFunctionNames();

}  // namespace Dynarmic::Backend::Wasm

// Host-callback thunks imported by generated modules (names must match
// abi.h). Extern "C" so the Emscripten glue can reference them by symbol.
extern "C" {
u32 dynarmic_wasm_read8(uptr state, uptr ectx, u32 vaddr);
u32 dynarmic_wasm_read16(uptr state, uptr ectx, u32 vaddr);
u32 dynarmic_wasm_read32(uptr state, uptr ectx, u32 vaddr);
u64 dynarmic_wasm_read64(uptr state, uptr ectx, u32 vaddr);
void dynarmic_wasm_write8(uptr state, uptr ectx, u32 vaddr, u32 value);
void dynarmic_wasm_write16(uptr state, uptr ectx, u32 vaddr, u32 value);
void dynarmic_wasm_write32(uptr state, uptr ectx, u32 vaddr, u32 value);
void dynarmic_wasm_write64(uptr state, uptr ectx, u32 vaddr, u64 value);
u32 dynarmic_wasm_xread8(uptr state, uptr ectx, u32 vaddr);
u32 dynarmic_wasm_xread16(uptr state, uptr ectx, u32 vaddr);
u32 dynarmic_wasm_xread32(uptr state, uptr ectx, u32 vaddr);
u64 dynarmic_wasm_xread64(uptr state, uptr ectx, u32 vaddr);
u32 dynarmic_wasm_xwrite8(uptr state, uptr ectx, u32 vaddr, u32 value);
u32 dynarmic_wasm_xwrite16(uptr state, uptr ectx, u32 vaddr, u32 value);
u32 dynarmic_wasm_xwrite32(uptr state, uptr ectx, u32 vaddr, u32 value);
u32 dynarmic_wasm_xwrite64(uptr state, uptr ectx, u32 vaddr, u64 value);
void dynarmic_wasm_xclear(uptr state, uptr ectx);
void dynarmic_wasm_svc(uptr state, uptr ectx, u32 swi);
void dynarmic_wasm_raise(uptr state, uptr ectx, u32 pc, u64 exception);
u32 dynarmic_wasm_interp(uptr state, uptr ectx, u32 next_low, u32 next_high, u32 num_instructions);
void dynarmic_wasm_dsb(uptr state, uptr ectx);
void dynarmic_wasm_icache(uptr state, uptr ectx);
}
