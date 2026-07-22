pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    required property var editor
    signal backRequested()
    property double deleteRequestId: 0

    function checkpointEditor() {
        Qt.inputMethod.commit()
        blockEditor.flushPendingEditorChanges()
        return mobileApp.saveCurrentNote()
    }

    function deleteEditor() {
        saveTimer.stop()
        Qt.inputMethod.commit()
        blockEditor.flushPendingEditorChanges()
        if (mobileApp.workspace.deleteNote(root.editor.storageId, root.editor.noteId))
            root.backRequested()
    }

    function requestDelete() {
        if (!mobileApp.askBeforeDelete) {
            deleteEditor()
            return
        }
        deleteRequestId = mobileApp.dialogs.confirm(
                    qsTr("Delete note"), qsTr("Delete this note?"),
                    qsTr("Delete"), qsTr("Cancel"), true)
    }

    function closeEditor() {
        saveTimer.stop()
        checkpointEditor()
        if (mobileApp.closeCurrentNote())
            backRequested()
    }

    function shareEditor() {
        checkpointEditor()
        mobileApp.shareCurrentNote()
    }

    function exportEditor() {
        checkpointEditor()
        mobileApp.exportCurrentNote()
    }

    header: Column {
        width: root.width

        EditorToolbar {
            width: parent.width
            editorBackend: root.editor
            blockEditor: blockEditor
            platformBackend: null
            compact: true
            showBackButton: true
            showDeleteButton: true
            showMobileActions: true
            microphoneVisible: mobileApp.androidSpeechEnabled
                               && mobileApp.androidSpeechAvailable
            shortcutVisible: mobileApp.homeScreenShortcutAvailable
                             && root.editor && root.editor.noteId.length > 0
            onBackRequested: root.closeEditor()
            onDeleteRequested: root.requestDelete()
            onFindRequested: findBar.open()
            onShareRequested: root.shareEditor()
            onExportRequested: root.exportEditor()
            onMicrophoneRequested: mobileApp.requestSpeechRecognition()
            onAddToHomeScreenRequested: {
                checkpointEditor()
                mobileApp.addCurrentNoteToHomeScreen()
            }
        }

        NoteFindBar {
            id: findBar
            width: parent.width
            blockEditor: blockEditor
        }
    }

    NoteBlockEditor {
        id: blockEditor
        anchors.fill: parent
        blockModel: root.editor ? root.editor.blockModel : null
        editorBackend: root.editor
        platformBackend: null
        onCountChanged: saveTimer.restart()
        onFindRequested: findBar.open()

        Connections {
            target: blockEditor.blockModel
            function onContentsChanged() {
                saveTimer.restart()
            }
        }

        Component.onCompleted: focusInitialEditor()
    }

    Connections {
        target: mobileApp
        function onSpeechRecognized(text) {
            blockEditor.insertTextAtCursor(text)
            saveTimer.restart()
        }
    }

    Connections {
        target: mobileApp.dialogs
        function onCompleted(requestId, accepted) {
            if (requestId !== root.deleteRequestId)
                return
            root.deleteRequestId = 0
            if (accepted)
                root.deleteEditor()
        }
    }

    Timer {
        id: saveTimer
        interval: 1000
        repeat: false
        onTriggered: root.checkpointEditor()
    }
}
