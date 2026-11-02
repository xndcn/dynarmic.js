// Shared measurement protocol. Every sample runs the same JIT instance.
#pragma once

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

inline double benchmark_now() { return emscripten_get_now(); }

EM_JS(void, benchmark_begin, (), {
    Module.benchmarkBefore = {...(Module.dynarmicWasmStats || {})};
    const profile = Module.benchmarkBefore.profile;
    if (profile) Module.benchmarkBefore.profile = {...profile};
});

EM_JS(void, benchmark_sample, (double ms, double instructions, int cold, int run_index), {
    const before = Module.benchmarkBefore || {};
    const after = Module.dynarmicWasmStats || {};
    const compile = {};
    for (const key of ['blocks', 'bytes', 'translateMs', 'optMs', 'emitMs', 'compileMs', 'instMs', 'tableMs']) {
        compile[key] = (after[key] || 0) - (before[key] || 0);
    }
    let profile = null;
    if (after.profile) {
        profile = {schema: after.profile.schema, enabled: true};
        for (const [key, value] of Object.entries(after.profile)) {
            if (typeof value === 'number') profile[key] = value - (before.profile?.[key] || 0);
        }
    }
    console.log('BENCHMARK ' + JSON.stringify({
        phase: cold ? 'cold' : 'warm', runIndex: run_index, ms, instructions,
        minstrPerSec: instructions / ms / 1000, compile, profile,
    }));
});

#else
#include <chrono>
#include <cstdio>

inline double benchmark_now() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}
inline void benchmark_begin() {}
inline void benchmark_sample(double ms, double instructions, int cold, int run_index) {
    // Native Dynarmic has no equivalent of the Wasm compilation phase counters.
    // Null means unavailable, rather than a fabricated zero-cost compilation.
    std::printf("BENCHMARK {\"phase\":\"%s\",\"runIndex\":%d,\"ms\":%.17g,"
                "\"instructions\":%.17g,\"minstrPerSec\":%.17g,\"compile\":null,\"profile\":null}\n",
                cold ? "cold" : "warm", run_index, ms, instructions, instructions / ms / 1000);
}
#endif
