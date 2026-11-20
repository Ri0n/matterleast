add_library(matterleast-test-conversation-plugin MODULE
    TestConversationPlugin.cpp
)

target_link_libraries(matterleast-test-conversation-plugin
    PRIVATE
        matterleast-plugin-api
        Qt${QT_VERSION_MAJOR}::Core
)

add_library(matterleast-test-calls-plugin MODULE
    TestCallsPlugin.cpp
)
target_link_libraries(matterleast-test-calls-plugin PRIVATE
    matterleast-plugin-api
    Qt${QT_VERSION_MAJOR}::Core
)

add_executable(plugin-host-test
    "${CMAKE_SOURCE_DIR}/tests/PluginHostTest.cpp"
    "${CMAKE_SOURCE_DIR}/sources/plugins/PluginHost.cpp"
)

target_include_directories(plugin-host-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sources"
)

target_link_libraries(plugin-host-test
    PRIVATE
        matterleast-plugin-api
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
)

add_dependencies(plugin-host-test
    matterleast-test-conversation-plugin
    matterleast-test-calls-plugin)

add_test(
    NAME plugin-host-test
    COMMAND "${CMAKE_COMMAND}" -E env
        "MATTERLEAST_TEST_PLUGIN_PATH=$<TARGET_FILE:matterleast-test-conversation-plugin>"
        "MATTERLEAST_TEST_CALLS_PLUGIN_PATH=$<TARGET_FILE:matterleast-test-calls-plugin>"
        "$<TARGET_FILE:plugin-host-test>"
)

set(_matterleast_qt_dir_var "Qt${QT_VERSION_MAJOR}_DIR")
set(MATTERLEAST_PLUGIN_SMOKE_QT_DIR "${${_matterleast_qt_dir_var}}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PluginSdkSmoke.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PluginSdkSmoke.cmake"
    @ONLY
)

add_test(
    NAME plugin-sdk-external-build-test
    COMMAND "${CMAKE_COMMAND}" -P
        "${CMAKE_CURRENT_BINARY_DIR}/PluginSdkSmoke.cmake"
)

# Test the real bundled plugin through the public ABI (no core linkage).
add_executable(seasonal-snow-test
    SeasonalSnowTest.cpp
)
target_include_directories(seasonal-snow-test PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(seasonal-snow-test PRIVATE
    matterleast-plugin-api
    Qt${QT_VERSION_MAJOR}::Test
    Qt${QT_VERSION_MAJOR}::Widgets
)
add_dependencies(seasonal-snow-test matterleast-seasonal-snow)
add_test(
    NAME seasonal-snow-test
    COMMAND "${CMAKE_COMMAND}" -E env
        "QT_QPA_PLATFORM=offscreen"
        "MATTERLEAST_SNOW_TEST_PLUGIN_PATH=$<TARGET_FILE:matterleast-seasonal-snow>"
        "$<TARGET_FILE:seasonal-snow-test>"
)

# Binary Calls SDP is not JSON websocket framing: test the exact
# MessagePack envelope and RFC1950 zlib payload independently.
add_executable(calls-messagepack-test
    "${CMAKE_SOURCE_DIR}/tests/CallsMessagePackTest.cpp"
    "${CMAKE_SOURCE_DIR}/sources/backend/CallsMessagePack.cpp"
)
target_include_directories(calls-messagepack-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(calls-messagepack-test PRIVATE
    Qt${QT_VERSION_MAJOR}::Test
    Qt${QT_VERSION_MAJOR}::Core
)
add_test(NAME calls-messagepack-test COMMAND calls-messagepack-test)

# Actual native Calls plugin smoke test; enable only on distributions where
# GStreamer WebRTC development/runtime modules are installed.
if(MATTERLEAST_BUILD_CALLS_GSTREAMER)
    add_executable(calls-gstreamer-test
        CallsGStreamerTest.cpp
        "${CMAKE_SOURCE_DIR}/sources/plugins/PluginHost.cpp"
    )
    target_include_directories(calls-gstreamer-test PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/sources"
    )
    target_link_libraries(calls-gstreamer-test PRIVATE
        matterleast-plugin-api
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
    )
    add_executable(calls-window-test
        CallsWindowTest.cpp
        "${CMAKE_SOURCE_DIR}/plugins/calls-gstreamer/CallsWindow.cpp"
    )
    target_include_directories(calls-window-test PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(calls-window-test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(
        NAME calls-window-test
        COMMAND "${CMAKE_COMMAND}" -E env
            "QT_QPA_PLATFORM=offscreen"
            "$<TARGET_FILE:calls-window-test>"
    )

    add_dependencies(calls-gstreamer-test matterleast-calls-gstreamer)
    add_test(
        NAME calls-gstreamer-test
        COMMAND "${CMAKE_COMMAND}" -E env
            "QT_QPA_PLATFORM=offscreen"
            "MATTERLEAST_CALLS_TEST_FAKE_AUDIO=1"
            "MATTERLEAST_CALLS_PLUGIN_PATH=$<TARGET_FILE:matterleast-calls-gstreamer>"
            "$<TARGET_FILE:calls-gstreamer-test>"
    )
endif()
