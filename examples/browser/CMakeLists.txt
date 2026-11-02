# Browser example: everything the page needs lands in <build>/examples/browser/dist.
set(DIST "${CMAKE_CURRENT_BINARY_DIR}/dist")
option(DYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS
    "Require every guest toolchain needed for the complete browser example" OFF)
set(DYNARMIC_QUICKJS_ARM_OPT "O2" CACHE STRING "QuickJS ARM optimization level: O2, O3 or Os")
set(DYNARMIC_QUICKJS_ARM_DISPATCH "switch" CACHE STRING "QuickJS ARM interpreter dispatch: switch or threaded")
set(DYNARMIC_QUICKJS_ARM_ISA "arm" CACHE STRING "QuickJS guest C instruction set: arm or thumb")
set_property(CACHE DYNARMIC_QUICKJS_ARM_OPT PROPERTY STRINGS O2 O3 Os)
set_property(CACHE DYNARMIC_QUICKJS_ARM_DISPATCH PROPERTY STRINGS switch threaded)
set_property(CACHE DYNARMIC_QUICKJS_ARM_ISA PROPERTY STRINGS arm thumb)
if(NOT DYNARMIC_QUICKJS_ARM_OPT MATCHES "^(O2|O3|Os)$")
    message(FATAL_ERROR "DYNARMIC_QUICKJS_ARM_OPT must be O2, O3 or Os")
endif()
if(NOT DYNARMIC_QUICKJS_ARM_DISPATCH MATCHES "^(switch|threaded)$")
    message(FATAL_ERROR "DYNARMIC_QUICKJS_ARM_DISPATCH must be switch or threaded")
endif()
if(NOT DYNARMIC_QUICKJS_ARM_ISA MATCHES "^(arm|thumb)$")
    message(FATAL_ERROR "DYNARMIC_QUICKJS_ARM_ISA must be arm or thumb")
endif()
option(DYNARMIC_QUICKJS_ARM_LTO "Enable link-time optimization for the QuickJS ARM guest" OFF)
option(DYNARMIC_QUICKJS_ARM_NO_GCSE "Disable GCSE for the QuickJS ARM guest" OFF)
option(DYNARMIC_QUICKJS_ARM_NO_CROSSJUMPING "Disable cross-jumping for the QuickJS ARM guest" OFF)

foreach(host arm_jit arm_bench_jit)
add_executable(${host} arm_web.cpp)
set_target_properties(${host} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DIST}")
if(host STREQUAL "arm_bench_jit")
    target_compile_definitions(${host} PRIVATE DYNARMIC_JS_EXAMPLE_RAM_SIZE=268435456)
    set(host_ram_size 268435456)
    set(host_initial_memory 402653184)
    set(host_manifest "arm-bench-build-manifest.json")
else()
    set(host_ram_size 16777216)
    set(host_initial_memory 67108864)
    set(host_manifest "wasm-build-manifest.json")
endif()
dynarmic_js_host(${host}
    EXPORTS _arm_init _arm_ram _arm_ram_size _arm_run _arm_reg
            _arm_console _arm_console_len _arm_console_clear _arm_marks _arm_mark_count _malloc
    RUNTIME_METHODS HEAPU8
    LINK_OPTIONS -sMODULARIZE=1 -sEXPORT_NAME=createArmJit -sENVIRONMENT=web,worker,node
                 "-sINITIAL_MEMORY=${host_initial_memory}"
)

