pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Page {
    id: root

    function createNote() {
        return mobileApp.createNote()
    }

    NotesManagerPage {
        anchors.fill: parent
        workspace: mobileApp.workspace
        platformBackend: null
        embeddedEditor: false
        showCreateButton: false
        confirmDelete: mobileApp.askBeforeDelete
        viewMode: recentMode
        touchActions: true
    }
}
