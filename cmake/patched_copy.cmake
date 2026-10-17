# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   snx_patched_copy(SOURCE <dir> DEST <dir> PATCHES <file>... FILES <name> <sha256>...)
#
# DEST: a copy of SOURCE with PATCHES applied by patch -p1, as CPM's PATCHES applies them. A package with patches is kept
# in CPM_SOURCE_CACHE as released, so a cache filled by hand (a Flatpak manifest's sources, a distribution's offline
# build) works too. patch(1) applies a hunk whose context moved or changed without an error, so each file the patches
# touch is in FILES with the SHA-256 it has patched; one that has another makes the copy again (a patch edited since, a
# copy left half patched) and, after that, stops the configure.
#
# The one way a package is patched here: CPM's PATCHES leaves a cache it did not fill unpatched (#214). Only hactool
# uses it, and it goes with hactool's patches once hactool is a fork of ours (#166).
function(snx_patched_copy)
    cmake_parse_arguments(arg "" "SOURCE;DEST" "PATCHES;FILES" ${ARGN})
    set(stale FALSE)
    set(pairs ${arg_FILES})
    while(pairs)
        list(POP_FRONT pairs name hash)
        set(have "")
        if(EXISTS "${arg_DEST}/${name}")
            file(SHA256 "${arg_DEST}/${name}" have)
        endif()
        if(NOT have STREQUAL hash)
            set(stale TRUE)
        endif()
    endwhile()
    if(NOT stale)
        return()
    endif()
    # CPM's search: on PATH, or the one Git for Windows carries
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
        get_filename_component(git_dir "${GIT_EXECUTABLE}" DIRECTORY)
    endif()
    find_program(PATCH_EXECUTABLE patch HINTS "${git_dir}/../usr/bin" "${git_dir}/../../usr/bin")
    if(NOT PATCH_EXECUTABLE)
        message(FATAL_ERROR "patch(1) is needed to patch ${arg_SOURCE}: install it, or Git for Windows")
    endif()
    file(REMOVE_RECURSE "${arg_DEST}")
    file(COPY "${arg_SOURCE}/" DESTINATION "${arg_DEST}")
    foreach(patch IN LISTS arg_PATCHES)
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 -i "${patch}" WORKING_DIRECTORY "${arg_DEST}"
                        RESULT_VARIABLE failed OUTPUT_VARIABLE said ERROR_VARIABLE said)
        if(failed)
            message(FATAL_ERROR "${patch} does not apply to ${arg_SOURCE}:\n${said}")
        endif()
    endforeach()
    set(pairs ${arg_FILES})
    while(pairs)
        list(POP_FRONT pairs name hash)
        file(SHA256 "${arg_DEST}/${name}" have)
        if(NOT have STREQUAL hash)
            message(FATAL_ERROR "${arg_DEST}/${name} (SHA-256 ${have}) is not what the patches make of ${arg_SOURCE}. "
                                "After a bump of it or a patch edit, check the patched files and update their hashes")
        endif()
    endwhile()
endfunction()
