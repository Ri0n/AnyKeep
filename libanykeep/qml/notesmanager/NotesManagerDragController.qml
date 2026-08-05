import QtQuick

QtObject {
    id: controller
    required property var page
    required property var workspace
    required property var groupedNotes

    function groupedItemAtRow(row) {
        return groupedNotes.itemAtRow(row)
    }

    function cancelGroupedDrag() {
        groupedNotes.cancelDrag()
    }

    function sharedGroupedNoteBoundaries(view, payload, delegates) {
        const remaining = view.remainingItems(delegates)
        return view.trailingBoundaries(delegates, function(item) {
            const owner = item || (remaining.length > 0 ? remaining[0] : null)
            return {
                storageId: owner ? owner.storageId : "",
                anchorNoteId: owner && owner.noteRow ? owner.noteId : "",
                insertAfter: Boolean(item && item.noteRow)
            }
        }, true)
    }

    function sharedGroupedStorageBoundaries(view, payload, delegates) {
        const remainingItems = view.remainingItems(delegates)
        const groups = []
        for (const item of remainingItems) {
            let group = groups.length > 0 ? groups[groups.length - 1] : null
            if (!group || group.storageId !== item.storageId) {
                group = {
                    storageId: item.storageId,
                    first: item,
                    last: item
                }
                groups.push(group)
            } else {
                group.last = item
            }
        }
        const boundaries = []
        if (groups.length > 0) {
            const leading = view.boundaryByOrder(groups[0].first, false, {
                storageDestinationRow: 0,
                rootGroupDrop: true
            })
            if (leading)
                boundaries.push(leading)
        }
        for (let index = 0; index < groups.length; ++index) {
            const after = view.boundaryByOrder(groups[index].last, true, {
                storageDestinationRow: index + 1,
                rootGroupDrop: true
            })
            if (after)
                boundaries.push(after)
        }
        return boundaries
    }

    function sharedGroupedBoundaries(view, payload, delegates) {
        return payload && payload.kind === "group"
                ? sharedGroupedStorageBoundaries(view, payload, delegates)
                : sharedGroupedNoteBoundaries(view, payload, delegates)
    }

    function sharedGroupedDirectTarget(view, payload, pointerX, pointerY) {
        return null
    }

    function commitSharedGroupedDrop(payload, boundary, directTarget) {
        if (!payload)
            return false
        if (payload.kind === "group") {
            return boundary
                    && workspace.moveStorageToRow(
                        payload.groupId,
                        Number(boundary.storageDestinationRow))
        }
        if (payload.kind !== "notes" || !payload.notes || payload.notes.length === 0)
            return false
        if (workspace.currentEditor && !page.checkpointEditor())
            return false
        const storageId = directTarget
                ? String(directTarget.storageId)
                : String(boundary ? boundary.storageId || "" : "")
        const anchorNoteId = directTarget
                ? "" : String(boundary ? boundary.anchorNoteId || "" : "")
        const insertAfter = directTarget
                ? false : Boolean(boundary && boundary.insertAfter)
        return workspace.moveNotes(payload.notes, storageId, anchorNoteId, insertAfter)
    }

    function recentNoteBoundaries(view, payload, delegates) {
        return view.boundaries(delegates, function(item, after) {
            return {
                storageId: item ? String(item.storageId) : "",
                anchorNoteId: item ? String(item.noteId) : "",
                insertAfter: Boolean(after)
            }
        })
    }

    function canReorderRecentNote(item) {
        if (!item || !item.noteRow)
            return false
        const storageId = String(item.storageId)
        for (const storage of workspace.storages || []) {
            if (String(storage.storageId) === storageId)
                return Boolean(storage.supportsNoteReordering)
        }
        return false
    }

    function commitRecentDrop(payload, boundary) {
        if (!payload || payload.kind !== "notes" || !payload.notes || payload.notes.length === 0
                || !boundary || !boundary.storageId || !boundary.anchorNoteId) {
            return false
        }
        if (workspace.currentEditor && !page.checkpointEditor())
            return false
        return workspace.reorderRecentNotes(payload.notes,
                                            String(boundary.storageId),
                                            String(boundary.anchorNoteId),
                                            Boolean(boundary.insertAfter))
    }
}
