# Native debugging tools (see the header comment of each source).
foreach(tool block_dump a32_decode region_corpus)
    add_executable(${tool} ${tool}.cpp)
    target_link_libraries(${tool} PRIVATE dynarmic merry::mcl fmt::fmt Boost::boost)
endforeach()

if (DYNARMIC_JS_NODE)
    add_test(NAME region_corpus
        COMMAND "${CMAKE_COMMAND}"
            -DREGION_CORPUS_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DREGION_CORPUS_RECORDER=$<TARGET_FILE:region_corpus>
            -DREGION_CORPUS_NODE=${DYNARMIC_JS_NODE}
            -DREGION_CORPUS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DREGION_CORPUS_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_region_corpus.cmake)
endif()
