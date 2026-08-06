cmake_minimum_required(VERSION 3.14.0)
include(AnyKeepMacro)
project(installer VERSION ${ANYKEEP_VERSION})

find_program(WINDEPLOYQT_EXECUTABLE windeployqt)
if(NOT WINDEPLOYQT_EXECUTABLE)
    message(WARNING "windeployqt not found! Dependencies for standalone deployment will not be gathered.")
endif()

if(WINDEPLOYQT_EXECUTABLE)
    get_property(ANYKEEP_UPDATE_PLUGIN_TARGETS GLOBAL PROPERTY ANYKEEP_ENABLED_PLUGIN_TARGETS)
    configure_file(make-update-package.cmake.in make-update-package.cmake @ONLY)
    add_custom_target(windows_update_package
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_INSTALL_PREFIX}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_INSTALL_PREFIX}
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG>
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/make-update-package.cmake
        DEPENDS anykeepexe anykeep_launcher anykeep_updater anykeep ${ANYKEEP_UPDATE_PLUGIN_TARGETS}
        COMMENT "Building versioned AnyKeep Windows update archive and manifest"
    )
    if(TARGET build_translations)
        add_dependencies(windows_update_package build_translations)
    endif()
    set_target_properties(windows_update_package PROPERTIES EXCLUDE_FROM_ALL TRUE)
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
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FOR_WIX=${VERSION_FOR_WIX}
        -DANYKEEP_VERSION=${ANYKEEP_VERSION}
        -DUPGRADE_CODE=${UPGRADE_CODE}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
        -DWIX_TEMPLATE=${WIX_TEMPLATE}
        -DWIX_BUNDLE_TEMPLATE=${WIX_BUNDLE_TEMPLATE}
        -DWIX_OUTPUT=${WIX_OUTPUT}
        -DWIX_BUNDLE_OUTPUT=${WIX_BUNDLE_OUTPUT}
        -DLICENSE_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/GPLv3.rtf.in
        -DLICENSE_TXT=${LICENSE_TXT}
        -DLICENSE_RTF=${LICENSE_RTF}
        -DLOGO_FILE=${LOGO_FILE}
        -DANYKEEP_MSI=${ANYKEEP_MSI}
        -P ${CMAKE_CURRENT_BINARY_DIR}/gen-wix.cmake
    COMMENT "Prepare installer files"
    DEPENDS gen-wix.cmake
)
add_custom_target(prepare_install
    DEPENDS ${WIX_OUTPUT} ${WIX_BUNDLE_OUTPUT} ${LICENSE_RTF}
)
set_target_properties(prepare_install PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_command(
    OUTPUT ${ANYKEEP_MSI}
    COMMAND ${WIX_EXECUTABLE} build ${WIX_OUTPUT} -platform x64 -ext WixToolset.Util.wixext -o ${ANYKEEP_MSI}
    COMMENT "Building MSI package with WiX"
    DEPENDS ${WIX_OUTPUT}
)
add_custom_target(package
    DEPENDS ${ANYKEEP_MSI}
)
set_target_properties(package PROPERTIES EXCLUDE_FROM_ALL TRUE)

# don't forget `wix extension add -g WixToolset.BootstrapperApplications.wixext`
add_custom_command(
    OUTPUT ${ANYKEEP_INSTALLER_EXE_PATH}
    COMMAND ${WIX_EXECUTABLE}
        build ${WIX_BUNDLE_OUTPUT}
        -ext WixToolset.BootstrapperApplications.wixext
        -ext WixToolset.Util.wixext
        -o ${ANYKEEP_INSTALLER_EXE_PATH}
    DEPENDS ${WIX_BUNDLE_OUTPUT} ${ANYKEEP_MSI} ${LICENSE_RTF}
    COMMENT "Building Burn Bootstrapper"
)

add_custom_target(burn_installer DEPENDS ${ANYKEEP_INSTALLER_EXE_PATH} ${LICENSE_TXT} ${WIX_TEMPLATE} ${WIX_BUNDLE_TEMPLATE})
set_target_properties(burn_installer PROPERTIES EXCLUDE_FROM_ALL TRUE)
