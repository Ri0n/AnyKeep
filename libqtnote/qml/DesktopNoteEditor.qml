import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property alias blockEditor: editor

    function flushPendingEditorChanges() { editor.flushPendingEditorChanges() }
    function insertTextAtCursor(value) { return editor.insertTextAtCursor(value) }
    function focusInitialEditor() { editor.focusInitialEditor() }
    function insertTableBlock() { return editor.insertTableBlock() }
    function insertListBlock(type) { return editor.insertListBlock(type) }
    function insertionRowAtPoint(x, y) { return editor.insertionRowAtPoint(x, y) }
    function captureEditorState() { return editor.captureEditorState() }
    function restoreEditorState(state) { return editor.restoreEditorState(state) }
    function prepareForHistoryRestore() { editor.prepareForHistoryRestore() }
    function documentHistoryOwnsFocus() { return editor.documentHistoryOwnsFocus() }
    function copyActiveSelection() { return editor.copyActiveSelection() }
    function cutActiveSelection() { return editor.cutActiveSelection() }
    function pasteClipboard() { return editor.pasteClipboard() }
    function openFind() { findBar.open() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        EditorToolbar {
            Layout.fillWidth: true
            editorBackend: noteEditor
            platformBackend: desktopEditorPlatform
            blockEditor: editor
            onFindRequested: findBar.open()
        }

        NoteFindBar {
            id: findBar
            Layout.fillWidth: true
            blockEditor: editor
        }

        NoteBlockEditor {
            id: editor
            Layout.fillWidth: true
            Layout.fillHeight: true
            blockModel: noteBlockModel
            editorBackend: noteEditor
            platformBackend: desktopEditorPlatform
            onFindRequested: findBar.open()
        }
    }
}
