// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#pragma once

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/address_space.h"
#include "dynarmic/backend/wasm/profiler.h"
#include "dynarmic/interface/halt_reason.h"

namespace Dynarmic::Backend::Wasm {

// The dispatcher loop shared by both frontends. UserConfig is A32::UserConfig
// or A64::UserConfig; JitState is the matching state struct.
template<typename UserConfig, typename JitState>
class Core final {
public:
    explicit Core(const UserConfig& conf, ProfileCounters* profiler = nullptr,
                  bool boundary_dispatch = false, BoundaryObserver observer = {})
            : conf(conf), profiler(profiler), boundary_dispatch(boundary_dispatch), observer(observer) {}

    // ectx is owned by the caller (the Jit impl) and persists across Run
    // calls: its halt_reason field doubles as the Jit's halt flag, so blocks
    // observe HaltExecution requests made from within callbacks immediately.
    HaltReason Run(AddressSpace& process, JitState& thread_ctx, ExecutionContext& ectx) {
        DYNARMIC_WASM_PROFILE_INC(profiler, run_count);
        if (boundary_dispatch) {
            return RunBoundaryDispatch(process, thread_ctx, ectx);
        }
        if (conf.enable_cycle_counting) {
            ectx.cycles_to_run = static_cast<s64>(conf.callbacks->GetTicksRemaining());
            ectx.cycles_remaining = ectx.cycles_to_run;
        }

        while (true) {
            if (ectx.halt_reason != 0) {
                const HaltReason result = static_cast<HaltReason>(ectx.halt_reason);
                return result;
            }

            DYNARMIC_WASM_PROFILE_INC(profiler, dispatch_count);
            const BlockFunc entry_point = process.GetOrEmit(thread_ctx.GetLocationDescriptor());
            entry_point(reinterpret_cast<uptr>(&thread_ctx), reinterpret_cast<uptr>(&ectx));

            if (ectx.fast_dispatch_entry != 0) {
                DYNARMIC_WASM_PROFILE_INC(profiler, fast_dispatch_misses);
                // An indirect branch missed the fast dispatch table: cache the
                // block for the location it went to.
                const IR::LocationDescriptor location = thread_ctx.GetLocationDescriptor();
                auto* const entry = reinterpret_cast<FastDispatchEntry*>(static_cast<uptr>(ectx.fast_dispatch_entry));
                entry->index = static_cast<u32>(reinterpret_cast<uptr>(process.GetOrEmit(location)));
                entry->descriptor = location.Value();
                ectx.fast_dispatch_entry = 0;
            }

            if (ectx.link_slot_addr != 0) {
                // A block hit an unlinked LinkBlockFast edge and already wrote
                // the next location descriptor to the state: patch its link
                // slot with the target block's function-table index so future
                // executions tail-call the target directly.
                const BlockFunc next = process.GetOrEmit(thread_ctx.GetLocationDescriptor());
                u32* const slot = reinterpret_cast<u32*>(static_cast<uptr>(ectx.link_slot_addr));
                *slot = static_cast<u32>(reinterpret_cast<uptr>(next));
                process.RecordLink(slot, thread_ctx.GetLocationDescriptor());
                process.RecordDispatchTarget(thread_ctx.GetLocationDescriptor(), next);
                ectx.link_slot_addr = 0;
            }

            if (conf.enable_cycle_counting && ectx.cycles_remaining < 0) {
                DYNARMIC_WASM_PROFILE_INC(profiler, cycle_budget_exits);
                conf.callbacks->AddTicks(static_cast<u64>(ectx.cycles_to_run - ectx.cycles_remaining));
                ectx.cycles_to_run = static_cast<s64>(conf.callbacks->GetTicksRemaining());
                ectx.cycles_remaining = ectx.cycles_to_run;
            }
        }
    }

