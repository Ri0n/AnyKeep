import QtQuick
import QtQuick.Controls

Item {
    id: harness
    objectName: "managerInteractionHarness"
    property int movedNotes: workspace.movedNotes
    property string noteDestination: workspace.noteDestination
    property string noteAnchor: workspace.noteAnchor
    property bool noteInsertAfter: workspace.noteInsertAfter
    property int movedStorages: workspace.movedStorages
    property string storageDestination: workspace.storageDestination
    property int storageDestinationRow: workspace.storageDestinationRow
    property string lastDraggedNoteId: ""

    QtObject {
        id: workspace
        objectName: "managerWorkspace"
        property var groupedNotesModel: testNotesModel
        property var recentNotesModel: testNotesModel
        property var folderNotesModel: null
        property bool folderCatalogAvailable: false
        property var currentEditor: null
        property string currentStorageId: ""
        property string currentNoteId: ""
        property string currentTitle: ""
        property string errorString: ""
        property string searchText: ""
        property bool searchInBody: false
        property bool loading: false
        property bool busy: false
        property int noteCount: 1
        property var storages: []
        property int movedNotes: 0
        property string noteDestination: ""
        property string noteAnchor: ""
        property bool noteInsertAfter: false
        property int movedStorages: 0
        property string storageDestination: ""
        property int storageDestinationRow: -1
        property int copiedNotes: 0
        property int assignedNotes: 0
        property int trashedNotes: 0
        property int deletedNotes: 0
        property int restoredNotes: 0
        property bool recycleAll: false
        function saveCurrentNote() { return true }
        function closeCurrentNote() { return true }
        function reloadCurrentNote() { return true }
        function openNote(storageId, noteId) { return true }
        function createNote(storageId) { return true }
        function folderIdForNote(storageId, noteId) {
            return noteId === "note-a2" ? "folder-b" : "folder-a"
        }
        function assignNoteFolder(storageId, noteId, folderId) {
            ++assignedNotes
            return true
        }
        function openStandalone(storageId, noteId) { return true }
        function deleteNote(storageId, noteId) {
            ++deletedNotes
            return true
        }
        function trashNote(storageId, noteId) {
            ++trashedNotes
            return true
        }
        function restoreRecycledNote(storageId, noteId) {
            ++restoredNotes
            return true
        }
        function isRecycledNote(storageId, noteId) { return recycleAll }
        function copyNote(sourceStorageId, noteId, destinationStorageId) {
            ++copiedNotes
            return true
        }
        function moveNote(sourceStorageId, noteId, destinationStorageId) { return true }
        function openStorageSettings(storageId) {}
        function moveNotes(notes, destinationStorageId, anchorNoteId, insertAfter) {
            movedNotes = notes.length
            noteDestination = destinationStorageId
            noteAnchor = anchorNoteId
            noteInsertAfter = insertAfter
            return true
        }
        function moveStorage(sourceStorageId, destinationStorageId) {
            ++movedStorages
            storageDestination = destinationStorageId
            return true
        }
        function moveStorageToRow(sourceStorageId, destinationRow) {
            ++movedStorages
            storageDestinationRow = destinationRow
            return true
        }
    }

    NotesManagerPage {
        id: page
        objectName: "managerPage"
        anchors.fill: parent
        workspace: workspace
        embeddedEditor: false
        showCreateButton: false
        showViewModeSelector: false
        viewMode: groupedByStorageMode
    }
}
