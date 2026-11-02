// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include <cstddef>

#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/compile_stats.h"

namespace Dynarmic::Backend::Wasm {

// Counters are deliberately POD and monotonically increasing. The layout is
// part of the Emscripten profile ABI: profiler.cpp publishes these words in
// the owning Jit's instance snapshot. Keep additions at the end and bump the
// schema when a field changes meaning.
struct ProfileCounters {
    u64 run_count = 0;
    u64 step_count = 0;
    u64 dispatch_count = 0;
    u64 dispatch_cache_hits = 0;
    u64 dispatch_cache_misses = 0;
    u64 compiled_blocks = 0;
    u64 compiled_regions = 0;
    u64 region_blocks = 0;
    u64 emitted_bytes = 0;
    u64 link_patches = 0;
    u64 fast_dispatch_misses = 0;
    u64 invalidation_count = 0;
    u64 invalidated_blocks = 0;
    u64 cycle_budget_exits = 0;
    u64 region_candidates = 0;
    u64 region_rejected_incompatible = 0;
    u64 region_rejected_distance = 0;
    u64 region_rejected_block_budget = 0;
    u64 region_rejected_ir_budget = 0;
    // High-water marks for live emitted modules. These remain monotonic so a
    // profile delta stays meaningful when the guest invalidates code.
    u64 peak_cached_blocks = 0;
    u64 peak_cached_bytes = 0;

    void Reset() {
        *this = {};
    }
};

inline constexpr std::size_t kProfileCounterCount = sizeof(ProfileCounters) / sizeof(u64);
static_assert(kProfileCounterCount == 21);
inline constexpr const char* kProfileSchema = "dynarmic.js/profile-v1";

// Publishes per-instance profile and compile snapshots to the host. The JS
// object keeps an `instances` registry and exposes the most recently published
// instance at its legacy top-level fields for existing embedders.
#if defined(__EMSCRIPTEN__)
u64 AllocateProfileInstanceId();
void PublishStats(const ProfileCounters* counters, const CompileStats& compile_stats, u64 instance_id);
#else
inline u64 AllocateProfileInstanceId() { return 0; }
inline void PublishStats(const ProfileCounters*, const CompileStats&, u64) {}
#endif

// These macros make the disabled build genuinely zero cost.  In particular,
// no branch is left in Core::Run or AddressSpace::GetOrEmit when the option is
// off, which matters for dispatch-heavy guests.
#if defined(DYNARMIC_WASM_PROFILE)
#define DYNARMIC_WASM_PROFILE_INC(profile, field) \
    do { if ((profile) != nullptr) { ++(profile)->field; } } while (false)
#define DYNARMIC_WASM_PROFILE_ADD(profile, field, value) \
    do { if ((profile) != nullptr) { (profile)->field += (value); } } while (false)
#else
#define DYNARMIC_WASM_PROFILE_INC(profile, field) do { } while (false)
#define DYNARMIC_WASM_PROFILE_ADD(profile, field, value) do { } while (false)
#endif

}  // namespace Dynarmic::Backend::Wasm

