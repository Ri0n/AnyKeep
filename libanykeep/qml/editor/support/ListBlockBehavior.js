.pragma library

// Convert a top-level task into a text paragraph, preserving list items on
// both sides. When the current item contains text, insert a temporary blank
// sibling first so that the text remains a checked/unchecked list item.
function finishList(host, controller, itemIndex, sourceText) {
    const model = controller.blockModel
    const cleaned = String(sourceText || "").replace(/([ \t]*\n)+$/, "")
    let paragraphRow = -1
    if (cleaned.trim().length === 0) {
        paragraphRow = model.unlistListItem(host.block.index, itemIndex)
    } else {
        model.setListItem(host.block.index, itemIndex, cleaned)
        host.insertItem(itemIndex + 1, "")
        paragraphRow = model.unlistListItem(host.block.index, itemIndex + 1)
    }
    if (paragraphRow < 0)
        return false
    controller.focusEditorAddress({
        blockIndex: paragraphRow,
        listItemIndex: -1,
        tableCellIndex: -1,
        cursorPosition: 0
    })
    return true
}

// Called after Android's IME has committed a newline to the QTextDocument.
// Unlike physical Enter, soft-keyboard Return may never produce a Key event.
function splitCommittedImeLine(host, controller, cell, itemIndex, separator, before, after, exit, caret) {
    return controller.runEditTransaction(exit ? "exit-list-ime" : "split-list-item-ime", function() {
        controller.prepareForStructuralMutation(cell)
        if (exit)
            return finishList(host, controller, itemIndex, before)
        const row = host.block.index
        controller.blockModel.setListItem(row, itemIndex, before)
        host.insertItem(itemIndex + 1, after)
        cell.retainTextPrefix(separator)
        host.focusItem(itemIndex + 1, caret)
        return true
    }, false)
}

