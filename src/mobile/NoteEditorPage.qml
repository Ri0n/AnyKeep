pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Page {
    id: root

    required property var editor
    property double deleteRequestId: 0
    signal backRequested()

    function checkpointEditor() { return editorPane.checkpointEditor() }
    function trashEditor() {
        if (mobileApp.workspace.trashNote(root.editor.storageId, root.editor.noteId))
            root.backRequested()
    }
    function requestTrashEditor() {
        if (!mobileApp.askBeforeDelete) {
            trashEditor()
            return
        }
        deleteRequestId = mobileApp.dialogs.confirm(
                    qsTr("Delete note"),
                    qsTr("Move this note to Trash?"),
                    qsTr("Delete"), qsTr("Cancel"), true)
    }
    function closeEditor() {
        if (editorPane.closeEditor())
            backRequested()
    }
    function shareEditor() {
        if (checkpointEditor())
            mobileApp.shareCurrentNote()
    }
    function exportEditor() {
        if (checkpointEditor())
            mobileApp.exportCurrentNote()
    }

    EditorActionController {
        id: editorActions
        editorBackend: root.editor
        blockEditor: editorPane.blockEditor
        platformBackend: mobileApp.editorPlatformBackend
    }

    header: MobileNoteTopBar {
        editorBackend: root.editor
        actions: editorActions
        shortcutVisible: mobileApp.homeScreenShortcutAvailable
                         && root.editor && root.editor.noteId.length > 0
        onBackRequested: root.closeEditor()
        onShareRequested: root.shareEditor()
        onExportRequested: root.exportEditor()
        onFindRequested: editorPane.openFind()
        onDeleteRequested: root.requestTrashEditor()
        onAddToHomeScreenRequested: {
            if (root.checkpointEditor())
                mobileApp.addCurrentNoteToHomeScreen()
        }
    }

    footer: MobileNoteActionBar {
        editorBackend: root.editor
        actions: editorActions
        voiceAvailable: mobileApp.microphoneAvailable
                        && (!mobileApp.speechController || !mobileApp.speechController.busy)
        voiceBusy: mobileApp.microphoneBusy
        voiceRecording: mobileApp.microphoneRecording
        voiceModeSwitchVisible: mobileApp.microphoneModeSwitchVisible
        voiceMode: mobileApp.microphoneMode
        voiceStatus: mobileApp.speechController ? mobileApp.speechController.statusText : ""
        onVoiceRequested: mobileApp.requestVoiceInput(editorPane.blockEditor.insertionBlockIndex())
        onVoiceCancelRequested: {
            if (mobileApp.speechController)
                mobileApp.speechController.cancel()
        }
        onVoiceModeRequested: mode => mobileApp.setMicrophoneMode(mode)
    }

    NoteEditorPane {
        id: editorPane
        anchors.fill: parent
        editor: root.editor
        platformBackend: mobileApp.editorPlatformBackend
        folderWorkspace: mobileApp.workspace
        saveHandler: function() { return mobileApp.saveCurrentNote() }
        closeHandler: function() { return mobileApp.closeCurrentNote() }
        toolbarVisible: false
        compactToolbar: true
        audioTranscriptionController: mobileApp.speechController
        onCheckpointFailed: message => mobileApp.dialogs.inform(qsTr("AnyKeep"), message)
    }

    Connections {
        target: mobileApp
        function onSpeechRecognized(text) { editorPane.insertTextAtCursor(text) }
    }

    Connections {
        target: mobileApp.dialogs
        function onCompleted(requestId, accepted) {
            if (requestId !== root.deleteRequestId)
                return
            root.deleteRequestId = 0
            if (accepted)
                root.trashEditor()
        }
    }
}
