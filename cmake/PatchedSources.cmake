# Out-of-tree source patching for the dynarmic submodule.
#
# The submodule is never modified. Instead, each file touched by a patch is
# copied into the build tree, the patch is applied to the copy, and the
# target's source entry is pointed at the patched copy. Only translation units
# (.c/.cpp) may be patched this way; a patched header would not be picked up
# by the files that include it.

find_program(PATCH_EXECUTABLE patch)
if (NOT PATCH_EXECUTABLE)
    message(FATAL_ERROR "GNU patch is required to apply the build-tree portability patches")
endif()

function(dynarmic_js_patch_target_sources target src_root out_root)
    set(patches ${ARGN})

    set(files)
    foreach(p IN LISTS patches)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${p}")
        file(STRINGS "${p}" lines REGEX "^\\+\\+\\+ b/")
        foreach(line IN LISTS lines)
            string(REGEX REPLACE "^\\+\\+\\+ b/([^\t ]+).*$" "\\1" f "${line}")
            list(APPEND files "${f}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES files)

    file(REMOVE_RECURSE "${out_root}")
    foreach(f IN LISTS files)
        if (NOT f MATCHES "\\.(c|cc|cpp)$")
            message(FATAL_ERROR "Patched file ${f} is not a translation unit")
        endif()
        get_filename_component(dest_dir "${out_root}/${f}" DIRECTORY)
        file(COPY "${src_root}/${f}" DESTINATION "${dest_dir}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src_root}/${f}")
    endforeach()

    foreach(p IN LISTS patches)
        execute_process(
            COMMAND "${PATCH_EXECUTABLE}" -p1 --forward --batch --silent -d "${out_root}" -i "${p}"
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE out
        )
        if (NOT rc EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${p}:\n${out}")
        endif()
    endforeach()

    get_target_property(target_dir ${target} SOURCE_DIR)
    get_target_property(srcs ${target} SOURCES)
    foreach(f IN LISTS files)
        file(RELATIVE_PATH rel "${target_dir}" "${src_root}/${f}")
        list(FIND srcs "${rel}" idx)
        if (idx EQUAL -1)
            list(FIND srcs "${src_root}/${f}" idx)
        endif()
        if (idx EQUAL -1)
            message(FATAL_ERROR "${f} is not a source of ${target}")
        endif()
        list(REMOVE_AT srcs ${idx})
        list(INSERT srcs ${idx} "${out_root}/${f}")
    endforeach()
    set_target_properties(${target} PROPERTIES SOURCES "${srcs}")

    list(LENGTH patches n)
    message(STATUS "dynarmic.js: applied ${n} patch(es) to ${target} out of tree")
endfunction()

# Drops sources matching `regex` from `target`.
function(dynarmic_js_remove_target_sources target regex)
    get_target_property(srcs ${target} SOURCES)
    list(FILTER srcs EXCLUDE REGEX "${regex}")
    set_target_properties(${target} PROPERTIES SOURCES "${srcs}")
endfunction()
