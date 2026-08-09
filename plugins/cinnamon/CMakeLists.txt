include(AnyKeepMacro)
include(GNUInstallDirs)

set(anykeep_cinnamon_applet_uuid anykeep@ri0n.github.io)
set(anykeep_cinnamon_desklet_uuid anykeep-sticky@ri0n.github.io)

add_anykeep_plugin(
  cinnamon
  "Cinnamon integration"
  ON
  unix
  METADATA
  plugin-metadata.json
  ICON
  cinnamon-logo.svg
  SOURCES
  cinnamon.cpp
  cinnamon.h
  cinnamon.qrc)

target_link_libraries(cinnamon ${EXTRA_LINK_TARGET} Qt6::Core Qt6::Gui Qt6::Widgets)

install(
  DIRECTORY applet/${anykeep_cinnamon_applet_uuid}/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/applets/${anykeep_cinnamon_applet_uuid}
  COMPONENT Executable)

install(
  DIRECTORY desklet/${anykeep_cinnamon_desklet_uuid}/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/desklets/${anykeep_cinnamon_desklet_uuid}
  COMPONENT Executable)

install(
  FILES ${CMAKE_SOURCE_DIR}/libanykeep/images/anykeep.svg ${CMAKE_SOURCE_DIR}/libanykeep/images/anykeep-symbolic.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/applets/${anykeep_cinnamon_applet_uuid}/icons
  COMPONENT Executable)

install_anykeep_plugin(cinnamon)