# Bind the measured JavaScript/Wasm files to the source and configuration that
# produced them. The collector freezes this file but cannot manufacture it.
string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UPPER)
get_filename_component(EMSCRIPTEN_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
file(READ "${EMSCRIPTEN_COMPILER_DIR}/emscripten-version.txt" EMSCRIPTEN_VERSION)
string(STRIP "${EMSCRIPTEN_VERSION}" EMSCRIPTEN_VERSION)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" BROWSER_CMAKE_SHA256)
set(BUILD_MANIFEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/write_build_manifest.cmake")
add_custom_command(TARGET ${host} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DCONFIG_FLAGS=${CMAKE_CXX_FLAGS_${BUILD_TYPE_UPPER}}"
        "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
        "-DCONFIG_LINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS_${BUILD_TYPE_UPPER}}"
        "-DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
        "-DCOMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
        "-DEMSCRIPTEN_VERSION=${EMSCRIPTEN_VERSION}"
        "-DPROFILE=${DYNARMIC_JS_PROFILE}"
        "-DREGION_DIAGNOSTICS=${DYNARMIC_JS_REGION_DIAGNOSTICS}"
        "-DA64_REGION_FOCUS=${DYNARMIC_JS_A64_REGION_FOCUS}"
        "-DFOCUS_TEST_EXCEPTIONS=${DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS}"
        "-DEXAMPLES=${DYNARMIC_JS_EXAMPLES}"
        "-DGUEST_RAM_BYTES=${host_ram_size}"
        "-DINITIAL_MEMORY_BYTES=${host_initial_memory}"
        "-DCONFIGURE_EMCC_CFLAGS=$ENV{EMCC_CFLAGS}"
        "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DBROWSER_CMAKE_SHA256=${BROWSER_CMAKE_SHA256}"
        "-DJS_FILE=$<TARGET_FILE:${host}>"
        "-DWASM_FILE=${DIST}/${host}.wasm"
        "-DOUTPUT_FILE=${DIST}/${host_manifest}"
        -P "${BUILD_MANIFEST_SCRIPT}"
    BYPRODUCTS "${DIST}/${host_manifest}"
    COMMENT "Writing ${host} build manifest"
    VERBATIM
)
endforeach()

set(page_files)
foreach(f
    styles.css app.js site.js worker.js quickjs-app.js quickjs-worker.js
    official-benchmarks-app.js official-benchmarks-worker.js
    c-bench-app.js c-bench-worker.js c-bench-protocol.js
    site.webmanifest robots.txt sitemap.xml 404.html
    icon.svg icon-192.png icon-512.png social-card.png
    vendor/prism-1.30.0/prism-core.js vendor/prism-1.30.0/prism-clike.js
    vendor/prism-1.30.0/prism-c.js vendor/prism-1.30.0/prism-javascript.js
    vendor/prism-1.30.0/LICENSE)
    get_filename_component(asset_directory "${DIST}/${f}" DIRECTORY)
    add_custom_command(
        OUTPUT "${DIST}/${f}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${asset_directory}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/${f}" "${DIST}/${f}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${f}"
        VERBATIM
    )
    list(APPEND page_files "${DIST}/${f}")
endforeach()

# Demo guest (flat binary linked at 0x10000) and its expected console output,
# computed by running the same source natively.
find_program(ARM_GCC arm-none-eabi-gcc)
find_program(ARM_OBJCOPY arm-none-eabi-objcopy)
find_program(HOST_CC NAMES cc gcc clang)
set(GUEST "${CMAKE_CURRENT_SOURCE_DIR}/guest")
set(missing_demo_tools)
if (ARM_GCC AND ARM_OBJCOPY)
    add_custom_command(
        OUTPUT "${DIST}/demo.bin"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DIST}"
        COMMAND "${ARM_GCC}" -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp -marm -O2 -nostdlib
                -T "${GUEST}/link.ld" "${GUEST}/start.S" "${GUEST}/demo.c" -o demo.elf
        COMMAND "${ARM_OBJCOPY}" -O binary demo.elf "${DIST}/demo.bin"
        DEPENDS "${GUEST}/demo.c" "${GUEST}/start.S" "${GUEST}/link.ld"
        VERBATIM
    )
    list(APPEND page_files "${DIST}/demo.bin")
else()
    list(APPEND missing_demo_tools "arm-none-eabi-gcc and arm-none-eabi-objcopy")
endif()
if (HOST_CC)
    add_custom_command(
        OUTPUT "${DIST}/expected.txt"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DIST}"
        COMMAND "${HOST_CC}" -O2 -DHOST "${GUEST}/demo.c" -o demo_host
        COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=${CMAKE_CURRENT_BINARY_DIR}/demo_host"
            "-DOUTPUT=${DIST}/expected.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/write_expected.cmake"
        DEPENDS "${GUEST}/demo.c" "${CMAKE_CURRENT_SOURCE_DIR}/write_expected.cmake"
        VERBATIM
    )
    list(APPEND page_files "${DIST}/expected.txt")
