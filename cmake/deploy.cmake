# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Copies the runtime to games/<Game> for every games/<Game>_code.so (games/<Game>.exe for every _code.dll on
# Windows). The runtime finds its game module by its own file name, so each game gets a copy under its name.
if(WINDOWS)
    set(module_ext dll)
    set(exe_ext .exe)
else()
    set(module_ext so)
    set(exe_ext "")
endif()
file(GLOB modules "${GAMES}/*_code.${module_ext}")
foreach(module IN LISTS modules)
    get_filename_component(name "${module}" NAME)
    string(REGEX REPLACE "_code\\.${module_ext}$" "" game "${name}")
    # Through a temporary file and a rename, so a game that is running keeps its old copy
    file(COPY_FILE "${RUNTIME}" "${GAMES}/.${game}.tmp")
    file(CHMOD "${GAMES}/.${game}.tmp" FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
         WORLD_READ WORLD_EXECUTE)
    file(RENAME "${GAMES}/.${game}.tmp" "${GAMES}/${game}${exe_ext}")
    message(STATUS "Runtime deployed to ${GAMES}/${game}${exe_ext}")
endforeach()
