// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Per-instance compile-time accounting (translate / optimize / emit), surfaced
// in the owning Jit's Emscripten snapshot next to engine-side numbers recorded
// by wasm_runtime_emscripten.cpp. Phase writes are no-ops outside Emscripten.

#pragma once

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Dynarmic::Backend::Wasm {

// Per-JIT compilation counters. The object is owned by the public JIT
// instance and is passed through the address space/runtime boundary; it is
// never a module-global accumulator. Doubles keep the Emscripten JS bridge
// free of BigInt boxing while counts remain exact for realistic workloads.
struct CompileStats {
    double blocks = 0;
    double bytes = 0;
    double translate_ms = 0;
    double opt_ms = 0;
    double emit_ms = 0;
    double compile_ms = 0;
    double inst_ms = 0;
    double table_ms = 0;

    bool operator==(const CompileStats&) const = default;
};
static_assert(sizeof(CompileStats) == sizeof(double) * 8);

enum class CompilePhase {
    Translate,
    Optimize,
    Emit,
};

// C++ phases update their owning object directly. The phase is selected at
// compile time; only engine compilation/instantiation needs a JS boundary.
template<CompilePhase phase>
inline void RecordCompilePhase([[maybe_unused]] CompileStats* stats, [[maybe_unused]] double ms) {
    static_assert(phase == CompilePhase::Translate || phase == CompilePhase::Optimize || phase == CompilePhase::Emit);
#ifdef __EMSCRIPTEN__
    if (!stats) return;
    if constexpr (phase == CompilePhase::Translate) {
        stats->translate_ms += ms;
    } else if constexpr (phase == CompilePhase::Optimize) {
        stats->opt_ms += ms;
    } else {
        stats->emit_ms += ms;
    }
#endif
}

#ifdef __EMSCRIPTEN__
inline double CompileStatsNowMs() {
    return emscripten_get_now();
}
#else
inline double CompileStatsNowMs() {
    return 0.0;
}
#endif

}  // namespace Dynarmic::Backend::Wasm
