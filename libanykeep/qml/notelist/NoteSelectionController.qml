pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    id: controller

    property var selectedNotes: ({})
    property string anchorKey: ""

    function key(storageId, noteId) {
        return String(storageId) + "\n" + String(noteId)
    }

    function descriptor(item) {
        if (!item)
            return ({})
        return {
            storageId: String(item.storageId || ""),
            noteId: String(item.noteId || ""),
            title: String(item.title || ""),
            groupId: String(item.groupId || ""),
            folderId: String(item.folderId || ""),
            order: Number(item.rowIndex === undefined ? -1 : item.rowIndex)
        }
    }

    function isSelected(storageId, noteId) {
        return selectedNotes[key(storageId, noteId)] !== undefined
    }

    function setSelected(item, selected) {
        if (!item || !item.noteRow)
            return
        const copy = Object.assign({}, selectedNotes)
        const itemKey = key(item.storageId, item.noteId)
        if (selected)
            copy[itemKey] = descriptor(item)
        else
            delete copy[itemKey]
        selectedNotes = copy
    }

    function clear() {
        selectedNotes = ({})
        anchorKey = ""
    }

    // Returns true when the click is a plain activation and the caller should
    // open the note. Ctrl and Shift only update the shared selection.
    function select(item, modifiers, orderedItems) {
        if (!item || !item.noteRow)
            return false

        const itemKey = key(item.storageId, item.noteId)
        const control = Boolean(modifiers & Qt.ControlModifier)
        const shift = Boolean(modifiers & Qt.ShiftModifier)
        const items = orderedItems || []

        if (shift && anchorKey.length > 0) {
            let anchorIndex = -1
            let targetIndex = -1
            for (let index = 0; index < items.length; ++index) {
                const candidate = items[index]
                if (!candidate || !candidate.noteRow)
                    continue
                const candidateKey = key(candidate.storageId, candidate.noteId)
                if (candidateKey === anchorKey)
                    anchorIndex = index
                if (candidateKey === itemKey)
                    targetIndex = index
            }
            if (anchorIndex >= 0 && targetIndex >= 0) {
                const copy = control ? Object.assign({}, selectedNotes) : ({})
                const first = Math.min(anchorIndex, targetIndex)
                const last = Math.max(anchorIndex, targetIndex)
                for (let index = first; index <= last; ++index) {
                    const candidate = items[index]
                    if (!candidate || !candidate.noteRow)
                        continue
                    copy[key(candidate.storageId, candidate.noteId)] = descriptor(candidate)
                }
                selectedNotes = copy
                return false
            }
        }

        if (control) {
            setSelected(item, !isSelected(item.storageId, item.noteId))
            anchorKey = itemKey
            return false
        }

        const single = ({})
        single[itemKey] = descriptor(item)
        selectedNotes = single
        anchorKey = itemKey
        return true
    }

    function notesForDrag(item, orderedItems) {
        if (!item || !item.noteRow)
            return []
        if (!isSelected(item.storageId, item.noteId))
            return [descriptor(item)]

        const result = []
        const seen = ({})
        for (const candidate of orderedItems || []) {
            if (!candidate || !candidate.noteRow
                    || !isSelected(candidate.storageId, candidate.noteId)) {
                continue
            }
            const candidateKey = key(candidate.storageId, candidate.noteId)
            result.push(descriptor(candidate))
            seen[candidateKey] = true
        }
        // A selected delegate may have scrolled out of the reusable view. Keep
        // it in the payload even though only visible rows can be snapshotted.
        for (const selectedKey of Object.keys(selectedNotes)) {
            if (!seen[selectedKey])
                result.push(Object.assign({}, selectedNotes[selectedKey]))
        }
        result.sort(function(left, right) { return Number(left.order) - Number(right.order) })
        return result
    }

    function sourceItemsForDrag(item, visibleItems) {
        if (!item || !item.noteRow)
            return []
        if (!isSelected(item.storageId, item.noteId))
            return [item]
        const result = []
        for (const candidate of visibleItems || []) {
            if (candidate && candidate.noteRow
                    && isSelected(candidate.storageId, candidate.noteId)) {
                result.push(candidate)
            }
        }
        return result
    }
}
