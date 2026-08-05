import QtQuick

QtObject {
    id: controller
    required property var page
    required property var workspace
    required property var noteSelection
    required property var editorPanel
    required property var editorPane
    required property var foldersPage
    required property var noteContextMenu
    required property var storageContextMenu
    required property var deleteDialog

    function flushEditorChanges() {
        Qt.inputMethod.commit()
        if (editorPanel.visible && workspace.currentEditor)
            editorPane.blockEditor.flushPendingEditorChanges()
    }

    function checkpointEditor() {
        flushEditorChanges()
        return workspace.saveCurrentNote()
    }

    function reloadEditor() {
        if (!workspace.currentEditor || workspace.currentEditor.dirty)
            return false
        return workspace.reloadCurrentNote()
    }

    function closeWorkspace() {
        flushEditorChanges()
        return workspace.closeCurrentNote()
    }

    function insertionRowAtPoint(x, y) {
        if (!page.embeddedEditor || !workspace.currentEditor)
            return -1
        const editor = editorPane.blockEditor
        const point = editor.mapFromItem(page, x, y)
        if (point.x < 0 || point.y < 0 || point.x >= editor.width || point.y >= editor.height)
            return -1
        return editor.insertionRowAtPoint(point.x, point.y)
    }

    function clearPendingBodyFind() {
        page.pendingBodyFindStorageId = ""
        page.pendingBodyFindNoteId = ""
        page.pendingBodyFindQuery = ""
    }

    function openPendingBodyFindIfReady() {
        if (!page.embeddedEditor || page.pendingBodyFindQuery.length === 0 || !workspace.currentEditor
                || workspace.currentStorageId !== page.pendingBodyFindStorageId
                || workspace.currentNoteId !== page.pendingBodyFindNoteId)
            return false
        const query = page.pendingBodyFindQuery
        clearPendingBodyFind()
        Qt.callLater(function() { editorPane.openFind(query, true) })
        return true
    }

    function selectNote(storageId, noteId, title) {
        if (workspace.currentEditor && !checkpointEditor())
            return false
        page.selectedStorageId = storageId
        page.selectedNoteId = noteId
        page.selectedTitle = title
        if (page.embeddedEditor && workspace.noteMatchesBodySearch(storageId, noteId)) {
            page.pendingBodyFindStorageId = storageId
            page.pendingBodyFindNoteId = noteId
            page.pendingBodyFindQuery = workspace.searchText.trim()
        } else {
            clearPendingBodyFind()
        }
        const opened = workspace.openNote(storageId, noteId)
        if (!opened) {
            clearPendingBodyFind()
            return false
        }
        openPendingBodyFindIfReady()
        return true
    }

    function createNote() {
        if (page.viewMode === page.foldersMode)
            return foldersPage.createNoteInSelectedFolder()
        if (workspace.currentEditor && !checkpointEditor())
            return false
        return workspace.createNote(page.selectedStorageId)
    }

    function openStandalone(storageId, noteId) {
        if (workspace.currentEditor && !checkpointEditor())
            return false
        return workspace.openStandalone(storageId, noteId)
    }

    function requestDelete(storageId, noteId, title) {
        page.selectedStorageId = storageId
        page.selectedNoteId = noteId
        page.selectedTitle = title
        if (workspace.isRecycledNote(storageId, noteId))
            return requestPermanentDeletion([{ storageId: storageId, noteId: noteId, title: title }])
        if (workspace.currentEditor && !checkpointEditor())
            return false
        return workspace.trashNote(storageId, noteId)
    }

    function shouldConfirmPermanentDeletion() {
        if (!page.confirmDelete)
            return false
        return !workspace || typeof workspace.askBeforePermanentDelete !== "function"
                || workspace.askBeforePermanentDelete()
    }

    function requestPermanentDeletion(notes) {
        if (!notes || notes.length === 0)
            return false
        page.pendingPermanentDeletionNotes = notes.slice()
        page.pendingRecycleNotes = []
        if (shouldConfirmPermanentDeletion()) {
            page.dontAskAgainForPermanentDeletion = false
            deleteDialog.open()
            return true
        }
        return commitPermanentDeletion()
    }

    function commitPermanentDeletion() {
        if ((!page.pendingPermanentDeletionNotes || page.pendingPermanentDeletionNotes.length === 0)
                && (!page.pendingRecycleNotes || page.pendingRecycleNotes.length === 0))
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false

        let changed = false
        for (const note of page.pendingRecycleNotes) {
            if (workspace.trashNote(note.storageId, note.noteId))
                changed = true
        }
        for (const note of page.pendingPermanentDeletionNotes) {
            if (workspace.deleteNote(note.storageId, note.noteId))
                changed = true
        }
        page.pendingPermanentDeletionNotes = []
        page.pendingRecycleNotes = []
        if (changed)
            noteSelection.clear()
        return changed
    }

    function handleNotesDroppedOutside(notes) {
        if (!notes || notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false

        const recycled = []
        const recycle = []
        for (const note of notes) {
            if (workspace.isRecycledNote(note.storageId, note.noteId))
                recycled.push(note)
            else
                recycle.push(note)
        }
        if (recycled.length > 0) {
            page.pendingPermanentDeletionNotes = recycled
            page.pendingRecycleNotes = recycle
            if (shouldConfirmPermanentDeletion()) {
                page.dontAskAgainForPermanentDeletion = false
                deleteDialog.open()
                return true
            }
            return commitPermanentDeletion()
        }
        let changed = false
        for (const note of recycle) {
            if (workspace.trashNote(note.storageId, note.noteId))
                changed = true
        }
        return changed
    }

    function handleOutsideDrop(payload) {
        return payload && payload.kind === "notes"
                ? handleNotesDroppedOutside(payload.notes) : false
    }

    function selectedNoteDescriptors() {
        const result = []
        const selection = page.selectedNotes || ({})
        for (const selectedKey of Object.keys(selection))
            result.push(Object.assign({}, selection[selectedKey]))
        result.sort(function(left, right) {
            const leftOrder = Number(left.order === undefined ? -1 : left.order)
            const rightOrder = Number(right.order === undefined ? -1 : right.order)
            if (leftOrder !== rightOrder)
                return leftOrder - rightOrder
            const leftKey = String(left.storageId || "") + "\n" + String(left.noteId || "")
            const rightKey = String(right.storageId || "") + "\n" + String(right.noteId || "")
            return leftKey < rightKey ? -1 : (leftKey > rightKey ? 1 : 0)
        })
        return result
    }

    function contextNoteCount() {
        return page.contextMenuNotes ? page.contextMenuNotes.length : 0
    }

    function contextNotesAllRecycled() {
        if (contextNoteCount() === 0)
            return false
        for (const note of page.contextMenuNotes) {
            if (!workspace.isRecycledNote(note.storageId, note.noteId))
                return false
        }
        return true
    }

    function contextNotesAnyRecycled() {
        for (const note of page.contextMenuNotes || []) {
            if (workspace.isRecycledNote(note.storageId, note.noteId))
                return true
        }
        return false
    }

    function contextNotesCommonFolderId() {
        if (contextNoteCount() === 0)
            return ""
        let folderId = workspace.folderIdForNote(page.contextMenuNotes[0].storageId,
                                                  page.contextMenuNotes[0].noteId)
        for (let index = 1; index < page.contextMenuNotes.length; ++index) {
            const note = page.contextMenuNotes[index]
            if (workspace.folderIdForNote(note.storageId, note.noteId) !== folderId)
                return ""
        }
        return folderId
    }

    function contextNotesHaveMixedFolders() {
        if (contextNoteCount() < 2)
            return false
        const firstFolder = workspace.folderIdForNote(page.contextMenuNotes[0].storageId,
                                                       page.contextMenuNotes[0].noteId)
        for (let index = 1; index < page.contextMenuNotes.length; ++index) {
            const note = page.contextMenuNotes[index]
            if (workspace.folderIdForNote(note.storageId, note.noteId) !== firstFolder)
                return true
        }
        return false
    }

    function requestContextNotesDeletion() {
        const notes = (page.contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        const changed = handleNotesDroppedOutside(notes)
        // Keep the selection when a permanent-delete confirmation is still
        // open. Cancelling that dialog must not silently discard the user's
        // multi-selection. commitPermanentDeletion() clears it on success.
        if (changed && page.pendingPermanentDeletionNotes.length === 0
                && page.pendingRecycleNotes.length === 0) {
            noteSelection.clear()
        }
        return changed
    }

    function restoreContextNotes() {
        const notes = (page.contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        let changed = false
        for (const note of notes) {
            if (workspace.isRecycledNote(note.storageId, note.noteId)
                    && workspace.restoreRecycledNote(note.storageId, note.noteId)) {
                changed = true
            }
        }
        if (changed)
            noteSelection.clear()
        return changed
    }

    function assignContextNotesFolder(folderId) {
        const notes = (page.contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        let changed = false
        for (const note of notes) {
            if (workspace.assignNoteFolder(note.storageId, note.noteId, folderId))
                changed = true
        }
        if (changed)
            noteSelection.clear()
        return changed
    }

    function moveContextNotesToStorage(destinationStorageId) {
        if (!destinationStorageId)
            return false
        const notes = []
        for (const note of page.contextMenuNotes || []) {
            if (note.storageId !== destinationStorageId) {
                notes.push({
                    storageId: note.storageId,
                    noteId: note.noteId,
                    title: note.title,
                    order: note.order
                })
            }
        }
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        const changed = workspace.moveNotes(notes, destinationStorageId, "", false)
        if (changed)
            noteSelection.clear()
        return changed
    }

    function copyContextNotesToStorage(destinationStorageId) {
        if (!destinationStorageId)
            return false
        const notes = (page.contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        let changed = false
        for (const note of notes) {
            if (note.storageId !== destinationStorageId
                    && workspace.copyNote(note.storageId, note.noteId, destinationStorageId)) {
                changed = true
            }
        }
        return changed
    }

    function showNoteMenu(storageId, noteId, title, position) {
        page.selectedStorageId = storageId
        page.selectedNoteId = noteId
        page.selectedTitle = title

        const selected = noteSelection.isSelected(storageId, noteId)
        const descriptors = selected ? selectedNoteDescriptors() : []
        page.contextMenuNotes = descriptors.length > 0 ? descriptors : [{
            storageId: String(storageId || ""),
            noteId: String(noteId || ""),
            title: String(title || ""),
            order: -1
        }]

        if (position !== undefined)
            noteContextMenu.popup(page, position)
        else
            noteContextMenu.popup()
    }

    function selectedNoteFolderId() {
        return contextNotesCommonFolderId()
    }

    function assignSelectedNoteFolder(folderId) {
        return assignContextNotesFolder(folderId)
    }

    function showStorageMenu(storageId, title, position) {
        page.selectedStorageId = storageId
        page.selectedNoteId = ""
        page.selectedTitle = title
        if (position !== undefined)
            storageContextMenu.popup(page, position)
        else
            storageContextMenu.popup()
    }
}
