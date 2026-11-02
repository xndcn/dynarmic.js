cmake_minimum_required(VERSION 3.16)
project(dynarmic_native_coremark LANGUAGES C CXX ASM)

# Deliberately independent of the repository's Wasm top-level configuration.
# This target executes A64 through dynarmic's native x86-64 backend.
if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Use a single-config generator so native build provenance is unambiguous")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Optimized native benchmark build" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR "The native benchmark requires an optimized CMAKE_BUILD_TYPE")
endif()
set(ARCHITECTURE x86_64 CACHE STRING "" FORCE)
set(DYNARMIC_FRONTENDS A64 CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
# CMake 4 removed the compatibility level requested by bundled robin-map.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(DYNARMIC_JS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
add_subdirectory("${DYNARMIC_JS_ROOT}/externals/dynarmic" dynarmic)

add_executable(a64_coremark_host a64_coremark_host.cpp)
target_link_libraries(a64_coremark_host PRIVATE dynarmic merry::mcl)

file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/a64_coremark_host.cpp" HOST_SHA256)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" CMAKE_SHA256)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/write_build_manifest.cmake" MANIFEST_SCRIPT_SHA256)
execute_process(COMMAND git -C "${DYNARMIC_JS_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE REPOSITORY_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git -C "${DYNARMIC_JS_ROOT}/externals/dynarmic" rev-parse HEAD
    OUTPUT_VARIABLE DYNARMIC_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UPPER)
set(CONFIG_FLAGS "${CMAKE_CXX_FLAGS_${BUILD_TYPE_UPPER}}")
set(MANIFEST_PATH "${CMAKE_CURRENT_BINARY_DIR}/native-build-manifest.json")
add_custom_command(TARGET a64_coremark_host POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DCONFIG_FLAGS=${CONFIG_FLAGS}"
        "-DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
        "-DCOMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
        "-DCOMPILER_PATH=${CMAKE_CXX_COMPILER}"
        "-DHOST_SHA256=${HOST_SHA256}"
        "-DCMAKE_SHA256=${CMAKE_SHA256}"
        "-DMANIFEST_SCRIPT_SHA256=${MANIFEST_SCRIPT_SHA256}"
        "-DREPOSITORY_HEAD=${REPOSITORY_HEAD}"
        "-DDYNARMIC_REVISION=${DYNARMIC_REVISION}"
        "-DTARGET_FILE=$<TARGET_FILE:a64_coremark_host>"
        "-DOUTPUT_FILE=${MANIFEST_PATH}"
        -P "${CMAKE_CURRENT_LIST_DIR}/write_build_manifest.cmake"
    VERBATIM)
