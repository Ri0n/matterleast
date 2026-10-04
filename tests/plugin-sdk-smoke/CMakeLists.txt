cmake_minimum_required(VERSION 3.16)
project(matterleast_plugin_sdk_smoke LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(MatterLeastPluginApi 1.0 CONFIG REQUIRED)

add_library(matterleast-plugin-sdk-smoke MODULE
    SmokePlugin.cpp
)

target_link_libraries(matterleast-plugin-sdk-smoke
    PRIVATE
        MatterLeast::PluginApi
)