else()
    list(APPEND missing_demo_tools "a native C compiler (cc, gcc or clang)")
endif()

# The same demo for AArch64, built with zig cc (pip install ziglang).
find_program(DEMO_PYTHON NAMES python3)
if (DEMO_PYTHON)
    execute_process(COMMAND "${DEMO_PYTHON}" -c "import ziglang" RESULT_VARIABLE _zig_missing OUTPUT_QUIET ERROR_QUIET)
endif()
if (DEMO_PYTHON AND NOT _zig_missing)
    add_custom_command(
        OUTPUT "${DIST}/demo_a64.bin"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DIST}"
        COMMAND "${DEMO_PYTHON}" -m ziglang cc -target aarch64-freestanding -O2 -ffreestanding -nostdlib
                -T "${GUEST}/link_a64.ld" "${GUEST}/start_a64.S" "${GUEST}/demo.c" -o demo_a64.elf
        COMMAND "${DEMO_PYTHON}" -m ziglang objcopy -O binary demo_a64.elf "${DIST}/demo_a64.bin"
        DEPENDS "${GUEST}/demo.c" "${GUEST}/start_a64.S" "${GUEST}/link_a64.ld"
        VERBATIM
    )
    list(APPEND page_files "${DIST}/demo_a64.bin")
else()
    list(APPEND missing_demo_tools "Python 3 with the ziglang module")
endif()

# One compiler builds the same freestanding C kernels for ARM32, x86 and Wasm.
# The pinned v86 runtime and its license travel with the generated page.
set(C_BENCH_EXAMPLE "${PROJECT_SOURCE_DIR}/examples/c-bench")
if (DEMO_PYTHON AND NOT _zig_missing)
    set(c_bench_files)
    foreach(f c-bench-arm.bin c-bench-x86.elf c-bench.wasm c-bench-manifest.json
              c-bench-reference.json c-bench-source.c c-bench-v86.js
              c-bench-v86.wasm c-bench-v86-LICENSE.txt)
        list(APPEND c_bench_files "${DIST}/${f}")
    endforeach()
    file(GLOB_RECURSE c_bench_sources CONFIGURE_DEPENDS "${C_BENCH_EXAMPLE}/*.py"
        "${C_BENCH_EXAMPLE}/*.c" "${C_BENCH_EXAMPLE}/*.h"
        "${C_BENCH_EXAMPLE}/*.S" "${C_BENCH_EXAMPLE}/*.ld"
        "${C_BENCH_EXAMPLE}/licenses/*.txt" "${C_BENCH_EXAMPLE}/licenses/*.md")
    add_custom_command(
        OUTPUT ${c_bench_files}
        COMMAND "${DEMO_PYTHON}" "${C_BENCH_EXAMPLE}/build.py"
            --dist "${DIST}" --cache "${CMAKE_CURRENT_BINARY_DIR}/c-bench-cache"
        DEPENDS ${c_bench_sources}
        COMMENT "Building matching freestanding C benchmarks for ARM32, x86 and Wasm"
        VERBATIM
    )
    add_custom_target(c_bench_example DEPENDS ${c_bench_files})
    list(APPEND page_files ${c_bench_files})
endif()

