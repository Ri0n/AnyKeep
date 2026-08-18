pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: root

    required property var noteEditor
    required property var desktopEditorPlatform
    required property var desktopNoteActions
    required property var desktopSpeech
    required property var standaloneHost

    function flushPendingEditorChanges() { editorPane.flushPendingEditorChanges() }
    function insertTextAtCursor(text) { return editorPane.insertTextAtCursor(text) }
    function insertDroppedTextAtPoint(text, x, y, codeLanguage, detectedCode) {
        return editorPane.insertDroppedTextAtPoint(text, x, y, codeLanguage, detectedCode)
    }
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
        sequences: [StandardKey.Cancel]
        context: Qt.WindowShortcut
        enabled: true
        onActivated: standaloneHost.requestClose()
    }

    NoteEditorPane {
        id: editorPane
        anchors.fill: parent
        editor: noteEditor
        platformBackend: desktopEditorPlatform
        audioTranscriptionController: desktopSpeech
        showDeleteButton: !standaloneHost.tutorial
        showDesktopActions: !standaloneHost.tutorial
        microphoneVisible: !standaloneHost.tutorial && desktopSpeech.available
        microphoneBusy: desktopSpeech.busy
        microphoneRecording: desktopSpeech.recording
        microphoneHoldToRecord: true
        microphoneModeSwitchVisible: desktopSpeech.modeSwitchVisible
        microphoneMode: desktopSpeech.mode
        pinActionsVisible: !standaloneHost.tutorial
        pinVisible: standaloneHost.pinAvailable
        alwaysOnTop: standaloneHost.alwaysOnTop
        onDeleteRequested: standaloneHost.trashNote()
        onPrintRequested: desktopNoteActions.printNote()
        onExportRequested: desktopNoteActions.exportNote()
        onPinRequested: standaloneHost.pinNote()
        onAlwaysOnTopRequested: enabled => standaloneHost.setAlwaysOnTop(enabled)
        onMicrophoneRequested: desktopSpeech.start(editorPane.blockEditor.insertionBlockIndex())
        onMicrophoneReleased: desktopSpeech.finish()
        onMicrophoneModeRequested: mode => desktopSpeech.setMode(mode)
        onCheckpointFailed: message => standaloneHost.reportError(message)
    }

    Connections {
        target: desktopSpeech
        function onRecognizedText(text) { editorPane.insertRecognizedText(text) }
    }

}
