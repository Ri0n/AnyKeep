cmake_minimum_required(VERSION 3.16.0)
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

# Microsoft Store packaging is independent of WiX. It reuses the same deployed runtime tree, removes the MSI-only
# launcher/updater helpers, and packages anykeep.exe as a full-trust desktop application. Store identity values are
# assigned by Partner Center after the product name is reserved, so keep them configurable and fail only when the MSIX
# target is explicitly requested.
set(ANYKEEP_MSIX_IDENTITY_NAME
    ""
    CACHE STRING "Microsoft Store Package/Identity/Name from Partner Center")
set(ANYKEEP_MSIX_PUBLISHER
    ""
    CACHE STRING "Microsoft Store Package/Identity/Publisher from Partner Center")
set(ANYKEEP_MSIX_PUBLISHER_DISPLAY_NAME
    ""
    CACHE STRING "Microsoft Store Package/Properties/PublisherDisplayName from Partner Center")
set(ANYKEEP_MSIX_MIN_WINDOWS_VERSION
    "10.0.17763.0"
    CACHE STRING "Minimum Windows version for the MSIX package")
set(ANYKEEP_MSIX_MAX_WINDOWS_VERSION_TESTED
    "10.0.26100.0"
    CACHE STRING "MaxVersionTested for the MSIX package manifest")
# Microsoft recommends copying the retail VCLibs identity from the Extension SDK that ships with the build
# environment. Discover it when available instead of freezing a framework version in the project. Keep cache
# overrides for unusual SDK layouts and reproducible local builds.
set(_anykeep_msix_vclibs_name "Microsoft.VCLibs.140.00.UWPDesktop")
set(_anykeep_msix_vclibs_min_version "14.0.27323.0")
set(_anykeep_msix_vclibs_publisher
    "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US")
set(_anykeep_msix_vclibs_sdk_manifest
    "C:/Program Files (x86)/Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs.Desktop/14.0/SDKManifest.xml")
if(EXISTS "${_anykeep_msix_vclibs_sdk_manifest}")
  file(READ "${_anykeep_msix_vclibs_sdk_manifest}" _anykeep_msix_vclibs_sdk_manifest_text)
  string(
    REGEX MATCH
          "FrameworkIdentity-Retail[ \t\r\n]*=[ \t\r\n]*\"Name[ \t]*=[ \t]*([^,]+),[ \t]*MinVersion[ \t]*=[ \t]*([^,]+),[ \t]*Publisher[ \t]*=[ \t]*'([^']+)'\""
          _anykeep_msix_vclibs_match
          "${_anykeep_msix_vclibs_sdk_manifest_text}")
  if(_anykeep_msix_vclibs_match)
    set(_anykeep_msix_vclibs_name "${CMAKE_MATCH_1}")
    set(_anykeep_msix_vclibs_min_version "${CMAKE_MATCH_2}")
    set(_anykeep_msix_vclibs_publisher "${CMAKE_MATCH_3}")
    foreach(_anykeep_msix_vclibs_value IN ITEMS _anykeep_msix_vclibs_name _anykeep_msix_vclibs_min_version
                                                _anykeep_msix_vclibs_publisher)
      string(STRIP "${${_anykeep_msix_vclibs_value}}" ${_anykeep_msix_vclibs_value})
    endforeach()
    message(STATUS
            "MSIX VCLibs dependency from SDKManifest.xml: ${_anykeep_msix_vclibs_name} ${_anykeep_msix_vclibs_min_version}")
  else()
    message(WARNING "Could not parse FrameworkIdentity-Retail from ${_anykeep_msix_vclibs_sdk_manifest}; using defaults")
  endif()
endif()
set(ANYKEEP_MSIX_VCLIBS_NAME
    "${_anykeep_msix_vclibs_name}"
    CACHE STRING "Visual C++ Desktop Bridge framework package name")
set(ANYKEEP_MSIX_VCLIBS_MIN_VERSION
    "${_anykeep_msix_vclibs_min_version}"
    CACHE STRING "Minimum Visual C++ Desktop Bridge framework package version")
set(ANYKEEP_MSIX_VCLIBS_PUBLISHER
    "${_anykeep_msix_vclibs_publisher}"
    CACHE STRING "Visual C++ Desktop Bridge framework publisher")
