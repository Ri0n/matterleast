cmake_minimum_required(VERSION 3.16)

set(MATTERLEAST_VERSION "" CACHE STRING "MatterLeast version override (for example 1.0 or v1.0)")
set(MATTERLEAST_DEBIAN_EXTRA_DEPENDS "" CACHE STRING "Additional Debian runtime dependencies")
set(version "")

if(MATTERLEAST_VERSION)
	set(version "${MATTERLEAST_VERSION}")
	string(REGEX REPLACE "^v" "" version "${version}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.version")
	# Generated source trees and shallow clones are self-contained: when a
	# .version file is present, determining the version must not require Git.
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.version" version)
	string(STRIP "${version}" version)
else()
	find_package(Git QUIET)
	if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*"
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			RESULT_VARIABLE git_version_result
			OUTPUT_VARIABLE git_version_tag
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
		)
		if(git_version_result EQUAL 0
			AND git_version_tag MATCHES "^v[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
			string(REGEX REPLACE "^v" "" version "${git_version_tag}")
		endif()
	endif()
endif()

if(NOT version)
	message(FATAL_ERROR
		"Cannot determine MatterLeast version: set MATTERLEAST_VERSION, provide .version, or use a Git checkout with a reachable v* tag")
endif()

if(NOT version MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
	message(FATAL_ERROR "Invalid MatterLeast version '${version}'")
endif()

project(matterleast VERSION ${version}
    DESCRIPTION "Native Qt client for Mattermost"
    HOMEPAGE_URL "https://github.com/Ri0n/matterleast"
    LANGUAGES CXX)
file(WRITE "${CMAKE_BINARY_DIR}/matterleast-version.txt" "${PROJECT_VERSION}\n")
message(STATUS "MatterLeast version: ${PROJECT_VERSION}")

if(WIN32)
	enable_language(RC)
endif()

configure_file(build-config.h.in ${CMAKE_CURRENT_SOURCE_DIR}/sources/build-config.h)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Persistent application settings have one owner. Bypassing MLOptions would
# skip its in-process state and change notifications.
file(GLOB_RECURSE SETTINGS_ACCESS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sources/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h")
set(forbidden_settings_tokens "Q" "Settings")
string(JOIN "" forbidden_settings_type ${forbidden_settings_tokens})
foreach(source_file IN LISTS SETTINGS_ACCESS_SOURCES)
    if(source_file MATCHES "/sources/options/MLOptions\\.(cpp|h)$")
        continue()
    endif()
    file(READ "${source_file}" source_contents)
    string(FIND "${source_contents}" "${forbidden_settings_type}" settings_position)
    if(NOT settings_position EQUAL -1)
        message(FATAL_ERROR
            "Direct Qt settings access is forbidden outside MLOptions: ${source_file}")
    endif()
endforeach()

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MATTERLEAST_DEBUG_UPLOAD_HEADERS
    "Compile verbose HTTP upload header logging (may expose sensitive server-specific values)" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options(-Wall -Wextra)
	if(WARNINGS_AS_ERRORS)
		# Keep intentional preprocessor #warning diagnostics visible, but non-fatal.
		add_compile_options(-Werror -Wno-error=cpp)
	endif()
elseif(MSVC)
	add_compile_options(/W4)
	if(WARNINGS_AS_ERRORS)
		add_compile_options(/WX)
	endif()
endif()

set(APPID "matterleast")
set(PRODUCT_NAME "MatterLeast")
set(PRODUCT_VENDOR "MatterLeast contributors")
set(DESKTOP_ID "io.github.Ri0n.MatterLeast")

option(QT_6 "Prefer Qt6" ON)

find_package(Qt6 COMPONENTS Core QUIET)
if(Qt6_FOUND AND QT_6)
    set(QT_VERSION_MAJOR 6)
else()
    set(QT_VERSION_MAJOR 5)
endif()

# QtCreator supports the following variables for Android, which are identical to qmake Android variables.
# Check http://doc.qt.io/qt-5/deployment-android.html for more information.
# They need to be set before the find_package(Qt5 ...) call.

#if(ANDROID)
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
#    if(ANDROID_ABI STREQUAL "armeabi-v7a")
#        set(ANDROID_EXTRA_LIBS
#            ${CMAKE_CURRENT_SOURCE_DIR}/path/to/libcrypto.so
#            ${CMAKE_CURRENT_SOURCE_DIR}/path/to/libssl.so)
#    endif()
#endif()

option(BUILD_MULTIMEDIA "Enable Multimedia" OFF)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Sql REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS WebSockets REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(Qt${QT_VERSION_MAJOR} COMPONENTS DBus QUIET)
endif()

include(CTest)
if(BUILD_TESTING)
	find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test REQUIRED)
endif()

if(BUILD_MULTIMEDIA)
	find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Multimedia REQUIRED)
	find_package(Qt${QT_VERSION_MAJOR} COMPONENTS MultimediaWidgets REQUIRED)
endif()

# Keep native plugin policy independent of authentication.
option(MATTERLEAST_ENABLE_NATIVE_PLUGINS "Allow loading native MatterLeast plugins" ON)

# Auto-enable keyring-backed sessions only if development files for the
# matching major Qt version are installed. Explicit ON/OFF always wins.
# An explicit ON may use our pinned QtKeychain fallback (Windows packaging);
# the automatic case must never download dependencies.
set(_matterleast_system_keychain_target "")
if(NOT DEFINED MATTERLEAST_USE_KEYCHAIN OR MATTERLEAST_USE_KEYCHAIN)
  find_package(Qt${QT_VERSION_MAJOR}Keychain CONFIG QUIET)
  if(TARGET Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
    set(_matterleast_system_keychain_target
        Qt${QT_VERSION_MAJOR}Keychain::Qt${QT_VERSION_MAJOR}Keychain)
  elseif(TARGET qt${QT_VERSION_MAJOR}keychain)
    set(_matterleast_system_keychain_target qt${QT_VERSION_MAJOR}keychain)
  endif()
endif()
if(_matterleast_system_keychain_target)
  set(_matterleast_keychain_default ON)
else()
  set(_matterleast_keychain_default OFF)
endif()
option(MATTERLEAST_USE_KEYCHAIN
  "Store saved login sessions in the platform keyring (default: ON only when QtKeychain dev files are installed)"
  ${_matterleast_keychain_default})

# Corporate builds embed only a PUBLIC Ed25519 verification key. Signing uses
# an external private key, never checked into the source tree or application.
set(MATTERLEAST_PLUGIN_VERIFY_KEY_FILE "" CACHE FILEPATH
    "PEM Ed25519 public key: require a valid detached signature on every MatterLeast native plugin")
set(MATTERLEAST_PLUGIN_SIGNING_KEY_FILE "" CACHE FILEPATH
    "PEM Ed25519 private key for signing bundled plugins; use only in trusted packaging")
if(MATTERLEAST_PLUGIN_VERIFY_KEY_FILE)
  if(NOT EXISTS "${MATTERLEAST_PLUGIN_VERIFY_KEY_FILE}")
    message(FATAL_ERROR "Plugin verification public key does not exist")
  endif()
  find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)
  file(READ "${MATTERLEAST_PLUGIN_VERIFY_KEY_FILE}" MATTERLEAST_PLUGIN_PUBLIC_KEY_PEM)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
  configure_file(sources/plugins/TrustedPluginKey.h.in
                 "${CMAKE_CURRENT_BINARY_DIR}/generated/TrustedPluginKey.h" @ONLY)
endif()
if(MATTERLEAST_PLUGIN_SIGNING_KEY_FILE)
  if(NOT MATTERLEAST_PLUGIN_VERIFY_KEY_FILE)
    message(FATAL_ERROR "Signing requires configuring the matching verification key")
  endif()
  if(NOT EXISTS "${MATTERLEAST_PLUGIN_SIGNING_KEY_FILE}")
    message(FATAL_ERROR "Plugin signing private key file does not exist")
  endif()
  find_program(MATTERLEAST_OPENSSL_EXECUTABLE openssl)
  if(NOT MATTERLEAST_OPENSSL_EXECUTABLE)
    message(FATAL_ERROR "OpenSSL CLI is needed when signing bundled native plugins")
  endif()
endif()

if(MATTERLEAST_USE_KEYCHAIN)
  if(_matterleast_system_keychain_target)
    set(MATTERLEAST_KEYCHAIN_TARGET ${_matterleast_system_keychain_target})
    message(STATUS "MatterLeast keychain: ON (system Qt${QT_VERSION_MAJOR}Keychain)")
  else()
    # Reaching this fallback requires an explicit -DMATTERLEAST_USE_KEYCHAIN=ON.
    # In AUTO mode missing dev files select OFF instead of downloading.
    message(STATUS "MatterLeast keychain: ON (bundled QtKeychain)")
    include(FetchContent)
    set(_matterleast_saved_build_testing ${BUILD_TESTING})
    set(_matterleast_saved_shared_libs ${BUILD_SHARED_LIBS})
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TRANSLATIONS OFF)
    if(QT_VERSION_MAJOR EQUAL 5)
      set(BUILD_WITH_QT5 ON)
    else()
      set(BUILD_WITH_QT5 OFF)
    endif()
    if(UNIX AND NOT APPLE AND NOT ANDROID)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(MATTERLEAST_LIBSECRET QUIET libsecret-1)
      endif()
      # QtKeychain's option() uses the OLD CMP0077 policy on older CMake:
      # a normal variable is silently discarded and it defaults back to ON.
      set(LIBSECRET_SUPPORT ${MATTERLEAST_LIBSECRET_FOUND}
          CACHE BOOL "Enable QtKeychain libsecret backend if available" FORCE)
    endif()
    FetchContent_Declare(matterleast_qtkeychain
      URL https://codeload.github.com/frankosterfeld/qtkeychain/tar.gz/875f77d9f61bd97fd84cca47ce3bc71186dfbd09
)
    FetchContent_MakeAvailable(matterleast_qtkeychain)
    # This pinned upstream release contains a legacy Linux callback cast.
    # Keep -Werror for MatterLeast without imposing it on this external target.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(qt${QT_VERSION_MAJOR}keychain PRIVATE
                             -Wno-error=cast-function-type)
    elseif(MSVC)
      # QtKeychain 0.17 has warnings in its Windows Credential Store backend.
      target_compile_options(qt${QT_VERSION_MAJOR}keychain PRIVATE /WX-)
    endif()
    set(BUILD_TESTING ${_matterleast_saved_build_testing})
    set(BUILD_SHARED_LIBS ${_matterleast_saved_shared_libs})
    set(MATTERLEAST_KEYCHAIN_TARGET qt${QT_VERSION_MAJOR}keychain)
  endif()
