# Record from synthetic guests, then compare with the source-tree baseline.
foreach(variable REGION_CORPUS_BUILD_DIR REGION_CORPUS_RECORDER REGION_CORPUS_NODE REGION_CORPUS_SOURCE_DIR REGION_CORPUS_BINARY_DIR)
    if (NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

set(actual "${REGION_CORPUS_BINARY_DIR}/region_corpus_actual")
file(REMOVE_RECURSE "${actual}")
# Source distributions intentionally omit Git metadata. Do not accidentally
# attribute an unpacked source tree to a parent repository's revision.
set(revision "source-archive")
if (EXISTS "${REGION_CORPUS_SOURCE_DIR}/../../.git")
    execute_process(
        COMMAND git -C "${REGION_CORPUS_SOURCE_DIR}/../.." rev-parse HEAD
        RESULT_VARIABLE result
        OUTPUT_VARIABLE revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "reading current Git revision failed: ${result}")
    endif()
endif()
execute_process(
    COMMAND "${REGION_CORPUS_RECORDER}" --output "${actual}" --source-revision "${revision}"
    RESULT_VARIABLE result
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "recording region corpus failed: ${result}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "DYNARMIC_REGION_CORPUS_RECORDER=${REGION_CORPUS_RECORDER}"
        "DYNARMIC_REGION_CORPUS_ACTUAL=${actual}"
        "DYNARMIC_REGION_CORPUS_REVISION=${revision}"
        "${REGION_CORPUS_NODE}" --test "${REGION_CORPUS_SOURCE_DIR}/region_corpus.test.mjs"
    RESULT_VARIABLE result
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "region corpus comparison failed: ${result}")
endif()
