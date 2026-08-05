import QtQuick

QtObject {
    id: controller
    required property var editorView
    required property var blockModel
    required property var editorBackend
    property int selectedImageIndex: -1
    property int selectedAudioIndex: -1
    property int selectedAttachmentIndex: -1

    function selectImageBlock(blockIndex) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== 4)
            return false
        editorView.activeTagLineIndex = -1
        ++editorView.focusRequestGeneration
        pendingFocusRetry.stop()
        editorView.pendingFocusAddress = null
        editorView.pendingEditorState = null
        editorView.flushPendingEditorChanges()
        editorView.clearDocumentSelection()
        editorView.activeEditor = null
        selectedAudioIndex = -1
        selectedAttachmentIndex = -1
        selectedImageIndex = blockIndex
        editorView.positionViewAtIndex(blockIndex, ListView.Contain)
        return true
    }

    function focusImageBlock(blockIndex) {
        if (!selectImageBlock(blockIndex))
            return false
        Qt.callLater(function() {
            const delegate = editorView.itemAtIndex(blockIndex)
            if (delegate && delegate.item
                    && typeof delegate.item.forceActiveFocus === "function") {
                delegate.item.forceActiveFocus()
            } else {
                editorView.forceActiveFocus()
            }
        })
        return true
    }

    function clearImageSelection() {
        selectedImageIndex = -1
    }

    function selectAudioBlock(blockIndex) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== 10)
            return false
        editorView.activeTagLineIndex = -1
        ++editorView.focusRequestGeneration
        pendingFocusRetry.stop()
        editorView.pendingFocusAddress = null
        editorView.pendingEditorState = null
        editorView.flushPendingEditorChanges()
        editorView.clearDocumentSelection()
        editorView.activeEditor = null
        selectedImageIndex = -1
        selectedAttachmentIndex = -1
        selectedAudioIndex = blockIndex
        editorView.positionViewAtIndex(blockIndex, ListView.Contain)
        return true
    }

    function focusAudioBlock(blockIndex) {
        if (!selectAudioBlock(blockIndex))
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

    function clearAudioSelection() {
        selectedAudioIndex = -1
    }

    function selectAttachmentBlock(blockIndex) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== 11)
            return false
        editorView.activeTagLineIndex = -1
        ++editorView.focusRequestGeneration
        pendingFocusRetry.stop()
        editorView.pendingFocusAddress = null
        editorView.pendingEditorState = null
        editorView.flushPendingEditorChanges()
        editorView.clearDocumentSelection()
        editorView.activeEditor = null
        selectedImageIndex = -1
        selectedAudioIndex = -1
        selectedAttachmentIndex = blockIndex
        editorView.positionViewAtIndex(blockIndex, ListView.Contain)
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
        return type === 4 || type === 10 || type === 11
    }

    function focusMediaBlock(blockIndex) {
        const type = blockModel.blockTypeAt(blockIndex)
        if (type === 4)
            return focusImageBlock(blockIndex)
        if (type === 10)
            return focusAudioBlock(blockIndex)
        if (type === 11)
            return focusAttachmentBlock(blockIndex)
        return false
    }

    function focusAfterMediaRemoval(blockIndex) {
        if (blockIndex < editorView.count && isMediaBlockType(blockModel.blockTypeAt(blockIndex)))
            return focusMediaBlock(blockIndex)
        if (blockIndex > 0 && isMediaBlockType(blockModel.blockTypeAt(blockIndex - 1)))
            return focusMediaBlock(blockIndex - 1)
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

    function focusAfterImageRemoval(blockIndex) {
        return focusAfterMediaRemoval(blockIndex)
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
        if (blockIndex + 1 < editorView.count && blockModel.blockTypeAt(blockIndex + 1) === 9)
            return editorView.focusTagLineBlock(blockIndex + 1, false, false)
        // Media blocks have no text cursor of their own. Find the next actual editor,
        // rather than pretending the immediately following block is one.
        const ordered = editorView.orderedEditors()
        for (const editor of ordered) {
            if (editor.blockIndex <= blockIndex)
                continue
            editor.forceActiveFocus()
            editor.cursorPosition = 0
            editorView.activeEditor = editor
            return true
        }
        // The next delegate may not be instantiated yet when it is outside
        // ListView's cache. Locate it from the model and let the normal
        // pending-focus path load it; structural media blocks are intentionally
        // skipped here.
        for (let row = blockIndex + 1; row < editorView.count; ++row) {
            if (!isMediaBlockType(blockModel.blockTypeAt(row))) {
                editorView.focusBlock(row)
                return true
            }
        }
        if (!appendIfMissing)
            return false
        editorView.runEditTransaction("append-following-text-block", function() {
            blockModel.appendTextBlock()
            editorView.focusBlock(editorView.count - 1)
        })
        return true
    }

    function focusPrecedingBlock(blockIndex) {
        if (blockIndex <= 0)
            return
        if (blockModel.blockTypeAt(blockIndex - 1) === 9) {
            editorView.focusTagLineBlock(blockIndex - 1, true, false)
            return
        }
        const ordered = editorView.orderedEditors()
        const activeIndex = ordered.indexOf(editorView.activeEditor)
        if (activeIndex > 0) {
            const editor = ordered[activeIndex - 1]
            editor.forceActiveFocus()
            editor.cursorPosition = editor.length
            editorView.activeEditor = editor
            return
        }
        editorView.focusBlock(blockIndex - 1)
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

    function removeImageBlock(blockIndex, focusAfter) {
        if (blockModel.blockTypeAt(blockIndex) !== 4)
            return false
        const restoreFocus = focusAfter === undefined ? true : Boolean(focusAfter)
        return editorView.runEditTransaction("remove-image", function() {
            const oldCount = editorView.count
            editorView.prepareForStructuralMutation()
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                editorView.focusBlock(0)
            } else if (restoreFocus) {
                focusAfterImageRemoval(blockIndex)
            }
            return true
        })
    }

    function removeAudioBlock(blockIndex, focusAfter) {
        if (blockModel.blockTypeAt(blockIndex) !== 10)
            return false
        const restoreFocus = focusAfter === undefined ? true : Boolean(focusAfter)
        return editorView.runEditTransaction("remove-audio", function() {
            const oldCount = editorView.count
            if (editorBackend && editorBackend.audioPlayback)
                editorBackend.audioPlayback.stop()
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
        if (blockModel.blockTypeAt(blockIndex) !== 11)
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
        if (!editor || event.modifiers || editor.selectionStart !== editor.selectionEnd)
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
        if (blockModel.blockTypeAt(mergeRow) !== 0 || blockModel.blockTypeAt(mergeRow + 1) !== 0)
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
                && blockModel.blockTypeAt(row) === 0) {
            return editorView.runEditTransaction("split-title", function() {
                editorView.prepareForStructuralMutation()
                if (!blockModel.splitTitleBlock(before, after))
                    return false
                editorView.focusBlock(1, false, 0)
                return true
            })
        }

        if (modifiers || blockModel.blockTypeAt(row) !== 6)
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
        const rectangle = editor.positionToRectangle(editor.cursorPosition)
        if (event.key === Qt.Key_Up) {
            if (editor.blockIndex <= 0 || rectangle.y > editor.positionToRectangle(0).y + 0.5)
                return false
            focusPrecedingBlock(editor.blockIndex)
        } else {
            const last = editor.positionToRectangle(editor.length)
            if (rectangle.y < last.y - 0.5)
                return false
            // Do not create ordinary empty paragraphs by pressing Down at the
            // end of a note. Structured code blocks and image-only tails need
            // a real text block so the cursor can leave them.
            const blockType = blockModel.blockTypeAt(editor.blockIndex)
            focusFollowingBlock(
                editor.blockIndex,
                !event.isAutoRepeat
                    && (editor.codeDocument || blockType === 6 || blockType === 7
                        || hasOnlyMediaFollowing(editor.blockIndex)))
        }
        return true
    }
}
