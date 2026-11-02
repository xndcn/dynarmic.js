// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Native (non-Emscripten) fallback for the WASM backend runtime. There is no
// WASM engine embedded in native builds, so instantiation is unavailable;
// set DYNARMIC_WASM_DUMP_DIR to have emitted block modules written to disk
// for external validation (e.g. with Node).

#ifndef __EMSCRIPTEN__

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/wasm_runtime.h"

namespace Dynarmic::Backend::Wasm {

BlockFunc InstantiateBlock(const u8* bytes, size_t size, CompileStats*) {
    if (const char* dump_dir = std::getenv("DYNARMIC_WASM_DUMP_DIR")) {
        static size_t counter = 0;
        const std::string path = std::string(dump_dir) + "/block_" + std::to_string(counter++) + ".wasm";
        std::ofstream out{path, std::ios::binary};
        out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    }
    ASSERT_MSG(false, "WASM backend: block instantiation requires an Emscripten host");
    return nullptr;
}

void ReleaseBlock(BlockFunc) {}

}  // namespace Dynarmic::Backend::Wasm

#endif  // __EMSCRIPTEN__
