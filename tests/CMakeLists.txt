if (EMSCRIPTEN)
    # JIT hosts, run under Node: node <host>.js [args]
    foreach(host jit_runbin a64_runbin a64_fuzz a32_fuzz smc_test instance_test thumb_it_return_test region_diagnostics_test a64_region_alias_test a64_region_focus_test lifecycle_test runtime_table_test jit_bench jit_thumb_bench jit_a64_bench jit_a64_simd_bench jit_callbench jit_cachebench jit_cachepopbench jit_demo)
        add_executable(${host} hosts/${host}.cpp)
        dynarmic_js_host(${host}
            EXPORTS _main
            LINK_OPTIONS -sNODERAWFS=1 -sINITIAL_MEMORY=67108864
        )
    endforeach()
    add_executable(a32_region_alias_test hosts/a32_region_alias_test.cpp)
    dynarmic_js_host(a32_region_alias_test EXPORTS _main
        LINK_OPTIONS -sNODERAWFS=1 -sINITIAL_MEMORY=67108864)
    target_link_libraries(a32_region_alias_test PRIVATE Boost::boost)
    add_executable(a64_region_sharing_test hosts/a64_region_sharing_test.cpp)
    dynarmic_js_host(a64_region_sharing_test EXPORTS _main
        LINK_OPTIONS -sNODERAWFS=1 -sINITIAL_MEMORY=67108864)
    target_link_libraries(a64_region_sharing_test PRIVATE Boost::boost)
    target_link_libraries(a64_region_alias_test PRIVATE Boost::boost)
    target_link_libraries(a64_region_focus_test PRIVATE Boost::boost)
    set(DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS OFF CACHE BOOL
        "Enable exception catching only for the stale A64 region-focus test build")
    # Keep benchmark and ordinary host builds free of test-only exception overhead.
    if (DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS)
        target_compile_options(dynarmic PRIVATE -fexceptions)
        target_compile_options(a64_region_focus_test PRIVATE -fexceptions)
        target_link_options(a64_region_focus_test PRIVATE -sNO_DISABLE_EXCEPTION_CATCHING)
    endif()
    if (DYNARMIC_JS_A64_REGION_FOCUS)
        target_compile_definitions(a64_region_focus_test PRIVATE
            "DYNARMIC_JS_A64_REGION_FOCUS=\"${DYNARMIC_JS_A64_REGION_FOCUS}\"")
    endif()
    target_link_libraries(region_diagnostics_test PRIVATE Boost::boost)
    add_executable(stats_test hosts/stats_test.cpp)
    dynarmic_js_host(stats_test EXPORTS _main
        LINK_OPTIONS -sNODERAWFS=1 -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=1)
    find_program(DYNARMIC_JS_NODE node)
    if (DYNARMIC_JS_NODE)
        foreach(host jit_demo instance_test thumb_it_return_test region_diagnostics_test
                a32_region_alias_test a64_region_alias_test a64_region_sharing_test a64_region_focus_test lifecycle_test
                runtime_table_test stats_test)
            add_test(NAME "runtime_${host}"
                COMMAND "${DYNARMIC_JS_NODE}" "$<TARGET_FILE:${host}>")
            set_tests_properties("runtime_${host}" PROPERTIES
                LABELS runtime TIMEOUT 120)
        endforeach()
        add_test(NAME runtime_a32_emission_options
            COMMAND "${DYNARMIC_JS_NODE}" "$<TARGET_FILE:a32_region_alias_test>" emission)
        set_tests_properties(runtime_a32_emission_options PROPERTIES LABELS runtime TIMEOUT 120)
    endif()