# QuickJS is built from one checksum-pinned upstream source release for both
# ARM32 guest execution and direct WebAssembly. Its complete upstream source
# archive and license accompany the runtime files in the Pages distribution.
set(QUICKJS_EXAMPLE "${PROJECT_SOURCE_DIR}/examples/quickjs")
if (DEMO_PYTHON AND ARM_GCC AND ARM_OBJCOPY)
    set(quickjs_arm_options --arm-opt "${DYNARMIC_QUICKJS_ARM_OPT}"
        --arm-dispatch "${DYNARMIC_QUICKJS_ARM_DISPATCH}" --arm-isa "${DYNARMIC_QUICKJS_ARM_ISA}")
    if(DYNARMIC_QUICKJS_ARM_LTO)
        list(APPEND quickjs_arm_options --arm-lto)
    endif()
    if(DYNARMIC_QUICKJS_ARM_NO_GCSE)
        list(APPEND quickjs_arm_options --arm-no-gcse)
    endif()
    if(DYNARMIC_QUICKJS_ARM_NO_CROSSJUMPING)
        list(APPEND quickjs_arm_options --arm-no-crossjumping)
    endif()
    # Makefile generators also need a changed dependency when cache options
    # change; a modified recipe alone does not rebuild existing outputs.
    set(quickjs_options_file "${CMAKE_CURRENT_BINARY_DIR}/quickjs-build-options.txt")
    file(GENERATE OUTPUT "${quickjs_options_file}" CONTENT "${quickjs_arm_options}\n")
    set(quickjs_files)
    foreach(f quickjs-arm.bin quickjs-wasm.js quickjs-wasm.wasm
              quickjs-workloads.js quickjs-manifest.json quickjs-LICENSE.txt quickjs-source.tar.xz
              quickjs-runtime-licenses.txt official-v8-upstream.js official-v8.js
              official-v8-adapter.js official-benchmarks.json quickjs-extras-source.tar.xz
              quickjs-extras-LICENSE.txt)
        list(APPEND quickjs_files "${DIST}/${f}")
    endforeach()
    file(GLOB_RECURSE quickjs_sources CONFIGURE_DEPENDS "${QUICKJS_EXAMPLE}/*.py"
        "${QUICKJS_EXAMPLE}/*.c" "${QUICKJS_EXAMPLE}/*.h" "${QUICKJS_EXAMPLE}/*.S"
        "${QUICKJS_EXAMPLE}/*.ld" "${QUICKJS_EXAMPLE}/*.js"
        "${QUICKJS_EXAMPLE}/licenses/*.txt" "${QUICKJS_EXAMPLE}/licenses/*.md")
    add_custom_command(
        OUTPUT ${quickjs_files}
        COMMAND "${DEMO_PYTHON}" "${QUICKJS_EXAMPLE}/build.py"
            --dist "${DIST}" --cache "${CMAKE_CURRENT_BINARY_DIR}/quickjs-cache"
            --emcc "${EMSCRIPTEN_COMPILER_DIR}/emcc"
            --arm-cc "${ARM_GCC}" --arm-objcopy "${ARM_OBJCOPY}"
            ${quickjs_arm_options}
        DEPENDS ${quickjs_sources} "${quickjs_options_file}"
        COMMENT "Building checksum-pinned QuickJS for ARM32 and WebAssembly"
        VERBATIM
    )
    add_custom_target(quickjs_example DEPENDS ${quickjs_files})
    list(APPEND page_files ${quickjs_files})
elseif (NOT DEMO_PYTHON)
    list(APPEND missing_demo_tools "Python 3 for the QuickJS build")
endif()

if (missing_demo_tools)
    string(JOIN ", " missing_demo_tools_text ${missing_demo_tools})
    set(missing_demo_message
        "The browser example is incomplete. Missing: ${missing_demo_tools_text}. Install these tools and rerun CMake; see examples/browser/README.md. The arm_jit target and Node hello example do not require guest compilers.")
    if (DYNARMIC_JS_EXAMPLES_REQUIRE_GUESTS)
        message(FATAL_ERROR "${missing_demo_message}")
    else()
        message(WARNING "${missing_demo_message}")
    endif()
endif()

add_custom_command(
    OUTPUT "${DIST}/index.html"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/index.html" "-DDIST=${DIST}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/render_page.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/render_page.cmake" ${page_files}
    COMMENT "Versioning page scripts and styles to avoid stale browser caches"
    VERBATIM
)
add_custom_target(browser_page ALL DEPENDS ${page_files} "${DIST}/index.html")
add_dependencies(browser_page arm_jit arm_bench_jit)
if (TARGET c_bench_example)
    add_dependencies(browser_page c_bench_example)
endif()
