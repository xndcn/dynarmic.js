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

#include "dynarmic/backend/wasm/a64_address_space.h"
#include "dynarmic/backend/wasm/a64_interface.h"
#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/core.h"
#include "dynarmic/backend/wasm/wasm_runtime.h"
#include "dynarmic/backend/wasm/profiler.h"
#include "dynarmic/backend/wasm/region_diagnostics.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"

namespace Dynarmic::Backend::Wasm {
namespace {
thread_local const BoundaryState* construction_boundary_state = nullptr;
thread_local BoundaryObserver construction_boundary_observer{};
thread_local A64::Jit* active_jit = nullptr;
thread_local ExecutionContext* active_context = nullptr;
thread_local A64::UserCallbacks* active_callbacks = nullptr;
}
}  // namespace Dynarmic::Backend::Wasm

namespace Dynarmic::A64 {

using namespace Backend::Wasm;

struct Jit::Impl final {
    Impl(Jit* jit_interface, A64::UserConfig conf)
            : jit_interface(jit_interface), conf(conf)
            , profile_instance_id(AllocateProfileInstanceId())
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
            , current_address_space(conf, &profiler, &compile_stats, &region_diagnostics, false, construction_boundary_state)
#else
            , current_address_space(conf, &profiler, &compile_stats, nullptr, false, construction_boundary_state)
#endif
            , core(conf, &profiler, conf.enable_cycle_counting && construction_boundary_state != nullptr,
                   construction_boundary_observer) {
        ectx.callback_config = &this->conf;
    }


    HaltReason Run() {
        ASSERT(!is_executing);
        is_executing = true;
        SCOPE_EXIT {
            is_executing = false;
        };

        // Apply invalidations requested while the Jit was idle before running.
        RequestCacheInvalidation();

        const auto old_jit = active_jit;
        const auto old_context = active_context;
        const auto old_callbacks = active_callbacks;
        active_jit = jit_interface; active_context = &ectx; active_callbacks = conf.callbacks;
        SCOPE_EXIT {
            active_jit = old_jit; active_context = old_context; active_callbacks = old_callbacks;
        };
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
        ASSERT(!is_executing);
        is_executing = true;
        SCOPE_EXIT {
            is_executing = false;
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

    void InvalidateCacheRange(u64 start_address, size_t length) {
        if (length == 0) {
            return;
        }
        std::unique_lock lock{invalidation_mutex};
        const u64 end = length - 1 > std::numeric_limits<u64>::max() - start_address
                                ? std::numeric_limits<u64>::max()
                                : start_address + length - 1;
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

    A64JitState& State() { return current_state; }
    const A64JitState& State() const { return current_state; }

    bool IsExecuting() const { return is_executing; }

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
    A64::UserConfig conf;
    A64JitState current_state{};
    ProfileCounters profiler{};
    CompileStats compile_stats{};
    u64 profile_instance_id = 0;
    ExecutionContext ectx{};
#if defined(DYNARMIC_WASM_REGION_DIAGNOSTICS)
    RegionDiagnostics region_diagnostics;
#endif
    A64AddressSpace current_address_space;
    Core<A64::UserConfig, A64JitState> core;

    std::mutex invalidation_mutex;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    bool invalidate_entire_cache = false;
    bool is_executing = false;
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

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
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

u64 Jit::GetSP() const {
    return impl->State().sp;
}

void Jit::SetSP(u64 value) {
    impl->State().sp = value;
}

u64 Jit::GetPC() const {
    return impl->State().pc;
}

void Jit::SetPC(u64 value) {
    impl->State().pc = value;
}

u64 Jit::GetRegister(size_t index) const {
    return impl->State().reg.at(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->State().reg.at(index) = value;
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->State().reg;
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->State().reg = value;
}

Vector Jit::GetVector(size_t index) const {
    const auto& vec = impl->State().vec;
    return {vec.at(index * 2), vec.at(index * 2 + 1)};
}

void Jit::SetVector(size_t index, Vector value) {
    auto& vec = impl->State().vec;
    vec.at(index * 2) = value[0];
    vec.at(index * 2 + 1) = value[1];
}

std::array<Vector, 32> Jit::GetVectors() const {
    std::array<Vector, 32> ret;
    static_assert(sizeof(ret) == sizeof(A64JitState::vec));
    std::memcpy(ret.data(), impl->State().vec.data(), sizeof(ret));
    return ret;
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    std::memcpy(impl->State().vec.data(), value.data(), sizeof(value));
}

u32 Jit::GetFpcr() const {
    return impl->State().fpcr;
}

void Jit::SetFpcr(u32 value) {
    impl->State().fpcr = value;
}

u32 Jit::GetFpsr() const {
    return impl->State().fpsr;
}

void Jit::SetFpsr(u32 value) {
    impl->State().fpsr = value;
}

u32 Jit::GetPstate() const {
    return impl->State().cpsr_nzcv;
}

void Jit::SetPstate(u32 value) {
    impl->State().cpsr_nzcv = value & 0xF0000000;
}

void Jit::ClearExclusiveState() {
    impl->State().exclusive_state = 0;
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

void Jit::DumpDisassembly() const {
    UNIMPLEMENTED();
}

std::vector<std::string> Jit::Disassemble() const {
    return {};
}

}  // namespace Dynarmic::A64

namespace Dynarmic::Backend::Wasm {

void CommitA64ExecutionTicks(A64::Jit& jit) {
    ASSERT(active_jit == &jit && active_context != nullptr);
    const u64 elapsed = static_cast<u64>(active_context->cycles_to_run - active_context->cycles_remaining);
    active_context->cycles_to_run = active_context->cycles_remaining;
    if (elapsed != 0) active_callbacks->AddTicks(elapsed);
}

std::unique_ptr<A64::Jit> CreateA64Jit(A64::UserConfig conf, const BoundaryState* boundary_state,
                                    BoundaryObserver observer) {
    const auto old_boundary = construction_boundary_state;
    const auto old_observer = construction_boundary_observer;
    construction_boundary_state = boundary_state;
    construction_boundary_observer = observer;
    SCOPE_EXIT {
        construction_boundary_state = old_boundary;
        construction_boundary_observer = old_observer;
    };
    return std::make_unique<A64::Jit>(conf);
}

}  // namespace Dynarmic::Backend::Wasm
