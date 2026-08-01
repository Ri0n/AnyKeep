pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var editor
    property var platformBackend: null
    // A NotesWorkspaceController when this editor belongs to a workspace.
    // Standalone legacy editor hosts intentionally leave it null until they
    // can participate in the same draft-aware folder assignment lifecycle.
    property var folderWorkspace: null
    property bool compactToolbar: false
    property bool toolbarVisible: true
    property var audioTranscriptionController: null
    property bool showBackButton: false
    property bool showDeleteButton: true
    property bool showMobileActions: false
    property bool showDesktopActions: false
    property bool microphoneVisible: false
    property bool microphoneBusy: false
    property bool microphoneRecording: false
    property bool microphoneHoldToRecord: false
    property bool microphoneModeSwitchVisible: false
    property int microphoneMode: 0
    property bool shortcutVisible: false
    property bool pinActionsVisible: false
    property bool pinVisible: false
    property bool alwaysOnTop: false
    property int autosaveInterval: 1000
    property bool autosaveEnabled: true
    property var saveHandler: null
    property var closeHandler: null
    property alias blockEditor: editorView
    readonly property bool findVisible: findBar.visible

    signal backRequested()
    signal deleteRequested()
    signal shareRequested()
    signal exportRequested()
    signal printRequested()
    signal pinRequested()
    signal alwaysOnTopRequested(bool enabled)
    signal microphoneRequested()
    signal microphoneReleased()
    signal microphoneModeRequested(int mode)
    signal addToHomeScreenRequested()
    signal checkpointFailed(string message)

    function flushPendingEditorChanges() {
        Qt.inputMethod.commit()
        editorView.flushPendingEditorChanges()
    }

    function checkpointEditor() {
        if (!root.editor)
            return true
        flushPendingEditorChanges()
        const saved = root.saveHandler ? root.saveHandler() : root.editor.save()
        if (!saved)
            root.checkpointFailed(root.editor.errorString)
        return saved
    }

    function closeEditor() {
        saveTimer.stop()
        flushPendingEditorChanges()
        const closed = root.closeHandler ? root.closeHandler() : (root.editor ? root.editor.close() : true)
        if (!closed && root.editor)
            root.checkpointFailed(root.editor.errorString)
        return closed
    }

    function discardAndClose() {
        saveTimer.stop()
        return root.editor ? root.editor.discardAndClose() : true
    }

    function focusInitialEditor() {
        editorView.focusInitialEditor()
    }

    function insertTextAtCursor(text) {
        const inserted = editorView.insertTextAtCursor(text)
        if (inserted)
            saveTimer.restart()
        return inserted
    }

    function captureEditorState() {
        return editorView.captureEditorState()
    }

    function restoreEditorState(state) {
        return editorView.restoreEditorState(state)
    }

    function prepareForHistoryRestore() {
        editorView.prepareForHistoryRestore()
    }

    function documentHistoryOwnsFocus() {
        return editorView.documentHistoryOwnsFocus()
    }

    function copyActiveSelection() {
        return editorView.copyActiveSelection()
    }

    function cutActiveSelection() {
        return editorView.cutActiveSelection()
    }

    function pasteClipboard() {
        return editorView.pasteClipboard()
    }

    function openFind(query, findImmediately) {
        const requestedQuery = query === undefined || query === null ? "" : String(query)
        if (requestedQuery.length > 0)
            findBar.query = requestedQuery
        findBar.open()
        if (findImmediately && requestedQuery.length > 0) {
            Qt.callLater(function() {
                editorView.resetFind()
                editorView.findNext(requestedQuery, false)
            })
        }
    }

    function insertionRowAtPoint(x, y) {
        const point = editorView.mapFromItem(root, x, y)
        if (point.x < 0 || point.y < 0 || point.x >= editorView.width || point.y >= editorView.height)
            return editorView.blockModel ? editorView.blockModel.rowCount() : 0
        return editorView.insertionRowAtPoint(point.x, point.y)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        EditorToolbar {
            visible: root.toolbarVisible
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            editorBackend: root.editor
            blockEditor: editorView
            platformBackend: root.platformBackend
            folderWorkspace: root.folderWorkspace
            compact: root.compactToolbar
            showBackButton: root.showBackButton
            showDeleteButton: root.showDeleteButton
            showMobileActions: root.showMobileActions
            showDesktopActions: root.showDesktopActions
            microphoneVisible: root.microphoneVisible
            microphoneBusy: root.microphoneBusy
            microphoneRecording: root.microphoneRecording
            microphoneHoldToRecord: root.microphoneHoldToRecord
            microphoneModeSwitchVisible: root.microphoneModeSwitchVisible
            microphoneMode: root.microphoneMode
            shortcutVisible: root.shortcutVisible
            pinActionsVisible: root.pinActionsVisible
            pinVisible: root.pinVisible
            alwaysOnTop: root.alwaysOnTop
            onBackRequested: root.backRequested()
            onDeleteRequested: root.deleteRequested()
            onFindRequested: findBar.open()
            onShareRequested: root.shareRequested()
            onExportRequested: root.exportRequested()
            onPrintRequested: root.printRequested()
            onPinRequested: root.pinRequested()
            onAlwaysOnTopRequested: enabled => root.alwaysOnTopRequested(enabled)
            onMicrophoneRequested: root.microphoneRequested()
            onMicrophoneReleased: root.microphoneReleased()
            onMicrophoneModeRequested: mode => root.microphoneModeRequested(mode)
            onAddToHomeScreenRequested: root.addToHomeScreenRequested()
        }

        NoteFindBar {
            id: findBar
            Layout.fillWidth: true
            blockEditor: editorView
        }

        NoteBlockEditor {
            id: editorView
            Layout.fillWidth: true
            Layout.fillHeight: true
            blockModel: root.editor ? root.editor.blockModel : null
            editorBackend: root.editor
            platformBackend: root.platformBackend
            audioTranscriptionController: root.audioTranscriptionController
            onCountChanged: {
                if (root.autosaveEnabled)
                    saveTimer.restart()
            }
            onFindRequested: findBar.open()

            Connections {
                target: editorView.blockModel
                function onContentsChanged() {
                    if (root.autosaveEnabled)
                        saveTimer.restart()
                }
            }

            Component.onCompleted: focusInitialEditor()
        }
    }

    Timer {
        id: saveTimer
        interval: root.autosaveInterval
        repeat: false
        onTriggered: root.checkpointEditor()
    }
}
