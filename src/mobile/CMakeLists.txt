cmake_minimum_required(VERSION 3.25.0)

project(qtnote_mobile VERSION ${QTNOTE_VERSION} LANGUAGES CXX)

if(ANDROID)
    find_package(Qt6 6.11 REQUIRED COMPONENTS Network CorePrivate)
else()
    find_package(Qt6 6.4 REQUIRED COMPONENTS Network)
endif()

qt_policy(SET QTP0001 NEW)

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
)

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

if(ANDROID)
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
