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
set(MATTERLEAST_MSIX_IDENTITY_NAME "SergeiIlinykh.Matterleast" CACHE STRING "Microsoft Store package identity")
set(MATTERLEAST_MSIX_PUBLISHER "CN=A752CCD7-6C4F-48CE-87D8-B22E3AD2713D" CACHE STRING "Microsoft Store publisher identity")
set(MATTERLEAST_MSIX_PUBLISHER_DISPLAY_NAME "Sergei Ilinykh" CACHE STRING "Microsoft Store publisher display name")

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
#    if (ANDROID_ABI STREQUAL "armeabi-v7a")
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

include_directories(${Qt${QT_VERSION_MAJOR}Network_INCLUDE_DIRS} sources/)
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
  target_link_libraries(matterleast-core PUBLIC
    Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Sql Qt${QT_VERSION_MAJOR}::WebSockets qsourcehighlite)
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
    # Store packages use four numeric components; the fourth is reserved by
    # Microsoft Store and must remain zero.
    set(MATTERLEAST_MSIX_VERSION
      "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${MATTERLEAST_VERSION_PATCH}.0")
    configure_file(
      resources/msix/AppxManifest.xml.in
      "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml"
      @ONLY)
    target_sources(${APPID} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/matterleast.rc")
    set_property(TARGET ${APPID} PROPERTY WIN32_EXECUTABLE true)
  endif()

endif()

add_subdirectory (tools)

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

target_link_libraries (${APPID} PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Sql Qt${QT_VERSION_MAJOR}::WebSockets qsourcehighlite)

if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
	target_link_libraries(${APPID} PRIVATE Qt${QT_VERSION_MAJOR}::DBus)
	target_compile_definitions(${APPID} PRIVATE MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS)
endif()

if(NOT ANDROID)
	include(GNUInstallDirs)
	install(TARGETS "${APPID}" RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
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
	install(SCRIPT "${qt_deploy_script}")
endif()

if(UNIX)
	install(FILES "img/MatterLeast.svg"
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
		RENAME "${DESKTOP_ID}.svg")

	configure_file(
		"resources/matterleast.desktop.in"
		"${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.desktop"
		@ONLY)
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.desktop"
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")

	configure_file(
		"resources/matterleast.appdata.xml.in"
		"${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.appdata.xml"
		@ONLY)
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_ID}.appdata.xml"
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo")
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

	# CPack's stock NSIS template does not add a VERSIONINFO resource to the
	# installer executable. Keep the installer identifiable in Explorer and
	# security/reputation tooling using the same canonical metadata as the app.
	# NSIS requires four numeric fields for the fixed version while the string
	# version remains the user-facing MatterLeast version (for example 1.0).
	string(APPEND CPACK_NSIS_DEFINES
		"\nVIProductVersion ${MATTERLEAST_MSIX_VERSION}"
		"\nVIFileVersion ${MATTERLEAST_MSIX_VERSION}"
		"\nVIAddVersionKey /LANG=1033 \\\"ProductName\\\" \\\"${PRODUCT_NAME}\\\""
		"\nVIAddVersionKey /LANG=1033 \\\"ProductVersion\\\" \\\"${PROJECT_VERSION}\\\""
		"\nVIAddVersionKey /LANG=1033 \\\"CompanyName\\\" \\\"${PRODUCT_VENDOR}\\\""
		"\nVIAddVersionKey /LANG=1033 \\\"FileDescription\\\" \\\"${PRODUCT_NAME} Installer\\\""
		"\nVIAddVersionKey /LANG=1033 \\\"FileVersion\\\" \\\"${PROJECT_VERSION}\\\""
	)
endif()

include(CPack)
