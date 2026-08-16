.pragma library

function handleKey(host, controller, event, cell, itemIndex)
{
    const blocked         = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
    const primaryModifier = event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)
    const listShortcut    = primaryModifier && event.modifiers & Qt.ShiftModifier && !(event.modifiers & Qt.AltModifier)
    if (listShortcut
        && (event.key === Qt.Key_7 || event.key === Qt.Key_Ampersand || event.key === Qt.Key_8
            || event.key === Qt.Key_Asterisk || event.key === Qt.Key_9 || event.key === Qt.Key_ParenLeft))
    {
        const type = event.key === Qt.Key_7 || event.key === Qt.Key_Ampersand ? 5
            : event.key === Qt.Key_8 || event.key === Qt.Key_Asterisk         ? 1
                                                                              : 2
        return controller.runEditTransaction("convert-list-level", function() {
            controller.blockModel.convertListLevel(host.block.index, itemIndex, type)
            return true
        })
    }
    if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) && !blocked) {
        let first = controller.wholeDocumentSelected ? 0 : itemIndex
        let last = controller.wholeDocumentSelected ? host.itemCount() - 1 : itemIndex
        const selected = host.selectedItemRange()
        if (selected.first >= 0)
        {
            first = Math.min(first, selected.first)
            last  = Math.max(last, selected.last)
        }
        return controller.runEditTransaction("indent-list-items", function() {
            controller.blockModel.indentListItems(
                host.block.index, first, last,
                event.key === Qt.Key_Backtab || event.modifiers & Qt.ShiftModifier ? -1 : 1)
            return true
        })
    }
    if (!blocked && !(event.modifiers & Qt.ShiftModifier) && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
        if (event.key === Qt.Key_Down && itemIndex + 1 >= host.itemCount()) {
            controller.focusFollowingBlock(host.block.index, !event.isAutoRepeat)
            return true
        }
        const rectangle = cell.positionToRectangle(cell.cursorPosition)
        const probeY    = event.key === Qt.Key_Up ? rectangle.y - rectangle.height : rectangle.y + rectangle.height + 1
        const probe    = cell.positionToRectangle(cell.positionAt(rectangle.x, probeY))
        const boundary = event.key === Qt.Key_Up ? probe.y >= rectangle.y - 0.5 : probe.y <= rectangle.y + 0.5
        if (boundary)
        {
            const target = itemIndex + (event.key === Qt.Key_Up ? -1 : 1)
            if (target >= 0 && target < host.itemCount())
            host.focusItemVertically(target, rectangle.x, event.key === Qt.Key_Up)
            else if (event.key === Qt.Key_Up)
            controller.focusPrecedingBlock(host.block.index)
            else if (event.key === Qt.Key_Down)
            controller.focusFollowingBlock(host.block.index, !event.isAutoRepeat)
            return true
        }
    }
    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !blocked
        && (controller.touchMode || !(event.modifiers & Qt.ShiftModifier))) {
        return controller.runEditTransaction("split-list-item", function() {
            const position = cell.cursorPosition
            if (cell.length === 0 && position === 0 && itemIndex + 1 === host.itemCount())
            {
                if (host.itemCount() === 1) {
                    controller.blockModel.convertListToText(host.block.index)
                    controller.focusBlock(host.block.index)
                } else {
                    controller.blockModel.removeListItem(host.block.index, itemIndex)
                    controller.focusFollowingBlock(host.block.index, true)
                }
                return true
            }
            const left  = cell.markdownRange(0, position)
            const right = cell.markdownRange(position, cell.length)
            controller.blockModel.setListItem(host.block.index, itemIndex, left)
            controller.blockModel.insertListItem(host.block.index, itemIndex + 1, right)
            host.focusItem(itemIndex + 1, 0)
            return true
        })
    }
    if (event.key === Qt.Key_Delete && !blocked && !(event.modifiers & Qt.ShiftModifier)
        && cell.selectionStart === cell.selectionEnd && cell.cursorPosition === cell.length) {
        return controller.runEditTransaction("merge-list-items", function() {
            const position  = cell.cursorPosition
            const viewportY = controller.contentY
            controller.prepareForStructuralMutation()
            let merged = false
            if (itemIndex + 1 < host.itemCount())
            {
                controller.blockModel.mergeListItemWithNext(host.block.index, itemIndex)
                merged = true
            }
            else { merged = controller.blockModel.mergeListItemWithFollowingBlock(
                       host.block.index, itemIndex) } host.focusItem(itemIndex, position, true, viewportY)
            if (merged)
            {
                Qt.callLater(function() {
                    const row    = host.rowAt(itemIndex)
                    const editor = row ? row.listEditor : null
                    if (editor && editor.sourceTextPending && typeof editor.applyPendingSourceText === "function")
                    editor.applyPendingSourceText()
                })
            }
            return merged
        })
    }
    if (event.key === Qt.Key_Backspace && !blocked && cell.selectionStart === cell.selectionEnd
        && cell.cursorPosition === 0) {
        if (cell.length === 0) {
            return controller.runEditTransaction("remove-list-item", function() {
                const viewportY = controller.contentY
                controller.prepareForStructuralMutation()
                if (host.itemCount() === 1)
                {
                    controller.blockModel.convertListToText(host.block.index)
                    controller.focusEditorAddress({
                        blockIndex : host.block.index,
                        listItemIndex : -1,
                        tableCellIndex : -1,
                        cursorPosition : 0,
                        preserveViewport : true,
                        viewportY : viewportY
                    })
                }
                else
                {
                    const target = itemIndex > 0 ? itemIndex - 1 : 0
                    const targetLength = itemIndex > 0 ? host.itemText(target).length : 0
                    controller.blockModel.removeListItem(host.block.index, itemIndex)
                    host.focusItem(target, targetLength, true, viewportY)
                }
                return true
            })
        }
        return controller.runEditTransaction("unlist-list-item", function() {
            const viewportY     = controller.contentY
            const itemPlainText = cell.currentPlainText()
            controller.prepareForStructuralMutation()
            if (host.itemIndent(itemIndex) > 0)
            {
                controller.blockModel.indentListItems(host.block.index, itemIndex, host.subtreeEnd(itemIndex) - 1, -1)
                host.focusItem(itemIndex, 0, true, viewportY)
                return true
            }
            const textRow = controller.blockModel.unlistListItem(host.block.index, itemIndex)
            if (textRow < 0)
            return false
            controller.focusEditorAddress({
                blockIndex : textRow,
                listItemIndex : -1,
                tableCellIndex : -1,
                cursorPosition : 0,
                preserveViewport : true,
                viewportY : viewportY
            })
            // The model keeps Markdown paragraph separators while TextArea
            // renders them as one visual line break. Resolve the position in
            // that final visual projection instead of treating a source
            // offset as a TextArea cursor position.
            Qt.callLater(function() {
                Qt.callLater(function() {
                    const delegate = controller.itemAtIndex(textRow)
                    const editor   = delegate ? delegate.item : null
                    if (editor)
                    {
                        editor.cursorPosition   = Math.max(0, editor.currentPlainText().lastIndexOf(itemPlainText))
                        controller.activeEditor = editor
                    }
                })
            })
            return true
        })
    }
    return false
}
