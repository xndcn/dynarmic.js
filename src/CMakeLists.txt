# Sources of the wasm backend, injected into the upstream `dynarmic` target.
target_sources(dynarmic PRIVATE
    dynarmic/frontend/wasm/arm_decoder.h
    dynarmic/backend/wasm/a32_address_space.cpp
    dynarmic/backend/wasm/a32_address_space.h
    dynarmic/backend/wasm/a32_jitstate.cpp
    dynarmic/backend/wasm/a32_jitstate.h
    dynarmic/backend/wasm/a64_address_space.cpp
    dynarmic/backend/wasm/a64_address_space.h
    dynarmic/backend/wasm/a64_jitstate.h
    dynarmic/backend/wasm/a64_region_focus.cpp
    dynarmic/backend/wasm/a64_region_focus.h
    dynarmic/backend/wasm/abi.h
    dynarmic/backend/wasm/address_space.cpp
    dynarmic/backend/wasm/address_space.h
    dynarmic/backend/wasm/compile_stats.h
    dynarmic/backend/wasm/core.h
    dynarmic/backend/wasm/emit_context.h
    dynarmic/backend/wasm/emit_wasm.cpp
    dynarmic/backend/wasm/emit_wasm.h
    dynarmic/backend/wasm/emit_wasm_a32.cpp
    dynarmic/backend/wasm/emit_wasm_a64.cpp
    dynarmic/backend/wasm/emit_wasm_a64_memory.cpp
    dynarmic/backend/wasm/emit_wasm_coproc.cpp
    dynarmic/backend/wasm/emit_wasm_crc32.cpp
    dynarmic/backend/wasm/emit_wasm_crypto.cpp
    dynarmic/backend/wasm/emit_wasm_data_processing.cpp
    dynarmic/backend/wasm/emit_wasm_fp.cpp
    dynarmic/backend/wasm/emit_wasm_fp_a64.cpp
    dynarmic/backend/wasm/emit_wasm_memory.cpp
    dynarmic/backend/wasm/emit_wasm_packed.cpp
    dynarmic/backend/wasm/emit_wasm_saturation.cpp
    dynarmic/backend/wasm/emit_wasm_region.cpp
    dynarmic/backend/wasm/emit_wasm_self_loop.cpp
    dynarmic/backend/wasm/emit_wasm_terminal.cpp
    dynarmic/backend/wasm/emit_wasm_vector.cpp
    dynarmic/backend/wasm/emit_wasm_vector2.cpp
    dynarmic/backend/wasm/emit_wasm_vector3.cpp
    dynarmic/backend/wasm/emit_wasm_vector_a64.cpp
    dynarmic/backend/wasm/emit_wasm_vector_fp.cpp
    dynarmic/backend/wasm/emit_wasm_vector_qc.cpp
    dynarmic/backend/wasm/emit_wasm_vector_util.h
    dynarmic/backend/wasm/exclusive_monitor.cpp
    dynarmic/backend/wasm/profiler.cpp
    dynarmic/backend/wasm/profiler.h
    dynarmic/backend/wasm/region_diagnostics.cpp
    dynarmic/backend/wasm/region_diagnostics.h
    dynarmic/backend/wasm/region_plan.cpp
    dynarmic/backend/wasm/region_plan.h
    dynarmic/backend/wasm/region_selector.cpp
    dynarmic/backend/wasm/region_selector.h
    dynarmic/backend/wasm/reg_alloc.h
    dynarmic/backend/wasm/spin_lock_wasm.cpp
    dynarmic/backend/wasm/thunks.cpp
    dynarmic/backend/wasm/thunks_a64.cpp
    dynarmic/backend/wasm/wasm_binary.cpp
    dynarmic/backend/wasm/wasm_binary.h
    dynarmic/backend/wasm/wasm_runtime.h
    dynarmic/backend/wasm/wasm_runtime_emscripten.cpp
    dynarmic/backend/wasm/wasm_runtime_stub.cpp
)

# The emitter, runtime glue and thunks serve both frontends.
if (NOT ("A32" IN_LIST DYNARMIC_FRONTENDS AND "A64" IN_LIST DYNARMIC_FRONTENDS))
    message(FATAL_ERROR "The wasm backend needs both the A32 and A64 frontends")
endif()
target_sources(dynarmic PRIVATE
    dynarmic/backend/wasm/a32_interface.cpp
    dynarmic/backend/wasm/a64_interface.cpp
)

# Backend headers are addressed as "dynarmic/backend/wasm/...", like upstream backends.
target_include_directories(dynarmic PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

if (DYNARMIC_JS_A64_REGION_FOCUS)
    target_compile_definitions(dynarmic PRIVATE
        "DYNARMIC_JS_A64_REGION_FOCUS=\"${DYNARMIC_JS_A64_REGION_FOCUS}\"")
endif()
