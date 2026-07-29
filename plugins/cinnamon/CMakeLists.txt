include(QtNoteMacro)
include(GNUInstallDirs)

set(qtnote_cinnamon_applet_uuid qtnote@ri0n.github.io)
set(qtnote_cinnamon_desklet_uuid qtnote-sticky@ri0n.github.io)

add_qtnote_plugin(cinnamon "Cinnamon integration" ON unix SOURCES
    cinnamon.cpp
    cinnamon.h
    cinnamon.qrc
)

target_link_libraries(cinnamon ${EXTRA_LINK_TARGET} Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

install(
    DIRECTORY applet/${qtnote_cinnamon_applet_uuid}/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/applets/${qtnote_cinnamon_applet_uuid}
    COMPONENT Executable
)

install(
    DIRECTORY desklet/${qtnote_cinnamon_desklet_uuid}/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/desklets/${qtnote_cinnamon_desklet_uuid}
    COMPONENT Executable
)

install(
    FILES
        ${CMAKE_SOURCE_DIR}/libqtnote/images/qtnote.svg
        ${CMAKE_SOURCE_DIR}/libqtnote/images/qtnote-symbolic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/cinnamon/applets/${qtnote_cinnamon_applet_uuid}/icons
    COMPONENT Executable
)

install_qtnote_plugin(cinnamon)
