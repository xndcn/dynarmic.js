cmake_minimum_required(VERSION 3.16)
project(dynarmic_js_consumer LANGUAGES CXX)

find_package(dynarmic CONFIG REQUIRED)
add_executable(dynarmic_js_consumer main.cpp)
target_link_libraries(dynarmic_js_consumer PRIVATE dynarmic::dynarmic)
