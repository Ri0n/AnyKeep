# -------------------------------------------------
# QtNote - Simple note-taking application
# Copyright (C) 2020 Sergei Ilinykh
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
# Contacts:
# E-Mail: rion4ik@gmail.com XMPP: rion@jabber.ru
# -------------------------------------------------

cmake_minimum_required(VERSION 3.25.0)
set(CMAKE_CXX_STANDARD 20)

set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
include(QtNoteMacro)
set(APP_NAME qtnote)
# Public desktop editor/manager classes were replaced during the shared QML
# editor migration. Keep the product version independent, but make the ABI
# break explicit in the libqtnote soname.
set(QTNOTE_ABI_VERSION 3 CACHE STRING "libqtnote ABI version")
message(STATUS "Version: ${APP_NAME}-${QTNOTE_VERSION}")
message(STATUS "libqtnote ABI version: ${QTNOTE_ABI_VERSION}")
project(${APP_NAME} VERSION ${QTNOTE_VERSION} LANGUAGES CXX)

include(CTest)

option(QTNOTE_DEVEL "Enable development mode (adjusts some paths and output)" OFF)
option(QTNOTE_BUILD_BUNDLED_QXMPP "Build bundled QXmpp when a suitable system QXmpp is unavailable" OFF)
option(QTNOTE_ENABLE_X11 "Build QtNote components that use X11 directly" ON)

if(ANDROID)
    set(_qtnote_mobile_default ON)
    set(_qtnote_bundled_qca_default ON)
    set(_qtnote_bundled_qtkeychain_default ON)
else()
    set(_qtnote_mobile_default OFF)
    set(_qtnote_bundled_qca_default OFF)
    set(_qtnote_bundled_qtkeychain_default OFF)
endif()
option(QTNOTE_BUILD_MOBILE "Build the experimental Qt Quick mobile application" ${_qtnote_mobile_default})
option(QTNOTE_BUILD_BUNDLED_QCA "Build bundled QCA when a suitable system QCA is unavailable" ${_qtnote_bundled_qca_default})
option(QTNOTE_BUILD_BUNDLED_QTKEYCHAIN "Build bundled QtKeychain when a suitable system QtKeychain is unavailable" ${_qtnote_bundled_qtkeychain_default})
unset(_qtnote_mobile_default)
unset(_qtnote_bundled_qca_default)
unset(_qtnote_bundled_qtkeychain_default)

if("${QT_VERSION_MAJOR}" STREQUAL "")
    set(QT_VERSION_MAJOR 6)
endif()
if(NOT QT_VERSION_MAJOR EQUAL 6)
    message(FATAL_ERROR "The shared Qt Quick editor and notes manager require Qt 6.4 or newer")
endif()

if(QTNOTE_BUILD_MOBILE)
    if(ANDROID)
        find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2)
    else()
        find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2)
    endif()
else()
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Core Gui PrintSupport Quick QuickWidgets Qml QuickControls2)
endif()
set(QT_VERSION_MAJOR 6)
set(QT_DEFAULT_MAJOR_VERSION ${QT_VERSION_MAJOR} CACHE STRING "Default Qt major version" FORCE)
include(GNUInstallDirs)

# Android uses the shared editor, models, storage contracts and settings
# controllers, but links only qtnote_core. Desktop-only window, tray and native
# integration shells stay in the shared Widgets library.
if(QTNOTE_BUILD_MOBILE)
    qt_standard_project_setup()
    set(PLUGINSDIR "plugins")
    set(TRANSLATIONSDIR "translations")
    configure_file(qtnote_config.h.in qtnote_config.h)
    if(QTNOTE_BUILD_BUNDLED_QCA)
        include(BundledQca)
    endif()
    if(QTNOTE_BUILD_BUNDLED_QTKEYCHAIN)
        include(BundledQtKeychain)
    endif()
    set(QTNOTE_CORE_ONLY ON)
    add_subdirectory(libqtnote)
    add_subdirectory(src/mobile)
    return()
endif()

# The OMEMO package pulls in its matching QXmpp core package. Searching it
# first also avoids creating a core-only imported target when OMEMO is absent,
# so the bundled fallback can define a complete, self-consistent target pair.
find_package(QXmppOmemoQt${QT_VERSION_MAJOR} 1.11 QUIET)

if(TARGET QXmpp::QXmpp AND TARGET QXmpp::Omemo)
    message(STATUS "Using system QXmpp with OMEMO")
elseif(QTNOTE_BUILD_BUNDLED_QXMPP)
    message(STATUS "System QXmpp 1.11+ with OMEMO not found; using bundled QXmpp")
    include(BundledQXmpp)
else()
    message(STATUS "QXmpp 1.11+ with OMEMO not found; the XMPP plugin will be disabled")
endif()

function(qtnote_configure_qxmpp_target target)
    if(TARGET qtnote_bundled_qxmpp)
        # Older CMake/Qt AUTOGEN implementations may turn an imported static
        # library into a file prerequisite without carrying over the imported
        # target's ExternalProject dependency. Make the ordering explicit for
        # both the consumer and its generated MOC/UIC timestamp target.
        add_dependencies(${target} qtnote_bundled_qxmpp)
        set_property(TARGET ${target} APPEND PROPERTY AUTOGEN_TARGET_DEPENDS qtnote_bundled_qxmpp)
    endif()
endfunction()

if(UNIX AND NOT (APPLE OR HAIKU OR ANDROID))
    set(UNIXLIKE ON)
endif()

set(I18N_TRANSLATED_LANGUAGES)
file(GLOB qtnote_TRS "${CMAKE_CURRENT_SOURCE_DIR}/qtnote_*.ts")
foreach(_LANG ${qtnote_TRS})
    get_filename_component(_SHORT_LANG ${_LANG} NAME_WE)
    string(REPLACE "@" "_" _SHORT_LANG ${_SHORT_LANG})
    list(APPEND I18N_TRANSLATED_LANGUAGES ${_SHORT_LANG})
endforeach()
if(Qt${QT_VERSION_MAJOR}Core_VERSION VERSION_LESS 6.3.0)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    set(USE_FOLDERS ON)
elseif(Qt${QT_VERSION_MAJOR}Core_VERSION VERSION_LESS 6.7.0)
    qt_standard_project_setup()
else()
    qt_standard_project_setup(
        I18N_TRANSLATED_LANGUAGES ${I18N_TRANSLATED_LANGUAGES}
        I18N_SOURCE_LANGUAGE en
    )
endif()

if(WIN32)
    #set(CMAKE_INSTALL_BINDIR ".") # Place executables in the root directory for Windows
    if(CMAKE_RUNTIME_OUTPUT_DIRECTORY STREQUAL "")
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()
    #message(STATUS "CMAKE_RUNTIME_OUTPUT_DIRECTORY=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/install")
    set(PLUGINSDIR "plugins")
    set(TRANSLATIONSDIR "translations")
    set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/qtnote)
else()
    set(PLUGINSDIR "${CMAKE_INSTALL_FULL_LIBDIR}/${APP_NAME}")
    set(TRANSLATIONSDIR ${CMAKE_INSTALL_FULL_DATADIR}/${APP_NAME})
endif()

message(STATUS "CMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}")

configure_file(qtnote_config.h.in qtnote_config.h)

if(QTNOTE_DEVEL)
    add_definitions(-DQTNOTE_DEVEL)
endif()

add_subdirectory(3rdparty)
add_subdirectory(libqtnote)
add_subdirectory(src)
add_subdirectory(plugins)
add_subdirectory(translations)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(WIN32)
    add_subdirectory(wix)
endif()
