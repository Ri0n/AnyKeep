cmake_minimum_required(VERSION 3.14.0)
include(AnyKeepMacro)
project(installer VERSION ${ANYKEEP_VERSION})

# windeployqt() is normally called while configuring src/plugins and discovers the tool next to qmake.  Keep this
# fallback for unusual toolchain layouts.
if(NOT WINDEPLOYQT_EXECUTABLE)
  find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt.exe windeployqt)
endif()
if(NOT WINDEPLOYQT_EXECUTABLE)
  message(WARNING "windeployqt not found! Windows update and installer deployment targets won't be available.")
endif()

set(ANYKEEP_UPDATE_OUTPUT_DIR
    ""
    CACHE PATH "Optional output directory for generated Windows update artifacts")
if(ANYKEEP_UPDATE_OUTPUT_DIR STREQUAL "")
  set(_anykeep_update_output_dir "${CMAKE_BINARY_DIR}/updates/${ANYKEEP_UPDATE_CHANNEL}")
else()
  set(_anykeep_update_output_dir "${ANYKEEP_UPDATE_OUTPUT_DIR}")
endif()

set(_anykeep_update_work_dir "${CMAKE_BINARY_DIR}/update-work/${ANYKEEP_UPDATE_CHANNEL}")
set(_anykeep_update_pack_script "${CMAKE_CURRENT_SOURCE_DIR}/make-update-package.cmake.in")

set(_anykeep_install_config_args)
if(CMAKE_CONFIGURATION_TYPES)
  list(APPEND _anykeep_install_config_args --config "$<CONFIG>")
elseif(CMAKE_BUILD_TYPE)
  list(APPEND _anykeep_install_config_args --config "${CMAKE_BUILD_TYPE}")
endif()

if(WINDEPLOYQT_EXECUTABLE)
  get_property(ANYKEEP_UPDATE_PLUGIN_TARGETS GLOBAL PROPERTY ANYKEEP_ENABLED_PLUGIN_TARGETS)
  # Third-party FetchContent projects may expose development-only install rules without assigning a component (QCoro is
  # one such dependency). End-user packages must install only our runtime components.
  add_custom_target(
    windows_runtime_install
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_INSTALL_PREFIX}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_INSTALL_PREFIX}"
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" ${_anykeep_install_config_args} --component Executable
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" ${_anykeep_install_config_args} --component Launcher
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" ${_anykeep_install_config_args} --component Libraries
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" ${_anykeep_install_config_args} --component Runtime
    # windeployqt adds QML debugger/profiler plugins when QML debugging is
    # enabled in the build. They are useful to Qt Creator only and are not
    # part of an end-user installation.
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_INSTALL_PREFIX}/qmltooling"
    DEPENDS anykeepexe anykeep_launcher anykeep_updater anykeep ${ANYKEEP_UPDATE_PLUGIN_TARGETS}
    COMMENT "Installing the AnyKeep Windows runtime tree"
    VERBATIM)
  set_target_properties(windows_runtime_install PROPERTIES EXCLUDE_FROM_ALL TRUE)

  # Qt 6.7 introduced qt_add_translations(), which exposes translations_lrelease instead of this project's legacy
  # build_translations target. The MSI packages the generated .qm files, so the runtime install must wait for either
  # spelling.
  if(TARGET translations_lrelease)
    add_dependencies(windows_runtime_install translations_lrelease)
  elseif(TARGET build_translations)
    add_dependencies(windows_runtime_install build_translations)
  endif()
endif()

find_program(WIX_EXECUTABLE wix.exe)
if(NOT WIX_EXECUTABLE)
  message(WARNING "WiX not found! Installer targets won't be built.")
endif()
if(NOT WIX_EXECUTABLE OR NOT WINDEPLOYQT_EXECUTABLE)
  return()
endif()

set(WIX_TEMPLATE "${CMAKE_SOURCE_DIR}/wix/installer.wxs.in")
set(WIX_BUNDLE_TEMPLATE "${CMAKE_SOURCE_DIR}/wix/AnyKeepInstaller.wixbundle.in")
set(WIX_OUTPUT "${CMAKE_BINARY_DIR}/installer.wxs")
set(WIX_BUNDLE_OUTPUT "${CMAKE_BINARY_DIR}/AnyKeepInstaller.wixbundle")
set(ANYKEEP_MSI ${CMAKE_BINARY_DIR}/AnyKeep.msi)

set(ANYKEEP_INSTALLER_EXE AnyKeep.Installer-${ANYKEEP_VERSION}.exe)
set(ANYKEEP_INSTALLER_EXE_PATH ${CMAKE_BINARY_DIR}/${ANYKEEP_INSTALLER_EXE})

