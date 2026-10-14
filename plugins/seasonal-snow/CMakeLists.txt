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
set_target_properties(matterleast-seasonal-snow PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

install(TARGETS matterleast-seasonal-snow
    LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
    COMPONENT Runtime
)
