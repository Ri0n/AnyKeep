pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Page {
    id: root

    required property var editor
    property double deleteRequestId: 0
    // The Android keyboard may overlap the Qt window instead of resizing it.
    // Work in content-item coordinates so the action strip never hides beneath
    // the IME, with no double offset on devices using adjustResize.
    readonly property real keyboardTop: {
        const area = root.contentItem
        if (!area || !Qt.inputMethod.visible)
            return area ? area.height : 0
        const keyboard = Qt.inputMethod.keyboardRectangle
        if (keyboard.height <= 0)
            return area.height
        const y = area.mapFromItem(null, 0, keyboard.y).y
        return y > 0 ? Math.min(area.height, y) : area.height
    }
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

    MobileNoteActionBar {
        id: actionBar
        objectName: "floatingEditorActionBar"
        parent: root.contentItem
        width: parent.width
        z: 2
        y: Math.max(0, root.keyboardTop - height)
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
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: actionBar.top
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
