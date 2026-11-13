# Desktop-only native plugin; built and packaged separately from core.
add_library(matterleast-seasonal-snow MODULE
    SeasonalSnowPlugin.cpp
)

target_link_libraries(matterleast-seasonal-snow PRIVATE
    matterleast-plugin-api
    Qt${QT_VERSION_MAJOR}::Widgets
)

# Keep the build-tree plugin next to the executable for developer runs.
# Installed Linux packages use the distro libdir, discovered relative to bin.
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

# Native Linux modules have no ELF library-style "lib" prefix; Windows and
# macOS keep their existing filenames and bundle layouts.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(matterleast-seasonal-snow PROPERTIES
        PREFIX ""
        OUTPUT_NAME "seasonal-snow")
endif()

# Signature must follow the renamed module to the same install directory.
matterleast_sign_plugin(matterleast-seasonal-snow
    DESTINATION "${MATTERLEAST_NATIVE_PLUGIN_INSTALL_DIR}")

install(TARGETS matterleast-seasonal-snow
    LIBRARY DESTINATION "${MATTERLEAST_NATIVE_PLUGIN_INSTALL_DIR}"
    RUNTIME DESTINATION "${MATTERLEAST_NATIVE_PLUGIN_INSTALL_DIR}"
    COMPONENT Runtime
)
