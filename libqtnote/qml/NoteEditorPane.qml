pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var editor
    property var platformBackend: null
    property bool compactToolbar: false
    property bool showBackButton: false
    property bool showDeleteButton: true
    property bool showMobileActions: false
    property bool showDesktopActions: false
    property bool microphoneVisible: false
    property bool microphoneBusy: false
    property bool microphoneHoldToRecord: false
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

    function openFind() {
        findBar.open()
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
            Layout.fillWidth: true
            editorBackend: root.editor
            blockEditor: editorView
            platformBackend: root.platformBackend
            compact: root.compactToolbar
            showBackButton: root.showBackButton
            showDeleteButton: root.showDeleteButton
            showMobileActions: root.showMobileActions
            showDesktopActions: root.showDesktopActions
            microphoneVisible: root.microphoneVisible
            microphoneBusy: root.microphoneBusy
            microphoneHoldToRecord: root.microphoneHoldToRecord
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
