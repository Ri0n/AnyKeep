cmake_minimum_required(VERSION 3.25.0)

project(qtnote_mobile VERSION ${QTNOTE_VERSION} LANGUAGES CXX)

if(ANDROID)
    find_package(Qt6 6.11 REQUIRED COMPONENTS Network CorePrivate)
else()
    find_package(Qt6 6.4 REQUIRED COMPONENTS Network)
endif()

qt_policy(SET QTP0001 NEW)

if(NOT TARGET QCoro::Core)
    find_package(QCoro6 QUIET COMPONENTS Core)
endif()

set(qtnote_mobile_xmpp_sources)
set(qtnote_mobile_xmpp_available OFF)
if(TARGET QXmpp::QXmpp AND TARGET QXmpp::Omemo AND TARGET QCoro::Core)
    message(STATUS "Including the XMPP Private Notes plugin in qtnote_mobile")
    set(qtnote_mobile_xmpp_available ON)
    list(APPEND qtnote_mobile_xmpp_sources
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/qtnotepubsubitem.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/qtnotepubsubitem.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppbackend.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppdialogpresenter.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppdialogpresenter.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppdto.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmpperror.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmpperror.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppkeyresolutioncontroller.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppkeyresolutioncontroller.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppkeysyncextension.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppkeysyncextension.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppnotecodec.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppnotecodec.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppomemopubsubitems.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppomemopubsubitems.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppomemostorage.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppomemostorage.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmpppepextension.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmpppepextension.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmpppersistenttruststorage.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmpppersistenttruststorage.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppplugin.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppplugin.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppsettingscontroller.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppsettingscontroller.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppstorage.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppstorage.h"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppworker.cpp"
        "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/xmppworker.h"
    )
else()
    message(STATUS "XMPP Private Notes is omitted from qtnote_mobile: QXmpp OMEMO and QCoro are required")
endif()

set(qtnote_mobile_shared_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/EditorContextMenu.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/EditorToolbar.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/DialogHost.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/ListBlockBehavior.js"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/NoteBlockEditor.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/NoteFindBar.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/NotesManagerPage.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/TableBlockBehavior.js"
)
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_shared_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties("${qtnote_mobile_qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qtnote_mobile_qml_alias}")
endforeach()

qt_add_executable(qtnote_mobile
    main.cpp
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud/nextclouddto.h"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud/nextcloudworker.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud/nextcloudworker.h"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud/nextcloudstorage.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud/nextcloudstorage.h"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud/nextcloudplugin.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud/nextcloudplugin.h"
    ${qtnote_mobile_xmpp_sources}
)

if(qtnote_mobile_xmpp_available)
    # The mobile target embeds the XMPP settings UI directly. Do not rely on
    # AUTORCC processing a plugin .qrc file that is also used by the desktop
    # shared plugin: the Android target is a separate shared library and must
    # own/register these resources itself.
    qt_add_resources(qtnote_mobile qtnote_mobile_xmpp_qml
        PREFIX "/qml"
        BASE "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub"
        FILES
            "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/XmppSettings.qml"
            "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/XmppKeyResolutionHost.qml"
            "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub/XmppKeySyncTrustHost.qml"
    )
endif()

qt_add_qml_module(qtnote_mobile
    URI QtNote.Mobile
    VERSION 1.0
    QML_FILES
        AppSettingsPage.qml
        Main.qml
        NoteEditorPage.qml
        NotesPage.qml
        PluginSettingsPage.qml
        PluginsPage.qml
        RootPage.qml
        StorageSettingsPage.qml
        StoragesPage.qml
        ${qtnote_mobile_shared_qml}
    SOURCES
        androidplatformservices.cpp
        androidplatformservices.h
        mobileapplication.cpp
        mobileapplication.h
        mobilebundledplugins.cpp
        mobilebundledplugins.h
)

target_compile_definitions(qtnote_mobile PRIVATE QTNOTE_BUNDLED_PLUGIN_BUILD)

target_include_directories(qtnote_mobile PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud"
    "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub"
    "${CMAKE_BINARY_DIR}"
)

target_link_libraries(qtnote_mobile PRIVATE
    qtnote_core
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
)

if(qtnote_mobile_xmpp_available)
    target_compile_definitions(qtnote_mobile PRIVATE QTNOTE_MOBILE_XMPP_AVAILABLE)
    target_link_libraries(qtnote_mobile PRIVATE
        Qt6::Xml
        QXmpp::QXmpp
        QXmpp::Omemo
        QCoro::Core
    )
    qtnote_configure_qxmpp_target(qtnote_mobile)
endif()

if(ANDROID)
    include(AndroidOpenSSL)
    qtnote_deploy_android_openssl(qtnote_mobile)
    target_link_libraries(qtnote_mobile PRIVATE Qt6::CorePrivate)
    qt_add_android_permission(qtnote_mobile NAME android.permission.RECORD_AUDIO)
endif()

set_target_properties(qtnote_mobile PROPERTIES
    OUTPUT_NAME qtnote
    QT_ANDROID_APP_NAME QtNote
    QT_ANDROID_PACKAGE_NAME com.github.ri0n.qtnote
    QT_ANDROID_VERSION_NAME ${QTNOTE_VERSION}
    QT_ANDROID_MIN_SDK_VERSION 28
)
