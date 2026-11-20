include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/MatterLeastPluginSigning.cmake")

set(MATTERLEAST_PLUGIN_API_VERSION "1.0.0")

# The traditional monolithic DEB includes the Plugin SDK. With Calls split
# into two components, keep those SDK files in the Runtime component rather
# than silently dropping headers/CMake exports from the main DEB.
set(_matterleast_plugin_sdk_component Development)
if(MATTERLEAST_BUILD_CALLS_GSTREAMER)
    set(_matterleast_plugin_sdk_component Runtime)
endif()

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
    COMPONENT ${_matterleast_plugin_sdk_component}
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/api/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/matterleast/plugin"
    COMPONENT ${_matterleast_plugin_sdk_component}
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
    COMPONENT ${_matterleast_plugin_sdk_component}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MatterLeastPluginApiConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MatterLeastPluginApiConfigVersion.cmake"
    DESTINATION "${MATTERLEAST_PLUGIN_API_CMAKE_DIR}"
    COMPONENT ${_matterleast_plugin_sdk_component}
)

# Bundle the seasonal visual effect on desktop; it remains a normal native
# plugin with an independent enable/disable lifecycle in Settings.
if(NOT ANDROID)
    add_subdirectory(seasonal-snow)
endif()

if(MATTERLEAST_BUILD_CALLS_GSTREAMER)
    add_subdirectory(calls-gstreamer)
endif()
