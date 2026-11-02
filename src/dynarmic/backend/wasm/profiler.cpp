// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include "dynarmic/backend/wasm/profiler.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

namespace Dynarmic::Backend::Wasm {

EM_JS(void, js_publish_stats, (const u8* profile_ptr, const u8* compile_ptr, u64 instance_id), {
    const stats = Module.dynarmicWasmStats || (Module.dynarmicWasmStats = {});
    const active = stats.activeInstance === instance_id;
    let compile = active ? stats.compile : stats.instances?.[String(instance_id)]?.compile;
    // Read the current Emscripten views, not views retained across memory.grow.
    const c = compile_ptr >>> 3;
    const unchanged = compile &&
        compile.blocks === HEAPF64[c] && compile.bytes === HEAPF64[c + 1] &&
        compile.translateMs === HEAPF64[c + 2] && compile.optMs === HEAPF64[c + 3] &&
        compile.emitMs === HEAPF64[c + 4] && compile.compileMs === HEAPF64[c + 5] &&
        compile.instMs === HEAPF64[c + 6] && compile.tableMs === HEAPF64[c + 7];
    // A production cache hit has nothing new to publish. Check every field:
    // phase totals can change even when the module count has not changed.
    if (!profile_ptr && active && unchanged && stats.profile === undefined) return;
    if (!unchanged) {
        compile = {blocks: HEAPF64[c], bytes: HEAPF64[c + 1],
            translateMs: HEAPF64[c + 2], optMs: HEAPF64[c + 3], emitMs: HEAPF64[c + 4],
            compileMs: HEAPF64[c + 5], instMs: HEAPF64[c + 6], tableMs: HEAPF64[c + 7]};
    }
    const instances = stats.instances || (stats.instances = {});
    if (!stats.getInstance) stats.getInstance = id => stats.instances?.[String(id)] || null;
    const id = String(instance_id);
    if (profile_ptr) {
        // u64 -> Number without allocating a BigInt for each counter. Wasm is
        // little endian. Both halves and hi * 2^32 are exact doubles, so the
        // final addition rounds exactly as Number(BigInt(u64)) does. Profiling
        // is per-instance and published synchronously at the end of Run/Step.
        const p = profile_ptr >>> 2;
        const profile = {schema: 'dynarmic.js/profile-v1', enabled: true,
            runCount: HEAPU32[p] + HEAPU32[p + 1] * 4294967296,
            stepCount: HEAPU32[p + 2] + HEAPU32[p + 3] * 4294967296,
            dispatchCount: HEAPU32[p + 4] + HEAPU32[p + 5] * 4294967296,
            dispatchCacheHits: HEAPU32[p + 6] + HEAPU32[p + 7] * 4294967296,
            dispatchCacheMisses: HEAPU32[p + 8] + HEAPU32[p + 9] * 4294967296,
            compiledBlocks: HEAPU32[p + 10] + HEAPU32[p + 11] * 4294967296,
            compiledRegions: HEAPU32[p + 12] + HEAPU32[p + 13] * 4294967296,
            regionBlocks: HEAPU32[p + 14] + HEAPU32[p + 15] * 4294967296,
            emittedBytes: HEAPU32[p + 16] + HEAPU32[p + 17] * 4294967296,
            linkPatches: HEAPU32[p + 18] + HEAPU32[p + 19] * 4294967296,
            fastDispatchMisses: HEAPU32[p + 20] + HEAPU32[p + 21] * 4294967296,
            invalidationCount: HEAPU32[p + 22] + HEAPU32[p + 23] * 4294967296,
            invalidatedBlocks: HEAPU32[p + 24] + HEAPU32[p + 25] * 4294967296,
            cycleBudgetExits: HEAPU32[p + 26] + HEAPU32[p + 27] * 4294967296,
            regionCandidates: HEAPU32[p + 28] + HEAPU32[p + 29] * 4294967296,
            regionRejectedIncompatible: HEAPU32[p + 30] + HEAPU32[p + 31] * 4294967296,
            regionRejectedDistance: HEAPU32[p + 32] + HEAPU32[p + 33] * 4294967296,
            regionRejectedBlockBudget: HEAPU32[p + 34] + HEAPU32[p + 35] * 4294967296,
            regionRejectedIrBudget: HEAPU32[p + 36] + HEAPU32[p + 37] * 4294967296,
            peakCachedBlocks: HEAPU32[p + 38] + HEAPU32[p + 39] * 4294967296,
            peakCachedBytes: HEAPU32[p + 40] + HEAPU32[p + 41] * 4294967296,
        };
        const snapshot = instances[id] || (instances[id] = {});
        snapshot.compile = compile;
        snapshot.profile = profile;
        stats.profile = profile;
    } else {
        // Keep previously returned compile snapshots intact when values change.
        if (instances[id]?.compile !== compile || instances[id]?.profile) instances[id] = {compile};
        delete stats.profile;
    }
    // Legacy fields describe the active instance, including switches between
    // instances with identical totals; they are never aggregate counters.
    if (!active || !unchanged) {
        stats.activeInstance = instance_id;
        stats.compile = compile;
        Object.assign(stats, compile);
    }
});

static u64 next_instance_id = 1;

u64 AllocateProfileInstanceId() {
    return next_instance_id++;
}

void PublishStats([[maybe_unused]] const ProfileCounters* counters, const CompileStats& compile_stats, u64 instance_id) {
#if defined(DYNARMIC_WASM_PROFILE)
    const auto* profile_ptr = reinterpret_cast<const u8*>(counters);
#else
    const u8* profile_ptr = nullptr;
#endif
    // Cache only the last compile-only publication, shared across JIT owners
    // in this Wasm module. A same-owner cache hit has no new observable data:
    // avoid the JS call (and boxing the u64 instance ID) altogether. Compare
    // every phase, not just block count. Owner switches must still publish.
    static CompileStats last_compile;
    static u64 last_instance_id = 0;
    static bool compile_only_published = false;
    if (!profile_ptr && compile_only_published && instance_id == last_instance_id && compile_stats == last_compile) {
        return;
    }
    // Do not leave a usable cache entry if publication throws partway through.
    compile_only_published = false;
    js_publish_stats(profile_ptr, reinterpret_cast<const u8*>(&compile_stats), instance_id);
    if (!profile_ptr) {
        last_instance_id = instance_id;
        last_compile = compile_stats;
        compile_only_published = true;
    }
}

}  // namespace Dynarmic::Backend::Wasm
#endif
