include(QtNoteMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network Xml QUIET)
find_package(QCoro${QT_VERSION_MAJOR} QUIET COMPONENTS Core)

set(_xmpppubsub_buildable OFF)
if(TARGET Qt${QT_VERSION_MAJOR}::Network
   AND TARGET Qt${QT_VERSION_MAJOR}::Xml
   AND TARGET QXmpp::QXmpp
   AND TARGET QXmpp::Omemo
   AND TARGET QCoro::Core)
    set(_xmpppubsub_buildable ON)
endif()

add_qtnote_plugin(
    xmpppubsub
    "Stores notes in a private persistent XMPP PEP node"
    ${_xmpppubsub_buildable}
    unix macosx windows android
    SOURCES
        xmppbackend.h
        xmppdto.h
        xmpperror.cpp
        xmpperror.h
        qtnotepubsubitem.cpp
        qtnotepubsubitem.h
        xmpppepextension.cpp
        xmpppepextension.h
        xmppkeysyncextension.cpp
        xmppkeysyncextension.h
        xmppkeyresolutiondialog.cpp
        xmppkeyresolutiondialog.h
        xmppnotecodec.cpp
        xmppnotecodec.h
        xmppomemostorage.cpp
        xmppomemostorage.h
        xmppomemopubsubitems.cpp
        xmppomemopubsubitems.h
        xmpppersistenttruststorage.cpp
        xmpppersistenttruststorage.h
        xmppworker.cpp
        xmppworker.h
        xmppsettings.qrc
        xmppsettingscontroller.cpp
        xmppsettingscontroller.h
        xmppstorage.cpp
        xmppstorage.h
        xmppplugin.cpp
        xmppplugin.h
)

target_link_libraries(
    xmpppubsub
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Xml
    QXmpp::QXmpp
    QXmpp::Omemo
    QCoro::Core
)
qtnote_configure_qxmpp_target(xmpppubsub)

install_qtnote_plugin(xmpppubsub)
