include("${CMAKE_SOURCE_DIR}/cmake/MatterLeastPluginSigning.cmake")

find_package(PkgConfig REQUIRED)
pkg_check_modules(MATTERLEAST_GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0>=1.24
    gstreamer-webrtc-1.0>=1.24
    gstreamer-sdp-1.0>=1.24
    gstreamer-rtp-1.0>=1.24
    gstreamer-video-1.0>=1.24
    gstreamer-app-1.0>=1.24
    glib-2.0
)

add_library(matterleast-calls-gstreamer MODULE
    CallsPlugin.cpp
    CallsWindow.cpp
    GstAudioEngine.cpp
    CallsMediaMap.cpp
)
target_link_libraries(matterleast-calls-gstreamer PRIVATE
    matterleast-plugin-api
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    PkgConfig::MATTERLEAST_GST
)
target_include_directories(matterleast-calls-gstreamer PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(matterleast-calls-gstreamer PRIVATE
    GST_USE_UNSTABLE_API
)
set_target_properties(matterleast-calls-gstreamer PROPERTIES
    CXX_STANDARD 17
)
if(CMAKE_CONFIGURATION_TYPES)
    set(_calls_output_dir "${CMAKE_BINARY_DIR}/$<CONFIG>/plugins")
else()
    set(_calls_output_dir "${CMAKE_BINARY_DIR}/plugins")
endif()
set_target_properties(matterleast-calls-gstreamer PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_calls_output_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${_calls_output_dir}"
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(matterleast-calls-gstreamer PROPERTIES
        PREFIX ""
        OUTPUT_NAME "calls-gstreamer")
endif()

matterleast_sign_plugin(matterleast-calls-gstreamer
    DESTINATION "${MATTERLEAST_NATIVE_PLUGIN_INSTALL_DIR}"
    COMPONENT Calls)

install(TARGETS matterleast-calls-gstreamer
    LIBRARY DESTINATION "${MATTERLEAST_NATIVE_PLUGIN_INSTALL_DIR}"
            COMPONENT Calls
    RUNTIME DESTINATION "${MATTERLEAST_NATIVE_PLUGIN_INSTALL_DIR}"
            COMPONENT Calls
)
