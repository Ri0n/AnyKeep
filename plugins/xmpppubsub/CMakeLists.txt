include(AnyKeepMacro)

find_package(
  Qt6
  COMPONENTS Network Xml
  QUIET)

set(_xmpppubsub_buildable OFF)
set(_xmpppubsub_backend_libraries)
set(_xmpppubsub_backend_definitions)
set(_xmpppubsub_backend_sources)

if(ANYKEEP_XMPP_BACKEND STREQUAL "IRIS")
  if(TARGET Iris::Iris)
    set(_xmpppubsub_buildable ON)
    list(APPEND _xmpppubsub_backend_libraries Iris::Iris)
    list(APPEND _xmpppubsub_backend_definitions ANYKEEP_XMPP_BACKEND_IRIS=1)
    list(APPEND _xmpppubsub_backend_sources
         iris/irisxmppbackend.cpp
         iris/irisxmppbackend.h
         iris/irisomemostorage.cpp
         iris/irisomemostorage.h
         iris/iristruststorage.cpp
         iris/iristruststorage.h
         iris/iriskeysynctask.cpp
         iris/iriskeysynctask.h)
  endif()
else()
  if(NOT TARGET QCoro::Core)
    find_package(QCoro6 QUIET COMPONENTS Core)
  endif()
  if(TARGET QXmpp::QXmpp AND TARGET QXmpp::Omemo AND TARGET QCoro::Core)
    set(_xmpppubsub_buildable ON)
    list(APPEND _xmpppubsub_backend_libraries QXmpp::QXmpp QXmpp::Omemo QCoro::Core)
    list(APPEND _xmpppubsub_backend_definitions ANYKEEP_XMPP_BACKEND_QXMPP=1)
    list(APPEND _xmpppubsub_backend_sources
         xmpperror.cpp
         xmpperror.h
         privatenotespubsubitem.cpp
         privatenotespubsubitem.h
         xmpppepextension.cpp
         xmpppepextension.h
         xmppkeysyncextension.cpp
         xmppkeysyncextension.h
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
         xmppworker.h)
  endif()
endif()

if(NOT TARGET Qt6::Network OR NOT TARGET Qt6::Xml)
  set(_xmpppubsub_buildable OFF)
endif()

set(xmpppubsub_sources
    PROTOXEP.md
    INTEROPERABILITY.md
    README.md
    xmppbackend.h
    xmppbackendfactory.cpp
    xmppbackendfactory.h
    xmppdto.h
    xmpppayloadxml.cpp
    xmpppayloadxml.h
    xmppxmllog.h
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
    xmppsettings.qrc
    xmppsettingscontroller.cpp
    xmppsettingscontroller.h
    storage/cache_folders.cpp
    storage/core.cpp
    storage/crud.cpp
    storage/events_retry.cpp
    xmppstorage.h
    xmppplugin.cpp
    xmppplugin.h
    ${_xmpppubsub_backend_sources})

function(_anykeep_configure_xmpppubsub_target target)
  target_link_libraries(
    ${target}
    PUBLIC Qt6::Core
           Qt6::Gui
           Qt6::Qml
           Qt6::Quick
           Qt6::QuickControls2
           Qt6::Network
           Qt6::Xml
           ${_xmpppubsub_backend_libraries})
  target_compile_definitions(${target} PRIVATE ${_xmpppubsub_backend_definitions})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(ANYKEEP_XMPP_BACKEND STREQUAL "IRIS")
    anykeep_configure_iris_target(${target})
  else()
    anykeep_configure_qxmpp_target(${target})
  endif()
endfunction()

if(ANYKEEP_BUILD_MOBILE)
  if(NOT _xmpppubsub_buildable)
    message(STATUS "XMPP Private Notes is omitted from anykeep_mobile: ${ANYKEEP_XMPP_BACKEND} backend dependencies are unavailable")
    return()
  endif()

  message(STATUS "Building XMPP Private Notes as a bundled static plugin (${ANYKEEP_XMPP_BACKEND} backend)")
  add_anykeep_bundled_plugin(xmpppubsub SOURCES ${xmpppubsub_sources})
  _anykeep_configure_xmpppubsub_target(anykeep_bundled_xmpppubsub)
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

if(TARGET xmpppubsub)
  # add_anykeep_plugin() exposes libanykeep as EXTRA_LINK_TARGET. Keep the
  # desktop plugin linked to it so AUTOMOC also inherits libanykeep's public
  # include directories (notably bundledplugininterface.h).
  target_link_libraries(xmpppubsub PUBLIC ${EXTRA_LINK_TARGET})
  _anykeep_configure_xmpppubsub_target(xmpppubsub)
endif()

install_anykeep_plugin(xmpppubsub)