set(LICENSE_TXT "${CMAKE_SOURCE_DIR}/license.txt")
set(LICENSE_RTF "GPLv3.rtf")
set(LOGO_FILE "${CMAKE_SOURCE_DIR}/src/win/pen64.ico")

configure_file(gen-wix.cmake.in gen-wix.cmake @ONLY)

add_custom_command(
  OUTPUT ${WIX_BUNDLE_OUTPUT} ${WIX_OUTPUT} ${LICENSE_RTF}
  COMMAND
    ${CMAKE_COMMAND} -DVERSION_FOR_WIX=${VERSION_FOR_WIX} -DANYKEEP_VERSION=${ANYKEEP_VERSION}
    -DUPGRADE_CODE=${UPGRADE_CODE} -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX} -DWIX_TEMPLATE=${WIX_TEMPLATE}
    -DWIX_BUNDLE_TEMPLATE=${WIX_BUNDLE_TEMPLATE} -DWIX_OUTPUT=${WIX_OUTPUT} -DWIX_BUNDLE_OUTPUT=${WIX_BUNDLE_OUTPUT}
    -DLICENSE_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/GPLv3.rtf.in -DLICENSE_TXT=${LICENSE_TXT}
    -DLICENSE_RTF=${LICENSE_RTF} -DLOGO_FILE=${LOGO_FILE} -DANYKEEP_MSI=${ANYKEEP_MSI} -P
    ${CMAKE_CURRENT_BINARY_DIR}/gen-wix.cmake
  COMMENT "Prepare installer files"
  DEPENDS gen-wix.cmake ${WIX_TEMPLATE} ${WIX_BUNDLE_TEMPLATE} windows_runtime_install)
add_custom_target(prepare_install DEPENDS windows_runtime_install ${WIX_OUTPUT} ${WIX_BUNDLE_OUTPUT} ${LICENSE_RTF})
set_target_properties(prepare_install PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_command(
  OUTPUT ${ANYKEEP_MSI}
  COMMAND ${WIX_EXECUTABLE} build ${WIX_OUTPUT} -platform x64 -ext WixToolset.Util.wixext -o ${ANYKEEP_MSI}
  COMMENT "Building MSI package with WiX"
  DEPENDS ${WIX_OUTPUT})
add_custom_target(package DEPENDS ${ANYKEEP_MSI})
set_target_properties(package PROPERTIES EXCLUDE_FROM_ALL TRUE)

# The MSI is also the self-update payload. Verify that Windows Installer can create an administrative image containing
# the version-owned runtime, then publish that exact MSI next to the immutable/latest manifests. The same script can be
# rerun against a SignPath-produced MSI so hashes are always calculated after signing.
add_custom_target(
  windows_update_package
  COMMAND
    ${CMAKE_COMMAND} "-DPACKAGE_PATH=${ANYKEEP_MSI}" "-DOUTPUT_ROOT=${_anykeep_update_output_dir}"
    "-DWORK_ROOT=${_anykeep_update_work_dir}" "-DVERSION=${ANYKEEP_VERSION}" "-DCHANNEL=${ANYKEEP_UPDATE_CHANNEL}"
    "-DPACKAGE_BASE_URL=${ANYKEEP_UPDATE_BASE_URL}" -P "${_anykeep_update_pack_script}"
  DEPENDS ${ANYKEEP_MSI}
  COMMENT "Verifying and publishing the ${ANYKEEP_UPDATE_CHANNEL} AnyKeep Windows MSI update payload"
  VERBATIM)
set_target_properties(windows_update_package PROPERTIES EXCLUDE_FROM_ALL TRUE)

# don't forget `wix extension add -g WixToolset.BootstrapperApplications.wixext`
add_custom_command(
  OUTPUT ${ANYKEEP_INSTALLER_EXE_PATH}
  COMMAND ${WIX_EXECUTABLE} build ${WIX_BUNDLE_OUTPUT} -ext WixToolset.BootstrapperApplications.wixext -ext
          WixToolset.Util.wixext -o ${ANYKEEP_INSTALLER_EXE_PATH}
  DEPENDS ${WIX_BUNDLE_OUTPUT} ${ANYKEEP_MSI} ${LICENSE_RTF}
  COMMENT "Building Burn Bootstrapper")

add_custom_target(burn_installer DEPENDS ${ANYKEEP_INSTALLER_EXE_PATH} ${LICENSE_TXT} ${WIX_TEMPLATE}
                                         ${WIX_BUNDLE_TEMPLATE})
set_target_properties(burn_installer PROPERTIES EXCLUDE_FROM_ALL TRUE)