unset(_anykeep_msix_vclibs_match)
unset(_anykeep_msix_vclibs_sdk_manifest_text)

set(MAKEAPPX_EXECUTABLE
    ""
    CACHE FILEPATH "Path to MakeAppx.exe from the Windows SDK")
if(MAKEAPPX_EXECUTABLE STREQUAL "")
  find_program(MAKEAPPX_EXECUTABLE NAMES makeappx.exe)
endif()
if(MAKEAPPX_EXECUTABLE STREQUAL "")
  file(GLOB _anykeep_makeappx_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/makeappx.exe")
  if(_anykeep_makeappx_candidates)
    list(SORT _anykeep_makeappx_candidates ORDER DESCENDING)
    list(GET _anykeep_makeappx_candidates 0 MAKEAPPX_EXECUTABLE)
    set(MAKEAPPX_EXECUTABLE
        "${MAKEAPPX_EXECUTABLE}"
        CACHE FILEPATH "Path to MakeAppx.exe from the Windows SDK" FORCE)
  endif()
endif()

if(WINDEPLOYQT_EXECUTABLE AND MAKEAPPX_EXECUTABLE)
  set(ANYKEEP_MSIX_OUTPUT "${CMAKE_BINARY_DIR}/AnyKeep-${ANYKEEP_VERSION}-windows-x86_64.msix")
  add_custom_target(
    msix_package
    COMMAND
      ${CMAKE_COMMAND} "-DRUNTIME_ROOT=${CMAKE_INSTALL_PREFIX}" "-DOUTPUT_PATH=${ANYKEEP_MSIX_OUTPUT}"
      "-DVERSION=${VERSION_FOR_WIX}" "-DIDENTITY_NAME=${ANYKEEP_MSIX_IDENTITY_NAME}"
      "-DPUBLISHER=${ANYKEEP_MSIX_PUBLISHER}"
      "-DPUBLISHER_DISPLAY_NAME=${ANYKEEP_MSIX_PUBLISHER_DISPLAY_NAME}"
      "-DMANIFEST_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/msix/Package.appxmanifest.in"
      "-DASSETS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/msix/Assets" "-DMAKEAPPX_EXECUTABLE=${MAKEAPPX_EXECUTABLE}"
      "-DWORK_ROOT=${CMAKE_BINARY_DIR}/msix-work" "-DMIN_WINDOWS_VERSION=${ANYKEEP_MSIX_MIN_WINDOWS_VERSION}"
      "-DMAX_WINDOWS_VERSION_TESTED=${ANYKEEP_MSIX_MAX_WINDOWS_VERSION_TESTED}"
      "-DVCLIBS_NAME=${ANYKEEP_MSIX_VCLIBS_NAME}" "-DVCLIBS_MIN_VERSION=${ANYKEEP_MSIX_VCLIBS_MIN_VERSION}"
      "-DVCLIBS_PUBLISHER=${ANYKEEP_MSIX_VCLIBS_PUBLISHER}" -P "${CMAKE_CURRENT_SOURCE_DIR}/make-msix.cmake"
    DEPENDS windows_runtime_install ${CMAKE_CURRENT_SOURCE_DIR}/make-msix.cmake
            ${CMAKE_CURRENT_SOURCE_DIR}/msix/Package.appxmanifest.in
            ${CMAKE_CURRENT_SOURCE_DIR}/msix/Assets/Square44x44Logo.png
            ${CMAKE_CURRENT_SOURCE_DIR}/msix/Assets/Square44x44Logo.targetsize-44_altform-unplated.png
            ${CMAKE_CURRENT_SOURCE_DIR}/msix/Assets/Square150x150Logo.png
            ${CMAKE_CURRENT_SOURCE_DIR}/msix/Assets/StoreLogo.png
    COMMENT "Building Microsoft Store MSIX package"
    VERBATIM)
  set_target_properties(msix_package PROPERTIES EXCLUDE_FROM_ALL TRUE)
elseif(WINDEPLOYQT_EXECUTABLE)
  message(WARNING "MakeAppx.exe not found! Microsoft Store MSIX target won't be available.")
endif()

find_program(WIX_EXECUTABLE wix.exe)
if(NOT WIX_EXECUTABLE)
  message(WARNING "WiX not found! Installer targets won't be built.")
endif()
if(NOT WIX_EXECUTABLE OR NOT WINDEPLOYQT_EXECUTABLE)
  return()
endif()

# A direct MSI cannot bootstrap the MSVC runtime itself, so it must reject a fresh install when the machine runtime is
# older than the toolset used to build AnyKeep. Microsoft defines that as the compatibility floor for the v14
# Redistributable. Prefer an explicit cache override, otherwise derive the three-component toolset version from cl.exe.
set(ANYKEEP_REQUIRED_VC_RUNTIME_VERSION
    ""
    CACHE STRING "Minimum x64 Visual C++ v14 Redistributable version required by the Windows MSI")
if(ANYKEEP_REQUIRED_VC_RUNTIME_VERSION STREQUAL "")
  file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" _anykeep_msvc_compiler)
  string(REGEX MATCH "/VC/Tools/MSVC/([0-9]+[.][0-9]+[.][0-9]+)([.][0-9]+)?/" _anykeep_msvc_toolset_match
               "${_anykeep_msvc_compiler}")
  if(CMAKE_MATCH_1)
    set(ANYKEEP_REQUIRED_VC_RUNTIME_VERSION "${CMAKE_MATCH_1}.0")
  elseif(CMAKE_VS_PLATFORM_TOOLSET_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
    set(ANYKEEP_REQUIRED_VC_RUNTIME_VERSION "${CMAKE_VS_PLATFORM_TOOLSET_VERSION}.0")
  else()
    message(FATAL_ERROR "Cannot derive the MSVC toolset version from '${CMAKE_CXX_COMPILER}'. "
                        "Set ANYKEEP_REQUIRED_VC_RUNTIME_VERSION explicitly, for example 14.44.35207.0.")
  endif()
endif()

if(NOT ANYKEEP_REQUIRED_VC_RUNTIME_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)[.]([0-9]+)$")
  message(FATAL_ERROR "ANYKEEP_REQUIRED_VC_RUNTIME_VERSION must have four numeric components, got "
                      "'${ANYKEEP_REQUIRED_VC_RUNTIME_VERSION}'")
endif()
set(_anykeep_vc_runtime_major "${CMAKE_MATCH_1}")
set(_anykeep_vc_runtime_minor "${CMAKE_MATCH_2}")
set(_anykeep_vc_runtime_build "${CMAKE_MATCH_3}")
if(_anykeep_vc_runtime_build LESS 1)
  message(FATAL_ERROR "Unsupported MSVC runtime version '${ANYKEEP_REQUIRED_VC_RUNTIME_VERSION}'")
endif()
math(EXPR _anykeep_vc_runtime_previous_build "${_anykeep_vc_runtime_build} - 1")
# Windows Installer also compares file language when FileSearch finds exactly MinVersion. Search one file-version tick
# below the real floor so every acceptable vcruntime140.dll is strictly newer and the language field is irrelevant.
set(ANYKEEP_VC_RUNTIME_FILE_SEARCH_MIN_VERSION
    "${_anykeep_vc_runtime_major}.${_anykeep_vc_runtime_minor}.${_anykeep_vc_runtime_previous_build}.65535")
message(STATUS "AnyKeep minimum Visual C++ v14 runtime: ${ANYKEEP_REQUIRED_VC_RUNTIME_VERSION}")

set(UPGRADE_CODE "E12D6C7A-8F57-479B-8AE9-49C411B82843")

set(WIX_TEMPLATE "${CMAKE_SOURCE_DIR}/wix/installer.wxs.in")
set(WIX_BUNDLE_TEMPLATE "${CMAKE_SOURCE_DIR}/wix/AnyKeepInstaller.wixbundle.in")
set(WIX_OUTPUT "${CMAKE_BINARY_DIR}/installer.wxs")
set(ANYKEEP_MSI ${CMAKE_BINARY_DIR}/AnyKeep.msi)

set(ANYKEEP_INSTALLER_EXE AnyKeep.Installer-${ANYKEEP_VERSION}.exe)
set(ANYKEEP_INSTALLER_EXE_PATH ${CMAKE_BINARY_DIR}/${ANYKEEP_INSTALLER_EXE})

set(LICENSE_TXT "${CMAKE_SOURCE_DIR}/license.txt")
set(LOGO_FILE "${CMAKE_SOURCE_DIR}/src/win/pen64.ico")

configure_file(gen-wix.cmake.in gen-wix.cmake @ONLY)

add_custom_command(
  OUTPUT ${WIX_OUTPUT}
  COMMAND
    ${CMAKE_COMMAND} -DVERSION_FOR_WIX=${VERSION_FOR_WIX} -DANYKEEP_VERSION=${ANYKEEP_VERSION}
    -DUPGRADE_CODE=${UPGRADE_CODE} -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX} -DWIX_TEMPLATE=${WIX_TEMPLATE}
    -DWIX_OUTPUT=${WIX_OUTPUT} -DLOGO_FILE=${LOGO_FILE} -DANYKEEP_MSI=${ANYKEEP_MSI}
    -DANYKEEP_REQUIRED_VC_RUNTIME_VERSION=${ANYKEEP_REQUIRED_VC_RUNTIME_VERSION}
    -DANYKEEP_VC_RUNTIME_FILE_SEARCH_MIN_VERSION=${ANYKEEP_VC_RUNTIME_FILE_SEARCH_MIN_VERSION} -P
    ${CMAKE_CURRENT_BINARY_DIR}/gen-wix.cmake
  COMMENT "Prepare installer files"
  DEPENDS gen-wix.cmake ${WIX_TEMPLATE} windows_runtime_install)
add_custom_target(prepare_install DEPENDS windows_runtime_install ${WIX_OUTPUT})
set_target_properties(prepare_install PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_command(
  OUTPUT ${ANYKEEP_MSI}
  COMMAND ${WIX_EXECUTABLE} build ${WIX_OUTPUT} -platform x64 -ext WixToolset.Util.wixext -o ${ANYKEEP_MSI}
  COMMENT "Building MSI package with WiX"
  DEPENDS ${WIX_OUTPUT})
add_custom_target(package DEPENDS ${ANYKEEP_MSI})
set_target_properties(package PROPERTIES EXCLUDE_FROM_ALL TRUE)

# The MSI is also the self-update payload. Verify that Windows Installer can create an administrative image containing
# the version-owned runtime, then publish those exact MSI bytes next to the immutable/latest manifests. For a signed
# direct-download release, rerun the script after signing so the manifest hashes the final bytes users receive.
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

# Burn is intentionally generated only after the MSI exists. The standalone packaging script also accepts an external
# MSI, so a future signing/publishing job does not need a Qt build tree. It follows Microsoft's
# rolling vc14 permalink to the concrete HTTPS CDN object and asks WiX to generate SHA-512/size/version metadata from
# those exact downloaded bytes.
add_custom_target(
  burn_installer
  COMMAND
    ${CMAKE_COMMAND} "-DPACKAGE_PATH=${ANYKEEP_MSI}" "-DOUTPUT_PATH=${ANYKEEP_INSTALLER_EXE_PATH}"
    "-DVERSION=${ANYKEEP_VERSION}" "-DREQUIRED_VC_RUNTIME_VERSION=${ANYKEEP_REQUIRED_VC_RUNTIME_VERSION}"
    "-DUPGRADE_CODE=${UPGRADE_CODE}" "-DBUNDLE_TEMPLATE=${WIX_BUNDLE_TEMPLATE}" "-DLICENSE_TXT=${LICENSE_TXT}"
    "-DLICENSE_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/GPLv3.rtf.in" "-DLOGO_FILE=${LOGO_FILE}"
    "-DWORK_ROOT=${CMAKE_BINARY_DIR}/burn-work" -P "${CMAKE_CURRENT_SOURCE_DIR}/make-burn-installer.cmake"
  DEPENDS package ${CMAKE_CURRENT_SOURCE_DIR}/make-burn-installer.cmake ${WIX_BUNDLE_TEMPLATE}
          ${CMAKE_CURRENT_SOURCE_DIR}/GPLv3.rtf.in ${LICENSE_TXT} ${LOGO_FILE}
  COMMENT "Resolving Visual C++ runtime payload and building Burn Bootstrapper"
  VERBATIM)
set_target_properties(burn_installer PROPERTIES EXCLUDE_FROM_ALL TRUE)
