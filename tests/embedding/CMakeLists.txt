cmake_minimum_required(VERSION 3.16)
project(dynarmic_js_embedding_smoke LANGUAGES C CXX)

if (NOT DYNARMIC_JS_SOURCE_DIR)
    message(FATAL_ERROR "Set DYNARMIC_JS_SOURCE_DIR to the source checkout")
endif()
set(DYNARMIC_JS_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_JS_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${DYNARMIC_JS_SOURCE_DIR}" dynarmic-js)

add_executable(embedding_smoke main.cpp)
target_compile_features(embedding_smoke PRIVATE cxx_std_20)
dynarmic_js_host(embedding_smoke EXPORTS _main)
