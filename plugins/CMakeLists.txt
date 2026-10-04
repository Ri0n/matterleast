include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(MATTERLEAST_PLUGIN_API_VERSION "1.0.0")

add_library(matterleast-plugin-api INTERFACE)
add_library(MatterLeast::PluginApi ALIAS matterleast-plugin-api)

set_target_properties(matterleast-plugin-api PROPERTIES
    EXPORT_NAME PluginApi
)

target_include_directories(matterleast-plugin-api
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(matterleast-plugin-api
    INTERFACE
        Qt${QT_VERSION_MAJOR}::Core
)

install(TARGETS matterleast-plugin-api
    EXPORT MatterLeastPluginApiTargets
    COMPONENT Development
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/api/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/matterleast/plugin"
    COMPONENT Development
    FILES_MATCHING PATTERN "*.h"
)

set(MATTERLEAST_PLUGIN_API_CMAKE_DIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/MatterLeastPluginApi")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/MatterLeastPluginApiConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MatterLeastPluginApiConfig.cmake"
    INSTALL_DESTINATION "${MATTERLEAST_PLUGIN_API_CMAKE_DIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MatterLeastPluginApiConfigVersion.cmake"
    VERSION "${MATTERLEAST_PLUGIN_API_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(EXPORT MatterLeastPluginApiTargets
    FILE MatterLeastPluginApiTargets.cmake
    NAMESPACE MatterLeast::
    DESTINATION "${MATTERLEAST_PLUGIN_API_CMAKE_DIR}"
    COMPONENT Development
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MatterLeastPluginApiConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MatterLeastPluginApiConfigVersion.cmake"
    DESTINATION "${MATTERLEAST_PLUGIN_API_CMAKE_DIR}"
    COMPONENT Development
)
