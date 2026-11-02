# Bare-metal ARM guest programs (flat binaries loaded at 0x10000) and the host
# reference programs whose checksums / mix logs they must reproduce.

find_program(ARM_GCC arm-none-eabi-gcc)
find_program(ARM_OBJCOPY arm-none-eabi-objcopy)

set(GUEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB GUEST_INCLUDES CONFIGURE_DEPENDS "${GUEST_DIR}/*.inc" "${GUEST_DIR}/*.def")

set(ARMV7_CA15 -mcpu=cortex-a15 -marm)
set(ARMV7_NEON -march=armv7-a -mfpu=neon -mfloat-abi=softfp -marm)

# add_guest(<name> FLAGS <gcc flags>... SOURCES <files>... [LIBS <libs>...] [NO_LOG])
# Produces <name>.bin, and <name>_log.bin built with -DLOG_MIX unless NO_LOG.
function(add_guest name)
    cmake_parse_arguments(G "NO_LOG" "" "FLAGS;SOURCES;LIBS" ${ARGN})
    list(TRANSFORM G_SOURCES PREPEND "${GUEST_DIR}/")
    set(variants "${name}")
    if (NOT G_NO_LOG)
        list(APPEND variants "${name}_log")
    endif()
    foreach(v IN LISTS variants)
        set(defs)
        if (v MATCHES "_log$")
            set(defs -DLOG_MIX)
        endif()
        add_custom_command(
            OUTPUT ${v}.bin
            COMMAND "${ARM_GCC}" ${G_FLAGS} ${defs} -O2 -nostdlib -T "${GUEST_DIR}/link.ld"
                    "${GUEST_DIR}/start.S" ${G_SOURCES} ${G_LIBS} -o ${v}.elf
            COMMAND "${ARM_OBJCOPY}" -O binary ${v}.elf ${v}.bin
            DEPENDS ${G_SOURCES} "${GUEST_DIR}/start.S" "${GUEST_DIR}/link.ld" ${GUEST_INCLUDES}
            VERBATIM
        )
        list(APPEND GUEST_BINS ${CMAKE_CURRENT_BINARY_DIR}/${v}.bin)
    endforeach()
    set(GUEST_BINS ${GUEST_BINS} PARENT_SCOPE)
endfunction()

if (ARM_GCC AND ARM_OBJCOPY)
    set(GUEST_BINS)
    add_guest(test   NO_LOG FLAGS ${ARMV7_CA15} -ffreestanding SOURCES guest.c compute.c LIBS -lgcc)
    add_guest(fp     NO_LOG FLAGS ${ARMV7_CA15} -mfpu=vfpv3 -mfloat-abi=softfp -ffreestanding -DGUEST_BUILD
                     SOURCES guest_fp.c compute_fp.c LIBS -lgcc)
    # Debug probes: a few bit-twiddling ops with fixed inputs (probe) and one
    # traced iteration of test.bin's test_bits stage (trace); results at 0x80000.
    add_guest(probe  NO_LOG FLAGS ${ARMV7_CA15} -ffreestanding SOURCES probe.c LIBS -lgcc)
    add_guest(trace  NO_LOG FLAGS ${ARMV7_CA15} -ffreestanding SOURCES trace.c LIBS -lgcc)
    add_guest(coproc NO_LOG FLAGS ${ARMV7_CA15} SOURCES coproc.c)
    add_guest(crc    NO_LOG FLAGS -march=armv8-a+crc -marm SOURCES crc.c)
    add_guest(crypto NO_LOG FLAGS -march=armv8-a+crypto -mfpu=crypto-neon-fp-armv8 -mfloat-abi=softfp -marm
                     SOURCES crypto.c)
    add_guest(media  FLAGS ${ARMV7_CA15} SOURCES media.c)
    add_guest(fp16   FLAGS -march=armv7-a -mfpu=fp-armv8 -mfloat-abi=softfp -marm SOURCES fp16.c)
    add_guest(neon   FLAGS ${ARMV7_NEON} SOURCES neon.c)
    add_guest(neon2  FLAGS ${ARMV7_NEON} SOURCES neon2.c)
    add_guest(neon3  FLAGS ${ARMV7_NEON} SOURCES neon3.c)
    add_guest(neon4  FLAGS -march=armv7-a -mfpu=neon-fp16 -mfloat-abi=softfp -marm SOURCES neon4.c)
    add_guest(neon5  FLAGS -march=armv8-a -mfpu=neon-fp-armv8 -mfloat-abi=softfp -marm SOURCES neon5.c)
    add_guest(neon6  FLAGS ${ARMV7_NEON} SOURCES neon6.c)
    add_guest(qc     FLAGS ${ARMV7_NEON} SOURCES qc.c)

    # Self-modifying-code test for smc_test (no C runtime, linked at 0x10000).
    add_custom_command(
        OUTPUT smc.bin
        COMMAND "${ARM_GCC}" -march=armv7-a -marm -nostdlib -Ttext=0x10000 "${GUEST_DIR}/smc.S" -o smc.elf
        COMMAND "${ARM_OBJCOPY}" -O binary smc.elf smc.bin
        DEPENDS "${GUEST_DIR}/smc.S"
        VERBATIM
    )
    list(APPEND GUEST_BINS ${CMAKE_CURRENT_BINARY_DIR}/smc.bin)

    add_custom_target(guests ALL DEPENDS ${GUEST_BINS})
else()
    message(WARNING "arm-none-eabi-gcc not found; ARM guest programs will not be built")
endif()

# Reference programs. Each prints the expected checksums; the *_log variants
# and the C++ ones also write the expected mix log into the working directory.
# Optimization levels match the ones the references were validated with.
add_executable(host_ref host_ref.c compute.c)
add_executable(fp_ref fp_ref.c compute_fp.c)
target_link_libraries(fp_ref PRIVATE m)
add_executable(crc_ref crc_ref.c)
add_executable(probe_ref probe_ref.c)
add_executable(trace_ref trace_ref.c)

foreach(t media neon neon2 neon3)
    add_executable(${t}_ref_log ${t}_ref.c)
    target_compile_definitions(${t}_ref_log PRIVATE LOG_MIX)
    target_compile_options(${t}_ref_log PRIVATE -O2)
endforeach()
target_compile_options(neon_ref_log PRIVATE -O1)

add_executable(neon6_ref neon6_ref.cpp)
add_executable(qc_ref qc_ref.cpp)
target_compile_options(qc_ref PRIVATE -O1 -fpermissive -w)

# These borrow dynarmic's soft-float FP helpers for the expected results.
foreach(t fp16 neon4 neon5)
    add_executable(${t}_ref ${t}_ref.cpp)
    target_link_libraries(${t}_ref PRIVATE dynarmic merry::mcl fmt::fmt)
endforeach()
target_compile_options(fp16_ref PRIVATE -O1)
target_compile_options(neon5_ref PRIVATE -O2 -msse4.1)
target_compile_options(neon4_ref PRIVATE -O2)
foreach(t host_ref fp_ref crc_ref neon6_ref probe_ref trace_ref)
    target_compile_options(${t} PRIVATE -O2)
endforeach()
