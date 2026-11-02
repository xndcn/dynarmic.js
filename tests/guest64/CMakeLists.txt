# Bare-metal A64 guest programs (flat binaries loaded at 0x10000), built with
# `zig cc` (pip install ziglang). They reuse the portable C workloads of the
# A32 guests, so the same native reference programs check their results.

# The python3 on PATH (where `pip install --user` puts packages), not
# necessarily the newest interpreter FindPython3 would pick.
find_program(GUEST64_PYTHON NAMES python3)
if (GUEST64_PYTHON)
    execute_process(COMMAND "${GUEST64_PYTHON}" -c "import ziglang"
                    RESULT_VARIABLE _zig_missing OUTPUT_QUIET ERROR_QUIET)
endif()
if (NOT GUEST64_PYTHON OR _zig_missing)
    message(WARNING "python3 -m ziglang not found (pip install ziglang); A64 guest programs will not be built")
    return()
endif()

set(ZIG_CC "${GUEST64_PYTHON}" -m ziglang cc -target aarch64-freestanding)
set(ZIG_OBJCOPY "${GUEST64_PYTHON}" -m ziglang objcopy)
set(GUEST64_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(GUEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../guest")

# add_guest64(<name> FLAGS <cc flags>... SOURCES <files in tests/guest>...)
function(add_guest64 name)
    cmake_parse_arguments(G "" "" "FLAGS;SOURCES" ${ARGN})
    list(TRANSFORM G_SOURCES PREPEND "${GUEST_DIR}/")
    add_custom_command(
        OUTPUT ${name}.bin
        COMMAND ${ZIG_CC} ${G_FLAGS} -ffreestanding -nostdlib -I "${GUEST64_DIR}/include"
                -T "${GUEST64_DIR}/link.ld" "${GUEST64_DIR}/start.S" ${G_SOURCES} -o ${name}.elf
        COMMAND ${ZIG_OBJCOPY} -O binary ${name}.elf ${name}.bin
        DEPENDS ${G_SOURCES} "${GUEST64_DIR}/start.S" "${GUEST64_DIR}/link.ld" "${GUEST64_DIR}/include/string.h"
        VERBATIM
    )
    set(GUEST64_BINS ${GUEST64_BINS} ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin PARENT_SCOPE)
endfunction()

set(GUEST64_BINS)
# -O3 auto-vectorizes the integer workload (NEON across-lane sums, widening
# multiplies, ...).
add_guest64(test    FLAGS -O2 SOURCES guest.c compute.c)
add_guest64(test_o3 FLAGS -O3 SOURCES guest.c compute.c)
# fp matches the (unfused) native fp_ref only without FP contraction;
# fp_fma lets clang fuse into fmadd/fmla (checked for consistency only).
add_guest64(fp      FLAGS -O2 -ffp-contract=off -DGUEST_BUILD SOURCES guest_fp.c compute_fp.c)
add_guest64(fp_fma  FLAGS -O3 -DGUEST_BUILD SOURCES guest_fp.c compute_fp.c)
add_custom_target(guests64 ALL DEPENDS ${GUEST64_BINS})
