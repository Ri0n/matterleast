# Desktop-only native plugin; built and packaged separately from core.
add_library(matterleast-seasonal-snow MODULE
    SeasonalSnowPlugin.cpp
)

target_link_libraries(matterleast-seasonal-snow PRIVATE
    matterleast-plugin-api
    Qt${QT_VERSION_MAJOR}::Widgets
)

# Match PluginManager's <applicationDir>/plugins discovery in the build tree
# and in DEB/RPM/AppImage/Windows package installations.
if(CMAKE_CONFIGURATION_TYPES)
    # Visual Studio and multi-config Ninja place the app under <build>/<config>.
    set(_snow_output_dir "${CMAKE_BINARY_DIR}/$<CONFIG>/plugins")
else()
    set(_snow_output_dir "${CMAKE_BINARY_DIR}/plugins")
endif()
set_target_properties(matterleast-seasonal-snow PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_snow_output_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${_snow_output_dir}"
)

# Signing happens after the final binary has been linked, but before packaging.
# The private key is supplied externally by trusted release infrastructure.
if(MATTERLEAST_PLUGIN_SIGNING_KEY_FILE)
    add_custom_command(TARGET matterleast-seasonal-snow POST_BUILD
        COMMAND "${MATTERLEAST_OPENSSL_EXECUTABLE}" pkeyutl -sign -rawin
                -inkey "${MATTERLEAST_PLUGIN_SIGNING_KEY_FILE}"
                -in "$<TARGET_FILE:matterleast-seasonal-snow>"
                -out "$<TARGET_FILE:matterleast-seasonal-snow>.sig"
        COMMENT "Sign bundled MatterLeast plugin with external Ed25519 key"
        VERBATIM)
    install(FILES "$<TARGET_FILE:matterleast-seasonal-snow>.sig"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
        COMPONENT Runtime)
endif()

install(TARGETS matterleast-seasonal-snow
    LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
    COMPONENT Runtime
)
