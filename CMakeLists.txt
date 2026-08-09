# -------------------------------------------------
# AnyKeep - Simple note-taking application Copyright (C) 2020 Sergei Ilinykh This program is free software: you can
# redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version. This program is distributed in the
# hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details. You should have received a copy of the
# GNU General Public License along with this program.  If not, see <http://www.gnu.org/licenses/>. Contacts: E-Mail:
# rion4ik@gmail.com XMPP: rion@jabber.ru
# -------------------------------------------------

cmake_minimum_required(VERSION 3.25.0)
set(CMAKE_CXX_STANDARD 20)

set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
include(AnyKeepMacro)
set(APP_NAME anykeep)
# Public desktop editor/manager classes were replaced during the shared QML editor migration. Keep the product version
# independent, but make the ABI break explicit in the libanykeep soname.
set(ANYKEEP_ABI_VERSION
    3
    CACHE STRING "libanykeep ABI version")
message(STATUS "Version: ${APP_NAME}-${ANYKEEP_VERSION}")
message(STATUS "libanykeep ABI version: ${ANYKEEP_ABI_VERSION}")
project(
  ${APP_NAME}
  VERSION ${ANYKEEP_VERSION}
  LANGUAGES C CXX)

include(ConanDependencies)
anykeep_setup_conan_dependencies()

include(CTest)

option(ANYKEEP_DEVEL "Enable development mode (adjusts some paths and output)" OFF)
option(ANYKEEP_ENABLE_X11 "Build AnyKeep components that use X11 directly" ON)
set(ANYKEEP_UPDATE_CHANNEL
    "stable"
    CACHE STRING "Windows self-update channel compiled into unpackaged builds")
