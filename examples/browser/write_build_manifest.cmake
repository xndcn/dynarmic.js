# Invoked by arm_jit POST_BUILD. Source revisions, dirty state and artifact
# identities are observed after linking, not reconstructed by the collector.
# Script mode does not inherit the parent project's policy settings. In
# particular, CMP0012 must recognize ON/OFF as boolean constants on CMake 3.
cmake_minimum_required(VERSION 3.16)

foreach(variable BUILD_TYPE CXX_FLAGS CONFIG_FLAGS LINK_FLAGS CONFIG_LINK_FLAGS
    COMPILER_ID COMPILER_VERSION EMSCRIPTEN_VERSION PROFILE REGION_DIAGNOSTICS
    A64_REGION_FOCUS FOCUS_TEST_EXCEPTIONS EXAMPLES CONFIGURE_EMCC_CFLAGS SOURCE_ROOT
    BROWSER_CMAKE_SHA256 JS_FILE WASM_FILE OUTPUT_FILE)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "missing Wasm build manifest input: ${variable}")
  endif()
endforeach()
find_package(Git QUIET)

function(git_output output)
  execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE value ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "git ${ARGN} failed: ${error}")
  endif()
  set(${output} "${value}" PARENT_SCOPE)
endfunction()
# A release source archive has no Git metadata. Do not discover a parent
# application's repository when dynarmic.js is embedded with add_subdirectory.
# Unknown provenance remains null; performance collectors require a known,
# clean checkout and must not mistake an archive for one.
set(REPOSITORY_HEAD_JSON null)
set(DYNARMIC_REVISION_JSON null)
set(SOURCE_DIRTY null)
if(GIT_FOUND AND EXISTS "${SOURCE_ROOT}/.git")
  git_output(REPOSITORY_HEAD -C "${SOURCE_ROOT}" rev-parse HEAD)
  set(REPOSITORY_HEAD_JSON "\"${REPOSITORY_HEAD}\"")
  git_output(SOURCE_STATUS -C "${SOURCE_ROOT}" status --porcelain --ignore-submodules=none --untracked-files=all)
  if(SOURCE_STATUS STREQUAL "")
    set(SOURCE_DIRTY false)
  else()
    set(SOURCE_DIRTY true)
  endif()
endif()
if(GIT_FOUND AND EXISTS "${SOURCE_ROOT}/externals/dynarmic/.git")
  git_output(DYNARMIC_REVISION -C "${SOURCE_ROOT}/externals/dynarmic" rev-parse HEAD)
  set(DYNARMIC_REVISION_JSON "\"${DYNARMIC_REVISION}\"")
endif()

file(SHA256 "${JS_FILE}" JS_SHA256)
file(SIZE "${JS_FILE}" JS_BYTES)
file(SHA256 "${WASM_FILE}" WASM_SHA256)
file(SIZE "${WASM_FILE}" WASM_BYTES)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" MANIFEST_SCRIPT_SHA256)
set(BUILD_EMCC_CFLAGS "$ENV{EMCC_CFLAGS}")

function(json_escape output value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(REPLACE "\n" "\\n" escaped "${escaped}")
  string(REPLACE "\r" "\\r" escaped "${escaped}")
  string(REPLACE "\t" "\\t" escaped "${escaped}")
  set(${output} "${escaped}" PARENT_SCOPE)
endfunction()
foreach(variable BUILD_TYPE CXX_FLAGS CONFIG_FLAGS LINK_FLAGS CONFIG_LINK_FLAGS
    COMPILER_ID COMPILER_VERSION EMSCRIPTEN_VERSION A64_REGION_FOCUS CONFIGURE_EMCC_CFLAGS BUILD_EMCC_CFLAGS SOURCE_ROOT JS_FILE WASM_FILE)
  json_escape(${variable}_JSON "${${variable}}")
endforeach()
function(json_bool output value)
  if(${value})
    set(${output} true PARENT_SCOPE)
  else()
    set(${output} false PARENT_SCOPE)
  endif()
endfunction()
json_bool(PROFILE_JSON "${PROFILE}")
json_bool(REGION_DIAGNOSTICS_JSON "${REGION_DIAGNOSTICS}")
json_bool(FOCUS_TEST_EXCEPTIONS_JSON "${FOCUS_TEST_EXCEPTIONS}")
json_bool(EXAMPLES_JSON "${EXAMPLES}")
if(NOT DEFINED GUEST_RAM_BYTES)
  set(GUEST_RAM_BYTES null)
endif()
if(NOT DEFINED INITIAL_MEMORY_BYTES)
  set(INITIAL_MEMORY_BYTES null)
endif()

file(WRITE "${OUTPUT_FILE}" [=[
{
  "schema": "dynarmic.js/wasm-coremark-host-build-v1",
  "sourceRoot": "]=] "${SOURCE_ROOT_JSON}" [=[",
  "build": {
    "type": "]=] "${BUILD_TYPE_JSON}" [=[",
    "profile": ]=] "${PROFILE_JSON}" [=[,
    "a64RegionFocus": "]=] "${A64_REGION_FOCUS_JSON}" [=[",
    "compilerId": "]=] "${COMPILER_ID_JSON}" [=[",
    "compilerVersion": "]=] "${COMPILER_VERSION_JSON}" [=[",
    "emscriptenVersion": "]=] "${EMSCRIPTEN_VERSION_JSON}" [=[",
    "cxxFlags": "]=] "${CXX_FLAGS_JSON}" [=[",
    "configCxxFlags": "]=] "${CONFIG_FLAGS_JSON}" [=[",
    "linkFlags": "]=] "${LINK_FLAGS_JSON}" [=[",
    "configLinkFlags": "]=] "${CONFIG_LINK_FLAGS_JSON}" [=["
  },
  "configuration": {
    "regionDiagnostics": ]=] "${REGION_DIAGNOSTICS_JSON}" [=[,
    "focusTestExceptions": ]=] "${FOCUS_TEST_EXCEPTIONS_JSON}" [=[,
    "examples": ]=] "${EXAMPLES_JSON}" [=[,
    "guestRamBytes": ]=] "${GUEST_RAM_BYTES}" [=[,
    "initialMemoryBytes": ]=] "${INITIAL_MEMORY_BYTES}" [=[
  },
  "buildEnvironment": {
    "configureEmccCflags": "]=] "${CONFIGURE_EMCC_CFLAGS_JSON}" [=[",
    "buildEmccCflags": "]=] "${BUILD_EMCC_CFLAGS_JSON}" [=["
  },
  "source": {
    "revision": ]=] "${REPOSITORY_HEAD_JSON}" [=[,
    "upstream": ]=] "${DYNARMIC_REVISION_JSON}" [=[,
    "dirty": ]=] "${SOURCE_DIRTY}" [=[
  },
  "artifacts": {
    "js": {
      "path": "]=] "${JS_FILE_JSON}" [=[",
      "sha256": "]=] "${JS_SHA256}" [=[",
      "bytes": ]=] "${JS_BYTES}" [=[
    },
    "wasm": {
      "path": "]=] "${WASM_FILE_JSON}" [=[",
      "sha256": "]=] "${WASM_SHA256}" [=[",
      "bytes": ]=] "${WASM_BYTES}" [=[
    }
  },
  "buildSources": {
    "browserCmakeSha256": "]=] "${BROWSER_CMAKE_SHA256}" [=[",
    "manifestScriptSha256": "]=] "${MANIFEST_SCRIPT_SHA256}" [=["
  }
}
]=])
