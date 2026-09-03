pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Page {
    id: root

    function createNote() {
        return mobileApp.createNote()
    }

    NotesManagerPage {
        id: manager
        anchors.fill: parent
        anchors.bottomMargin: 0
        workspace: mobileApp.workspace
        platformBackend: mobileApp.editorPlatformBackend
        embeddedEditor: false
        showCreateButton: false
        confirmDelete: mobileApp.askBeforeDelete
        viewMode: recentMode
        touchActions: true
    }

    RoundButton {
        id: addButton
        objectName: "addNoteButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 20
        anchors.bottomMargin: 20
        width: 58
        height: 58
        z: 20
        visible: !manager.selectionMode
        enabled: visible
        text: qsTr("+")
        font.pixelSize: 30
        Accessible.name: qsTr("Add note")
        onClicked: root.createNote()
    }
}