set_property(CACHE ANYKEEP_UPDATE_CHANNEL PROPERTY STRINGS stable nightly)
if(NOT ANYKEEP_UPDATE_CHANNEL MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
  message(FATAL_ERROR "ANYKEEP_UPDATE_CHANNEL contains characters that are unsafe in URLs or paths")
endif()

set(ANYKEEP_UPDATE_SERVER_ROOT
    "https://anykeep.net/updates"
    CACHE STRING "Root URL containing Windows self-update channels")
set(ANYKEEP_UPDATE_MANIFEST_URL
    ""
    CACHE STRING "Optional explicit Windows self-update manifest URL; empty derives it from server root and channel")
set(ANYKEEP_UPDATE_BASE_URL
    ""
    CACHE STRING "Optional absolute package URL prefix; empty writes package URLs relative to the manifest")

# Migrate the stage-1 cache defaults.  Without this, changing only ANYKEEP_UPDATE_CHANNEL in an existing Visual
# Studio/CMake build tree would silently keep pointing at the stable channel.
if(ANYKEEP_UPDATE_MANIFEST_URL STREQUAL "https://anykeep.net/updates/stable/windows-x86_64.json")
  set(ANYKEEP_UPDATE_MANIFEST_URL
      ""
      CACHE STRING "Optional explicit Windows self-update manifest URL; empty derives it from server root and channel"
            FORCE)
endif()
if(ANYKEEP_UPDATE_BASE_URL STREQUAL "https://anykeep.net/updates/stable")
  set(ANYKEEP_UPDATE_BASE_URL
      ""
      CACHE STRING "Optional absolute package URL prefix; empty writes package URLs relative to the manifest" FORCE)
endif()

if(ANYKEEP_UPDATE_MANIFEST_URL STREQUAL "")
  string(REGEX REPLACE "/+$" "" _anykeep_update_server_root "${ANYKEEP_UPDATE_SERVER_ROOT}")
  set(ANYKEEP_UPDATE_MANIFEST_URL "${_anykeep_update_server_root}/${ANYKEEP_UPDATE_CHANNEL}/windows-x86_64.json")
  unset(_anykeep_update_server_root)
endif()

if(ANDROID)
  set(_anykeep_mobile_default ON)
else()
  set(_anykeep_mobile_default OFF)
endif()

if(ANDROID OR WIN32)
  set(_anykeep_bundled_qca_default ON)
  set(_anykeep_bundled_qcoro_default ON)
  set(_anykeep_bundled_qtkeychain_default ON)
  set(_anykeep_bundled_qxmpp_default ON)
  set(_anykeep_bundled_omemo_c_default ON)
else()
  set(_anykeep_bundled_qca_default OFF)
  set(_anykeep_bundled_qcoro_default OFF)
  set(_anykeep_bundled_qtkeychain_default OFF)
  set(_anykeep_bundled_qxmpp_default OFF)
  set(_anykeep_bundled_omemo_c_default OFF)
endif()
option(ANYKEEP_BUILD_MOBILE "Build the experimental Qt Quick mobile application" ${_anykeep_mobile_default})
option(ANYKEEP_BUILD_BUNDLED_QXMPP "Build bundled QXmpp when a suitable system QXmpp is unavailable"
       ${_anykeep_bundled_qxmpp_default})
option(ANYKEEP_BUILD_BUNDLED_OMEMO_C "Build bundled libomemo-c and protobuf-c for bundled QXmpp"
       ${_anykeep_bundled_omemo_c_default})
option(ANYKEEP_BUILD_BUNDLED_QCA "Build bundled QCA when a suitable system QCA is unavailable"
       ${_anykeep_bundled_qca_default})
option(ANYKEEP_BUILD_BUNDLED_QCORO "Build bundled QCoro when a suitable system QCoro is unavailable"
       ${_anykeep_bundled_qcoro_default})
option(ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN "Build bundled QtKeychain when a suitable system QtKeychain is unavailable"
       ${_anykeep_bundled_qtkeychain_default})
unset(_anykeep_mobile_default)
unset(_anykeep_bundled_qca_default)
unset(_anykeep_bundled_qcoro_default)
unset(_anykeep_bundled_qtkeychain_default)
unset(_anykeep_bundled_qxmpp_default)
unset(_anykeep_bundled_omemo_c_default)

set(QT_COMPONENTS
    Core
    Gui
    Network
    Qml
    Quick
    QuickControls2
    Svg)
if(QT_SINGLE_APPLICATION_WIDGET)
  list(APPEND QT_COMPONENTS Widgets)
endif()
if(ANYKEEP_BUILD_MOBILE)
  find_package(Qt6 6.11 REQUIRED ${QT_COMPONENTS} COMPONENTS Xml)
else()
  find_package(Qt6 6.4 REQUIRED COMPONENTS ${QT_COMPONENTS} Widgets PrintSupport QuickWidgets)
endif()
include(GNUInstallDirs)

# The OMEMO package pulls in its matching QXmpp core package. Searching it first also avoids creating a core-only
# imported target when OMEMO is absent, so the bundled fallback can define a complete, self-consistent target pair.
find_package(QXmppOmemoQt6 1.11 QUIET)

if(TARGET QXmpp::QXmpp AND TARGET QXmpp::Omemo)
  message(STATUS "Using system QXmpp with OMEMO")
elseif(ANYKEEP_BUILD_BUNDLED_QXMPP)
  message(STATUS "System QXmpp 1.11+ with OMEMO not found; using bundled QXmpp")
  include(BundledQXmpp)
else()
  message(STATUS "QXmpp 1.11+ with OMEMO not found; the XMPP plugin will be disabled")
endif()

find_package(Qca-qt6 QUIET)
if(NOT TARGET qca-qt6 AND ANYKEEP_BUILD_BUNDLED_QCA)
  message(STATUS "System QCA not found; using bundled QCA")
  include(BundledQca)
endif()

find_package(Qt6Keychain QUIET)
if(NOT TARGET Qt6Keychain::Qt6Keychain AND ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN)
  message(STATUS "System QtKeychain not found; using bundled QtKeychain")
  include(BundledQtKeychain)
endif()

find_package(QSourceHighlite CONFIG QUIET NAMES QSourceHighlite qsourcehighlite)
if(TARGET qsourcehighlite::qsourcehighlite AND NOT TARGET QSourceHighlite::QSourceHighlite)
  add_library(QSourceHighlite::QSourceHighlite ALIAS qsourcehighlite::qsourcehighlite)
elseif(TARGET QSourceHighlite::qsourcehighlite AND NOT TARGET QSourceHighlite::QSourceHighlite)
  add_library(QSourceHighlite::QSourceHighlite ALIAS QSourceHighlite::qsourcehighlite)
elseif(TARGET QSourceHighlite AND NOT TARGET QSourceHighlite::QSourceHighlite)
  add_library(QSourceHighlite::QSourceHighlite ALIAS QSourceHighlite)
endif()
if(TARGET QSourceHighlite::QSourceHighlite)
  message(STATUS "Using QSourceHighlite from the configured package manager")
else()
  message(STATUS "QSourceHighlite package not found; using pinned ExternalProject fallback")
  include(BundledQSourceHighlite)
endif()

find_package(QCoro6 QUIET COMPONENTS Core)
if(TARGET QCoro::Core)
  message(STATUS "Using system QCoro")
elseif(ANYKEEP_BUILD_BUNDLED_QCORO)
  message(STATUS "System QCoro not found; using bundled QCoro")
  include(BundledQCoro)
else()
  message(STATUS "QCoro not found; the XMPP plugin will be disabled")
endif()

function(anykeep_configure_qxmpp_target target)
  if(TARGET anykeep_bundled_qxmpp)
    # Older CMake/Qt AUTOGEN implementations may turn an imported static library into a file prerequisite without
    # carrying over the imported target's ExternalProject dependency. Make the ordering explicit for both the consumer
    # and its generated MOC/UIC timestamp target.
    add_dependencies(${target} anykeep_bundled_qxmpp)
    set_property(TARGET ${target} APPEND PROPERTY AUTOGEN_TARGET_DEPENDS anykeep_bundled_qxmpp)
  endif()
endfunction()

# Android uses the shared editor, models, storage contracts and settings controllers, but links only anykeep_core.
# Desktop-only window, tray and native integration shells stay in the shared Widgets library.
if(ANYKEEP_BUILD_MOBILE)
  qt_standard_project_setup()
  set(PLUGINSDIR "plugins")
  set(TRANSLATIONSDIR "translations")
  configure_file(anykeep_config.h.in anykeep_config.h)
  set(ANYKEEP_CORE_ONLY ON)
  add_subdirectory(libanykeep)
  # Android plugins are built as bundled static libraries. Defining them before anykeep_mobile lets the application link
  # each plugin as one unit, including its resources, instead of recompiling plugin sources in the APK.
  add_subdirectory(plugins)
  add_subdirectory(src/mobile)
  return()
endif()

if(UNIX
   AND NOT
       (APPLE
        OR HAIKU
        OR ANDROID))
  set(UNIXLIKE ON)
endif()

set(I18N_TRANSLATED_LANGUAGES)
file(GLOB anykeep_TRS "${CMAKE_CURRENT_SOURCE_DIR}/anykeep_*.ts")
foreach(_LANG ${anykeep_TRS})
  get_filename_component(_SHORT_LANG ${_LANG} NAME_WE)
  string(REPLACE "@" "_" _SHORT_LANG ${_SHORT_LANG})
  list(APPEND I18N_TRANSLATED_LANGUAGES ${_SHORT_LANG})
endforeach()
if(Qt6Core_VERSION VERSION_LESS 6.3.0)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)
  set(CMAKE_AUTOUIC ON)
  set(USE_FOLDERS ON)
