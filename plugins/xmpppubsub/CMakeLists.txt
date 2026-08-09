include(AnyKeepMacro)

find_package(
  Qt6
  COMPONENTS Network Xml
  QUIET)
if(NOT TARGET QCoro::Core)
  find_package(QCoro6 QUIET COMPONENTS Core)
endif()

set(_xmpppubsub_buildable OFF)
if(TARGET Qt6::Network
   AND TARGET Qt6::Xml
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
    privatenotespubsubitem.cpp
    privatenotespubsubitem.h
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
    private-notes.xsd
    private-notes-encrypted-reference.py
    private-notes-encrypted-vectors.json
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
    worker/connection.cpp
    worker/maintenance.cpp
    worker/notes.cpp
    worker/omemo.cpp
    xmppworker.h
    xmppsettings.qrc
    xmppsettingscontroller.cpp
    xmppsettingscontroller.h
    storage/cache_folders.cpp
    storage/core.cpp
    storage/crud.cpp
    storage/events_retry.cpp
    xmppstorage.h
    xmppplugin.cpp
    xmppplugin.h)

if(ANYKEEP_BUILD_MOBILE)
  if(NOT _xmpppubsub_buildable)
    message(STATUS "XMPP Private Notes is omitted from anykeep_mobile: QXmpp OMEMO and QCoro are required")
    return()
  endif()

  message(STATUS "Building XMPP Private Notes as a bundled static plugin")
  add_anykeep_bundled_plugin(xmpppubsub SOURCES ${xmpppubsub_sources})
  target_link_libraries(
    anykeep_bundled_xmpppubsub
    PUBLIC Qt6::Core
           Qt6::Gui
           Qt6::Qml
           Qt6::Quick
           Qt6::QuickControls2
           Qt6::Network
           Qt6::Xml
           QXmpp::QXmpp
           QXmpp::Omemo
           QCoro::Core)
  target_include_directories(anykeep_bundled_xmpppubsub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  anykeep_configure_qxmpp_target(anykeep_bundled_xmpppubsub)
  return()
endif()

add_anykeep_plugin(
  xmpppubsub
  "Stores notes in a private persistent XMPP PEP node"
  ${_xmpppubsub_buildable}
  unix
  macosx
  windows
  android
  METADATA
  plugin-metadata.json
  ICON
  xmpp.svg
  SOURCES
  ${xmpppubsub_sources})

target_link_libraries(
  xmpppubsub
  ${EXTRA_LINK_TARGET}
  Qt6::Core
  Qt6::Gui
  Qt6::Qml
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::Network
  Qt6::Xml
  QXmpp::QXmpp
  QXmpp::Omemo
  QCoro::Core)
target_include_directories(xmpppubsub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
anykeep_configure_qxmpp_target(xmpppubsub)

install_anykeep_plugin(xmpppubsub)
