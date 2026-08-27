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
if(ANDROID OR WIN32 OR APPLE)
  set(_anykeep_bundled_iris_default ON)
else()
  set(_anykeep_bundled_iris_default OFF)
endif()
option(ANYKEEP_BUILD_MOBILE "Build the experimental Qt Quick mobile application" ${_anykeep_mobile_default})
option(ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS
       "Allow FetchContent/ExternalProject fallbacks for missing third-party dependencies" ON)
set(ANYKEEP_QCA_SDK_ROOT
    ""
    CACHE PATH "Root of a prebuilt QCA 3 SDK; when set it takes precedence over the bundled QCA fallback")
set(ANYKEEP_IRIS_SDK_ROOT
    ""
    CACHE PATH "Root of a prebuilt Iris SDK; when set it takes precedence over the bundled Iris fallback")
set(ANYKEEP_QTKEYCHAIN_SDK_ROOT
    ""
    CACHE PATH "Root of a prebuilt QtKeychain SDK; when set it takes precedence over installed/source fallbacks")
option(ANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME
       "Install/package runtime files from ANYKEEP_QCA_SDK_ROOT and ANYKEEP_IRIS_SDK_ROOT" OFF)
option(ANYKEEP_BUILD_BUNDLED_QXMPP "Build bundled QXmpp when a suitable system QXmpp is unavailable"
       ${_anykeep_bundled_qxmpp_default})
option(ANYKEEP_BUILD_BUNDLED_OMEMO_C "Build bundled libomemo-c and protobuf-c for bundled QXmpp"
       ${_anykeep_bundled_omemo_c_default})
option(ANYKEEP_BUILD_BUNDLED_QCA "Build and use bundled QCA 3 instead of a system QCA"
       ${_anykeep_bundled_qca_default})
option(ANYKEEP_BUILD_BUNDLED_IRIS "Build bundled Iris when a suitable system Iris is unavailable"
       ${_anykeep_bundled_iris_default})
set(ANYKEEP_IRIS_SOURCE_DIR
    ""
    CACHE PATH "Local Iris source directory (forces the in-tree Iris build and avoids cloning)")
option(ANYKEEP_BUILD_BUNDLED_QCORO "Build bundled QCoro when a suitable system QCoro is unavailable"
       ${_anykeep_bundled_qcoro_default})
option(ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN "Build bundled QtKeychain when a suitable system QtKeychain is unavailable"
       ${_anykeep_bundled_qtkeychain_default})
set(ANYKEEP_XMPP_BACKEND
    "IRIS"
    CACHE STRING "XMPP implementation used by the Private Notes plugin (QXMPP or IRIS)")
set_property(CACHE ANYKEEP_XMPP_BACKEND PROPERTY STRINGS QXMPP IRIS)
string(TOUPPER "${ANYKEEP_XMPP_BACKEND}" ANYKEEP_XMPP_BACKEND)
if(NOT ANYKEEP_XMPP_BACKEND STREQUAL "QXMPP" AND NOT ANYKEEP_XMPP_BACKEND STREQUAL "IRIS")
  message(FATAL_ERROR "ANYKEEP_XMPP_BACKEND must be QXMPP or IRIS (got '${ANYKEEP_XMPP_BACKEND}')")
endif()
unset(_anykeep_mobile_default)
unset(_anykeep_bundled_qca_default)
unset(_anykeep_bundled_iris_default)
unset(_anykeep_bundled_qcoro_default)
unset(_anykeep_bundled_qtkeychain_default)
unset(_anykeep_bundled_qxmpp_default)
unset(_anykeep_bundled_omemo_c_default)

# Iris unconditionally calls find_package(ZLIB).  Windows does not provide a
# suitable system zlib in the supported toolchain, so make it available through
# the same Conan setup used for OpenSSL.  Keep the requirement scoped to the
# bundled fallback: prebuilt Iris SDKs own their dependency closure.
set(ANYKEEP_CONAN_EXTRA_REQUIRES)
if(WIN32 AND ANYKEEP_XMPP_BACKEND STREQUAL "IRIS" AND ANYKEEP_BUILD_BUNDLED_IRIS
   AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS AND NOT ANYKEEP_IRIS_SDK_ROOT)
  list(APPEND ANYKEEP_CONAN_EXTRA_REQUIRES "zlib/1.3.1")
