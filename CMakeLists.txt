cmake_minimum_required(VERSION 3.16)
project(dynarmic_js LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Bundled dynarmic externals declare minimums CMake 4 no longer accepts.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

option(DYNARMIC_JS_TESTS "Build test hosts (Emscripten) or guests and reference programs (native)" ON)
option(DYNARMIC_JS_BUILD_INTERNAL_TESTS "Run archived experiment and evidence checks" OFF)
option(DYNARMIC_JS_EXAMPLES "Build the browser example (Emscripten only)" ON)
option(DYNARMIC_JS_NPM_PACKAGE "Build the standalone JavaScript package runtime (Emscripten only)" OFF)
option(DYNARMIC_JS_PROFILE "Instrument the WASM backend and publish profile-v1 counters" OFF)
option(DYNARMIC_JS_REGION_DIAGNOSTICS "Retain and publish compile-boundary region histories (Emscripten only)" OFF)
set(DYNARMIC_JS_A64_REGION_FOCUS "" CACHE STRING
    "Exact A64 focus: entry,exit_block,direct_target[,return_block...] as 16-digit lowercase hex descriptors")

include(cmake/A64RegionFocus.cmake)
dynarmic_js_validate_a64_region_focus("${DYNARMIC_JS_A64_REGION_FOCUS}" DYNARMIC_JS_A64_REGION_FOCUS_VALID)
if (NOT DYNARMIC_JS_A64_REGION_FOCUS_VALID)
    message(FATAL_ERROR
        "DYNARMIC_JS_A64_REGION_FOCUS is invalid; expected entry,exit_block,direct_target[,return_block...], "
        "with each field exactly '0x' plus 16 lowercase hexadecimal digits and strictly increasing return blocks")
endif()

if (DYNARMIC_JS_REGION_DIAGNOSTICS AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "DYNARMIC_JS_REGION_DIAGNOSTICS requires the Emscripten host")
endif()
if (DYNARMIC_JS_NPM_PACKAGE AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "DYNARMIC_JS_NPM_PACKAGE requires Emscripten; configure with emcmake")
endif()

# The wasm backend is distributed as a static CMake package. Upstream's
# generated dynarmicConfig.cmake uses BUILD_SHARED_LIBS to decide whether it
# must emit dependency lookups; leaving the variable undefined when Dynarmic
# is embedded makes an installed package fail for downstream consumers.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dynarmic.js libraries as static archives" FORCE)

# Profiling is an explicit build choice.  The disabled configuration removes
# all counter updates from the dispatch path instead of paying a runtime branch
# in production builds.
if (DYNARMIC_JS_PROFILE)
    add_compile_definitions(DYNARMIC_WASM_PROFILE=1)
endif()
if (DYNARMIC_JS_REGION_DIAGNOSTICS)
    add_compile_definitions(DYNARMIC_WASM_REGION_DIAGNOSTICS=1)
endif()

set(DYNARMIC_JS_UPSTREAM "${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic")
if (NOT EXISTS "${DYNARMIC_JS_UPSTREAM}/CMakeLists.txt")
    message(FATAL_ERROR "externals/dynarmic is missing; run `git submodule update --init`")
endif()

include(cmake/PatchedSources.cmake)
include(cmake/EmscriptenHost.cmake)

# Upstream dynarmic, configured for the wasm backend (A32 and A64 guests).
set(ARCHITECTURE wasm)
set(DYNARMIC_FRONTENDS A32 A64)
set(DYNARMIC_TESTS OFF CACHE BOOL "")
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "")

if (EMSCRIPTEN)
    # Boost 1.74 headers still use std::unary_function, which emscripten's libc++ drops.
    add_compile_definitions(BOOST_NO_CXX98_FUNCTION_BASE)
    add_compile_options(-Wno-counter-extension)
endif()

# Found here as well so Boost::boost is visible to our own targets; upstream's
# imported target would be scoped to its directory.
find_package(Boost 1.57 REQUIRED)

add_subdirectory(externals/dynarmic)

if (EMSCRIPTEN)
    # fmt's own FMT_STRING calls trip a consteval bug in emscripten's clang.
    target_compile_definitions(fmt PRIVATE FMT_CONSTEVAL=)
endif()

# Upstream picks a host signal handler by OS; the wasm backend has no fastmem
# faults and never touches ExceptionHandler.
dynarmic_js_remove_target_sources(dynarmic "backend/exception_handler_[a-z_]+\\.(c|cpp)$")
# Only the x64/arm64 backends call the LLVM disassembler glue, and it assumes
# 64-bit pointers.
dynarmic_js_remove_target_sources(dynarmic "common/llvm_disassemble\\.cpp$")

file(GLOB DYNARMIC_JS_PATCHES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
list(SORT DYNARMIC_JS_PATCHES)
dynarmic_js_patch_target_sources(dynarmic
    "${DYNARMIC_JS_UPSTREAM}"
    "${CMAKE_CURRENT_BINARY_DIR}/dynarmic-patched"
    ${DYNARMIC_JS_PATCHES}
)

# The wasm backend itself.
add_subdirectory(src)

if (DYNARMIC_JS_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (DYNARMIC_JS_BUILD_INTERNAL_TESTS)
    enable_testing()
    add_subdirectory(internals)
endif()

if (DYNARMIC_JS_EXAMPLES AND EMSCRIPTEN)
    add_subdirectory(examples/browser)
endif()

if (DYNARMIC_JS_NPM_PACKAGE)
    add_subdirectory(packages/dynarmic)
endif()

include(GNUInstallDirs)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dynarmic.js"
)
install(FILES "${DYNARMIC_JS_UPSTREAM}/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dynarmic.js/third-party"
    RENAME dynarmic-LICENSE.txt)
install(FILES "${DYNARMIC_JS_UPSTREAM}/externals/fmt/LICENSE.rst"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dynarmic.js/third-party"
    RENAME fmt-LICENSE.rst)
install(FILES "${DYNARMIC_JS_UPSTREAM}/externals/mcl/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dynarmic.js/third-party"
    RENAME mcl-LICENSE)
install(FILES "${DYNARMIC_JS_UPSTREAM}/externals/robin-map/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dynarmic.js/third-party"
    RENAME robin-map-LICENSE)
