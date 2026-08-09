include(AnyKeepMacro)

set(kdeintegration_sources
    kdeintegration.cpp
    kdeintegration.h
    kdeintegration.qrc
    kdeintegrationtray.cpp
    kdeintegrationtray.h
    sonnetspellcheckprovider.cpp
    sonnetspellcheckprovider.h
    kwinscript/metadata.json
    kwinscript/contents/code/main.js)

# Avoid probing Linux-only KDE and Plasma packages on other platforms.  Still register the disabled placeholder target
# so configuration output stays consistent with the other platform-specific plugins.
anykeep_platform_has_plugin(kdeintegration_platform_enabled unix)
if(NOT kdeintegration_platform_enabled)
  add_anykeep_plugin(
    kdeintegration
    "Integration with KDE Plasma"
    ON
    unix
    METADATA
    plugin-metadata.json
    ICON
    logo.png
    SOURCES
    ${kdeintegration_sources})
  return()
endif()

function(anykeep_kde_adopt_cmake_install_dirs)
  foreach(
    dir_name
    BINDIR
    SBINDIR
    LIBDIR
    LIBEXECDIR
    INCLUDEDIR
    LOCALSTATEDIR
    SHAREDSTATEDIR
    DATAROOTDIR
    DATADIR
    LOCALEDIR
    MANDIR
    INFODIR
    SYSCONFDIR)
    if(NOT DEFINED KDE_INSTALL_${dir_name} AND DEFINED CMAKE_INSTALL_${dir_name})
      set(KDE_INSTALL_${dir_name}
          "${CMAKE_INSTALL_${dir_name}}"
          CACHE PATH "Install path adopted from CMAKE_INSTALL_${dir_name}" FORCE)
    endif()
  endforeach()
endfunction()

set(anykeep_plasmoid_buildable OFF)
if(UNIXLIKE)
  find_package(Qt6 REQUIRED COMPONENTS DBus Qml)
  find_package(ECM QUIET NO_MODULE)
  if(ECM_FOUND)
    set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
    anykeep_kde_adopt_cmake_install_dirs()
    find_package(KF6Config QUIET)
    find_package(KF6CoreAddons QUIET)
    find_package(KF6WindowSystem QUIET)
    find_package(Plasma 6.0 QUIET)
    if(KF6Config_FOUND
       AND KF6CoreAddons_FOUND
       AND KF6WindowSystem_FOUND
       AND Plasma_FOUND)
      set(anykeep_plasmoid_buildable ON)
    endif()
  endif()
endif()

if(anykeep_plasmoid_buildable)
  qt_policy(SET QTP0001 NEW)
  include(KDEInstallDirs)
  include("${Plasma_DIR}/PlasmaMacros.cmake")

  set(anykeep_plasmoid_id com.github.ri0n.anykeep)
  set(anykeep_sticky_plasmoid_id com.github.ri0n.anykeep.sticky)

  install(
    DIRECTORY plasmoid/package/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasma/plasmoids/${anykeep_plasmoid_id}
    COMPONENT Executable
    PATTERN ".svn" EXCLUDE
    PATTERN "*.qmlc" EXCLUDE
    PATTERN "CMakeLists.txt" EXCLUDE
    PATTERN "Messages.sh" EXCLUDE
    PATTERN "dummydata" EXCLUDE)

  install(
    DIRECTORY stickyplasmoid/package/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasma/plasmoids/${anykeep_sticky_plasmoid_id}
    COMPONENT Executable
    PATTERN "*.qmlc" EXCLUDE)

  set(PLASMA_SKIP_APPLET_INSTALL ON)
  plasma_add_applet(
    ${anykeep_plasmoid_id}
    QML_SOURCES
    plasmoid/package/contents/ui/main.qml
    plasmoid/package/contents/ui/CompactRepresentation.qml
    plasmoid/package/contents/ui/FullRepresentation.qml
    CPP_SOURCES
    plasmoid/appletplugin.cpp
    plasmoid/plasmoid.cpp
    plasmoid/plasmoid.h
    QML_ARGS
    VERSION
    1.0)
  unset(PLASMA_SKIP_APPLET_INSTALL)

  set(PLASMA_SKIP_APPLET_INSTALL ON)
  plasma_add_applet(
    ${anykeep_sticky_plasmoid_id}
    QML_SOURCES
    stickyplasmoid/package/contents/ui/main.qml
    CPP_SOURCES
    stickyplasmoid/appletplugin.cpp
    plasmoid/plasmoid.cpp
    plasmoid/plasmoid.h
    QML_ARGS
    VERSION
    1.0)
  unset(PLASMA_SKIP_APPLET_INSTALL)

  add_custom_target(plasmoid_anykeep DEPENDS ${anykeep_plasmoid_id})
  set_target_properties(${anykeep_plasmoid_id} PROPERTIES LIBRARY_OUTPUT_DIRECTORY
                                                          "${CMAKE_CURRENT_BINARY_DIR}/plasma/applets")
  target_include_directories(${anykeep_plasmoid_id} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plasmoid)
  target_link_libraries(${anykeep_plasmoid_id} PRIVATE Qt6::Core Qt6::DBus Qt6::Qml KF6::CoreAddons KF6::WindowSystem
                                                       Plasma::Plasma)
  set_target_properties(${anykeep_sticky_plasmoid_id} PROPERTIES LIBRARY_OUTPUT_DIRECTORY
                                                                 "${CMAKE_CURRENT_BINARY_DIR}/plasma/applets")
  target_include_directories(${anykeep_sticky_plasmoid_id} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plasmoid)
  target_link_libraries(${anykeep_sticky_plasmoid_id} PRIVATE Qt6::Core Qt6::DBus Qt6::Qml KF6::CoreAddons
                                                              KF6::WindowSystem Plasma::Plasma)
  install(
    TARGETS ${anykeep_plasmoid_id}
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/plasma/applets
    COMPONENT Executable)
  install(
    TARGETS ${anykeep_sticky_plasmoid_id}
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/plasma/applets
    COMPONENT Executable)

  if(ANYKEEP_DEVEL)
    set(anykeep_devel_qml_module_dir ${CMAKE_CURRENT_BINARY_DIR}/qml/AnyKeep)
    qt_add_qml_module(
      plasmoid_anykeep_devel
      URI
      AnyKeep
      VERSION
      1.0
      PLUGIN_TARGET
      plasmoid_anykeep_devel
      OUTPUT_DIRECTORY
      ${anykeep_devel_qml_module_dir}
      SOURCES
      plasmoid/plasmoid.cpp
      plasmoid/plasmoid.h)
    target_include_directories(plasmoid_anykeep_devel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plasmoid)
    target_link_libraries(plasmoid_anykeep_devel PRIVATE Qt6::Core Qt6::DBus Qt6::Qml KF6::WindowSystem)
    target_compile_definitions(
      plasmoid_anykeep_devel PRIVATE ANYKEEP_DEVEL_EXECUTABLE="$<TARGET_FILE:anykeepexe>"
                                     ANYKEEP_DEVEL_TRANSLATIONSDIR="${CMAKE_BINARY_DIR}/translations")
  endif()
