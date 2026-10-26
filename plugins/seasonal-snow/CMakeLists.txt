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

# Detached signature (when corporate release signing is enabled).
matterleast_sign_plugin(matterleast-seasonal-snow)

install(TARGETS matterleast-seasonal-snow
    LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
    COMPONENT Runtime
)
