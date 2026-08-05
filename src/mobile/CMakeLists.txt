cmake_minimum_required(VERSION 3.25.0)

project(anykeep_mobile VERSION ${ANYKEEP_VERSION} LANGUAGES CXX)

find_package(Qt6 6.11 REQUIRED COMPONENTS Network Multimedia)
if(ANDROID)
    find_package(Qt6 6.11 REQUIRED COMPONENTS CorePrivate)
endif()

if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
endif()

set(anykeep_mobile_xmpp_available OFF)
if(TARGET anykeep_bundled_xmpppubsub)
    message(STATUS "Including the bundled XMPP Private Notes plugin in anykeep_mobile")
    set(anykeep_mobile_xmpp_available ON)
endif()

if(NOT TARGET anykeep_bundled_nextcloud OR NOT TARGET anykeep_bundled_gemini
   OR NOT TARGET anykeep_bundled_openaiwhisper OR NOT TARGET anykeep_bundled_yandex)
    message(FATAL_ERROR "Required bundled mobile plugin targets were not created")
endif()

set(anykeep_mobile_shared_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/EditorToolbar.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/EditorActionController.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/DialogHost.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/NoteBlockEditor.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/NoteEditorPane.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/FolderPickerMenu.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/FoldersPage.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/NotesManagerPage.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/ThemedIcon.qml"
)
set(anykeep_mobile_shared_impl_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/shared/ThemedIconImpl.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/shared/FolderPickerMenuImpl.qml"
)
set(anykeep_mobile_notesmanager_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/notesmanager/NotesManagerActionController.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/notesmanager/NotesManagerDragController.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/notesmanager/NotesManagerPageImpl.qml"
)
set(anykeep_mobile_editor_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/EditorContextMenu.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/EditorReorderController.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/EditorToolbarImpl.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/EditorActionControllerImpl.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/NoteBlockEditorImpl.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/NoteEditorPaneImpl.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/NoteFindBar.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/CustomDictionaryDialog.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/EditorBlockDelegate.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/EditorBlockFactories.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/InterBlockHitLayer.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/LinkEditorPopup.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/TrailingDocumentArea.qml"
)
set(anykeep_mobile_editor_component_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/components/CompactPopupSelector.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/components/HoverActionStrip.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/components/ListBlockEditor.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/components/NoteBlockTextArea.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/components/TagLineEditor.qml"
)
set(anykeep_mobile_editor_controller_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/controllers/EditorFocusController.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/controllers/EditorMediaNavigationController.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/controllers/EditorSelectionController.qml"
)
set(anykeep_mobile_editor_support_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/support/ListBlockBehavior.js"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/support/TableBlockBehavior.js"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/support/EditorMarkdownRendering.js"
)
set(anykeep_mobile_editor_block_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/AttachmentBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/AudioBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/BlockQuoteBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/CodeBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/HeadingBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/ImageAlignmentGlyph.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/ImageBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/ImageResizeHandle.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/ListBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/TableBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/TagLineBlock.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/editor/blocks/TextBlock.qml"
)
set(anykeep_mobile_reorder_qml
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/reorder/DragPreviewLayer.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/reorder/GenericReorderController.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/reorder/LinearReorderLayout.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/reorder/ReorderDisplacement.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/reorder/ReorderDragHandle.qml"
    "${CMAKE_SOURCE_DIR}/libanykeep/qml/reorder/ReorderGeometry.qml"
)
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_shared_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties("${anykeep_mobile_qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${anykeep_mobile_qml_alias}")
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_shared_impl_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "shared/${anykeep_mobile_qml_alias}"
    )
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_notesmanager_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "notesmanager/${anykeep_mobile_qml_alias}"
    )
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_editor_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/${anykeep_mobile_qml_alias}"
    )
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_editor_component_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/components/${anykeep_mobile_qml_alias}"
    )
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_editor_controller_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/controllers/${anykeep_mobile_qml_alias}"
    )
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_editor_support_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/support/${anykeep_mobile_qml_alias}"
    )
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_editor_block_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "editor/blocks/${anykeep_mobile_qml_alias}"
    )
endforeach()
foreach(anykeep_mobile_qml_file IN LISTS anykeep_mobile_reorder_qml)
    get_filename_component(anykeep_mobile_qml_alias "${anykeep_mobile_qml_file}" NAME)
    set_source_files_properties(
        "${anykeep_mobile_qml_file}"
        PROPERTIES QT_RESOURCE_ALIAS "reorder/${anykeep_mobile_qml_alias}"
    )
endforeach()

qt_add_executable(anykeep_mobile
    main.cpp
)

qt_add_qml_module(anykeep_mobile
    URI AnyKeep.Mobile
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
        ${anykeep_mobile_shared_qml}
        ${anykeep_mobile_shared_impl_qml}
        ${anykeep_mobile_notesmanager_qml}
        ${anykeep_mobile_editor_qml}
        ${anykeep_mobile_editor_component_qml}
        ${anykeep_mobile_editor_controller_qml}
        ${anykeep_mobile_editor_support_qml}
        ${anykeep_mobile_editor_block_qml}
        ${anykeep_mobile_reorder_qml}
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
target_compile_definitions(anykeep_mobile PRIVATE ANYKEEP_BUNDLED_PLUGIN_BUILD)

target_include_directories(anykeep_mobile PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins"
    "${CMAKE_SOURCE_DIR}/plugins/nextcloud"
    "${CMAKE_SOURCE_DIR}/plugins/gemini"
    "${CMAKE_SOURCE_DIR}/plugins/openaiwhisper"
    "${CMAKE_SOURCE_DIR}/plugins/yandex"
    "${CMAKE_SOURCE_DIR}/plugins/xmpppubsub"
    "${CMAKE_BINARY_DIR}"
)

target_link_libraries(anykeep_mobile PRIVATE
    anykeep_core
    anykeep_bundled_nextcloud
    anykeep_bundled_gemini
    anykeep_bundled_openaiwhisper
    anykeep_bundled_yandex
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Multimedia
)

if(anykeep_mobile_xmpp_available)
    target_compile_definitions(anykeep_mobile PRIVATE ANYKEEP_MOBILE_XMPP_AVAILABLE)
    target_link_libraries(anykeep_mobile PRIVATE anykeep_bundled_xmpppubsub)
endif()

if(ANDROID)
    include(AndroidOpenSSL)
    anykeep_deploy_android_openssl(anykeep_mobile)
    target_link_libraries(anykeep_mobile PRIVATE Qt6::CorePrivate)
    qt_add_android_permission(anykeep_mobile NAME android.permission.RECORD_AUDIO)
    set_target_properties(anykeep_mobile PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_APP_ICON "@mipmap/anykeep"
    )
endif()

set_target_properties(anykeep_mobile PROPERTIES
    OUTPUT_NAME anykeep
    QT_ANDROID_APP_NAME AnyKeep
    QT_ANDROID_PACKAGE_NAME com.github.ri0n.anykeep
    QT_ANDROID_VERSION_NAME ${ANYKEEP_VERSION}
    QT_ANDROID_MIN_SDK_VERSION 28
)
