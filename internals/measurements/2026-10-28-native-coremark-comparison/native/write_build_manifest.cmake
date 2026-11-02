# Invoked by CMake after linking a64_coremark_host. All inputs are passed by
# the configuring build; the executable hash is computed from the linked file.
foreach(variable BUILD_TYPE CXX_FLAGS CONFIG_FLAGS COMPILER_ID COMPILER_VERSION COMPILER_PATH
    HOST_SHA256 CMAKE_SHA256 MANIFEST_SCRIPT_SHA256 REPOSITORY_HEAD DYNARMIC_REVISION TARGET_FILE OUTPUT_FILE)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "missing build manifest input: ${variable}")
  endif()
endforeach()
file(SHA256 "${TARGET_FILE}" ARTIFACT_SHA256)
file(SIZE "${TARGET_FILE}" ARTIFACT_BYTES)

function(json_escape output value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(REPLACE "\n" "\\n" escaped "${escaped}")
  string(REPLACE "\r" "\\r" escaped "${escaped}")
  set(${output} "${escaped}" PARENT_SCOPE)
endfunction()
foreach(variable BUILD_TYPE CXX_FLAGS CONFIG_FLAGS COMPILER_ID COMPILER_VERSION COMPILER_PATH TARGET_FILE)
  json_escape(${variable}_JSON "${${variable}}")
endforeach()

file(WRITE "${OUTPUT_FILE}" [=[
{
  "schema": "dynarmic.js/native-coremark-host-build-v1",
  "buildType": "]=] "${BUILD_TYPE_JSON}" [=[",
  "compiler": {
    "id": "]=] "${COMPILER_ID_JSON}" [=[",
    "version": "]=] "${COMPILER_VERSION_JSON}" [=[",
    "path": "]=] "${COMPILER_PATH_JSON}" [=["
  },
  "flags": {
    "cxx": "]=] "${CXX_FLAGS_JSON}" [=[",
    "configuration": "]=] "${CONFIG_FLAGS_JSON}" [=["
  },
  "sources": {
    "hostSha256": "]=] "${HOST_SHA256}" [=[",
    "cmakeSha256": "]=] "${CMAKE_SHA256}" [=[",
    "manifestScriptSha256": "]=] "${MANIFEST_SCRIPT_SHA256}" [=["
  },
  "revisions": {
    "repositoryHead": "]=] "${REPOSITORY_HEAD}" [=[",
    "dynarmicRevision": "]=] "${DYNARMIC_REVISION}" [=["
  },
  "artifact": {
    "path": "]=] "${TARGET_FILE_JSON}" [=[",
    "sha256": "]=] "${ARTIFACT_SHA256}" [=[",
    "bytes": ]=] "${ARTIFACT_BYTES}" [=[
  }
}
]=])
