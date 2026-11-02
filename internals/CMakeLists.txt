# Historical experiment checks are opt-in and never part of the public
# benchmark protocol test. They use the retained evidence under this directory.
find_program(DYNARMIC_JS_INTERNAL_NODE node)
if (NOT DYNARMIC_JS_INTERNAL_NODE)
    message(FATAL_ERROR "Node.js is required for internal evidence checks")
endif()

file(GLOB_RECURSE DYNARMIC_JS_INTERNAL_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/measurements/*.test.mjs")
# This integration test needs the original experiment's native emitter and
# CoreMark image. Run it explicitly with EMIT_PROBE and EMIT_PROBE_INPUT set.
list(FILTER DYNARMIC_JS_INTERNAL_TESTS EXCLUDE REGEX "/emit_probe\\.test\\.mjs$")

add_test(NAME internal_measurement_tools
    COMMAND "${DYNARMIC_JS_INTERNAL_NODE}" --test ${DYNARMIC_JS_INTERNAL_TESTS})
set_tests_properties(internal_measurement_tools PROPERTIES
    LABELS "internal"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
