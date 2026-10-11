# Copies the runtime to games/<Game> for every games/<Game>_code.so. The runtime finds its game module by its own
# file name, so each game gets a copy under its name.
file(GLOB modules "${GAMES}/*_code.so")
foreach(module IN LISTS modules)
    get_filename_component(name "${module}" NAME)
    string(REGEX REPLACE "_code\\.so$" "" game "${name}")
    # Through a temporary file and a rename, so a game that is running keeps its old copy
    file(COPY_FILE "${RUNTIME}" "${GAMES}/.${game}.tmp")
    file(CHMOD "${GAMES}/.${game}.tmp" FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
         WORLD_READ WORLD_EXECUTE)
    file(RENAME "${GAMES}/.${game}.tmp" "${GAMES}/${game}")
    message(STATUS "Runtime deployed to ${GAMES}/${game}")
endforeach()
