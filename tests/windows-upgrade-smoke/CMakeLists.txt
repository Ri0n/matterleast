cmake_minimum_required(VERSION 3.24)
project(MatterLeastWindowsUpgradeSmoke LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This integration test runs on Windows only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_library(matterleast-miniz STATIC
    "${REPO_ROOT}/third_party/miniz/miniz.c"
    "${REPO_ROOT}/third_party/miniz/miniz_tinfl.c"
    "${REPO_ROOT}/third_party/miniz/miniz_zip.c")
target_include_directories(matterleast-miniz PUBLIC
    "${REPO_ROOT}/third_party/miniz")
target_compile_definitions(matterleast-miniz PRIVATE
    MINIZ_NO_DEFLATE_APIS MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_ZLIB_APIS)

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
target_link_libraries(update-download-probe PRIVATE Qt6::Core Qt6::Network matterleast-miniz)

# Signed-update transport smoke uses the exact pinned Windows QCA3 revision
# with native CNG wincrypto. The public GitHub test above remains unsigned.
include(FetchContent)
file(READ "${REPO_ROOT}/dependencies.lock.json" _matterleast_deps)
string(JSON _qca_master_sha GET "${_matterleast_deps}" qca_windows commit)
set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE)
set(BUILD_TESTS OFF CACHE BOOL "QCA internal tests" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "QCA tools" FORCE)
set(BUILD_PLUGINS "wincrypto" CACHE STRING "Only QCA native CNG provider" FORCE)
set(WITH_ossl_PLUGIN "no" CACHE STRING "" FORCE)
set(WITH_wincrypto_PLUGIN "yes" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LOAD_SHARED_PLUGINS OFF CACHE BOOL "" FORCE)
set(DEVELOPER_MODE ON CACHE BOOL "" FORCE)
set(BUILD_WITH_QT6 ON CACHE BOOL "" FORCE)
FetchContent_Declare(smoke_qca
    URL "https://codeload.github.com/psi-im/qca/tar.gz/${_qca_master_sha}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(smoke_qca)
target_include_directories(qca3-qt6 PUBLIC
    "$<BUILD_INTERFACE:${smoke_qca_SOURCE_DIR}/include/QtCrypto>"
    "$<BUILD_INTERFACE:${smoke_qca_BINARY_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>")
target_include_directories(qca-wincrypto PRIVATE "${smoke_qca_BINARY_DIR}")

add_executable(update-signed-probe
    SmokeSigned.cpp
    TrustPolicySignedSmoke.cpp
    "${REPO_ROOT}/sources/update/WindowsUpdateController.cpp"
    "${REPO_ROOT}/sources/update/WindowsZipInstaller.cpp"
    "${REPO_ROOT}/sources/crypto/RsaSha256Verifier.cpp"
    "${REPO_ROOT}/sources/options/MLOptions.cpp")
target_include_directories(update-signed-probe PRIVATE "${REPO_ROOT}/sources")
target_compile_definitions(update-signed-probe PRIVATE QCA_STATIC)
target_link_libraries(update-signed-probe PRIVATE
    Qt6::Core Qt6::Network matterleast-miniz Qca3::Qca qca-wincrypto)
