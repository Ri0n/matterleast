include_guard(GLOBAL)

include(GNUInstallDirs)

# Sign a native MatterLeast plugin after linking and install its detached
# RSA/SHA-256 signature alongside the final binary. The key is supplied only by
# trusted packaging infrastructure and is never bundled with the application.
#
# Usage:
#   matterleast_sign_plugin(my-plugin)
#   matterleast_sign_plugin(my-plugin
#       DESTINATION "${CMAKE_INSTALL_LIBDIR}/matterleast/plugins"
#       COMPONENT Runtime)
#
# Without MATTERLEAST_PLUGIN_SIGNING_KEY_FILE this is intentionally a no-op.
function(matterleast_sign_plugin target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot sign unknown MatterLeast plugin target: ${target}")
    endif()

    cmake_parse_arguments(SIGN "" "DESTINATION;COMPONENT" "" ${ARGN})
    if(SIGN_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown arguments for matterleast_sign_plugin: ${SIGN_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT MATTERLEAST_PLUGIN_SIGNING_KEY_FILE)
        return()
    endif()

    get_target_property(_target_type "${target}" TYPE)
    if(NOT _target_type STREQUAL "MODULE_LIBRARY"
            AND NOT _target_type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR
            "matterleast_sign_plugin requires a module or shared library target: ${target}")
    endif()

    if(NOT SIGN_DESTINATION)
        set(SIGN_DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins")
    endif()
    if(NOT SIGN_COMPONENT)
        set(SIGN_COMPONENT Runtime)
    endif()

    if(NOT MATTERLEAST_OPENSSL_EXECUTABLE)
        message(FATAL_ERROR "Signing ${target} requires an OpenSSL executable")
    endif()

    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${MATTERLEAST_OPENSSL_EXECUTABLE}" dgst -sha256 -sign
                "${MATTERLEAST_PLUGIN_SIGNING_KEY_FILE}"
                -in "$<TARGET_FILE:${target}>"
                -out "$<TARGET_FILE:${target}>.sig"
        COMMENT "Sign MatterLeast plugin ${target} with external RSA/SHA-256 key"
        VERBATIM)

    install(FILES "$<TARGET_FILE:${target}>.sig"
        DESTINATION "${SIGN_DESTINATION}"
        COMPONENT "${SIGN_COMPONENT}")
endfunction()