else()
    add_executable(arm_decoder_test unit/arm_decoder_test.cpp)
    target_link_libraries(arm_decoder_test PRIVATE dynarmic merry::mcl)
    add_test(NAME arm_decoder COMMAND arm_decoder_test)
    add_executable(address_space_test unit/address_space_test.cpp)
    target_link_libraries(address_space_test PRIVATE dynarmic merry::mcl)
    add_test(NAME emission_options COMMAND address_space_test emission_options)
    add_test(NAME address_space COMMAND address_space_test)
    add_executable(region_plan_test unit/region_plan_test.cpp
        ../src/dynarmic/backend/wasm/region_plan.cpp)
    target_include_directories(region_plan_test PRIVATE ../src
        ../externals/dynarmic/externals/mcl/include
        ../externals/dynarmic/src)
    target_link_libraries(region_plan_test PRIVATE merry::mcl)
    target_compile_definitions(region_plan_test PRIVATE DYNARMIC_NO_PANIC=1)
    add_test(NAME region_plan COMMAND region_plan_test)
    add_executable(region_selector_test unit/region_selector_test.cpp)
    target_link_libraries(region_selector_test PRIVATE dynarmic merry::mcl)
    add_test(NAME region_selector COMMAND region_selector_test)
    add_executable(region_selector_focus_test unit/region_selector_focus_test.cpp)
    target_link_libraries(region_selector_focus_test PRIVATE dynarmic merry::mcl)
    add_test(NAME region_selector_focus COMMAND region_selector_focus_test)
    add_executable(a64_region_focus_test unit/a64_region_focus_test.cpp)
    target_link_libraries(a64_region_focus_test PRIVATE dynarmic merry::mcl)
    if (DYNARMIC_JS_A64_REGION_FOCUS)
        target_compile_definitions(a64_region_focus_test PRIVATE
            "DYNARMIC_JS_A64_REGION_FOCUS=\"${DYNARMIC_JS_A64_REGION_FOCUS}\"")
    endif()
    add_test(NAME a64_region_focus COMMAND a64_region_focus_test)
    add_test(NAME a64_region_focus_cmake_grammar
        COMMAND "${CMAKE_COMMAND}"
            -DA64_REGION_FOCUS_MODULE=${CMAKE_SOURCE_DIR}/cmake/A64RegionFocus.cmake
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_a64_region_focus.cmake)
    add_executable(region_diagnostics_test unit/region_diagnostics_test.cpp)
    target_link_libraries(region_diagnostics_test PRIVATE dynarmic merry::mcl)
    add_test(NAME region_diagnostics COMMAND region_diagnostics_test)

    # Exercise the installed package from the same CTest invocation as the
    # native unit suite.  The standalone script is also used by CI, but
    # keeping this as a registered test prevents a local release check from
    # silently omitting the exported target and bundled license files.
    add_test(NAME install_consumer
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/install/smoke.sh"
            "${CMAKE_BINARY_DIR}")
    set_tests_properties(install_consumer PROPERTIES
        LABELS packaging
        TIMEOUT 120
        # Static archives retain sanitizer references. The separate consumer
        # must use the same compiler/link flags to verify instrumented builds.
        ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER};CXXFLAGS=${CMAKE_CXX_FLAGS};LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}")

    find_program(DYNARMIC_JS_NODE node)
    if (DYNARMIC_JS_NODE)
        add_test(NAME a64_region_focus_cmake_integration COMMAND "${DYNARMIC_JS_NODE}" --test
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/a64_region_focus.test.mjs")
        add_test(NAME browser_manifest COMMAND "${DYNARMIC_JS_NODE}" --test
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/browser_manifest.test.mjs")
        add_test(NAME regression_artifacts COMMAND "${DYNARMIC_JS_NODE}" --test
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/regression_artifacts.test.mjs")
        set_tests_properties(regression_artifacts PROPERTIES LABELS infrastructure TIMEOUT 120)
        add_test(NAME fuzz_campaign_contract COMMAND "${DYNARMIC_JS_NODE}" --test
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fuzz_campaign.test.mjs")
        add_test(NAME browser_host_contract COMMAND "${DYNARMIC_JS_NODE}" --test
            "${CMAKE_CURRENT_SOURCE_DIR}/../examples/browser/host.test.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/../examples/browser/worker.test.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/../examples/browser/verify_browser.test.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/../examples/browser/verify_chrome.test.mjs")
        # Match the public CLI/CI suite so newly added collector tests cannot
        # silently disappear from local CTest and sanitizer validation.
        file(GLOB benchmark_protocol_tests CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.test.mjs")
        add_test(NAME benchmark_protocol COMMAND "${DYNARMIC_JS_NODE}" --test
            ${benchmark_protocol_tests})
    endif()
    # IR-level tests: emit modules natively (the emitter alone, or hand-built
    # IR blocks) for Node to instantiate and check.
    foreach(tool smoke_ir sat64_ir wasm_binary_test integer_edge_test vector_edge_test fp_edge_test memory_edge_test a32_memory_edge_test)
        add_executable(${tool} ir/${tool}.cpp)
        target_link_libraries(${tool} PRIVATE dynarmic merry::mcl fmt::fmt Boost::boost)
    endforeach()
    if (DYNARMIC_JS_NODE)
        # These suites generate more than one Wasm module; keep each test in
        # its own output directory so parallel CTest runs cannot share files.
        foreach(suite smoke sat64 wasm_binary_test)
            if (suite STREQUAL "wasm_binary_test")
                set(emitter wasm_binary_test)
                set(output_name "")
            else()
                set(emitter "${suite}_ir")
                set(output_name "${suite}.wasm")
            endif()
            add_test(NAME "${suite}_wasm"
                COMMAND "${CMAKE_COMMAND}"
                    "-DEMITTER=$<TARGET_FILE:${emitter}>"
                    "-DNODE=${DYNARMIC_JS_NODE}"
                    "-DVALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/ir/${suite}.mjs"
                    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/ir-test-output/${suite}"
                    "-DOUTPUT_NAME=${output_name}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/ir/run_emitted.cmake")
            set_tests_properties("${suite}_wasm" PROPERTIES LABELS "ir;wasm" TIMEOUT 120)
        endforeach()
        foreach(tool integer_edge_test vector_edge_test fp_edge_test memory_edge_test a32_memory_edge_test)
            add_test(NAME ${tool} COMMAND "${DYNARMIC_JS_NODE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ir/${tool}.mjs" $<TARGET_FILE:${tool}>)
        endforeach()
    endif()

    add_subdirectory(guest)
    add_subdirectory(guest64)
    add_subdirectory(tools)
endif()