else()
  message(STATUS "MatterLeast keychain: OFF (missing system dev files or explicitly disabled)")
endif()

# Use system QCA >=3.0.11 where available; otherwise build the pinned
# upstream version (including its OpenSSL-backed AES-GCM provider).
find_package(Qca3-qt${QT_VERSION_MAJOR} 3.0.11 CONFIG QUIET)
if(NOT TARGET Qca3::Qca)
  include(FetchContent)
  # Embedded QCA's optional Doxygen rule assumes it is the top-level source.
  # Documentation is not part of our dependency build.
  set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE)
  set(BUILD_TESTS OFF CACHE BOOL "QCA tests" FORCE)
  set(BUILD_TOOLS OFF CACHE BOOL "QCA tools" FORCE)
  set(BUILD_PLUGINS "ossl" CACHE STRING "QCA providers" FORCE)
  set(BUILD_SHARED_LIBS ON CACHE BOOL "QCA shared library" FORCE)
  if(QT_VERSION_MAJOR EQUAL 6)
    set(BUILD_WITH_QT6 ON CACHE BOOL "QCA Qt version" FORCE)
  else()
    set(BUILD_WITH_QT6 OFF CACHE BOOL "QCA Qt version" FORCE)
  endif()
  FetchContent_Declare(matterleast_qca
    URL https://codeload.github.com/psi-im/qca/tar.gz/0adfcc757974ca865f6d1de0955df68e7a304676
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(matterleast_qca)
  # QCA 3.0.11 generates qca_export.h in its own build directory but
  # assumes CMAKE_BINARY_DIR points there. Supply the directory explicitly
  # when it is embedded inside another project.
  target_include_directories(qca3-qt${QT_VERSION_MAJOR} PUBLIC
    "$<BUILD_INTERFACE:${matterleast_qca_SOURCE_DIR}/include/QtCrypto>"
    "$<BUILD_INTERFACE:${matterleast_qca_BINARY_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>")
  if(TARGET qca-ossl)
    target_include_directories(qca-ossl PRIVATE "${matterleast_qca_BINARY_DIR}")
  endif()
  # Keep project -Werror separate from our pinned third-party library.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qca3-qt${QT_VERSION_MAJOR} PRIVATE -Wno-error)
    if(TARGET qca-ossl)
      target_compile_options(qca-ossl PRIVATE -Wno-error)
    endif()
  elseif(MSVC)
    target_compile_options(qca3-qt${QT_VERSION_MAJOR} PRIVATE /WX-)
    if(TARGET qca-ossl)
      target_compile_options(qca-ossl PRIVATE /WX-)
    endif()
  endif()
endif()
if(NOT TARGET Qca3::Qca)
  message(FATAL_ERROR "QCA 3.0.11 or newer is needed to encrypt local messages")
endif()

include_directories(${Qt${QT_VERSION_MAJOR}Network_INCLUDE_DIRS} sources/)
add_subdirectory(plugins)
add_subdirectory(third_party/QSourceHighlite)

if(ANDROID)
  add_library(${APPID} SHARED
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    logindialog.cpp
    logindialog.h
    logindialog.ui
    qmattermostbackend.cpp
    qmattermostbackend.h
    resource.qrc
  )
else()
  # Keep an existing build tree aware of newly added/removed implementation
  # files. CI always configures from scratch, but local incremental builds must
  # also rerun CMake when the recursive source set changes.
  file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS sources/*.cpp)
  # deprecated/ is intentionally a compile-time quarantine. There are no
  # forwarding headers: any remaining dependency on the old chat timeline must
  # fail at its real active call site and become part of the migration checklist.
  list(FILTER SOURCES EXCLUDE REGEX "(^|/)deprecated/")
  # Share the production implementation with integration tests, including the
  # real repository, HTTP connector and source/widget signal wiring.
  list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/sources/main.cpp")
  add_library(matterleast-core OBJECT ${SOURCES})
  if(MATTERLEAST_DEBUG_UPLOAD_HEADERS)
    target_compile_definitions(matterleast-core PRIVATE MATTERLEAST_DEBUG_UPLOAD_HEADERS)
  endif()
  if(NOT MATTERLEAST_ENABLE_NATIVE_PLUGINS)
    target_compile_definitions(matterleast-core PRIVATE MATTERLEAST_NATIVE_PLUGINS_DISABLED)
  endif()
  if(MATTERLEAST_PLUGIN_VERIFY_KEY_FILE)
    target_compile_definitions(matterleast-core PRIVATE MATTERLEAST_TRUSTED_PLUGINS_ONLY)
    target_include_directories(matterleast-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(matterleast-core PUBLIC OpenSSL::Crypto)
  endif()
  if(MATTERLEAST_USE_KEYCHAIN)
    target_link_libraries(matterleast-core PUBLIC ${MATTERLEAST_KEYCHAIN_TARGET})
    target_compile_definitions(matterleast-core PRIVATE MATTERLEAST_HAVE_KEYCHAIN)
  endif()
  target_link_libraries(matterleast-core PUBLIC
    Qca3::Qca
    Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Sql Qt${QT_VERSION_MAJOR}::WebSockets qsourcehighlite
    matterleast-plugin-api)
  if(BUILD_MULTIMEDIA)
    # matterleast-core compiles the multimedia-aware production sources. Keep
    # those Qt modules in its public link interface so integration tests that
    # consume the object library inherit the same link requirements instead of
    # duplicating them target by target.
    target_link_libraries(matterleast-core PUBLIC
      Qt${QT_VERSION_MAJOR}::Multimedia
      Qt${QT_VERSION_MAJOR}::MultimediaWidgets)
  endif()
  if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
    target_link_libraries(matterleast-core PUBLIC Qt${QT_VERSION_MAJOR}::DBus)
    target_compile_definitions(matterleast-core PRIVATE MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS)
  endif()
  add_executable(${APPID} sources/main.cpp resource.qrc)
  target_link_libraries(${APPID} PRIVATE matterleast-core)

  if(WIN32)
    # Keep Windows Explorer/AV/product metadata tied to the same version that
    # CMake and CPack publish. A fourth numeric component is required by PE.
    set(MATTERLEAST_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
    if(MATTERLEAST_VERSION_PATCH STREQUAL "")
      set(MATTERLEAST_VERSION_PATCH 0)
    endif()
    # The configured .rc lives in the build tree, so make the icon path
    # independent of the resource file's output location. Forward slashes are
    # accepted by the Windows resource compiler and avoid RC string escaping.
    file(TO_CMAKE_PATH
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/matterleast.ico"
      MATTERLEAST_WINDOWS_ICON)
    configure_file(
      resources/matterleast.rc
      "${CMAKE_CURRENT_BINARY_DIR}/matterleast.rc"
      @ONLY)
    set(MATTERLEAST_WINDOWS_FILE_VERSION
      "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${MATTERLEAST_VERSION_PATCH}.0")
    target_sources(${APPID} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/matterleast.rc")
    set_property(TARGET ${APPID} PROPERTY WIN32_EXECUTABLE true)
  endif()

endif()

add_subdirectory (tools)

if(BUILD_TESTING)
	add_subdirectory(tests)
	add_subdirectory(tests/plugins)
endif()

target_link_libraries (${APPID} PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Sql Qt${QT_VERSION_MAJOR}::WebSockets qsourcehighlite)

if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
	target_link_libraries(${APPID} PRIVATE Qt${QT_VERSION_MAJOR}::DBus)
	target_compile_definitions(${APPID} PRIVATE MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS)
endif()

if(NOT ANDROID)
	include(GNUInstallDirs)
	install(TARGETS "${APPID}"
		RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
		COMPONENT Runtime)
endif()

# Qt's deployment API runs windeployqt at install/package time and copies the
# Qt runtime, plugins and the MSVC runtime into CPack's staging tree. Keeping
# deployment in the install rules means a normal `cmake --install` and the
# Windows installer contain the same runtime payload.
if(WIN32 AND QT_VERSION_MAJOR EQUAL 6)
	qt_generate_deploy_app_script(
		TARGET "${APPID}"
		OUTPUT_SCRIPT qt_deploy_script
		NO_TRANSLATIONS
		NO_UNSUPPORTED_PLATFORM_ERROR
	)
	install(SCRIPT "${qt_deploy_script}" COMPONENT Runtime)
endif()

if(UNIX)
	install(FILES "img/MatterLeast.svg"
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
		RENAME "${DESKTOP_ID}.svg"
		COMPONENT Runtime)

	configure_file(
		"resources/matterleast.desktop.in"
		"${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.desktop"
		@ONLY)
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.desktop"
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications"
		COMPONENT Runtime)

	configure_file(
		"resources/matterleast.appdata.xml.in"
		"${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.appdata.xml"
		@ONLY)
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.appdata.xml"
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo"
		COMPONENT Runtime)
endif()

if(BUILD_MULTIMEDIA)
	target_link_libraries(${APPID} PRIVATE Qt${QT_VERSION_MAJOR}::Multimedia Qt${QT_VERSION_MAJOR}::MultimediaWidgets)
endif()

set(CPACK_PACKAGE_NAME "${APPID}")
set(CPACK_PACKAGE_VENDOR "${PRODUCT_VENDOR}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_CONTACT "Sergei Ilinykh <rion4ik@gmail.com>")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING.LESSER")
set(CPACK_PACKAGE_EXECUTABLES "${APPID}" "${PRODUCT_NAME}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Sergei Ilinykh <rion4ik@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "mattermost-qt")
if(QT_VERSION_MAJOR EQUAL 6)
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")
else()
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt5sql5-sqlite")
endif()
if(MATTERLEAST_DEBIAN_EXTRA_DEPENDS)
	string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", ${MATTERLEAST_DEBIAN_EXTRA_DEPENDS}")
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
set(CPACK_RPM_PACKAGE_LICENSE "LGPL-3.0-or-later")
if(QT_VERSION_MAJOR EQUAL 6)
	set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtsvg")
else()
	set(CPACK_RPM_PACKAGE_REQUIRES "qt5-qtsvg")
endif()

if(WIN32)
	set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PRODUCT_NAME}")
	set(CPACK_NSIS_DISPLAY_NAME "${PRODUCT_NAME}")
	set(CPACK_NSIS_PACKAGE_NAME "${PRODUCT_NAME}")
	set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/matterleast.ico")
	set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/matterleast.ico")
	set(CPACK_NSIS_INSTALLED_ICON_NAME "bin/matterleast.exe")
	set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
	set(CPACK_NSIS_HELP_LINK "${PROJECT_HOMEPAGE_URL}/issues")
	set(CPACK_NSIS_URL_INFO_ABOUT "${PROJECT_HOMEPAGE_URL}")
	set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")
	set(CPACK_NSIS_BRANDING_TEXT "${PRODUCT_NAME} ${PROJECT_VERSION}")
	set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)

	# Component-aware CPack NSIS packaging replaces CPACK_NSIS_DEFINES with
	# generated component macros. Keep installer VERSIONINFO in a template slot
	# that cmCPackNSISGenerator only fills when the project has not supplied it.
	# CPack serializes this value through CPackConfig.cmake before substituting it
	# into project.nsi, so quotes required by NSIS need to survive one extra layer.
	string(ASCII 92 34 MATTERLEAST_NSIS_QUOTE)
	string(CONCAT CPACK_NSIS_BRANDING_TEXT_CODE
		"BrandingText /TRIMLEFT ${MATTERLEAST_NSIS_QUOTE}${PRODUCT_NAME} ${PROJECT_VERSION}${MATTERLEAST_NSIS_QUOTE}\n"
		"VIProductVersion ${MATTERLEAST_WINDOWS_FILE_VERSION}\n"
		"VIFileVersion ${MATTERLEAST_WINDOWS_FILE_VERSION}\n"
		"VIAddVersionKey /LANG=1033 ${MATTERLEAST_NSIS_QUOTE}ProductName${MATTERLEAST_NSIS_QUOTE} ${MATTERLEAST_NSIS_QUOTE}${PRODUCT_NAME}${MATTERLEAST_NSIS_QUOTE}\n"
		"VIAddVersionKey /LANG=1033 ${MATTERLEAST_NSIS_QUOTE}ProductVersion${MATTERLEAST_NSIS_QUOTE} ${MATTERLEAST_NSIS_QUOTE}${PROJECT_VERSION}${MATTERLEAST_NSIS_QUOTE}\n"
		"VIAddVersionKey /LANG=1033 ${MATTERLEAST_NSIS_QUOTE}CompanyName${MATTERLEAST_NSIS_QUOTE} ${MATTERLEAST_NSIS_QUOTE}${PRODUCT_VENDOR}${MATTERLEAST_NSIS_QUOTE}\n"
		"VIAddVersionKey /LANG=1033 ${MATTERLEAST_NSIS_QUOTE}FileDescription${MATTERLEAST_NSIS_QUOTE} ${MATTERLEAST_NSIS_QUOTE}${PRODUCT_NAME} Installer${MATTERLEAST_NSIS_QUOTE}\n"
		"VIAddVersionKey /LANG=1033 ${MATTERLEAST_NSIS_QUOTE}FileVersion${MATTERLEAST_NSIS_QUOTE} ${MATTERLEAST_NSIS_QUOTE}${PROJECT_VERSION}${MATTERLEAST_NSIS_QUOTE}\n"
	)
endif()

include(CPack)
