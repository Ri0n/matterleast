add_library(matterleast-test-conversation-plugin MODULE
    TestConversationPlugin.cpp
)

target_link_libraries(matterleast-test-conversation-plugin
    PRIVATE
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

add_dependencies(plugin-host-test matterleast-test-conversation-plugin)

add_test(
    NAME plugin-host-test
    COMMAND "${CMAKE_COMMAND}" -E env
        "MATTERLEAST_TEST_PLUGIN_PATH=$<TARGET_FILE:matterleast-test-conversation-plugin>"
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