endif()
anykeep_setup_conan_dependencies()
unset(ANYKEEP_CONAN_EXTRA_REQUIRES)

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

foreach(_anykeep_sdk_root IN ITEMS "${ANYKEEP_QCA_SDK_ROOT}" "${ANYKEEP_IRIS_SDK_ROOT}"
                                   "${ANYKEEP_QTKEYCHAIN_SDK_ROOT}")
  if(_anykeep_sdk_root)
    if(NOT IS_DIRECTORY "${_anykeep_sdk_root}")
      message(FATAL_ERROR "Configured AnyKeep SDK root does not exist: ${_anykeep_sdk_root}")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${_anykeep_sdk_root}")
  endif()
endforeach()
unset(_anykeep_sdk_root)

# QCA is selected exactly once at the top level. QCA 3 is the primary ABI;
# QCA 2 remains a temporary system-only compatibility path. Every AnyKeep
# consumer links the target recorded in ANYKEEP_QCA_TARGET, while Iris sees
# that same Qca3::Qca / Qca::Qca target directly.
set(ANYKEEP_QCA_TARGET "")
set(ANYKEEP_QCA_MAJOR "")
set(ANYKEEP_QCA_IS_BUNDLED FALSE)
set(_anykeep_qca_native_target "")

if(ANYKEEP_QCA_SDK_ROOT)
  set(_anykeep_qca_package_dir "${ANYKEEP_QCA_SDK_ROOT}/lib/cmake/Qca3-qt6")
  if(NOT EXISTS "${_anykeep_qca_package_dir}/Qca3-qt6Config.cmake")
    message(FATAL_ERROR "QCA SDK CMake package missing: ${_anykeep_qca_package_dir}")
  endif()
  # Cross toolchains such as Android normally re-root package searches. An
  # explicit SDK root is already a target SDK, so never apply the sysroot to it.
  find_package(Qca3-qt6 CONFIG REQUIRED
               PATHS "${_anykeep_qca_package_dir}"
               NO_DEFAULT_PATH
               NO_CMAKE_FIND_ROOT_PATH)
  if(NOT TARGET Qca3::Qca)
    message(FATAL_ERROR "QCA SDK did not provide Qca3::Qca: ${ANYKEEP_QCA_SDK_ROOT}")
  endif()
  set(ANYKEEP_QCA_TARGET Qca3::Qca)
  set(ANYKEEP_QCA_MAJOR 3)
  set(_anykeep_qca_native_target Qca3::Qca)
  message(STATUS "QCA: using prebuilt QCA 3 SDK (${ANYKEEP_QCA_SDK_ROOT})")
  unset(_anykeep_qca_package_dir)
