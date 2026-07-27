include(QtNoteMacro)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Network Xml QUIET)
if(NOT TARGET QCoro::Core)
    find_package(QCoro${QT_VERSION_MAJOR} QUIET COMPONENTS Core)
endif()

set(_xmpppubsub_buildable OFF)
if(TARGET Qt${QT_VERSION_MAJOR}::Network
   AND TARGET Qt${QT_VERSION_MAJOR}::Xml
   AND TARGET QXmpp::QXmpp
   AND TARGET QXmpp::Omemo
   AND TARGET QCoro::Core)
    set(_xmpppubsub_buildable ON)
endif()

set(xmpppubsub_sources
    PROTOXEP.md
    INTEROPERABILITY.md
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
    xmppdialogpresenter.cpp
    xmppdialogpresenter.h
    xmppkeyresolutioncontroller.cpp
    xmppkeyresolutioncontroller.h
    XmppKeyResolutionHost.qml
    XmppKeySyncTrustHost.qml
    xmppnotecodec.cpp
    xmppnotecodec.h
    qtnote-notes.xsd
    qtnote-encrypted-reference.py
    qtnote-encrypted-vectors.json
    conformance/rust/Cargo.toml
    conformance/rust/README.md
    conformance/rust/src/lib.rs
    conformance/rust/src/main.rs
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

if(QTNOTE_BUILD_MOBILE)
    if(NOT _xmpppubsub_buildable)
        message(STATUS "XMPP Private Notes is omitted from qtnote_mobile: QXmpp OMEMO and QCoro are required")
        return()
    endif()

    message(STATUS "Building XMPP Private Notes as a bundled static plugin")
    add_qtnote_bundled_plugin(xmpppubsub SOURCES ${xmpppubsub_sources})
    target_link_libraries(qtnote_bundled_xmpppubsub PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Qml
        Qt${QT_VERSION_MAJOR}::Quick
        Qt${QT_VERSION_MAJOR}::QuickControls2
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Xml
        QXmpp::QXmpp
        QXmpp::Omemo
        QCoro::Core
    )
    qtnote_configure_qxmpp_target(qtnote_bundled_xmpppubsub)
    return()
endif()

add_qtnote_plugin(
    xmpppubsub
    "Stores notes in a private persistent XMPP PEP node"
    ${_xmpppubsub_buildable}
    unix macosx windows android
    SOURCES ${xmpppubsub_sources}
)

target_link_libraries(
    xmpppubsub
    ${EXTRA_LINK_TARGET}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Qml
    Qt${QT_VERSION_MAJOR}::Quick
    Qt${QT_VERSION_MAJOR}::QuickControls2
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Xml
    QXmpp::QXmpp
    QXmpp::Omemo
    QCoro::Core
)
qtnote_configure_qxmpp_target(xmpppubsub)

install_qtnote_plugin(xmpppubsub)
