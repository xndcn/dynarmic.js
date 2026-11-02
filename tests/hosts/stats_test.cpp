// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <emscripten.h>
#include <emscripten/heap.h>

#include "dynarmic/backend/wasm/profiler.h"

using namespace Dynarmic::Backend::Wasm;

EM_JS(void, begin_publication_probe, (), {
    const descriptor = Object.getOwnPropertyDescriptor(Module, 'dynarmicWasmStats');
    Module.statsTestDescriptor = descriptor;
    Module.statsTestReads = 0;
    Object.defineProperty(Module, 'dynarmicWasmStats', {configurable: true,
        get() { ++Module.statsTestReads; return descriptor.value; }});
});
EM_JS(void, end_publication_probe, (int should_publish), {
    Object.defineProperty(Module, 'dynarmicWasmStats', Module.statsTestDescriptor);
    if ((Module.statsTestReads > 0) !== !!should_publish)
        throw new Error('stats: wrong publication boundary behavior');
});

EM_JS(void, check_stats, (const double* compile, const u64* profile, u64 id, int enabled), {
    const require = (test, message) => { if (!test) throw new Error('stats: ' + message); };
    const s = Module.dynarmicWasmStats;
    require(s.activeInstance === id, 'wrong active owner');
    // The helper must work when extracted from the stats object.
    const lookup = s.getInstance;
    const snapshot = lookup(id);
    require(snapshot && snapshot === s.instances[String(id)], 'missing instance');
    require(lookup('missing') === null, 'missing lookup must return null');
    const keys = ['blocks', 'bytes', 'translateMs', 'optMs', 'emitMs', 'compileMs', 'instMs', 'tableMs'];
    const expected = Array.from(new Float64Array(HEAPU8.buffer, compile, keys.length));
    for (let i = 0; i < keys.length; ++i) {
        require(s[keys[i]] === expected[i], 'legacy compile mirror ' + keys[i]);
        require(s.compile[keys[i]] === expected[i], 'active compile ' + keys[i]);
        require(snapshot.compile[keys[i]] === expected[i], 'instance compile ' + keys[i]);
    }
    if (enabled) {
        const names = ['runCount', 'stepCount', 'dispatchCount', 'dispatchCacheHits', 'dispatchCacheMisses',
            'compiledBlocks', 'compiledRegions', 'regionBlocks', 'emittedBytes', 'linkPatches',
            'fastDispatchMisses', 'invalidationCount', 'invalidatedBlocks', 'cycleBudgetExits',
            'regionCandidates', 'regionRejectedIncompatible', 'regionRejectedDistance',
            'regionRejectedBlockBudget', 'regionRejectedIrBudget', 'peakCachedBlocks', 'peakCachedBytes'];
        // Independent BigInt conversion checks the publisher's u64 ABI,
        // including values above 2^32, near 2^53 and at the u64 boundary.
        const expectedProfile = new BigUint64Array(HEAPU8.buffer, profile, names.length);
        require(s.profile === snapshot.profile, 'profile owner differs');
        require(s.profile.schema === 'dynarmic.js/profile-v1' && s.profile.enabled === true, 'profile schema');
        for (let i = 0; i < names.length; ++i) {
            require(s.profile[names[i]] === Number(expectedProfile[i]), 'profile field ' + names[i]);
        }
    } else {
        require(snapshot.profile === undefined && s.profile === undefined, 'unexpected profile');
    }
    // Retaining a compile/profile leaf must not turn it into a live view of
    // later calls, even when revisiting another JIT or growing linear memory.
    const held = Module.statsTestHeld || (Module.statsTestHeld = []);
    for (const item of held) require(JSON.stringify(item.value) === item.json, 'historical snapshot changed');
    for (const value of [snapshot.compile, snapshot.profile]) {
        if (value) held.push({value: Object.freeze(value), json: JSON.stringify(value)});
    }
});

namespace {
#if defined(DYNARMIC_WASM_PROFILE)
constexpr int profile_enabled = 1;
#else
constexpr int profile_enabled = 0;
#endif

void PublishAndCheck(const ProfileCounters* profile, const CompileStats& stats, u64 id) {
    const CompileStats original_compile = stats;
    const ProfileCounters original_profile = profile ? *profile : ProfileCounters{};
    const std::array<double, 8> expected_compile{stats.blocks, stats.bytes, stats.translate_ms,
        stats.opt_ms, stats.emit_ms, stats.compile_ms, stats.inst_ms, stats.table_ms};
    // Build the oracle by member name, independently of the serializer's word
    // offsets, so a reordered C++ layout cannot silently swap named counters.
    std::array<u64, kProfileCounterCount> expected{};
    if (profile) {
        expected = {profile->run_count, profile->step_count, profile->dispatch_count,
            profile->dispatch_cache_hits, profile->dispatch_cache_misses, profile->compiled_blocks,
            profile->compiled_regions, profile->region_blocks, profile->emitted_bytes, profile->link_patches,
            profile->fast_dispatch_misses, profile->invalidation_count, profile->invalidated_blocks,
            profile->cycle_budget_exits, profile->region_candidates, profile->region_rejected_incompatible,
            profile->region_rejected_distance, profile->region_rejected_block_budget, profile->region_rejected_ir_budget,
            profile->peak_cached_blocks, profile->peak_cached_bytes};
    }
    PublishStats(profile, stats, id);
    if (std::memcmp(&stats, &original_compile, sizeof(stats)) != 0 ||
        (profile && std::memcmp(profile, &original_profile, sizeof(*profile)) != 0)) std::abort();
    check_stats(expected_compile.data(), expected.data(), id,
                profile_enabled && profile != nullptr);
}
}  // namespace

