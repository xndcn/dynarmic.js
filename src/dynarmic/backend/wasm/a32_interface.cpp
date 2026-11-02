// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include <mcl/assert.hpp>
#include <mcl/scope_exit.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/a32_address_space.h"
#include "dynarmic/backend/wasm/a32_interface.h"
#include "dynarmic/backend/wasm/core.h"
#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/wasm_runtime.h"
#include "dynarmic/backend/wasm/profiler.h"
#include "dynarmic/backend/wasm/region_diagnostics.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/interface/A32/a32.h"

namespace Dynarmic::Backend::Wasm {
namespace {
thread_local const BoundaryState* construction_boundary_state = nullptr;
thread_local bool construction_specialize_boundary_hooks = false;
thread_local BoundaryObserver construction_boundary_observer{};
thread_local A32::Jit* active_jit = nullptr;
thread_local ExecutionContext* active_context = nullptr;
thread_local A32::UserCallbacks* active_callbacks = nullptr;
}
}  // namespace Dynarmic::Backend::Wasm

namespace Dynarmic::A32 {

using namespace Backend::Wasm;

struct Jit::Impl final {
    Impl(Jit* jit_interface, A32::UserConfig conf)
            : jit_interface(jit_interface)
            , conf(conf)
            , profile_instance_id(AllocateProfileInstanceId())
            , boundary_state(construction_boundary_state)
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
            , current_address_space(conf, &profiler, &compile_stats, &region_diagnostics, boundary_state, construction_specialize_boundary_hooks)
#else
            , current_address_space(conf, &profiler, &compile_stats, nullptr, boundary_state, construction_specialize_boundary_hooks)
#endif
            , core(conf, &profiler, conf.enable_cycle_counting && boundary_state != nullptr, construction_boundary_observer) {
        ectx.callback_config = &this->conf;
    }


    HaltReason Run() {
        ASSERT(!jit_interface->is_executing);
        jit_interface->is_executing = true;
        SCOPE_EXIT {
            jit_interface->is_executing = false;
        };

        // Apply invalidations requested while the Jit was idle before running.
        RequestCacheInvalidation();

        const auto old_jit = active_jit;
        const auto old_context = active_context;
        const auto old_callbacks = active_callbacks;
        active_jit = jit_interface; active_context = &ectx; active_callbacks = conf.callbacks;
        SCOPE_EXIT { active_jit = old_jit; active_context = old_context; active_callbacks = old_callbacks; };
        const HaltReason hr = core.Run(current_address_space, current_state, ectx);

        RequestCacheInvalidation();
        PublishStats(&profiler, compile_stats, profile_instance_id);
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        PublishRegionDiagnostics(region_diagnostics, profile_instance_id);
        PublishRegionTransferDiagnostics(region_diagnostics, profile_instance_id);
#endif

        return hr;
    }

    HaltReason Step() {
        ASSERT(!jit_interface->is_executing);
        jit_interface->is_executing = true;
        SCOPE_EXIT {
            jit_interface->is_executing = false;
        };

        RequestCacheInvalidation();

        const IR::LocationDescriptor step_location{LocationDescriptor{current_state.GetLocationDescriptor()}.SetSingleStepping(true)};
        const HaltReason hr = core.Step(current_address_space, current_state, ectx, step_location);

        RequestCacheInvalidation();
        PublishStats(&profiler, compile_stats, profile_instance_id);
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
        PublishRegionDiagnostics(region_diagnostics, profile_instance_id);
        PublishRegionTransferDiagnostics(region_diagnostics, profile_instance_id);
#endif

        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u32 start_address, size_t length) {
        if (length == 0) {
            return;
        }
        std::unique_lock lock{invalidation_mutex};
        const u64 span = static_cast<u64>(length - 1);
        const u64 end = span > std::numeric_limits<u32>::max() - start_address
                                ? std::numeric_limits<u32>::max()
                                : static_cast<u64>(start_address) + span;
        invalid_cache_ranges.add(boost::icl::discrete_interval<u64>::closed(start_address, end));
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        current_state = {};
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&ectx.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&ectx.halt_reason, ~static_cast<u32>(hr));
    }

