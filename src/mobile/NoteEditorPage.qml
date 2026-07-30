pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Page {
    id: root

    required property var editor
    signal backRequested()

    function checkpointEditor() {
        return editorPane.checkpointEditor()
    }

    function trashEditor() {
        if (mobileApp.workspace.trashNote(root.editor.storageId, root.editor.noteId))
            root.backRequested()
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

    NoteEditorPane {
        id: editorPane
        anchors.fill: parent
        editor: root.editor
        platformBackend: mobileApp.editorPlatformBackend
        folderWorkspace: mobileApp.workspace
        saveHandler: function() { return mobileApp.saveCurrentNote() }
        closeHandler: function() { return mobileApp.closeCurrentNote() }
        compactToolbar: true
        showBackButton: true
        showDeleteButton: true
        showMobileActions: true
        microphoneVisible: mobileApp.androidSpeechEnabled && mobileApp.androidSpeechAvailable
        shortcutVisible: mobileApp.homeScreenShortcutAvailable
                         && root.editor && root.editor.noteId.length > 0
        onBackRequested: root.closeEditor()
        onDeleteRequested: root.trashEditor()
        onShareRequested: root.shareEditor()
        onExportRequested: root.exportEditor()
        onMicrophoneRequested: mobileApp.requestSpeechRecognition()
        onAddToHomeScreenRequested: {
            if (root.checkpointEditor())
                mobileApp.addCurrentNoteToHomeScreen()
        }
        onCheckpointFailed: message => mobileApp.dialogs.inform(qsTr("QtNote"), message)
    }

    Connections {
        target: mobileApp
        function onSpeechRecognized(text) {
            editorPane.insertTextAtCursor(text)
        }
    }

}
