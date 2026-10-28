cmake_minimum_required(VERSION 3.24)
project(MatterLeastWindowsUpgradeSmoke LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This integration test runs on Windows only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Qt6 REQUIRED COMPONENTS Core Network)

# Use the production Win32 sources, not imitations of the launcher/switcher.
set(MATTERLEAST_INSTALL_ID "v1.0")
configure_file("${REPO_ROOT}/sources/update/win/LauncherVersion.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/LauncherVersion.h" @ONLY)
add_executable(MatterLeastLauncher WIN32
    "${REPO_ROOT}/sources/update/win/launcher.cpp")
target_include_directories(MatterLeastLauncher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(MatterLeastUpdater WIN32
    "${REPO_ROOT}/sources/update/win/updater.cpp")

add_executable(update-good-stub WIN32 StubApplication.cpp)
add_executable(update-bad-stub WIN32 StubApplication.cpp)
target_compile_definitions(update-bad-stub PRIVATE FAIL_STARTUP=1)

# Compiles only the client update controller, ZIP installer and options.
# TrustPolicySmoke.cpp pins the exact public community URL, avoiding QCA,
# while the production signature verifier is covered by UpdateTrustPolicyTest.
add_executable(update-download-probe
    SmokeDownload.cpp
    TrustPolicySmoke.cpp
    "${REPO_ROOT}/sources/update/WindowsUpdateController.cpp"
    "${REPO_ROOT}/sources/update/WindowsZipInstaller.cpp"
    "${REPO_ROOT}/sources/options/MLOptions.cpp")
target_include_directories(update-download-probe PRIVATE "${REPO_ROOT}/sources")
target_link_libraries(update-download-probe PRIVATE Qt6::Core Qt6::Network)