elseif(ANYKEEP_BUILD_BUNDLED_QCA AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
  message(STATUS "QCA: using bundled QCA 3")
  include(BundledQca)
  if(NOT TARGET Qca3::Qca)
    message(FATAL_ERROR "BundledQca.cmake did not provide Qca3::Qca")
  endif()
  set(ANYKEEP_QCA_TARGET Qca3::Qca)
  set(ANYKEEP_QCA_MAJOR 3)
  set(_anykeep_qca_native_target Qca3::Qca)
else()
  if(ANYKEEP_BUILD_BUNDLED_QCA AND NOT ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
    message(STATUS "QCA: bundled fallback requested but source dependency fallbacks are disabled; searching packages")
  endif()
  find_package(Qca3-qt6 CONFIG QUIET)
  if(TARGET Qca3::Qca)
    set(ANYKEEP_QCA_TARGET Qca3::Qca)
    set(ANYKEEP_QCA_MAJOR 3)
    set(_anykeep_qca_native_target Qca3::Qca)
    message(STATUS "QCA: using installed QCA 3 (${ANYKEEP_QCA_TARGET})")
  else()
    # Temporary QCA 2 compatibility. Upstream QCA 2 with Qt 6 installs the
    # Qca-qt6 package and exports the legacy qca-qt6 target. Normalize that
    # package-local name to the canonical Qca::Qca target expected by Iris and
    # the rest of AnyKeep, while retaining the real target for ABI/type checks.
    find_package(Qca-qt6 CONFIG QUIET)
    if(TARGET Qca::Qca)
      set(ANYKEEP_QCA_TARGET Qca::Qca)
      set(ANYKEEP_QCA_MAJOR 2)
      set(_anykeep_qca_native_target Qca::Qca)
      message(STATUS "QCA: using installed QCA 2 (${ANYKEEP_QCA_TARGET})")
    elseif(TARGET qca-qt6)
      add_library(Qca::Qca ALIAS qca-qt6)
      set(ANYKEEP_QCA_TARGET Qca::Qca)
      set(ANYKEEP_QCA_MAJOR 2)
      set(_anykeep_qca_native_target qca-qt6)
      message(STATUS "QCA: using installed QCA 2 (qca-qt6, exposed as ${ANYKEEP_QCA_TARGET})")
    endif()
  endif()
endif()

if(NOT ANYKEEP_QCA_TARGET)
  message(FATAL_ERROR
          "No usable QCA found. Provide ANYKEEP_QCA_SDK_ROOT, install QCA 3 (Qca3-qt6, preferred), "
          "provide QCA 2, or enable the bundled QCA source fallback")
endif()
if(NOT TARGET ${ANYKEEP_QCA_TARGET})
  message(FATAL_ERROR "Selected QCA target does not exist: ${ANYKEEP_QCA_TARGET}")
endif()

if(ANYKEEP_XMPP_BACKEND STREQUAL "IRIS")
  # Iris and libanykeep must share one dynamic QCA runtime. Inspect the real
  # package target rather than the QCA 2 compatibility facade above.
  get_target_property(_anykeep_qca_type ${_anykeep_qca_native_target} TYPE)
  get_target_property(_anykeep_qca_definitions ${_anykeep_qca_native_target} INTERFACE_COMPILE_DEFINITIONS)
  if(_anykeep_qca_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR
            "The Iris backend requires shared QCA so AnyKeep and Iris use one QCA instance; "
            "${_anykeep_qca_native_target} is static")
  endif()
  if(_anykeep_qca_definitions)
    list(FIND _anykeep_qca_definitions "QCA_STATIC" _anykeep_qca_static_definition)
    if(NOT _anykeep_qca_static_definition EQUAL -1)
      message(FATAL_ERROR
              "The Iris backend requires shared QCA so AnyKeep and Iris use one QCA instance; "
              "${_anykeep_qca_native_target} exports QCA_STATIC")
    endif()
    unset(_anykeep_qca_static_definition)
  endif()
  unset(_anykeep_qca_type)
  unset(_anykeep_qca_definitions)
endif()

if(ANYKEEP_XMPP_BACKEND STREQUAL "QXMPP")
  # The OMEMO package pulls in its matching QXmpp core package. Searching it first also avoids creating a core-only
  # imported target when OMEMO is absent, so the bundled fallback can define a complete, self-consistent target pair.
  find_package(QXmppOmemoQt6 1.11 QUIET)

  if(TARGET QXmpp::QXmpp AND TARGET QXmpp::Omemo)
    message(STATUS "XMPP backend: system QXmpp with OMEMO")
  elseif(ANYKEEP_BUILD_BUNDLED_QXMPP AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
    message(STATUS "XMPP backend: bundled QXmpp with OMEMO")
    include(BundledQXmpp)
  elseif(ANYKEEP_BUILD_BUNDLED_QXMPP)
    message(STATUS "QXmpp source fallback is disabled; the XMPP plugin will be disabled")
  else()
    message(STATUS "QXmpp 1.11+ with OMEMO not found; the XMPP plugin will be disabled")
  endif()
else()
  # A prebuilt SDK is explicit and takes precedence over source fallbacks. A
  # local Iris source checkout remains available for development when source
  # dependency fallbacks are enabled. System/package Iris is used whenever QCA
  # itself is not a private bundled build.
  if(ANYKEEP_IRIS_SDK_ROOT)
    set(_anykeep_iris_package_dir "${ANYKEEP_IRIS_SDK_ROOT}/lib/cmake/Iris")
    if(NOT EXISTS "${_anykeep_iris_package_dir}/IrisConfig.cmake")
      message(FATAL_ERROR "Iris SDK CMake package missing: ${_anykeep_iris_package_dir}")
    endif()
    find_package(Iris CONFIG REQUIRED
                 PATHS "${_anykeep_iris_package_dir}"
                 NO_DEFAULT_PATH
                 NO_CMAKE_FIND_ROOT_PATH)
    if(NOT TARGET Iris::Iris)
      message(FATAL_ERROR "Iris SDK did not provide Iris::Iris: ${ANYKEEP_IRIS_SDK_ROOT}")
    endif()
    message(STATUS "XMPP backend: prebuilt Iris SDK (${ANYKEEP_IRIS_SDK_ROOT})")
    unset(_anykeep_iris_package_dir)
  elseif(ANYKEEP_IRIS_SOURCE_DIR)
    if(NOT ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
      message(FATAL_ERROR
              "ANYKEEP_IRIS_SOURCE_DIR was set while ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS=OFF")
    endif()
    message(STATUS "XMPP backend: local Iris source (${ANYKEEP_IRIS_SOURCE_DIR})")
    include(BundledIris)
  else()
    if(NOT ANYKEEP_QCA_IS_BUNDLED)
      find_package(Iris CONFIG QUIET)
    endif()

    if(TARGET Iris::Iris)
      if(DEFINED Iris_QCA_MAJOR AND NOT "${Iris_QCA_MAJOR}" STREQUAL "${ANYKEEP_QCA_MAJOR}")
        message(FATAL_ERROR
                "Iris uses QCA ${Iris_QCA_MAJOR}, but AnyKeep selected QCA ${ANYKEEP_QCA_MAJOR}. "
                "Use matching packages/SDKs or build Iris with AnyKeep.")
      endif()
      message(STATUS "XMPP backend: installed Iris (Iris::Iris)")
    elseif(ANYKEEP_BUILD_BUNDLED_IRIS AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
      message(STATUS "XMPP backend: bundled Iris FetchContent")
      include(BundledIris)
    elseif(ANYKEEP_QCA_IS_BUNDLED)
      message(STATUS
              "Installed Iris is not used with bundled QCA and bundled Iris is unavailable; the XMPP plugin will be disabled")
    elseif(ANYKEEP_BUILD_BUNDLED_IRIS)
      message(STATUS "Iris source fallback is disabled; the XMPP plugin will be disabled")
    else()
      message(STATUS "Iris package not found and bundled Iris is disabled; the XMPP plugin will be disabled")
    endif()
  endif()

  if(TARGET Iris::Iris AND DEFINED Iris_QCA_MAJOR AND NOT "${Iris_QCA_MAJOR}" STREQUAL "${ANYKEEP_QCA_MAJOR}")
    message(FATAL_ERROR
            "Iris uses QCA ${Iris_QCA_MAJOR}, but AnyKeep selected QCA ${ANYKEEP_QCA_MAJOR}. "
            "Use matching packages/SDKs or build Iris with AnyKeep.")
  endif()
endif()

if(ANYKEEP_QTKEYCHAIN_SDK_ROOT)
  set(_anykeep_qtkeychain_package_dir "${ANYKEEP_QTKEYCHAIN_SDK_ROOT}/lib/cmake/Qt6Keychain")
  if(NOT EXISTS "${_anykeep_qtkeychain_package_dir}/Qt6KeychainConfig.cmake")
    message(FATAL_ERROR "QtKeychain SDK CMake package missing: ${_anykeep_qtkeychain_package_dir}")
  endif()
  # Android's Qt toolchain normally re-roots package searches. The SDK lives
  # outside that sysroot, so keep this explicit package lookup unrooted.
  find_package(Qt6Keychain CONFIG REQUIRED
               PATHS "${_anykeep_qtkeychain_package_dir}"
               NO_DEFAULT_PATH
               NO_CMAKE_FIND_ROOT_PATH)
  if(NOT TARGET Qt6Keychain::Qt6Keychain)
    message(FATAL_ERROR
            "QtKeychain SDK did not provide Qt6Keychain::Qt6Keychain: ${ANYKEEP_QTKEYCHAIN_SDK_ROOT}")
  endif()
  message(STATUS "QtKeychain: using prebuilt SDK (${ANYKEEP_QTKEYCHAIN_SDK_ROOT})")
  unset(_anykeep_qtkeychain_package_dir)
else()
  find_package(Qt6Keychain QUIET)
  if(TARGET Qt6Keychain::Qt6Keychain)
    message(STATUS "QtKeychain: using installed package")
  elseif(ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
    message(STATUS "System QtKeychain not found; using bundled QtKeychain")
    include(BundledQtKeychain)
  elseif(ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN)
    message(STATUS "QtKeychain source fallback is disabled")
  endif()
endif()

# QSourceHighlite is tiny and MIT-licensed, so keep the pinned source in-tree
# instead of running a configure-time ExternalProject.
add_subdirectory(3rdparty/qsourcehighlite)
if(NOT TARGET QSourceHighlite::QSourceHighlite)
  message(FATAL_ERROR "Vendored QSourceHighlite did not provide QSourceHighlite::QSourceHighlite")
endif()

if(ANYKEEP_XMPP_BACKEND STREQUAL "QXMPP")
  find_package(QCoro6 QUIET COMPONENTS Core)
  if(TARGET QCoro::Core)
    message(STATUS "Using system QCoro")
  elseif(ANYKEEP_BUILD_BUNDLED_QCORO AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
    message(STATUS "System QCoro not found; using bundled QCoro")
    include(BundledQCoro)
  elseif(ANYKEEP_BUILD_BUNDLED_QCORO)
    message(STATUS "QCoro source fallback is disabled; the QXmpp XMPP backend will be disabled")
  else()
    message(STATUS "QCoro not found; the QXmpp XMPP backend will be disabled")
  endif()
endif()

if(ANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME)
  include(PrebuiltSdkRuntime)
  anykeep_install_prebuilt_sdk_runtime()
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

function(anykeep_configure_iris_target target)
  if(TARGET anykeep_bundled_iris)
    add_dependencies(${target} anykeep_bundled_iris)
    set_property(TARGET ${target} APPEND PROPERTY AUTOGEN_TARGET_DEPENDS anykeep_bundled_iris)
  endif()
endfunction()

# Android uses the shared editor, models, storage contracts and settings controllers, but links only anykeep_core.
# Desktop-only window, tray and native integration shells stay in the shared Widgets library.
if(ANYKEEP_BUILD_MOBILE)
  qt_standard_project_setup()
  set(PLUGINSDIR "plugins")
  set(TRANSLATIONSDIR "translations")
  set(TRANSLATIONS_INSTALL_DIR "translations")
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
  set(TRANSLATIONS_INSTALL_DIR "translations")
  set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/anykeep)
else()
  # Install destinations must remain relative so that `cmake --install
  # --prefix …` and DESTDIR-based package builds do not write into /usr.
  set(PLUGINSDIR "${CMAKE_INSTALL_LIBDIR}/${APP_NAME}")
  set(TRANSLATIONSDIR ${CMAKE_INSTALL_FULL_DATADIR}/${APP_NAME})
  set(TRANSLATIONS_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/${APP_NAME}")
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