    // Executes exactly one guest instruction: `step_location` is the current
    // location with the frontend's single-stepping flag set.
    HaltReason Step(AddressSpace& process, JitState& thread_ctx, ExecutionContext& ectx, IR::LocationDescriptor step_location) {
        DYNARMIC_WASM_PROFILE_INC(profiler, step_count);
        if (ectx.halt_reason != 0) {
            return static_cast<HaltReason>(ectx.halt_reason);
        }
        if (conf.enable_cycle_counting) {
            ectx.cycles_to_run = static_cast<s64>(conf.callbacks->GetTicksRemaining());
            ectx.cycles_remaining = ectx.cycles_to_run;
        }
        const BlockFunc entry_point = process.GetOrEmit(step_location);
        entry_point(reinterpret_cast<uptr>(&thread_ctx), reinterpret_cast<uptr>(&ectx));
        ectx.link_slot_addr = 0;
        ectx.fast_dispatch_entry = 0;
        if (conf.enable_cycle_counting) {
            conf.callbacks->AddTicks(static_cast<u64>(ectx.cycles_to_run - ectx.cycles_remaining));
        }
        const HaltReason result = static_cast<HaltReason>(ectx.halt_reason | static_cast<u32>(HaltReason::Step));
        return result;
    }

private:
    HaltReason RunBoundaryDispatch(AddressSpace& process, JitState& thread_ctx, ExecutionContext& ectx) {
        while (true) {
            if (ectx.halt_reason != 0) {
                return static_cast<HaltReason>(ectx.halt_reason);
            }
            // The host allowance covers the next budget, timer or wall-clock
            // event. Account for all completed guest work before returning.
            const u64 allowance = conf.callbacks->GetTicksRemaining();
            if (ectx.halt_reason != 0) {
                return static_cast<HaltReason>(ectx.halt_reason);
            }
            ectx.cycles_to_run = static_cast<s64>(allowance);
            ectx.cycles_remaining = ectx.cycles_to_run;
            DYNARMIC_WASM_PROFILE_INC(profiler, dispatch_count);
            const BlockFunc entry = process.GetOrEmit(thread_ctx.GetLocationDescriptor());
            if (!entry) {
                return HaltReason::UserDefined8;
            }
            entry(reinterpret_cast<uptr>(&thread_ctx), reinterpret_cast<uptr>(&ectx));
            if (ectx.fast_dispatch_entry != 0) {
                DYNARMIC_WASM_PROFILE_INC(profiler, fast_dispatch_misses);
                const IR::LocationDescriptor location = thread_ctx.GetLocationDescriptor();
                auto* const entry = reinterpret_cast<FastDispatchEntry*>(static_cast<uptr>(ectx.fast_dispatch_entry));
                entry->index = static_cast<u32>(reinterpret_cast<uptr>(process.GetOrEmit(location)));
                entry->descriptor = location.Value();
                ectx.fast_dispatch_entry = 0;
            }
            if (ectx.link_slot_addr != 0) {
                // First execution of a static cross-region edge: compile and
                // link its target before the next native dispatch. Subsequent
                // executions tail-call directly after the generated guard.
                const BlockFunc next = process.GetOrEmitForLink(thread_ctx.GetLocationDescriptor());
                u32* const slot = reinterpret_cast<u32*>(static_cast<uptr>(ectx.link_slot_addr));
                *slot = static_cast<u32>(reinterpret_cast<uptr>(next));
                process.RecordLink(slot, thread_ctx.GetLocationDescriptor());
                process.RecordDispatchTarget(thread_ctx.GetLocationDescriptor(), next);
                ectx.link_slot_addr = 0;
            }
            conf.callbacks->AddTicks(static_cast<u64>(ectx.cycles_to_run - ectx.cycles_remaining));
            ectx.cycles_to_run = ectx.cycles_remaining;
            if (observer.after_ticks != nullptr) {
                observer.after_ticks(observer.user);
            }
        }
    }

    const UserConfig conf;
    ProfileCounters* profiler;
    bool boundary_dispatch = false;
    BoundaryObserver observer;
};

}  // namespace Dynarmic::Backend::Wasm
