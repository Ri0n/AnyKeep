import QtQuick

QtObject {
    id: controller
    required property var editorView
    required property var blockModel
    required property var editorBackend
    required property var platformBackend
    required property var focusCoordinator
    property var selectionAnchorEditor: null
    property int selectionAnchorPosition: 0
    property bool wholeDocumentSelected: false
    property int fullSelectionPasses: 0
    property bool selectionSpansEditors: false
    property var documentSelectionStartEditor: null
    property int documentSelectionStartPosition: 0
    property var documentSelectionEndEditor: null
    property int documentSelectionEndPosition: 0
    property bool documentSelectionAvailable: false
    property var contextEditor: null
    property bool mouseSelectionActive: false
    property int blankSelectionBoundary: -1
    property real blankSelectionPressX: 0
    property real blankSelectionPressY: 0
    property bool blankSelectionMoved: false
    property var blankSelectionAnchorEditor: null
    property int blankSelectionAnchorPosition: 0
    property int documentSelectionBlankBoundary: -1
    property int documentSelectionBlankDirection: 0
    property var keyboardSelectionAnchorEditor: null
    property int keyboardSelectionAnchorPosition: 0
    property var retainedEmptySelectionEditor: null
    property int pendingInsertionBoundary: -1
    property int pendingLeadingParagraphFocus: -1
    property int editTransactionDepth: 0

    function refreshSelectionState() {
        if (wholeDocumentSelected) {
            documentSelectionAvailable = true
            return
        }
        for (const editor of editorView.editors) {
            if (editor && editor.selectionStart !== editor.selectionEnd) {
                documentSelectionAvailable = true
                return
            }
        }
        documentSelectionAvailable = false
    }

    function scheduleSelectionStateRefresh() {
        selectionStateRefresh.restart()
    }

    property Timer selectionStateRefresh: Timer {
        id: selectionStateRefresh
        interval: 0
        onTriggered: editorView.refreshSelectionState()
    }

    function selectionAnchorAtBoundary(boundary, direction) {
        const ordered = editorView.orderedEditors()
        if (direction < 0) {
            for (let index = ordered.length - 1; index >= 0; --index) {
                const editor = ordered[index]
                if (editor.blockIndex < boundary)
                    return { editor: editor, position: editor.length }
            }
        } else {
            for (const editor of ordered) {
                if (editor.blockIndex >= boundary)
                    return { editor: editor, position: 0 }
            }
        }
        return null
    }

    function beginBlankAreaSelection(boundary, x, y) {
        blankSelectionBoundary = boundary
        blankSelectionPressX = x
        blankSelectionPressY = y
        blankSelectionMoved = false
        blankSelectionAnchorEditor = null
        blankSelectionAnchorPosition = 0
        if (editorBackend)
            editorBackend.updateHistoryViewState(editorView.captureEditorState(), true)
    }

    function updateBlankAreaSelection(x, y) {
        if (blankSelectionBoundary < 0)
            return
        if (!blankSelectionMoved) {
            const dx = x - blankSelectionPressX
            const dy = y - blankSelectionPressY
            if (dx * dx + dy * dy < 16)
                return
            const direction = dy <= 0 ? -1 : 1
            const anchor = selectionAnchorAtBoundary(blankSelectionBoundary, direction)
            if (!anchor)
                return
            clearDocumentSelection()
            documentSelectionBlankBoundary = blankSelectionBoundary
            documentSelectionBlankDirection = direction
            blankSelectionAnchorEditor = anchor.editor
            blankSelectionAnchorPosition = anchor.position
            selectionAnchorEditor = anchor.editor
            selectionAnchorPosition = anchor.position
            mouseSelectionActive = true
            blankSelectionMoved = true
        }
        const structuralBoundary = structuralBoundaryAtPoint(blankSelectionAnchorEditor, y)
        if (structuralBoundary) {
            const structuralFocus = selectionAnchorAtBoundary(structuralBoundary.boundary,
                                                              structuralBoundary.direction)
            if (structuralFocus) {
                documentSelectionBlankBoundary = structuralBoundary.boundary
                documentSelectionBlankDirection = structuralBoundary.direction
                applyDocumentSelection(blankSelectionAnchorEditor, blankSelectionAnchorPosition,
                                       structuralFocus.editor, structuralFocus.position, true)
                return
            }
        }
        documentSelectionBlankBoundary = blankSelectionBoundary
        documentSelectionBlankDirection = y <= blankSelectionPressY ? -1 : 1
        const hit = editorView.editorAtPoint(x, y)
        if (hit)
            applyDocumentSelection(blankSelectionAnchorEditor, blankSelectionAnchorPosition,
                                   hit.editor, hit.position, true)
    }

    function structuralBoundaryAtPoint(anchorEditor, y) {
        if (!anchorEditor || !blockModel)
            return null
        const anchorRow = Number(anchorEditor.blockIndex)
        for (let row = 0; row < editorView.count; ++row) {
            const type = blockModel.blockTypeAt(row)
            if (type !== 4 && type !== 9 && type !== 10 && type !== 11)
                continue
            const block = editorView.itemAtIndex(row)
            if (!block)
                continue
            const topLeft = block.mapToItem(editorView, 0, 0)
            if (y < topLeft.y || y > topLeft.y + block.height)
                continue
            const forward = row > anchorRow
            return {
                boundary: forward ? row + 1 : row,
                direction: forward ? -1 : 1
            }
        }

        // A TextArea keeps the mouse grab while the pointer leaves it. When
        // the final row is structural, dragging below the player/image/card
        // must still end at the document boundary rather than at the nearest
        // preceding text editor.
        const lastRow = editorView.count - 1
        if (lastRow > anchorRow) {
            const type = blockModel.blockTypeAt(lastRow)
            const lastBlock = editorView.itemAtIndex(lastRow)
            if ((type === 4 || type === 9 || type === 10 || type === 11) && lastBlock) {
                const bottom = lastBlock.mapToItem(editorView, 0, lastBlock.height).y
                if (y > bottom)
                    return { boundary: editorView.count, direction: -1 }
            }
        }
        return null
    }

    function applyMouseDocumentSelection(anchorEditor, anchorPosition, x, y) {
        const boundary = structuralBoundaryAtPoint(anchorEditor, y)
        if (boundary) {
            const focus = selectionAnchorAtBoundary(boundary.boundary, boundary.direction)
            if (!focus)
                return false
            documentSelectionBlankBoundary = boundary.boundary
            documentSelectionBlankDirection = boundary.direction
            applyDocumentSelection(anchorEditor, anchorPosition, focus.editor, focus.position, true)
            return true
        }
        const hit = editorView.editorAtPoint(x, y)
        if (!hit)
            return false
        applyDocumentSelection(anchorEditor, anchorPosition, hit.editor, hit.position)
        return true
    }

    function finishBlankAreaSelection() {
        const moved = blankSelectionMoved
        blankSelectionBoundary = -1
        blankSelectionAnchorEditor = null
        selectionAnchorEditor = null
        mouseSelectionActive = false
        if (!moved) {
            documentSelectionBlankBoundary = -1
            documentSelectionBlankDirection = 0
            releaseRetainedEmptySelectionEditor()
        }
        if (moved)
            copyDocumentSelectionToPrimary()
        selectionStateRefresh.restart()
        return moved
    }

    function cancelBlankAreaSelection() {
        blankSelectionBoundary = -1
        blankSelectionMoved = false
        blankSelectionAnchorEditor = null
        selectionAnchorEditor = null
        mouseSelectionActive = false
        documentSelectionBlankBoundary = -1
        documentSelectionBlankDirection = 0
        releaseRetainedEmptySelectionEditor()
        selectionStateRefresh.restart()
    }

    function insertParagraphAtBoundary(row) {
        if (!blockModel)
            return false
        row = Math.max(0, Math.min(row, blockModel.rowCount()))
        return runEditTransaction("insert-text-block", function() {
            prepareForStructuralMutation()
            blockModel.insertTextBlock(row)
            if (row > 0) {
                editorView.focusBlock(row)
            } else {
                // At a leading boundary the old first delegate can still
                // report row zero while ListView retires it.  Wait until the
                // new paragraph is the real row-zero delegate before focus.
                pendingLeadingParagraphFocus = row
                editorView.forceLayout()
                Qt.callLater(function() {
                    Qt.callLater(function() {
                        if (blockModel && row < blockModel.rowCount()
                                && blockModel.blockTypeAt(row) === 0)
                            editorView.focusBlock(row)
                        // The focus hand-off may itself retire the old first
                        // delegate.  Ignore its transient blur; later real
                        // focus loss still uses the ordinary cleanup path.
                        Qt.callLater(function() {
                            if (pendingLeadingParagraphFocus === row)
                                pendingLeadingParagraphFocus = -1
                        })
                    })
                })
            }
            return true
        })
    }

    function isEmptyInsertedParagraph(editor, row) {
        if (!editor || !blockModel || !blockModel.isExplicitEmptyTextBlock(row))
            return false
        // An empty Markdown title is rendered as a paragraph separator by
        // QTextDocument.  The model is the authoritative source at row zero.
        return row === 0 || (typeof editor.currentPlainText === "function"
                             && editor.currentPlainText().length === 0)
    }

    function scheduleDiscardEmptyInsertedParagraph(editor) {
        // A temporary paragraph created below a final structural block is a
        // real endpoint while a drag selection is in progress. Its focus
        // moves to the editor under the pointer, but removing it on that blur
        // destroys the anchor and collapses the structural selection.
        let pendingBlankAnchor = false
        if (blankSelectionBoundary >= 0) {
            const preceding = selectionAnchorAtBoundary(blankSelectionBoundary, -1)
            const following = selectionAnchorAtBoundary(blankSelectionBoundary, 1)
            pendingBlankAnchor = (preceding && preceding.editor === editor)
                    || (following && following.editor === editor)
        }
        if ((mouseSelectionActive && selectionAnchorEditor === editor) || pendingBlankAnchor) {
            retainedEmptySelectionEditor = editor
            return true
        }
        const scheduledRow = Number(editor && editor.blockIndex)
        if (scheduledRow === pendingLeadingParagraphFocus)
            return true
        if (scheduledRow >= 0 && isEmptyInsertedParagraph(editor, scheduledRow))
            pendingInsertionBoundary = scheduledRow
        Qt.callLater(function() {
            if (!editor || editor.activeFocus || !blockModel || editorView.count <= 1)
                return
            const row = Number(editor.blockIndex)
            if (row < 0 || !isEmptyInsertedParagraph(editor, row))
                return
            runEditTransaction("discard-empty-text-block", function() {
                if (!editor || editor.activeFocus)
                    return false
                const currentRow = Number(editor.blockIndex)
                if (currentRow < 0 || !isEmptyInsertedParagraph(editor, currentRow))
                    return false
                if (editorView.activeEditor === editor)
                    editorView.activeEditor = null
                if (editorView.selectedImageIndex > currentRow)
                    --editorView.selectedImageIndex
                if (editorView.selectedAudioIndex > currentRow)
                    --editorView.selectedAudioIndex
                if (editorView.selectedAttachmentIndex > currentRow)
                    --editorView.selectedAttachmentIndex
                blockModel.removeBlock(currentRow)
                return true
            })
        })
        return true
    }

    function clearPendingInsertionBoundary() {
        pendingInsertionBoundary = -1
    }

    function releaseRetainedEmptySelectionEditor() {
        const editor = retainedEmptySelectionEditor
        retainedEmptySelectionEditor = null
        if (editor)
            scheduleDiscardEmptyInsertedParagraph(editor)
    }

    function handleEmptyTextBlockDeletion(event, editor) {
        if (!editor || event.modifiers
                || (event.key !== Qt.Key_Delete && event.key !== Qt.Key_Backspace)
                || editor.selectionStart !== editor.selectionEnd
                || editor.currentPlainText().length !== 0)
            return false

        const row = editor.blockIndex
        const type = blockModel.blockTypeAt(row)
        const emptyTextBlock = type === 0
        const emptyBlockQuote = type === 7
        if (!emptyTextBlock && !emptyBlockQuote)
            return false
        if (emptyTextBlock && (editorView.count <= 1
                || (row === 0 && !blockModel.isExplicitEmptyTextBlock(row))))
            return false

        const backwards = event.key === Qt.Key_Backspace
        return runEditTransaction(emptyBlockQuote ? "remove-empty-blockquote"
                                                       : "remove-empty-text-block", function() {
            prepareForStructuralMutation()
            if (editorView.count <= 1) {
                // A structurally empty quote still has to leave one editable
                // paragraph behind when it is the only block.
                blockModel.convertTextBlockToQuote(row, 0, false)
                editorView.focusBlock(row)
                return true
            }
            blockModel.removeBlock(row)
            const hasPreceding = row > 0
            const hasFollowing = row < editorView.count
            const focusPrevious = backwards ? hasPreceding : !hasFollowing
            const target = focusPrevious ? Math.max(0, row - 1)
                                         : Math.min(row, editorView.count - 1)
            if (editorView.isMediaBlockType(blockModel.blockTypeAt(target)))
                editorView.focusMediaBlock(target)
            else
                editorView.focusBlock(target, focusPrevious)
            return true
        })
    }

    function clearDocumentSelection() {
        wholeDocumentSelected = false
        selectionSpansEditors = false
        documentSelectionStartEditor = null
        documentSelectionStartPosition = 0
        documentSelectionEndEditor = null
        documentSelectionEndPosition = 0
        documentSelectionAvailable = false
        documentSelectionBlankBoundary = -1
        documentSelectionBlankDirection = 0
        for (const editor of editorView.editors) {
            if (!editor)
                continue
            if (editor.selectionStart !== editor.selectionEnd)
                editor.select(editor.cursorPosition, editor.cursorPosition)
        }
        releaseRetainedEmptySelectionEditor()
    }

    function flushPendingEditorChanges() {
        const candidates = []
        if (editorView.activeEditor)
            candidates.push(editorView.activeEditor)
        if (contextEditor && candidates.indexOf(contextEditor) < 0)
            candidates.push(contextEditor)
        for (const editor of candidates) {
            if (editor && editor.flushToModel)
                editor.flushToModel()
        }
    }

    function beginEditTransaction(kind, flushEditors) {
        if (editTransactionDepth === 0) {
            if (flushEditors !== false)
                flushPendingEditorChanges()
            editorBackend.beginHistoryTransaction(kind, editorView.captureEditorState())
        }
        ++editTransactionDepth
    }

    function endEditTransaction() {
        if (editTransactionDepth <= 0)
            return
        --editTransactionDepth
        if (editTransactionDepth === 0)
            editorBackend.endHistoryTransaction(editorView.captureEditorState())
    }

    function runEditTransaction(kind, callback, flushEditors) {
        beginEditTransaction(kind, flushEditors)
        try {
            return callback()
        } finally {
            endEditTransaction()
        }
    }

    function prepareForStructuralMutation() {
        // A focused delegate may defer applying a changed sourceText until it
        // loses focus. The target address applies that pending source before
        // restoring its cursor; do not move focus here, because doing so makes
        // ListView reposition the viewport before the mutation.
        flushPendingEditorChanges()
        clearDocumentSelection()
        editorView.selectedImageIndex = -1
        editorView.selectedAudioIndex = -1
        editorView.selectedAttachmentIndex = -1
        editorView.activeTagLineIndex = -1
        editorView.activeEditor = null
    }

    function prepareForHistoryRestore() {
        // History restore replaces model state and may destroy every current
        // delegate. Commit once while addresses still refer to the old state,
        // then make it impossible for a delayed focus request or selection
        // callback to write through a stale delegate.
        flushPendingEditorChanges()
        ++editorView.focusRequestGeneration
        focusCoordinator.stopPendingFocusRetry()
        editorView.pendingFocusAddress = null
        editorView.pendingEditorState = null
        clearDocumentSelection()
        editorView.selectedImageIndex = -1
        editorView.selectedAudioIndex = -1
        editorView.selectedAttachmentIndex = -1
        editorView.activeTagLineIndex = -1
        keyboardSelectionAnchorEditor = null
        selectionAnchorEditor = null
        contextEditor = null
        editorView.forceActiveFocus()
        editorView.activeEditor = null
    }

    function setEditorSelection(editor, start, end) {
        const selectionStart = Math.min(start, end)
        const selectionEnd = Math.max(start, end)
        if (editor.selectionStart === selectionStart && editor.selectionEnd === selectionEnd)
            return
        editor.select(start, end)
    }

    function handleKeyboardSelection(event, editor) {
        const arrow = event.key === Qt.Key_Left || event.key === Qt.Key_Right
                   || event.key === Qt.Key_Up || event.key === Qt.Key_Down
        const navigation = arrow || event.key === Qt.Key_Home || event.key === Qt.Key_End
                         || event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown
        if (!(event.modifiers & Qt.ShiftModifier)) {
            keyboardSelectionAnchorEditor = null
            if (!navigation || (!wholeDocumentSelected && !selectionSpansEditors
                                && editorView.selectedEditorCount() < 2)) {
                return false
            }
            const backwards = event.key === Qt.Key_Left || event.key === Qt.Key_Up
                           || event.key === Qt.Key_Home || event.key === Qt.Key_PageUp
            const ordered = editorView.orderedEditors()
            const selected = ordered.filter(candidate => candidate.selectionStart !== candidate.selectionEnd)
            const target = backwards
                    ? (documentSelectionStartEditor || selected[0] || ordered[0])
                    : (documentSelectionEndEditor || selected[selected.length - 1]
                       || ordered[ordered.length - 1])
            if (!target)
                return true
            const position = backwards ? target.selectionStart : target.selectionEnd
            clearDocumentSelection()
            target.forceActiveFocus()
            target.cursorPosition = position
            editorView.activeEditor = target
            documentSelectionAvailable = false
            return true
        }
        if (!arrow)
            return false
        if (!keyboardSelectionAnchorEditor) {
            keyboardSelectionAnchorEditor = editor
            keyboardSelectionAnchorPosition = editor.selectionStart !== editor.selectionEnd
                ? (editor.cursorPosition === editor.selectionStart ? editor.selectionEnd : editor.selectionStart)
                : editor.cursorPosition
        }
        let direction = 0
        if (event.key === Qt.Key_Left && editor.cursorPosition === 0)
            direction = -1
        else if (event.key === Qt.Key_Right && editor.cursorPosition === editor.length)
            direction = 1
        else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            const rectangle = editor.positionToRectangle(editor.cursorPosition)
            const probeY = event.key === Qt.Key_Up ? rectangle.y - rectangle.height
                                                   : rectangle.y + rectangle.height + 1
            const probe = editor.positionToRectangle(editor.positionAt(rectangle.x, probeY))
            const boundary = event.key === Qt.Key_Up ? probe.y >= rectangle.y - 0.5
                                                     : probe.y <= rectangle.y + 0.5
            if (boundary)
                direction = event.key === Qt.Key_Up ? -1 : 1
        }
        if (direction === 0)
            return false
        const ordered = editorView.orderedEditors()
        const index = ordered.indexOf(editor)
        const targetIndex = index + direction
        if (targetIndex < 0 || targetIndex >= ordered.length)
            return true
        let target = ordered[targetIndex]
        if ((event.key === Qt.Key_Up || event.key === Qt.Key_Down) && editor.tableCell) {
            const currentRow = ordered.filter(function(candidate) {
                return candidate.blockIndex === editor.blockIndex
                        && candidate.tableRow === editor.tableRow
            })
            const targetRow = ordered.filter(function(candidate) {
                return candidate.blockIndex === editor.blockIndex
                        && candidate.tableRow === editor.tableRow + direction
            })
            const column = currentRow.indexOf(editor)
            if (column >= 0 && column < targetRow.length)
                target = targetRow[column]
        }
        const position = direction < 0 ? target.length : 0
        applyDocumentSelection(keyboardSelectionAnchorEditor, keyboardSelectionAnchorPosition, target, position)
        return true
    }

    function applyDocumentSelection(anchorEditor, anchorPosition, focusEditor, focusPosition,
                                    preserveBlankBoundary) {
        if (!preserveBlankBoundary) {
            documentSelectionBlankBoundary = -1
            documentSelectionBlankDirection = 0
        }
        wholeDocumentSelected = false
        if (anchorEditor === focusEditor) {
            const crossesStructuralBoundary = Boolean(preserveBlankBoundary)
                    && documentSelectionBlankBoundary >= 0
            if (selectionSpansEditors) {
                for (const editor of editorView.editors) {
                    if (editor && editor !== focusEditor && editor.selectionStart !== editor.selectionEnd)
                        editor.select(editor.cursorPosition, editor.cursorPosition)
                }
            }
            selectionSpansEditors = crossesStructuralBoundary
            documentSelectionStartEditor = crossesStructuralBoundary ? focusEditor : null
            documentSelectionStartPosition = crossesStructuralBoundary
                    ? Math.min(anchorPosition, focusPosition) : 0
            documentSelectionEndEditor = crossesStructuralBoundary ? focusEditor : null
            documentSelectionEndPosition = crossesStructuralBoundary
                    ? Math.max(anchorPosition, focusPosition) : 0
            setEditorSelection(focusEditor, anchorPosition, focusPosition)
            documentSelectionAvailable = crossesStructuralBoundary || anchorPosition !== focusPosition
            if (editorView.activeEditor !== focusEditor) {
                focusEditor.forceActiveFocus()
                editorView.activeEditor = focusEditor
            }
            return
        }
        const ordered = editorView.orderedEditors()
        const anchorIndex = ordered.indexOf(anchorEditor)
        const focusIndex = ordered.indexOf(focusEditor)
        if (anchorIndex < 0 || focusIndex < 0)
            return
        selectionSpansEditors = true
        documentSelectionAvailable = true
        const forward = anchorIndex < focusIndex || (anchorIndex === focusIndex && anchorPosition <= focusPosition)
        const firstIndex = forward ? anchorIndex : focusIndex
        const lastIndex = forward ? focusIndex : anchorIndex
        documentSelectionStartEditor = ordered[firstIndex]
        documentSelectionStartPosition = forward ? anchorPosition : focusPosition
        documentSelectionEndEditor = ordered[lastIndex]
        documentSelectionEndPosition = forward ? focusPosition : anchorPosition
        for (let index = 0; index < ordered.length; ++index) {
            const editor = ordered[index]
            if (index < firstIndex || index > lastIndex) {
                if (editor.selectionStart !== editor.selectionEnd)
                    editor.select(editor.cursorPosition, editor.cursorPosition)
            } else if (anchorIndex === focusIndex) {
                setEditorSelection(editor, anchorPosition, focusPosition)
            } else if (index === anchorIndex) {
                setEditorSelection(editor, anchorPosition, forward ? editor.length : 0)
            } else if (index === focusIndex) {
                setEditorSelection(editor, forward ? 0 : editor.length, focusPosition)
            } else {
                setEditorSelection(editor, 0, editor.length)
            }
        }
        if (editorView.activeEditor !== focusEditor) {
            focusEditor.forceActiveFocus()
            editorView.activeEditor = focusEditor
        }
    }

    function hasDocumentSelection() {
        if (wholeDocumentSelected)
            return true
        // documentSelectionAvailable is updated by a zero-delay timer. A
        // quick Ctrl+C immediately after mouse release must still see the
        // actual selection instead of falling through to TextArea.copy().
        for (const editor of editorView.editors) {
            if (editor && editor.selectionStart !== editor.selectionEnd)
                return true
        }
        if (documentSelectionBlankBoundary >= 0) {
            const ranges = structuredSelectionRanges(false)
            for (const range of ranges) {
                const type = blockModel.blockTypeAt(range.blockIndex)
                if (range.wholeEditor && (type === 4 || type === 9 || type === 10 || type === 11))
                    return true
            }
        }
        return false
    }

    function structuralBlockSelected(blockIndex) {
        if (wholeDocumentSelected)
            return true
        if (selectionSpansEditors && documentSelectionStartEditor
                && documentSelectionEndEditor) {
            const firstRow = Math.min(documentSelectionStartEditor.blockIndex,
                                      documentSelectionEndEditor.blockIndex)
            const lastRow = Math.max(documentSelectionStartEditor.blockIndex,
                                     documentSelectionEndEditor.blockIndex)
            if (blockIndex > firstRow && blockIndex < lastRow)
                return true
        }
        if (documentSelectionBlankBoundary >= 0) {
            const ranges = structuredSelectionRanges(false)
            for (const range of ranges) {
                if (range.wholeEditor && Number(range.blockIndex) === blockIndex)
                    return true
            }
        }
        return false
    }

    function selectedDocumentText() {
        if (wholeDocumentSelected)
            return blockModel ? blockModel.contents : ""
        const parts = []
        for (const editor of editorView.orderedEditors()) {
            if (editor.selectionStart !== editor.selectionEnd)
                parts.push(editor.selectedText)
        }
        return parts.join("\n")
    }

    function selectedDocumentMarkdown() {
        if (wholeDocumentSelected)
            return blockModel ? blockModel.contents : ""
        const parts = []
        for (const editor of editorView.orderedEditors()) {
            if (editor.selectionStart !== editor.selectionEnd)
                parts.push(editorBackend.markdownSelection(editor.textDocument,
                                                            editor.selectionStart, editor.selectionEnd))
        }
        return parts.join("\n")
    }

    function selectionNeedsStructure(ranges) {
        if (selectionSpansEditors)
            return true
        for (const range of ranges)
            if (range.wholeEditor)
                return true
        return false
    }

    function structuredSelectionRanges(includeBoundaryEditors) {
        const ordered = editorView.orderedEditors()
        let first = -1
        let last = -1
        const useDocumentBoundaries = (Boolean(includeBoundaryEditors)
                || documentSelectionBlankBoundary >= 0) && selectionSpansEditors
                && documentSelectionStartEditor && documentSelectionEndEditor
        if (useDocumentBoundaries) {
            first = ordered.indexOf(documentSelectionStartEditor)
            last = ordered.indexOf(documentSelectionEndEditor)
            if (first < 0 || last < first)
                return []
        } else {
            for (let index = 0; index < ordered.length; ++index) {
                const editor = ordered[index]
                if (editor.selectionStart !== editor.selectionEnd) {
                    if (first < 0)
                        first = index
                    last = index
                }
            }
        }
        if (first < 0)
            return []

        const ranges = []
        for (let index = first; index <= last; ++index) {
            const editor = ordered[index]
            const rangeStart = useDocumentBoundaries
                    ? (index === first ? documentSelectionStartPosition : 0)
                    : editor.selectionStart
            const rangeEnd = useDocumentBoundaries
                    ? (index === last ? documentSelectionEndPosition : editor.length)
                    : editor.selectionEnd
            const selected = rangeStart !== rangeEnd
            const boundaryOnly = useDocumentBoundaries && !selected
                    && (index === first || index === last)
            // Empty editors between the selection boundaries still represent
            // structural cells/items and must not disappear from the fragment.
            if (!selected && !selectionSpansEditors)
                continue
            ranges.push({
                blockIndex: editor.blockIndex,
                listItemIndex: editor.listItemIndex,
                tableCellIndex: editor.tableCellIndex,
                tableRow: editor.tableRow,
                markdown: selected ? editor.markdownRange(rangeStart, rangeEnd) : "",
                wholeEditor: !boundaryOnly && rangeStart === 0 && rangeEnd === editor.length,
                boundaryOnly: boundaryOnly,
                selectionStart: rangeStart,
                before: selected ? editor.markdownRange(0, rangeStart)
                                 : (boundaryOnly && rangeStart > 0
                                    ? editor.markdownRange(0, rangeStart) : ""),
                after: selected ? editor.markdownRange(rangeEnd, editor.length)
                                : (boundaryOnly && rangeEnd < editor.length
                                   ? editor.markdownRange(rangeEnd, editor.length) : "")
            })
        }

        // Text editors provide the visible selection endpoints, but images and
        // tag lines do not own a TextArea. Add crossed structural blocks
        // explicitly when a selection starts at a blank document boundary.
        if (documentSelectionBlankBoundary >= 0 && documentSelectionBlankDirection !== 0
                && ranges.length > 0 && blockModel) {
            let firstRow = ranges[0].blockIndex
            let lastRow = ranges[0].blockIndex
            for (const range of ranges) {
                firstRow = Math.min(firstRow, range.blockIndex)
                lastRow = Math.max(lastRow, range.blockIndex)
            }
            function wholeStructuralRange(row) {
                return {
                    blockIndex: row,
                    listItemIndex: -1,
                    tableCellIndex: -1,
                    tableRow: -1,
                    markdown: "",
                    wholeEditor: true,
                    boundaryOnly: false,
                    selectionStart: 0,
                    before: "",
                    after: ""
                }
            }
            if (documentSelectionBlankDirection < 0) {
                for (let row = lastRow + 1; row < documentSelectionBlankBoundary; ++row) {
                    const type = blockModel.blockTypeAt(row)
                    if (type === 4 || type === 9 || type === 10 || type === 11)
                        ranges.push(wholeStructuralRange(row))
                }
            } else {
                for (let row = firstRow - 1; row >= documentSelectionBlankBoundary; --row) {
                    const type = blockModel.blockTypeAt(row)
                    if (type === 4 || type === 9 || type === 10 || type === 11)
                        ranges.unshift(wholeStructuralRange(row))
                }
            }
        }
        return ranges
    }

    function fullySelectedTableBlock(ranges) {
        if (!ranges || ranges.length === 0)
            return -1
        const block = Number(ranges[0].blockIndex)
        const tableEditors = editorView.orderedEditors().filter(function(editor) {
            return editor.blockIndex === block && editor.tableCellIndex >= 0
        })
        if (tableEditors.length === 0 || ranges.length !== tableEditors.length)
            return -1
        const seen = []
        for (const range of ranges) {
            if (Number(range.blockIndex) !== block || Number(range.tableCellIndex) < 0
                    || String(range.before || "").length > 0
                    || String(range.after || "").length > 0)
                return -1
            const cell = Number(range.tableCellIndex)
            if (seen[cell])
                return -1
            seen[cell] = true
        }
        for (const editor of tableEditors)
            if (!seen[Number(editor.tableCellIndex)])
                return -1
        return block
    }

    function copyDocumentSelection() {
        if (wholeDocumentSelected) {
            editorBackend.copyDocumentToClipboard()
            return
        }
        if (editorBackend.markdown) {
            const ranges = structuredSelectionRanges(false)
            if (selectionNeedsStructure(ranges)
                    && editorBackend.copySelectionToClipboard(ranges))
                return
        }
        if (editorBackend.markdown) {
            const markdown = selectedDocumentMarkdown()
            if (markdown.length > 0)
                editorBackend.copyMarkdownToClipboard(markdown)
        } else {
            const text = selectedDocumentText()
            if (text.length > 0)
                editorBackend.copyToClipboard(text)
        }
    }

    function copyDocumentSelectionAsMarkdown() {
        if (!editorBackend || !editorBackend.markdown || !hasDocumentSelection())
            return false
        if (wholeDocumentSelected) {
            editorBackend.copyMarkdownAsPlainTextToClipboard(blockModel ? blockModel.contents : "")
            return true
        }
        const ranges = structuredSelectionRanges(false)
        if (selectionNeedsStructure(ranges)
                && editorBackend.copySelectionAsMarkdownToClipboard(ranges))
            return true
        const markdown = selectedDocumentMarkdown()
        if (markdown.length === 0)
            return false
        editorBackend.copyMarkdownAsPlainTextToClipboard(markdown)
        return true
    }

    function copyDocumentSelectionToPrimary() {
        if (!hasDocumentSelection() || !editorBackend)
            return false
        if (editorBackend.markdown) {
            const ranges = structuredSelectionRanges(false)
            if (selectionNeedsStructure(ranges)
                    && editorBackend.copySelectionToPrimarySelection(ranges))
                return true
        }
        if (editorBackend.markdown)
            return editorBackend.copyMarkdownToPrimarySelection(selectedDocumentMarkdown())
        return editorBackend.copyTextToPrimarySelection(selectedDocumentText())
    }

    function copyActiveSelection() {
        const mediaIndex = editorView.selectedImageIndex >= 0 ? selectedImageIndex
                         : editorView.selectedAudioIndex >= 0 ? selectedAudioIndex
                         : editorView.selectedAttachmentIndex
        if (mediaIndex >= 0 && editorBackend
                && typeof editorBackend.copyBlockToClipboard === "function")
            return editorBackend.copyBlockToClipboard(mediaIndex)
        if (!hasDocumentSelection())
            return false
        copyDocumentSelection()
        return true
    }

    function renameAudioBlock(blockIndex, title) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== 10)
            return false
        return runEditTransaction("rename-audio", function() {
            return blockModel.setAudioTitle(blockIndex, String(title || ""))
        })
    }

    function pasteStructuredSelection(editor) {
        if (!editor)
            return false
        editor.commitText(false)
        if (editor.listItemIndex >= 0) {
            const listPasted = editorBackend.pasteListFromClipboard(editor.textDocument, editor.blockIndex,
                                                                      editor.listItemIndex, editor.selectionStart,
                                                                      editor.selectionEnd)
            if (!listPasted.handled)
                return false
            clearDocumentSelection()
            editorView.focusEditorAddress({
                blockIndex: editor.blockIndex,
                listItemIndex: listPasted.focusItem,
                tableCellIndex: -1,
                field: "listItem",
                cursorPosition: 0
            })
            return true
        }
        if (editor.tableCell) {
            const tablePasted = editorBackend.pasteTableFromClipboard(editor.blockIndex, editor.tableCellIndex)
            return tablePasted.handled
        }
        const pasted = editorBackend.pasteStructuredFromClipboard(editor.textDocument, editor.blockIndex,
                                                                    editor.selectionStart, editor.selectionEnd)
        if (!pasted.handled)
            return false
        clearDocumentSelection()
        editorView.focusBlock(pasted.focusRow)
        return true
    }

    function pasteClipboard() {
        if (!editorView.activeEditor)
            return false
        // Code blocks are literal text containers. Never let image or rich
        // structured clipboard import turn their contents into other block
        // types or pass them through QTextDocument's Markdown importer.
        if (editorView.activeEditor.codeDocument) {
            return runEditTransaction("paste-code", function() {
                editorView.activeEditor.paste()
                return true
            })
        }
        if (platformBackend && typeof platformBackend.insertClipboardImage === "function"
                && platformBackend.insertClipboardImage(editorView.insertionBlockIndex()))
            return true

        // Try AnyKeep's private/Markdown clipboard representation before the
        // title's plain-text fallback. This preserves a copied whole document
        // (title plus lists/tables/media) when it is pasted into a new note.
        const structuredPasted = runEditTransaction("paste", function() {
            return pasteStructuredSelection(editorView.activeEditor)
        })
        if (structuredPasted)
            return true

        // The first line is the note title and deliberately does not support
        // inline styling. Single-block rich text therefore still uses the
        // clipboard's plain-text representation.
        if (editorView.selectionTouchesTitle(editorView.activeEditor) && editorBackend
                && typeof editorBackend.pastePlainText === "function") {
            return runEditTransaction("paste", function() {
                const position = editorBackend.pastePlainText(editorView.activeEditor.textDocument,
                                                                   editorView.activeEditor.selectionStart,
                                                                   editorView.activeEditor.selectionEnd)
                if (position < 0)
                    return false
                editorView.activeEditor.cursorPosition = position
                editorView.activeEditor.commitText(false)
                editorView.activeEditor.rememberPlainText()
                if (typeof editorView.activeEditor.tryPromoteTagLine === "function")
                    editorView.activeEditor.tryPromoteTagLine(true)
                return true
            })
        }

        return runEditTransaction("paste", function() {
            const editor = editorView.activeEditor
            const insertionStart = Math.min(editor.selectionStart, editor.selectionEnd)
            editor.paste()
            editorBackend.normalizePastedTextFormats(editor.textDocument, insertionStart,
                                                     editor.cursorPosition)
            if (typeof editor.tryPromoteTagLine === "function")
                editor.tryPromoteTagLine(true)
            return true
        })
    }

    function cutDocumentSelection() {
        if (!hasDocumentSelection())
            return
        return runEditTransaction("cut", function() {
            copyDocumentSelection()
            if (deleteStructuredSelection())
                return true
            for (const editor of editorView.orderedEditors()) {
                if (editor.selectionStart === editor.selectionEnd)
                    continue
                const start = editor.selectionStart
                const end = editor.selectionEnd
                editor.remove(start, end)
                editor.cursorPosition = start
                editor.commitText()
            }
            return true
        })
    }

    function cutActiveSelection() {
        if (!hasDocumentSelection())
            return false
        cutDocumentSelection()
        return true
    }

    function deleteStructuredSelection(backwards) {
        // Ordinary selection inside one editor is handled by TextArea. Avoid
        // taking a structural before-state for every Backspace/Delete key.
        if (!wholeDocumentSelected && !selectionSpansEditors && editorView.selectedEditorCount() < 2) {
            let includesBoundaryStructuralBlock = false
            if (documentSelectionBlankBoundary >= 0) {
                const ranges = structuredSelectionRanges(false)
                includesBoundaryStructuralBlock = ranges.some(function(range) {
                    const type = blockModel.blockTypeAt(range.blockIndex)
                    return range.wholeEditor && (type === 4 || type === 9 || type === 10 || type === 11)
                })
            }
            if (!includesBoundaryStructuralBlock)
                return false
        }
        return runEditTransaction("delete-selection", function() {
            return deleteStructuredSelectionImpl(Boolean(backwards))
        })
    }

    function deleteStructuredSelectionImpl(backwards) {
        if (wholeDocumentSelected) {
            wholeDocumentSelected = false
            selectionSpansEditors = false
            documentSelectionStartEditor = null
            documentSelectionEndEditor = null
            documentSelectionAvailable = false
            blockModel.contents = ""
            editorView.focusBlock(0)
            return true
        }
        const ranges = structuredSelectionRanges(true)
        const selectedTableBlock = fullySelectedTableBlock(ranges)
        if (selectedTableBlock >= 0) {
            prepareForStructuralMutation()
            return editorView.removeTableBlock(selectedTableBlock, backwards)
        }
        if (ranges.length > 1
                && ranges[0].blockIndex !== ranges[ranges.length - 1].blockIndex) {
            prepareForStructuralMutation()
            const removed = editorBackend.deleteSelection(ranges)
            if (removed.handled) {
                if (removed.focusPosition !== undefined) {
                    editorView.focusBlock(removed.focusRow, false, removed.focusPosition)
                } else {
                    editorView.focusBlock(removed.focusRow)
                }
                return true
            }
        }
        if (selectionSpansEditors && ranges.length > 1) {
            const affected = ranges.filter(range => !range.boundaryOnly)
            if (affected.length > 0) {
                const block = affected[0].blockIndex
                const listItems = affected.filter(range => range.listItemIndex >= 0)
                if (block >= 0 && listItems.length === affected.length
                        && affected.every(range => range.blockIndex === block)) {
                    const indexes = listItems.map(range => range.listItemIndex)
                    const firstItem = Math.min(...indexes)
                    const lastItem = Math.max(...indexes)
                    const blockEditors = editorView.orderedEditors().filter(editor => editor.blockIndex === block
                                                                         && editor.listItemIndex >= 0)
                    const removesWholeList = firstItem === 0 && lastItem === blockEditors.length - 1
                    let focusItem = Math.max(0, firstItem - 1)
                    let focusPosition = 0
                    if (firstItem > 0) {
                        const previous = blockEditors.find(editor => editor.listItemIndex === focusItem)
                        focusPosition = previous ? previous.length : 0
                    }
                    prepareForStructuralMutation()
                    blockModel.removeListItems(block, firstItem, lastItem)
                    if (removesWholeList)
                        editorView.focusBlock(block)
                    else
                        editorView.focusEditorAddress({
                            blockIndex: block,
                            listItemIndex: focusItem,
                            tableCellIndex: -1,
                            field: "listItem",
                            cursorPosition: focusPosition
                        })
                    return true
                }

                const tableCells = affected.filter(range => range.tableCellIndex >= 0)
                if (tableCells.length === affected.length
                        && tableCells.every(range => range.blockIndex === block)
                        && tableCells.every(range => range.tableRow === tableCells[0].tableRow)) {
                    // A selection crossing neighbouring cells in one table
                    // row is still a text selection, not a request to remove
                    // the row.  Native TextArea only knows the focused cell,
                    // so apply both boundary fragments explicitly.
                    const start = ranges.find(range => range.boundaryOnly) || tableCells[0]
                    prepareForStructuralMutation()
                    for (const cell of tableCells) {
                        blockModel.setTableCell(block, cell.tableCellIndex, cell.before + cell.after)
                    }
                    editorView.focusEditorAddress({
                        blockIndex: block,
                        listItemIndex: -1,
                        tableCellIndex: start.tableCellIndex,
                        field: "tableCell",
                        cursorPosition: start.selectionStart
                    })
                    return true
                }
            }
        }
        const selected = editorView.orderedEditors().filter(editor => editor.selectionStart !== editor.selectionEnd)
        if (selected.length < 2)
            return false
        const block = selected[0].blockIndex
        if (block < 0 || selected.some(editor => editor.blockIndex !== block))
            return false
        const listItems = selected.filter(editor => editor.listItemIndex >= 0)
        if (listItems.length === selected.length) {
            const indexes = listItems.map(editor => editor.listItemIndex)
            prepareForStructuralMutation()
            blockModel.removeListItems(block, Math.min(...indexes), Math.max(...indexes))
            editorView.focusBlock(block)
            return true
        }
        const tableCells = selected.filter(editor => editor.tableRow >= 0)
        if (tableCells.length === selected.length) {
            const rows = tableCells.map(editor => editor.tableRow)
            if (Math.min(...rows) === Math.max(...rows))
                return false
            prepareForStructuralMutation()
            blockModel.removeTableRows(block, Math.min(...rows), Math.max(...rows))
            editorView.focusBlock(block)
            return true
        }
        return false
    }

    function selectAllDocument() {
        const ordered = editorView.orderedEditors()
        if (ordered.length > 0 && ordered.indexOf(editorView.activeEditor) < 0) {
            editorView.activeEditor = ordered[0]
            editorView.activeEditor.forceActiveFocus()
        }
        wholeDocumentSelected = true
        wholeDocumentSelected = true
        documentSelectionAvailable = true
        for (const editor of ordered)
            setEditorSelection(editor, 0, editor.length)
    }
}
