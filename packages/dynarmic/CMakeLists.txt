# Runtime shipped by the experimental npm package, separate from the Pages ABI.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(dynarmic_package_runtime host.cpp)
set_target_properties(dynarmic_package_runtime PROPERTIES
    OUTPUT_NAME runtime
    SUFFIX ".mjs"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
dynarmic_js_host(dynarmic_package_runtime
    EXPORTS _dj_create _dj_destroy _dj_ram _dj_run
            _dj_event_kind _dj_event_detail _dj_event_address
            _dj_get_reg _dj_set_reg _dj_get_pc _dj_set_pc _dj_get_sp _dj_set_sp
            _dj_get_status _dj_set_status _dj_invalidate _dj_clear_cache
    RUNTIME_METHODS HEAPU8
    LINK_OPTIONS --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1
                 -sEXPORT_NAME=createDynarmicRuntime
                 -sINCOMING_MODULE_JS_API=wasmBinary
                 -sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1
                 -sINITIAL_MEMORY=67108864 -sMAXIMUM_MEMORY=1073741824
                 -sSTACK_SIZE=1048576 -sFILESYSTEM=0
)
set(package_manifest_script "${CMAKE_CURRENT_SOURCE_DIR}/write_build_manifest.py")
# A changed post-link recorder must force a real relink, including Makefiles.
set_property(TARGET dynarmic_package_runtime APPEND PROPERTY LINK_DEPENDS
    "${package_manifest_script}" "${CMAKE_CURRENT_LIST_FILE}")
add_custom_command(TARGET dynarmic_package_runtime POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${package_manifest_script}"
        --runtime-js "$<TARGET_FILE:dynarmic_package_runtime>"
        --runtime-wasm "${CMAKE_CURRENT_BINARY_DIR}/runtime.wasm"
        --output "${CMAKE_CURRENT_BINARY_DIR}/runtime-build-manifest.json"
        --build-dir "${CMAKE_BINARY_DIR}"
        --compiler "${CMAKE_CXX_COMPILER}"
        --build-type "${CMAKE_BUILD_TYPE}"
        --profile "${DYNARMIC_JS_PROFILE}"
        --region-diagnostics "${DYNARMIC_JS_REGION_DIAGNOSTICS}"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/runtime-build-manifest.json"
    COMMENT "Recording the npm runtime build and source identities"
    VERBATIM
)