function handleKey(host, controller, event, cell, itemIndex) {
    const blocked = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
    if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) && !blocked) {
        let first = controller.wholeDocumentSelected ? 0 : itemIndex
        let last = controller.wholeDocumentSelected ? host.itemCount() - 1 : itemIndex
        const selected = host.selectedItemRange()
        if (selected.first >= 0) {
            first = Math.min(first, selected.first)
            last = Math.max(last, selected.last)
        }
        return controller.runEditTransaction("indent-list-items", function() {
            controller.blockModel.indentListItems(host.block.index, first, last,
                                                  event.key === Qt.Key_Backtab
                                                  || event.modifiers & Qt.ShiftModifier ? -1 : 1)
            return true
        })
    }
    if (!blocked && !(event.modifiers & Qt.ShiftModifier)
            && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
        const rectangle = cell.positionToRectangle(cell.cursorPosition)
        const probeY = event.key === Qt.Key_Up ? rectangle.y - rectangle.height
                                               : rectangle.y + rectangle.height + 1
        const probe = cell.positionToRectangle(cell.positionAt(rectangle.x, probeY))
        const boundary = event.key === Qt.Key_Up ? probe.y >= rectangle.y - 0.5
                                                 : probe.y <= rectangle.y + 0.5
        if (boundary) {
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
    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            && !blocked && (controller.touchMode || !(event.modifiers & Qt.ShiftModifier))) {
        // Do not structurally mutate preedit. Android may still deliver the
        // committed newline as an InputMethodEvent instead of this Key event.
        if (cell.androidImeWorkaround && cell.inputMethodComposing)
            return false
        // Keep the established desktop Return semantics unchanged.
        if (!controller.touchMode) {
            return controller.runEditTransaction("split-list-item", function() {
                const position = cell.cursorPosition
                if (cell.length === 0 && position === 0 && host.itemIndent(itemIndex) === 0)
                    return finishList(host, controller, itemIndex, "")
                const left = cell.markdownRange(0, position)
                const right = cell.markdownRange(position, cell.length)
                controller.blockModel.setListItem(host.block.index, itemIndex, left)
                host.insertItem(itemIndex + 1, right)
                cell.retainTextPrefix(position)
                host.focusItem(itemIndex + 1, 0)
                return true
            })
        }
        const position = cell.cursorPosition
        const atEnd = position === cell.length
        const plain = cell.currentPlainText().replace(/\r\n/g, "\n")
                          .replace(/[\r\u2028\u2029]/g, "\n")
        const emptyItem = cell.length === 0
        const emptyLastLine = controller.touchMode && atEnd && plain.endsWith("\n")
        const exit = host.itemIndent(itemIndex) === 0 && (emptyItem || emptyLastLine)
        const source = cell.markdownRange(0, cell.length)
        const left = cell.markdownRange(0, position)
        const right = cell.markdownRange(position, cell.length)
        return controller.runEditTransaction(exit ? "exit-list" : "split-list-item", function() {
            controller.prepareForStructuralMutation(cell)
            if (exit)
                return finishList(host, controller, itemIndex, source)
            controller.blockModel.setListItem(host.block.index, itemIndex, left)
            host.insertItem(itemIndex + 1, right)
            cell.retainTextPrefix(position)
            host.focusItem(itemIndex + 1, 0)
            return true
        }, false)
    }
    if (event.key === Qt.Key_Delete && !blocked && !(event.modifiers & Qt.ShiftModifier)
            && cell.selectionStart === cell.selectionEnd && cell.cursorPosition === cell.length) {
        return controller.runEditTransaction("merge-list-items", function() {
            const position = cell.cursorPosition
            const viewportY = controller.contentY
            controller.prepareForStructuralMutation()
            let merged = false
            if (itemIndex + 1 < host.itemCount()) {
                controller.blockModel.mergeListItemWithNext(host.block.index, itemIndex)
                merged = true
            } else {
                merged = controller.blockModel.mergeListItemWithFollowingBlock(host.block.index, itemIndex)
            }
            host.focusItem(itemIndex, position, true, viewportY)
            if (merged) {
                Qt.callLater(function() {
                    const row = host.rowAt(itemIndex)
                    const editor = row ? row.listEditor : null
                    if (editor && editor.sourceTextPending
                            && typeof editor.applyPendingSourceText === "function")
                        editor.applyPendingSourceText()
                })
            }
            return merged
        })
    }
    if (event.key === Qt.Key_Backspace && !blocked
            && cell.selectionStart === cell.selectionEnd && cell.cursorPosition === 0) {
        if (cell.length === 0) {
            // Backspace on a fresh empty sibling turns that sibling into a
            // continuation line of the preceding item. The next Backspace
            // deletes the newline normally from the previous TextArea.
            if (controller.touchMode && itemIndex > 0
                    && host.itemIndent(itemIndex) === host.itemIndent(itemIndex - 1)) {
                return controller.runEditTransaction("continue-list-item", function() {
                    controller.prepareForStructuralMutation(cell)
                    const previous = host.itemText(itemIndex - 1).replace(/\n+$/, "")
                    // Two spaces encode an explicit CommonMark hard break.
                    // Existing soft-wrapped list items keep their semantics.
                    controller.blockModel.setListItem(host.block.index, itemIndex - 1,
                                                       previous + "  \n")
                    host.removeItems(itemIndex, itemIndex)
                    controller.focusEditorAddress({
                        blockIndex: host.block.index,
                        listItemIndex: itemIndex - 1,
                        tableCellIndex: -1,
                        field: "listItem",
                        cursorPosition: -1,
                        atEnd: true
                    })
                    return true
                }, false)
            }
            return controller.runEditTransaction("remove-list-item", function() {
                controller.prepareForStructuralMutation()
                if (host.itemCount() === 1) {
                    controller.blockModel.convertListToText(host.block.index)
                    controller.focusEditorAddress({
                        blockIndex: host.block.index,
                        listItemIndex: -1,
                        tableCellIndex: -1,
                        cursorPosition: 0
                    })
                } else {
                    const target = itemIndex > 0 ? itemIndex - 1 : 0
                    const targetLength = itemIndex > 0 ? host.itemText(target).length : 0
                    host.removeItems(itemIndex, itemIndex)
                    host.focusItem(target, targetLength)
                }
                return true
            })
        }
        return controller.runEditTransaction("unlist-list-item", function() {
            const viewportY = controller.contentY
            const itemPlainText = cell.currentPlainText()
            controller.prepareForStructuralMutation()
            if (host.itemIndent(itemIndex) > 0) {
                controller.blockModel.indentListItems(host.block.index, itemIndex,
                                                      host.subtreeEnd(itemIndex) - 1, -1)
                host.focusItem(itemIndex, 0, true, viewportY)
                return true
            }
            const textRow = controller.blockModel.unlistListItem(host.block.index, itemIndex)
            if (textRow < 0)
                return false
            controller.focusEditorAddress({
                blockIndex: textRow,
                listItemIndex: -1,
                tableCellIndex: -1,
                cursorPosition: 0,
                preserveViewport: true,
                viewportY: viewportY
            })
            // The model keeps Markdown paragraph separators while TextArea
            // renders them as one visual line break. Resolve the position in
            // that final visual projection instead of treating a source
            // offset as a TextArea cursor position. Only refine the cursor of
            // the editor the request above focused: a newer focus request or
            // a click elsewhere in the meantime wins.
            const focusGeneration = controller.focusRequestGeneration
            Qt.callLater(function() {
                Qt.callLater(function() {
                    if (focusGeneration !== controller.focusRequestGeneration)
                        return
                    const delegate = controller.itemAtIndex(textRow)
                    const editor = delegate ? delegate.item : null
                    if (editor && editor.activeFocus && Number(editor.listItemIndex) < 0)
                        editor.cursorPosition = Math.max(0, editor.currentPlainText().lastIndexOf(itemPlainText))
                })
            })
            return true
        })
    }
    return false
}