elseif(Qt6Core_VERSION VERSION_LESS 6.7.0)
  qt_standard_project_setup()
else()
  qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES ${I18N_TRANSLATED_LANGUAGES} I18N_SOURCE_LANGUAGE en)
endif()

if(WIN32)
  # set(CMAKE_INSTALL_BINDIR ".") # Place executables in the root directory for Windows
  if(CMAKE_RUNTIME_OUTPUT_DIRECTORY STREQUAL "")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  endif()
  # message(STATUS "CMAKE_RUNTIME_OUTPUT_DIRECTORY=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/install")
  set(PLUGINSDIR "plugins")
  set(TRANSLATIONSDIR "translations")
  set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/anykeep)
else()
  set(PLUGINSDIR "${CMAKE_INSTALL_FULL_LIBDIR}/${APP_NAME}")
  set(TRANSLATIONSDIR ${CMAKE_INSTALL_FULL_DATADIR}/${APP_NAME})
endif()

message(STATUS "CMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}")

configure_file(anykeep_config.h.in anykeep_config.h)

if(ANYKEEP_DEVEL)
  add_definitions(-DANYKEEP_DEVEL)
endif()

add_subdirectory(3rdparty)
add_subdirectory(libanykeep)
add_subdirectory(src)
add_subdirectory(plugins)
add_subdirectory(translations)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(WIN32)
  add_subdirectory(wix)
endif()
