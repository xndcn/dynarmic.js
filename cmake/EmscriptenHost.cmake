# Helpers for Emscripten executables that host the wasm JIT.
#
# Generated block modules import the host-callback thunks (thunks.cpp,
# thunks_a64.cpp) from the main module's exports, so every host must export them.

set(DYNARMIC_JS_THUNK_EXPORTS)
foreach(_thunks_file thunks.cpp thunks_a64.cpp)
    set(_thunks_path "${CMAKE_CURRENT_LIST_DIR}/../src/dynarmic/backend/wasm/${_thunks_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_thunks_path}")
    file(READ "${_thunks_path}" _thunks_src)
    string(REGEX MATCHALL "dynarmic_wasm_[a-z0-9_]+\\(" _names "${_thunks_src}")
    list(TRANSFORM _names REPLACE "\\($" "")
    list(APPEND DYNARMIC_JS_THUNK_EXPORTS ${_names})
endforeach()
list(REMOVE_DUPLICATES DYNARMIC_JS_THUNK_EXPORTS)
list(SORT DYNARMIC_JS_THUNK_EXPORTS)
list(TRANSFORM DYNARMIC_JS_THUNK_EXPORTS PREPEND "_")
# The helper is also called by parent projects after add_subdirectory().
# Functions are visible there, but this directory's ordinary variables are not.
set_property(GLOBAL PROPERTY DYNARMIC_JS_THUNK_EXPORTS "${DYNARMIC_JS_THUNK_EXPORTS}")
unset(_thunks_file)
unset(_thunks_path)
unset(_thunks_src)
unset(_names)

# dynarmic_js_host(<target>
#                  [EXPORTS <symbol>...]          extra exported C symbols (with leading _)
#                  [RUNTIME_METHODS <name>...]    extra EXPORTED_RUNTIME_METHODS
#                  [LINK_OPTIONS <opt>...])
function(dynarmic_js_host target)
    if (NOT EMSCRIPTEN)
        message(FATAL_ERROR "dynarmic_js_host requires an Emscripten build")
    endif()
    cmake_parse_arguments(ARG "" "" "EXPORTS;RUNTIME_METHODS;LINK_OPTIONS" ${ARGN})
    if (ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown dynarmic_js_host arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    get_property(thunk_exports GLOBAL PROPERTY DYNARMIC_JS_THUNK_EXPORTS)
    set(exports ${ARG_EXPORTS} ${thunk_exports})
    list(REMOVE_DUPLICATES exports)
    list(JOIN exports "," exports)
    set(methods addFunction removeFunction ${ARG_RUNTIME_METHODS})
    list(JOIN methods "," methods)
    target_link_libraries(${target} PRIVATE dynarmic merry::mcl)
    target_link_options(${target} PRIVATE
        -sALLOW_TABLE_GROWTH
        "-sEXPORTED_FUNCTIONS=${exports}"
        "-sEXPORTED_RUNTIME_METHODS=${methods}"
        ${ARG_LINK_OPTIONS}
    )
endfunction()
