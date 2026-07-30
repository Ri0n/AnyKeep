pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

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

    Shortcut {
        sequence: StandardKey.Cancel
        context: Qt.WindowShortcut
        enabled: true
        onActivated: standaloneHost.requestClose()
    }

    NoteEditorPane {
        id: editorPane
        anchors.fill: parent
        editor: noteEditor
        platformBackend: desktopEditorPlatform
        showDeleteButton: true
        showDesktopActions: true
        microphoneVisible: desktopSpeech.available
        microphoneBusy: desktopSpeech.busy
        microphoneHoldToRecord: true
        pinActionsVisible: true
        pinVisible: standaloneHost.pinAvailable
        alwaysOnTop: standaloneHost.alwaysOnTop
        onDeleteRequested: standaloneHost.trashNote()
        onPrintRequested: desktopNoteActions.printNote()
        onExportRequested: desktopNoteActions.exportNote()
        onPinRequested: standaloneHost.pinNote()
        onAlwaysOnTopRequested: enabled => standaloneHost.setAlwaysOnTop(enabled)
        onMicrophoneRequested: desktopSpeech.start()
        onMicrophoneReleased: desktopSpeech.finish()
        onCheckpointFailed: message => standaloneHost.reportError(message)
    }

    Connections {
        target: desktopSpeech
        function onRecognizedText(text) { editorPane.insertTextAtCursor(text) }
    }

}
