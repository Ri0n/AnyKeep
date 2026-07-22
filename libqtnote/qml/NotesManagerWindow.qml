import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: window

    width: 1180
    height: 760
    minimumWidth: 720
    minimumHeight: 440
    visible: false
    title: qsTr("Note Manager (%1)").arg(notesWorkspace.noteCount)
    property alias navigationWidth: managerPage.navigationWidth

    function insertionRowAtPoint(x, y) { return managerPage.insertionRowAtPoint(x, y) }
    function flushEditorChanges() { managerPage.flushEditorChanges() }
    function closeWorkspace() { return managerPage.closeWorkspace() }

    NotesManagerPage {
        id: managerPage
        anchors.fill: parent
        workspace: notesWorkspace
        platformBackend: desktopEditorPlatform
        embeddedEditor: true
    }

    onActiveChanged: {
        if (active)
            managerPage.reloadEditor()
        else
            managerPage.checkpointEditor()
    }

    onClosing: function(close) {
        if (!managerPage.closeWorkspace())
            close.accepted = false
    }
}
