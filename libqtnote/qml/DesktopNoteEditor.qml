pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    function flushPendingEditorChanges() { editorPane.flushPendingEditorChanges() }
    function insertTextAtCursor(text) { return editorPane.insertTextAtCursor(text) }
    function focusInitialEditor() { editorPane.focusInitialEditor() }
    function insertionRowAtPoint(x, y) { return editorPane.insertionRowAtPoint(x, y) }
    function captureEditorState() { return editorPane.captureEditorState() }
    function restoreEditorState(state) { return editorPane.restoreEditorState(state) }
    function prepareForHistoryRestore() { editorPane.prepareForHistoryRestore() }
    function documentHistoryOwnsFocus() { return editorPane.documentHistoryOwnsFocus() }
    function copyActiveSelection() { return editorPane.copyActiveSelection() }
    function cutActiveSelection() { return editorPane.cutActiveSelection() }
    function pasteClipboard() { return editorPane.pasteClipboard() }
    function openFind() { editorPane.openFind() }
    function insertTableBlock() { return editorPane.blockEditor.insertTableBlock() }
    function insertListBlock(type) { return editorPane.blockEditor.insertListBlock(type) }

    NoteEditorPane {
        id: editorPane
        anchors.fill: parent
        editor: noteEditor
        platformBackend: desktopEditorPlatform
        showDeleteButton: false
        showDesktopActions: false
    }
}
