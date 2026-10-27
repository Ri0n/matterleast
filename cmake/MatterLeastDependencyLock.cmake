# Central dependency lock, matching Iris's dependencies.lock.json structure.
# Parsing with regex keeps support for MatterLeast's CMake 3.16 minimum.
set(_matterleast_dependency_lock_file "${CMAKE_CURRENT_LIST_DIR}/../dependencies.lock.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_matterleast_dependency_lock_file}")
file(READ "${_matterleast_dependency_lock_file}" _matterleast_dependency_lock)

function(matterleast_load_dependency name output)
    string(REGEX MATCH
        "\"${name}\"[ \t\r\n]*:[ \t\r\n]*\\{[^}]*\\}"
        entry "${_matterleast_dependency_lock}")
    if(NOT entry)
        message(FATAL_ERROR "dependencies.lock.json: missing ${name}")
    endif()

    string(REGEX MATCH
        "\"commit\"[ \t\r\n]*:[ \t\r\n]*\"([0-9a-f]+)\""
        commit_match "${entry}")
    set(commit "${CMAKE_MATCH_1}")
    string(LENGTH "${commit}" commit_length)
    if(NOT commit_length EQUAL 40)
        message(FATAL_ERROR "dependencies.lock.json: ${name} must have a full 40-character lowercase Git SHA")
    endif()

    string(REGEX MATCH
        "\"tag\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\""
        tag_match "${entry}")
    set(tag "${CMAKE_MATCH_1}")
    if(NOT tag MATCHES "^v?[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "dependencies.lock.json: invalid ${name} release tag '${tag}'")
    endif()

    string(REGEX MATCH
        "\"minimum_version\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\""
        minimum_match "${entry}")
    set(minimum "${CMAKE_MATCH_1}")
    if(NOT minimum MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "dependencies.lock.json: invalid ${name} minimum_version '${minimum}'")
    endif()
    string(REGEX REPLACE "^v" "" tag_version "${tag}")
    if(tag_version VERSION_LESS minimum)
        message(FATAL_ERROR "dependencies.lock.json: ${name} tag ${tag} is older than minimum ${minimum}")
    endif()

    set("${output}_COMMIT" "${commit}" PARENT_SCOPE)
    set("${output}_TAG" "${tag}" PARENT_SCOPE)
    set("${output}_MIN_VERSION" "${minimum}" PARENT_SCOPE)
endfunction()