else()
  message(STATUS "Plasma 6 development files not found; the AnyKeep plasmoid will not be built")
endif()

find_package(KF6GlobalAccel CONFIG)
find_package(KF6Notifications CONFIG)
find_package(KF6WindowSystem CONFIG)
find_package(KF6Config CONFIG)
find_package(KF6Sonnet CONFIG QUIET)

set(kdeintegration_buildable OFF)
if(KF6GlobalAccel_FOUND
   AND KF6Notifications_FOUND
   AND KF6WindowSystem_FOUND
   AND KF6Config_FOUND)
  find_package(Qt6 COMPONENTS DBus REQUIRED)
  set(kdeintegration_buildable ON)
endif()
set(kdeintegration_auto_disabled_cache_key ANYKEEP_PLUGIN_AUTO_DISABLED_kdeintegration)

if(NOT kdeintegration_buildable)
  if(ANYKEEP_PLUGIN_ENABLE_kdeintegration)
    set(${kdeintegration_auto_disabled_cache_key}
        ON
        CACHE INTERNAL "kdeintegration was disabled automatically")
  endif()
  set(ANYKEEP_PLUGIN_ENABLE_kdeintegration
      OFF
      CACHE BOOL "Enable AnyKeep plugin: Integration with KDE Plasma" FORCE)
elseif(${kdeintegration_auto_disabled_cache_key} OR (DEFINED CACHE{ANYKEEP_PLUGIN_ENABLE_kdeintegration}
                                                     AND NOT ANYKEEP_PLUGIN_ENABLE_kdeintegration))
  set(ANYKEEP_PLUGIN_ENABLE_kdeintegration
      ON
      CACHE BOOL "Enable AnyKeep plugin: Integration with KDE Plasma" FORCE)
  unset(${kdeintegration_auto_disabled_cache_key} CACHE)
endif()

add_anykeep_plugin(
  kdeintegration
  "Integration with KDE Plasma"
  ${kdeintegration_buildable}
  unix
  METADATA
  plugin-metadata.json
  ICON
  logo.png
  SOURCES
  ${kdeintegration_sources})

target_link_libraries(
  kdeintegration
  anykeep
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::DBus
  KF6::ConfigGui
  KF6::GlobalAccel
  KF6::Notifications
  KF6::WindowSystem)

if(KF6Sonnet_FOUND)
  target_link_libraries(kdeintegration KF6::SonnetCore)
  target_compile_definitions(kdeintegration PRIVATE ANYKEEP_SONNET_AVAILABLE)
endif()

if(ANYKEEP_DEVEL AND anykeep_plasmoid_buildable)
  target_compile_definitions(
    kdeintegration
    PRIVATE ANYKEEP_DEVEL_PLASMOID_PACKAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/plasmoid/package"
            ANYKEEP_DEVEL_STICKY_PLASMOID_PACKAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stickyplasmoid/package"
            ANYKEEP_DEVEL_PLASMOID_QML_DIR="${anykeep_devel_qml_module_dir}"
            ANYKEEP_DEVEL_PLASMOID_QML_INSTALL_DIR="${KDE_INSTALL_QMLDIR}/com/github/ri0n/anykeep")
endif()

if(ANYKEEP_DEVEL)
  target_compile_definitions(
    kdeintegration PRIVATE ANYKEEP_DEVEL_KWIN_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/kwinscript/contents/code/main.js")
endif()

install(
  DIRECTORY kwinscript/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/kwin/scripts/anykeepwindowgeometry
  COMPONENT Libraries)

install_anykeep_plugin(kdeintegration)
