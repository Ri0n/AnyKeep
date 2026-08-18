cmake_minimum_required(VERSION 3.22.0)
include(AnyKeepMacro)
project(
  anykeepexe
  VERSION ${ANYKEEP_VERSION}
  LANGUAGES CXX)

if(WIN32)
  set(WINRESFILE "win/anykeep.rc")
endif()

set(MACOSX_BUNDLE_ICON_FILE anykeep)
set(APP_ICON_MACOSX ${CMAKE_CURRENT_SOURCE_DIR}/mac/anykeep.icns)
set_source_files_properties(${APP_ICON_MACOSX} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(${PROJECT_NAME} WIN32 MACOSX_BUNDLE main.cpp ${WINRESFILE} ${APP_ICON_MACOSX})

if(WIN32)
  target_sources(${PROJECT_NAME} PRIVATE win/windowstaskbarintegration.cpp win/windowstaskbarintegration.h)
endif()

# add_dependencies(${PROJECT_NAME} build_translations)
set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES OUTPUT_NAME anykeep
             MACOSX_BUNDLE_BUNDLE_NAME AnyKeep
             MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in"
             MACOSX_BUNDLE_SHORT_VERSION_STRING "${ANYKEEP_VERSION}" # version
             MACOSX_BUNDLE_BUNDLE_VERSION "${ANYKEEP_VERSION}" # git tag
             MACOSX_BUNDLE_COPYRIGHT "Copyright (C) 2010-2026 Sergei Ilinykh"
             MACOSX_BUNDLE_GUI_IDENTIFIER "com.github.ri0n.anykeep")
if(WIN32)
  set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}")
endif()

target_link_libraries(${PROJECT_NAME} ${anykeep_lib} QtSingleApplication)
if(WIN32)
  target_link_libraries(${PROJECT_NAME} shell32 ole32)
endif()

if(WIN32)
  target_link_libraries(${PROJECT_NAME} Qt6::QuickControls2)
endif()

if(WIN32)
  install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "." COMPONENT Executable)
  windeployqt(${PROJECT_NAME})
elseif(APPLE)
  # MACOSX_BUNDLE targets require an explicit BUNDLE destination. Keep Linux desktop integration files out of the .app
  # installation. Packaging/notarizing the bundle is handled separately from this basic install rule.
  install(TARGETS ${PROJECT_NAME} BUNDLE DESTINATION "." COMPONENT Executable)
else()
  install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Executable)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/com.github.ri0n.AnyKeep.service.in
                 ${CMAKE_CURRENT_BINARY_DIR}/com.github.ri0n.AnyKeep.service @ONLY)
  install(
    FILES ${CMAKE_SOURCE_DIR}/anykeep.desktop
    DESTINATION ${CMAKE_INSTALL_FULL_DATADIR}/applications
    COMPONENT Executable)
  install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/com.github.ri0n.AnyKeep.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services
    COMPONENT Executable)
  install(
    FILES ${CMAKE_SOURCE_DIR}/libanykeep/images/anykeep.svg
    DESTINATION ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/scalable/apps
    COMPONENT Executable)
  foreach(icon_size 16 22 24 32 48 64)
    install(
      FILES ${CMAKE_SOURCE_DIR}/libanykeep/images/anykeep${icon_size}.png
      DESTINATION ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
      RENAME anykeep.png
      COMPONENT Executable)
  endforeach()
  install(
    FILES ${CMAKE_SOURCE_DIR}/libanykeep/images/anykeep-symbolic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps
    COMPONENT Executable)
endif()

if(WIN32)
  configure_file(win/launcher_config.h.in launcher_config.h @ONLY)

  add_executable(anykeep_launcher WIN32 win/launcher.cpp win/anykeep.rc "${CMAKE_CURRENT_BINARY_DIR}/launcher_config.h")
  set_target_properties(anykeep_launcher PROPERTIES OUTPUT_NAME AnyKeepLauncher RUNTIME_OUTPUT_DIRECTORY
                                                                                "${EXECUTABLE_OUTPUT_PATH}")
  target_include_directories(anykeep_launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(anykeep_launcher PRIVATE shell32)

  add_executable(anykeep_updater WIN32 win/updater.cpp win/anykeep.rc)
  set_target_properties(anykeep_updater PROPERTIES OUTPUT_NAME AnyKeepUpdater RUNTIME_OUTPUT_DIRECTORY
                                                                              "${EXECUTABLE_OUTPUT_PATH}")
  target_link_libraries(anykeep_updater PRIVATE shell32)

  install(TARGETS anykeep_launcher RUNTIME DESTINATION "." COMPONENT Launcher)
  install(TARGETS anykeep_updater RUNTIME DESTINATION "." COMPONENT Executable)
endif()
