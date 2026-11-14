import QtQuick
import AnyKeep.Editor 1.0

QtObject {
    id: controller
    required property var editorView
    required property var blockModel
    required property var editorBackend
    required property var focusCoordinator
    property int selectedMediaIndex: -1
    property int selectedAttachmentIndex: -1

    function prepareStructuralSelection() {
        editorView.activeTagLineIndex = -1
        ++editorView.focusRequestGeneration
        focusCoordinator.stopPendingFocusRetry()
        editorView.pendingFocusAddress = null
        editorView.pendingEditorState = null
        editorView.flushPendingEditorChanges()
        editorView.clearDocumentSelection()
        editorView.activeEditor = null
    }

    function selectMediaBlock(blockIndex) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== NoteBlockType.Media)
            return false
        prepareStructuralSelection()
        selectedAttachmentIndex = -1
        selectedMediaIndex = blockIndex
        focusCoordinator.revealBlock(blockIndex)
        return true
    }

    function focusMediaBlock(blockIndex) {
        if (!selectMediaBlock(blockIndex))
            return false
        Qt.callLater(function() {
            const delegate = editorView.itemAtIndex(blockIndex)
            if (delegate && delegate.item
                    && typeof delegate.item.forceActiveFocus === "function")
                delegate.item.forceActiveFocus()
            else
                editorView.forceActiveFocus()
        })
        return true
    }

    function clearMediaSelection() {
        selectedMediaIndex = -1
    }

    function selectAttachmentBlock(blockIndex) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== NoteBlockType.Attachment)
            return false
        prepareStructuralSelection()
        selectedMediaIndex = -1
        selectedAttachmentIndex = blockIndex
        focusCoordinator.revealBlock(blockIndex)
        return true
    }

    function focusAttachmentBlock(blockIndex) {
        if (!selectAttachmentBlock(blockIndex))
            return false
        Qt.callLater(function() {
            const delegate = editorView.itemAtIndex(blockIndex)
            if (delegate && delegate.item
                    && typeof delegate.item.forceActiveFocus === "function")
                delegate.item.forceActiveFocus()
            else
                editorView.forceActiveFocus()
        })
        return true
    }

    function clearAttachmentSelection() {
        selectedAttachmentIndex = -1
    }

    function isMediaBlockType(type) {
        return type === NoteBlockType.Media || type === NoteBlockType.Attachment
    }

    function focusStructuralMediaBlock(blockIndex) {
        const type = blockModel.blockTypeAt(blockIndex)
        if (type === NoteBlockType.Media)
            return focusMediaBlock(blockIndex)
        if (type === NoteBlockType.Attachment)
            return focusAttachmentBlock(blockIndex)
        return false
    }

    // Cross a model-row boundary through the common focus dispatcher. The
    // registry of instantiated text editors cannot describe document order:
    // media has no text editor, and distant delegates may not exist yet.
    function navigateBlock(blockIndex, direction, createParagraph) {
        if (!blockModel)
            return false
        const target = blockIndex + direction
        if (target >= 0 && target < blockModel.rowCount()) {
            selectedMediaIndex = -1
            selectedAttachmentIndex = -1
            return editorView.focusBlock(target, direction < 0)
        }
        if (!createParagraph || !blockModel.markdown)
            return false
        return editorView.insertParagraphAtBoundary(direction < 0 ? 0 : blockModel.rowCount())
    }

    function navigateMediaBlock(blockIndex, direction) {
        return navigateBlock(blockIndex, direction, true)
    }

    function allowsBoundaryParagraph(blockIndex) {
        const type = blockModel ? blockModel.blockTypeAt(blockIndex) : -1
        return type >= 0 && type !== NoteBlockType.Text && type !== NoteBlockType.TagLine
    }

    function focusAfterMediaRemoval(blockIndex) {
        if (blockIndex < editorView.count && isMediaBlockType(blockModel.blockTypeAt(blockIndex)))
            return focusStructuralMediaBlock(blockIndex)
        if (blockIndex > 0 && isMediaBlockType(blockModel.blockTypeAt(blockIndex - 1)))
            return focusStructuralMediaBlock(blockIndex - 1)
        if (blockIndex < editorView.count) {
            editorView.focusBlock(blockIndex)
            return true
        }
        if (editorView.count > 0) {
            editorView.focusBlock(editorView.count - 1, true)
            return true
        }
        return false
    }

    function removeTableBlock(blockIndex, backwards) {
        return editorView.runEditTransaction("remove-table", function() {
            const oldCount = editorView.count
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                editorView.focusBlock(0)
                return true
            }
            const target = backwards ? Math.max(0, blockIndex - 1)
                                     : Math.min(blockIndex, oldCount - 2)
            editorView.focusBlock(target, backwards)
            return true
        })
    }

    function focusFollowingBlock(blockIndex, appendIfMissing) {
        return navigateBlock(blockIndex, 1, appendIfMissing)
    }

    function focusPrecedingBlock(blockIndex) {
        return navigateBlock(blockIndex, -1, allowsBoundaryParagraph(blockIndex))
    }

    function hasOnlyMediaFollowing(blockIndex) {
        if (blockIndex + 1 >= editorView.count)
            return false
        for (let row = blockIndex + 1; row < editorView.count; ++row) {
            if (!isMediaBlockType(blockModel.blockTypeAt(row)))
                return false
        }
        return true
    }

    function removeMediaBlock(blockIndex, focusAfter) {
        if (blockModel.blockTypeAt(blockIndex) !== NoteBlockType.Media)
            return false
        const restoreFocus = focusAfter === undefined ? true : Boolean(focusAfter)
        return editorView.runEditTransaction("remove-media", function() {
            const oldCount = editorView.count
            editorView.prepareForStructuralMutation()
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                editorView.focusBlock(0)
            } else if (restoreFocus) {
                focusAfterMediaRemoval(blockIndex)
            }
            return true
        })
    }

    function removeAttachmentBlock(blockIndex, focusAfter) {
        if (blockModel.blockTypeAt(blockIndex) !== NoteBlockType.Attachment)
            return false
        const restoreFocus = focusAfter === undefined ? true : Boolean(focusAfter)
        return editorView.runEditTransaction("remove-attachment", function() {
            const oldCount = editorView.count
            editorView.prepareForStructuralMutation()
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                editorView.focusBlock(0)
            } else if (restoreFocus) {
                focusAfterMediaRemoval(blockIndex)
            }
            return true
        })
    }

    function handleAdjacentImageDeletion(event, editor) {
        // A list item's or table cell's text boundary is inside its block.
        // Its own handler must consume structural deletion before a key can
        // affect media in a neighboring document row.
        if (!editor || editor.listItemIndex >= 0 || editor.tableCellIndex >= 0
                || event.modifiers || editor.selectionStart !== editor.selectionEnd)
            return false
        const backwards = event.key === Qt.Key_Backspace && editor.cursorPosition === 0
        const forwards = event.key === Qt.Key_Delete && editor.cursorPosition === editor.length
        if (!backwards && !forwards)
            return false
        const editorRow = editor.blockIndex
        const mediaRow = backwards ? editorRow - 1 : editorRow + 1
        if (!isMediaBlockType(blockModel.blockTypeAt(mediaRow)))
            return false
        const focusAddress = editorView.editorAddress(editor)
        return editorView.runEditTransaction("remove-adjacent-media", function() {
            editorView.prepareForStructuralMutation()
            blockModel.removeBlock(mediaRow)
            focusAddress.blockIndex = backwards ? editorRow - 1 : editorRow
            focusAddress.selectionStart = focusAddress.cursorPosition
            focusAddress.selectionEnd = focusAddress.cursorPosition
            editorView.focusEditorAddress(focusAddress)
            return true
        })
    }

    function handleAdjacentTextBlockMerge(event, editor) {
        if (!editor || event.modifiers || editor.selectionStart !== editor.selectionEnd)
            return false
        const isBackspace = event.key === Qt.Key_Backspace && editor.cursorPosition === 0
        const isDelete = event.key === Qt.Key_Delete && editor.cursorPosition === editor.length
        if (!isBackspace && !isDelete)
            return false

        const mergeRow = isBackspace ? editor.blockIndex - 1 : editor.blockIndex
        if (blockModel.blockTypeAt(mergeRow) !== NoteBlockType.Text || blockModel.blockTypeAt(mergeRow + 1) !== NoteBlockType.Text)
            return false

        let cursorPosition = isDelete ? editor.cursorPosition : -1
        if (isBackspace) {
            for (const candidate of editorView.orderedEditors()) {
                if (candidate.blockIndex === mergeRow) {
                    cursorPosition = candidate.length
                    break
                }
            }
        }
        return editorView.runEditTransaction("merge-text-blocks", function() {
            editorView.prepareForStructuralMutation()
            if (!blockModel.mergeTextBlockWithNext(mergeRow))
                return false
            if (cursorPosition >= 0)
                editorView.focusBlock(mergeRow, false, cursorPosition)
            else
                editorView.focusBlock(mergeRow, true)
            return true
        })
    }

    function handleStructuredEnter(event, editor) {
        if (!editor || editor.codeDocument
                || (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter))
            return false

        const modifiers = event.modifiers
                & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
        const row = editor.blockIndex
        const before = editor.markdownRange(0, editor.selectionStart)
        const after = editor.markdownRange(editor.selectionEnd, editor.length)
        const titleSplitModifiers = modifiers
                & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
        if (!titleSplitModifiers && editor.titleDocument && row === 0
                && blockModel.blockTypeAt(row) === NoteBlockType.Text) {
            return editorView.runEditTransaction("split-title", function() {
                editorView.prepareForStructuralMutation()
                if (!blockModel.splitTitleBlock(before, after))
                    return false
                editorView.focusBlock(1, false, 0)
                return true
            })
        }

        const blockType = blockModel.blockTypeAt(row)
        if (modifiers)
            return false

        // A quote keeps ordinary Enter as a soft line break. A second Enter
        // on the resulting empty trailing line leaves the quote. Shift+Enter
        // is deliberately excluded above, so it never terminates the quote.
        if (blockType === NoteBlockType.BlockQuote) {
            // QTextDocument exposes paragraph separators as U+2029 through
            // TextArea.getText(), not necessarily as '\n'. Treat an empty
            // final paragraph semantically instead of keying off one encoding.
            const plain = String(editor.currentPlainText ? editor.currentPlainText() : editor.text)
                                .replace(/\r\n/g, "\n").replace(/[\r\u2028\u2029]/g, "\n")
            const plainCursor = Math.max(0, Math.min(editor.cursorPosition, plain.length))
            const lineStart = plain.lastIndexOf("\n", Math.max(0, plainCursor - 1)) + 1
            const lineEndProbe = plain.indexOf("\n", plainCursor)
            const lineEnd = lineEndProbe < 0 ? plain.length : lineEndProbe
            const currentLineEmpty = plain.substring(lineStart, lineEnd).trim().length === 0
            const hasQuoteContentBefore = plain.substring(0, lineStart).trim().length > 0
            if (after.length !== 0 || !currentLineEmpty || !hasQuoteContentBefore)
                return false
            return editorView.runEditTransaction("exit-blockquote", function() {
                editorView.prepareForStructuralMutation()
                if (!blockModel.splitStructuredBlockToText(row, before, ""))
                    return false
                editorView.focusBlock(row + 1, false, 0)
                return true
            })
        }

        if (blockType !== NoteBlockType.Heading)
            return false
        return editorView.runEditTransaction("exit-heading", function() {
            editorView.prepareForStructuralMutation()
            if (!blockModel.splitStructuredBlockToText(row, before, after))
                return false
            editorView.focusBlock(row + 1, false, 0)
            return true
        })
    }

    function handleBlockBoundaryNavigation(event, editor) {
        const modifiers = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
        if (modifiers || (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down))
            return false
        const backwards = event.key === Qt.Key_Up
        const rectangle = editor.positionToRectangle(editor.cursorPosition)
        const edge = editor.positionToRectangle(backwards ? 0 : editor.length)
        if (backwards ? rectangle.y > edge.y + 0.5 : rectangle.y < edge.y - 0.5)
            return false
        return navigateBlock(editor.blockIndex, backwards ? -1 : 1,
                             !event.isAutoRepeat && allowsBoundaryParagraph(editor.blockIndex))
    }
}
