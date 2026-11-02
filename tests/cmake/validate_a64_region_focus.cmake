cmake_minimum_required(VERSION 3.16)

if (NOT DEFINED A64_REGION_FOCUS_MODULE)
    message(FATAL_ERROR "A64_REGION_FOCUS_MODULE is required")
endif()
include("${A64_REGION_FOCUS_MODULE}")

function(expect_focus_valid text expected)
    dynarmic_js_validate_a64_region_focus("${text}" actual)
    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR "focus validity mismatch for [${text}]: expected ${expected}, got ${actual}")
    endif()
endfunction()

set(entry "0x000000000000dead")
set(exit_block "0x000000000000deae")
set(direct_target "0x000000000000deaf")
set(valid "${entry},${exit_block},${direct_target}")

expect_focus_valid("" TRUE)
expect_focus_valid("${valid}" TRUE)
expect_focus_valid("${valid},0x0000000000000000" TRUE)
expect_focus_valid("${valid},0x0000000000000000,0x0000000000000001,0xffffffffffffffff" TRUE)
expect_focus_valid("0x0000000000000000,0xffffffffffffffff,0x8000000000000000" TRUE)
# Roles may intentionally alias: the selector supports direct self-loops and
# zero-length required paths. Only consecutive return blocks must increase.
expect_focus_valid("${entry},${entry},${entry}" TRUE)
expect_focus_valid("${entry},${entry},${entry},${entry}" TRUE)

set(malformed
    " "
    "\t"
    "\n"
    "${entry}"
    "${entry},${exit_block}"
    ",${valid}"
    "${entry},,${exit_block},${direct_target}"
    "${valid},"
    "${entry},${exit_block},,${direct_target}"
    " ${valid}"
    "${valid} "
    "${entry} ,${exit_block},${direct_target}"
    "${entry}, ${exit_block},${direct_target}"
    "${valid}\n"
    "+0x00000000000dead,${exit_block},${direct_target}"
    "-0x00000000000dead,${exit_block},${direct_target}"
    "0X000000000000dead,${exit_block},${direct_target}"
    "0x000000000000DEAD,${exit_block},${direct_target}"
    "0x000000000000dea,${exit_block},${direct_target}"
    "0x000000000000deadd,${exit_block},${direct_target}"
    "0000000000000dead,${exit_block},${direct_target}"
    "0x000000000000deag,${exit_block},${direct_target}"
    "${valid}junk"
    "${valid},0x0000000000000000,0x0000000000000000"
    "${valid},0x0000000000000001,0x0000000000000000"
    "${valid},0xffffffffffffffff,0x0000000000000000"
)
foreach(text IN LISTS malformed)
    expect_focus_valid("${text}" FALSE)
endforeach()
expect_focus_valid("${entry};${exit_block};${direct_target}" FALSE)
expect_focus_valid("${valid};" FALSE)

message(STATUS "A64 region focus CMake grammar tests passed")
