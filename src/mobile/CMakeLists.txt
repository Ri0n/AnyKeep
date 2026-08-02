cmake_minimum_required(VERSION 3.25.0)

project(qtnote_mobile VERSION ${QTNOTE_VERSION} LANGUAGES CXX)

find_package(Qt6 6.11 REQUIRED COMPONENTS Network Multimedia)
if(ANDROID)
    find_package(Qt6 6.11 REQUIRED COMPONENTS CorePrivate)
endif()

if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
endif()

set(qtnote_mobile_xmpp_available OFF)
if(TARGET qtnote_bundled_xmpppubsub)
    message(STATUS "Including the bundled XMPP Private Notes plugin in qtnote_mobile")
    set(qtnote_mobile_xmpp_available ON)
endif()

if(NOT TARGET qtnote_bundled_nextcloud OR NOT TARGET qtnote_bundled_gemini
   OR NOT TARGET qtnote_bundled_openaiwhisper)
    message(FATAL_ERROR "Required bundled mobile plugin targets were not created")
endif()

set(qtnote_mobile_shared_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/EditorToolbar.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/EditorActionController.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/DialogHost.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/NoteBlockEditor.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/NoteEditorPane.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/FolderPickerMenu.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/FoldersPage.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/NotesManagerPage.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/ThemedIcon.qml"
)
set(qtnote_mobile_shared_impl_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/shared/ThemedIconImpl.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/shared/FolderPickerMenuImpl.qml"
)
set(qtnote_mobile_editor_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/EditorContextMenu.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/EditorReorderController.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/EditorToolbarImpl.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/EditorActionControllerImpl.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/NoteBlockEditorImpl.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/NoteEditorPaneImpl.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/NoteFindBar.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/CustomDictionaryDialog.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/EditorBlockDelegate.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/EditorBlockFactories.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/InterBlockHitLayer.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/LinkEditorPopup.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/TrailingDocumentArea.qml"
)
set(qtnote_mobile_editor_component_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/components/CompactPopupSelector.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/components/HoverActionStrip.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/components/ListBlockEditor.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/components/NoteBlockTextArea.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/components/TagLineEditor.qml"
)
set(qtnote_mobile_editor_support_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/support/ListBlockBehavior.js"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/support/TableBlockBehavior.js"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/support/EditorMarkdownRendering.js"
)
set(qtnote_mobile_editor_block_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/AttachmentBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/AudioBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/BlockQuoteBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/CodeBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/HeadingBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/ImageAlignmentGlyph.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/ImageBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/ImageResizeHandle.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/ListBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/TableBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/TagLineBlock.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/editor/blocks/TextBlock.qml"
)
set(qtnote_mobile_reorder_qml
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/reorder/DragPreviewLayer.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/reorder/GenericReorderController.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/reorder/LinearReorderLayout.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/reorder/ReorderDisplacement.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/reorder/ReorderDragHandle.qml"
    "${CMAKE_SOURCE_DIR}/libqtnote/qml/reorder/ReorderGeometry.qml"
)
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_shared_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties("${qtnote_mobile_qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qtnote_mobile_qml_alias}")
endforeach()
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_shared_impl_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${qtnote_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "shared/${qtnote_mobile_qml_alias}"
    )
endforeach()
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_editor_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${qtnote_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/${qtnote_mobile_qml_alias}"
    )
endforeach()
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_editor_component_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${qtnote_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/components/${qtnote_mobile_qml_alias}"
    )
endforeach()
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_editor_support_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${qtnote_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/support/${qtnote_mobile_qml_alias}"
    )
endforeach()
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_editor_block_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${qtnote_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/blocks/${qtnote_mobile_qml_alias}"
    )
endforeach()
foreach(qtnote_mobile_qml_file IN LISTS qtnote_mobile_reorder_qml)
    get_filename_component(qtnote_mobile_qml_alias "${qtnote_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${qtnote_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "reorder/${qtnote_mobile_qml_alias}"
    )
endforeach()

qt_add_executable(qtnote_mobile
    main.cpp
)

qt_add_qml_module(qtnote_mobile
    URI QtNote.Mobile
    VERSION 1.0
    QML_FILES
        AppSettingsPage.qml
        DraftsPage.qml
        Main.qml
        MobileNoteActionBar.qml
        MobileNoteTopBar.qml
        NoteEditorPage.qml
        NotesPage.qml
        PluginSettingsPage.qml
        PluginsPage.qml
        RootPage.qml
        SettingsHubPage.qml
        StorageSettingsPage.qml
        StoragesPage.qml
        ${qtnote_mobile_shared_qml}
        ${qtnote_mobile_shared_impl_qml}
        ${qtnote_mobile_editor_qml}
        ${qtnote_mobile_editor_component_qml}
        ${qtnote_mobile_editor_support_qml}
        ${qtnote_mobile_editor_block_qml}
        ${qtnote_mobile_reorder_qml}
    SOURCES
        androidplatformservices.cpp
        androidplatformservices.h
        mobileapplication.cpp
        mobileapplication.h
        mobileeditorplatformbackend.cpp
        mobileeditorplatformbackend.h
        mobilebundledplugins.cpp
        mobilebundledplugins.h
)

# mobilebundledplugins.cpp includes the concrete plugin entry point headers.
# Suppress dynamic plugin metadata in this target as well as in the bundled
# static libraries, so AUTOMOC never produces duplicate plugin entry symbols.
target_compile_definitions(qtnote_mobile PRIVATE QTNOTE_BUNDLED_PLUGIN_BUILD)

target_include_directories(qtnote_mobile PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud"
    "${CMAKE_SOURCE_DIR}/plugins/gemini"
    "${CMAKE_SOURCE_DIR}/plugins/openaiwhisper"
    "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub"
    "${CMAKE_BINARY_DIR}"
)

target_link_libraries(qtnote_mobile PRIVATE
    qtnote_core
    qtnote_bundled_nextcloud
    qtnote_bundled_gemini
    qtnote_bundled_openaiwhisper
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Multimedia
)

if(qtnote_mobile_xmpp_available)
    target_compile_definitions(qtnote_mobile PRIVATE QTNOTE_MOBILE_XMPP_AVAILABLE)
    target_link_libraries(qtnote_mobile PRIVATE qtnote_bundled_xmpppubsub)
endif()

if(ANDROID)
    include(AndroidOpenSSL)
    qtnote_deploy_android_openssl(qtnote_mobile)
    target_link_libraries(qtnote_mobile PRIVATE Qt6::CorePrivate)
    qt_add_android_permission(qtnote_mobile NAME android.permission.RECORD_AUDIO)
    set_target_properties(qtnote_mobile PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_APP_ICON "@mipmap/qtnote"
    )
endif()

set_target_properties(qtnote_mobile PROPERTIES
    OUTPUT_NAME qtnote
    QT_ANDROID_APP_NAME QtNote
    QT_ANDROID_PACKAGE_NAME com.github.ri0n.qtnote
    QT_ANDROID_VERSION_NAME ${QTNOTE_VERSION}
    QT_ANDROID_MIN_SDK_VERSION 28
)
