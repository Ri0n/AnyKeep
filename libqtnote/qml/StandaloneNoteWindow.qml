pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: root

    property string errorText: ""

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
        enabled: !deleteDialog.visible && !errorDialog.visible
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
        onDeleteRequested: {
            if (standaloneHost.askBeforeDelete && noteEditor.text.trim().length > 0)
                deleteDialog.open()
            else
                standaloneHost.deleteNote()
        }
        onPrintRequested: desktopNoteActions.printNote()
        onExportRequested: desktopNoteActions.exportNote()
        onPinRequested: standaloneHost.pinNote()
        onAlwaysOnTopRequested: enabled => standaloneHost.setAlwaysOnTop(enabled)
        onMicrophoneRequested: desktopSpeech.start()
        onMicrophoneReleased: desktopSpeech.finish()
        onCheckpointFailed: message => {
            root.errorText = message
            errorDialog.open()
        }
    }

    Connections {
        target: desktopSpeech
        function onRecognizedText(text) { editorPane.insertTextAtCursor(text) }
    }

    Connections {
        target: standaloneHost
        function onOperationFailed(message) {
            root.errorText = message
            errorDialog.open()
        }
    }

    Dialog {
        id: deleteDialog
        anchors.centerIn: parent
        modal: true
        title: qsTr("Delete note")
        standardButtons: Dialog.Yes | Dialog.No
        Label { text: qsTr("Delete this note?") }
        onAccepted: standaloneHost.deleteNote()
    }

    Dialog {
        id: errorDialog
        anchors.centerIn: parent
        modal: true
        title: qsTr("QtNote")
        standardButtons: Dialog.Ok
        Label {
            width: Math.min(420, root.width - 48)
            wrapMode: Text.WordWrap
            text: root.errorText
        }
    }
}