    std::array<u32, 16>& Regs() {
        return current_state.regs;
    }

    const std::array<u32, 16>& Regs() const {
        return current_state.regs;
    }

    std::array<u32, 64>& ExtRegs() {
        return current_state.ext_regs;
    }

    const std::array<u32, 64>& ExtRegs() const {
        return current_state.ext_regs;
    }

    u32 Cpsr() const {
        return current_state.Cpsr();
    }

    void SetCpsr(u32 value) {
        current_state.SetCpsr(value);
    }

    u32 Fpscr() const {
        return current_state.Fpscr();
    }

    void SetFpscr(u32 value) {
        current_state.SetFpscr(value);
    }

    void ClearExclusiveState() {
        current_state.exclusive_state = 0;
    }

    void DumpDisassembly() const {
        UNIMPLEMENTED();
    }

private:
    void RequestCacheInvalidation() {
        std::unique_lock lock{invalidation_mutex};
        if (invalidate_entire_cache || !invalid_cache_ranges.empty()) {
            if (invalidate_entire_cache) {
                current_address_space.ClearCache();
            } else {
                current_address_space.InvalidateRanges(invalid_cache_ranges);
            }
            // RSB entries hold table indices of possibly released blocks.
            ectx.rsb_top = 0;
            ectx.link_slot_addr = 0;
            ectx.fast_dispatch_entry = 0;
            std::memset(&ectx.rsb, 0, sizeof(ectx.rsb));
            invalidate_entire_cache = false;
            invalid_cache_ranges.clear();
            ClearHalt(HaltReason::CacheInvalidation);
        }
    }

    Jit* jit_interface;
    A32::UserConfig conf;
    A32JitState current_state{};
    ProfileCounters profiler{};
    CompileStats compile_stats{};
    u64 profile_instance_id = 0;
    const BoundaryState* boundary_state = nullptr;
    ExecutionContext ectx{};
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    RegionDiagnostics region_diagnostics;
#endif
    A32AddressSpace current_address_space;
    Core<A32::UserConfig, A32JitState> core;

    std::mutex invalidation_mutex;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    bool invalidate_entire_cache = false;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u32 start_address, std::size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

std::array<u32, 16>& Jit::Regs() {
    return impl->Regs();
}

const std::array<u32, 16>& Jit::Regs() const {
    return impl->Regs();
}

std::array<u32, 64>& Jit::ExtRegs() {
    return impl->ExtRegs();
}

const std::array<u32, 64>& Jit::ExtRegs() const {
    return impl->ExtRegs();
}

u32 Jit::Cpsr() const {
    return impl->Cpsr();
}

void Jit::SetCpsr(u32 value) {
    impl->SetCpsr(value);
}

u32 Jit::Fpscr() const {
    return impl->Fpscr();
}

void Jit::SetFpscr(u32 value) {
    impl->SetFpscr(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

void Jit::DumpDisassembly() const {
    impl->DumpDisassembly();
}

}  // namespace Dynarmic::A32

namespace Dynarmic::Backend::Wasm {

void CommitA32ExecutionTicks(A32::Jit& jit) {
    ASSERT(active_jit == &jit && active_context != nullptr);
    const u64 elapsed = static_cast<u64>(active_context->cycles_to_run - active_context->cycles_remaining);
    active_context->cycles_to_run = active_context->cycles_remaining;
    if (elapsed != 0) active_callbacks->AddTicks(elapsed);
}

std::unique_ptr<A32::Jit> CreateA32Jit(A32::UserConfig conf, const BoundaryState* boundary_state, bool specialize_boundary_hooks, BoundaryObserver observer) {
    construction_boundary_observer = observer;
    construction_boundary_state = boundary_state;
    construction_specialize_boundary_hooks = specialize_boundary_hooks;
    SCOPE_EXIT {
        construction_boundary_state = nullptr;
        construction_specialize_boundary_hooks = false;
        construction_boundary_observer = {};
    };
    return std::make_unique<A32::Jit>(conf);
}

}  // namespace Dynarmic::Backend::Wasm
