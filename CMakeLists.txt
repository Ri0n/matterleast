cmake_minimum_required(VERSION 3.16)

file(READ .version version)
project(qmattermost VERSION ${version} LANGUAGES CXX)
configure_file(build-config.h.in ${CMAKE_CURRENT_SOURCE_DIR}/sources/build-config.h)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
set(DESKTOP_ID "matterleast")

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
  add_library(mattermost-client SHARED
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
  add_library(mattermost-core OBJECT ${SOURCES})
  target_link_libraries(mattermost-core PUBLIC
    Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Sql Qt${QT_VERSION_MAJOR}::WebSockets qsourcehighlite)
  if(TARGET Qt${QT_VERSION_MAJOR}::DBus)
    target_link_libraries(mattermost-core PUBLIC Qt${QT_VERSION_MAJOR}::DBus)
    target_compile_definitions(mattermost-core PRIVATE MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS)
  endif()
  add_executable(${APPID} sources/main.cpp resource.qrc)
  target_link_libraries(${APPID} PRIVATE mattermost-core)
  
  if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
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
	target_link_libraries(qmattermost PRIVATE Qt${QT_VERSION_MAJOR}::Multimedia Qt${QT_VERSION_MAJOR}::MultimediaWidgets)
endif()

set(CPACK_PACKAGE_NAME "${APPID}")
set(CPACK_PACKAGE_VENDOR "${PRODUCT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MatterLeast - native Qt client for Mattermost")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Ri0n/mattermost-qt")
set(CPACK_PACKAGE_CONTACT "MatterLeast Maintainers <maintainers@mattermost-qt.invalid>")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING.LESSER")
set(CPACK_PACKAGE_EXECUTABLES "${APPID}" "${PRODUCT_NAME}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "MatterLeast Maintainers <maintainers@mattermost-qt.invalid>")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "mattermost-qt")
if(QT_VERSION_MAJOR EQUAL 6)
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")
else()
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt5sql5-sqlite")
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

if(WIN32)
	set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PRODUCT_NAME}")
	set(CPACK_NSIS_DISPLAY_NAME "${PRODUCT_NAME}")
	set(CPACK_NSIS_PACKAGE_NAME "${PRODUCT_NAME}")
	set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()

include(CPack)