int main() {
    CompileStats a{1, 1234, 1.25, 2.5, 3.75, 4.5, 5.25, 6.5};
    const CompileStats b = a;
    ProfileCounters profile;
    std::array<u64, kProfileCounterCount> words{};
    for (size_t i = 0; i < words.size(); ++i) words[i] = (u64{1} << 40) + i * 12345;
    words[3] = 0;
    words[5] = (u64{1} << 32) - 1;
    words[7] = (u64{1} << 53) - 1;
    words[9] = (u64{1} << 53) + 1;
    words[11] = (u64{1} << 53) + 3;
    words[13] = (u64{1} << 63) + 2049;
    words[20] = std::numeric_limits<u64>::max();
    static_assert(sizeof(words) == sizeof(profile));
    std::memcpy(&profile, words.data(), sizeof(profile));
    const u64 id_a = AllocateProfileInstanceId(), id_b = AllocateProfileInstanceId();
    PublishAndCheck(&profile, a, id_a);
    PublishAndCheck(&profile, a, id_a);
    // Identical compile totals must still switch the active instance.
    PublishAndCheck(&profile, b, id_b);
    PublishAndCheck(&profile, a, id_a);

    // Isolate every field change. A blocks-only dirty test would miss phases.
    const std::array<double*, 8> fields{&a.blocks, &a.bytes, &a.translate_ms, &a.opt_ms,
        &a.emit_ms, &a.compile_ms, &a.inst_ms, &a.table_ms};
    for (auto* field : fields) {
        *field += 16;
        PublishAndCheck(&profile, a, id_a);
    }
    ++profile.run_count;
    PublishAndCheck(&profile, a, id_a); // profile changes without compilation
    PublishAndCheck(&profile, b, id_b);
    PublishAndCheck(&profile, a, id_a);

    const size_t old_heap = emscripten_get_heap_size();
    if (!emscripten_resize_heap(old_heap + 65536) || emscripten_get_heap_size() <= old_heap) std::abort();
    a.table_ms += 8;
    ++profile.step_count;
    PublishAndCheck(&profile, a, id_a);
    PublishAndCheck(&profile, b, id_b);
    PublishAndCheck(nullptr, a, id_a); // do not leak another owner's profile
    PublishAndCheck(&profile, a, id_a);

    // Exercise the actual phase recorder with two owners and nonzero engine
    // counters. An independent expected object checks both the selected field
    // and preservation of all other fields, after the heap has grown.
    CompileStats recorded = b;
    CompileStats expected = b;
    const auto check_phase = [&] {
        if (std::memcmp(&recorded, &expected, sizeof(recorded)) != 0) std::abort();
        PublishAndCheck(&profile, recorded, id_b);
        PublishAndCheck(&profile, a, id_a);
    };
    RecordCompilePhase<CompilePhase::Translate>(nullptr, 100);
    RecordCompilePhase<CompilePhase::Optimize>(nullptr, 100);
    RecordCompilePhase<CompilePhase::Emit>(nullptr, 100);
    RecordCompilePhase<CompilePhase::Translate>(&recorded, 0.125);
    RecordCompilePhase<CompilePhase::Translate>(&recorded, 0.375);
    expected.translate_ms += 0.5;
    check_phase();
    RecordCompilePhase<CompilePhase::Optimize>(&recorded, 1.25);
    expected.opt_ms += 1.25;
    check_phase();
    RecordCompilePhase<CompilePhase::Emit>(&recorded, 2.5);
    expected.emit_ms += 2.5;
    check_phase();

    PublishAndCheck(nullptr, a, id_a);
    begin_publication_probe();
    PublishStats(nullptr, a, id_a);
    end_publication_probe(false); // unchanged compile-only owner never enters JS
    begin_publication_probe();
    PublishStats(&profile, a, id_a);
    end_publication_probe(profile_enabled); // profiling still publishes every call
    PublishAndCheck(&profile, a, id_a);
    std::puts("PASS stats snapshots and heap growth");
}
