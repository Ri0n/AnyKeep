import QtQuick
import AnyKeep.Editor 1.0

QtObject {
    id: controller
    required property var editorView
    required property var blockModel
    required property var editorBackend
    property var activeEditor: null
    property int activeTagLineIndex: -1
    property bool mediaTitleEditorFocused: false
    property var pendingFocusAddress: null
    property var pendingEditorState: null
    property int focusRequestGeneration: 0
    property var editors: []
    property int editorRegistrations: 0
    property bool suppressCursorVisibility: false
    property int viewportRestoreGeneration: 0

    function registerEditor(editor) {
        if (editors.indexOf(editor) < 0) {
            editors = editors.concat([editor])
            ++editorRegistrations
        }
        // ListView creates delegates while it lays itself out. Completing a
        // pending request synchronously can position the view from inside that
        // layout, before the new row is a visible item: ListView then discards
        // every delegate and rebuilds the viewport from an estimated offset.
        if (pendingFocusAddress || pendingEditorState)
            Qt.callLater(controller.tryPendingEditorFocus)
    }

    function unregisterEditor(editor) {
        editors = editors.filter(candidate => candidate !== editor)
        if (activeEditor === editor)
            activeEditor = null
        if (cursorVisibilityRefresh.editor === editor) {
            cursorVisibilityRefresh.stop()
            cursorVisibilityRefresh.editor = null
        }
        editorView.scheduleSelectionStateRefresh()
    }

    // TextArea knows how to keep a cursor visible inside itself, but the
    // structured editor is an outer ListView. Coalesce cursor moves into one
    // outer scroll adjustment per event-loop turn.
    property Timer cursorVisibilityRefresh: Timer {
        id: cursorVisibilityRefresh
        interval: 0
        property var editor: null
        onTriggered: editorView.ensureEditorCursorVisible(editor)
    }

    function scheduleCursorVisibility(editor) {
        if (suppressCursorVisibility || !editor || !editor.activeFocus)
            return
        cursorVisibilityRefresh.editor = editor
        cursorVisibilityRefresh.restart()
    }

    function ensureEditorCursorVisible(editor) {
        if (!editor || editor !== activeEditor || !editor.activeFocus)
            return
        revealEditorRange(editor, editor.cursorPosition, editor.cursorPosition)
    }

    // Scrolls by the smallest amount that shows a character range of an
    // editor, keeping a small margin to the viewport edges.
    function revealEditorRange(editor, start, end) {
        const first = editor.positionToRectangle(Math.min(start, end))
        const last = editor.positionToRectangle(Math.max(start, end))
        const top = editor.mapToItem(editorView, 0, first.y).y
        const bottom = editor.mapToItem(editorView, 0, last.y + Math.max(1, last.height)).y
        revealViewportSpan(top, bottom - top, Math.max(4, Math.round(editorView.editorFontMetricsHeight / 2)))
    }

    // Brings a block into view by the smallest scroll. ListView.Contain snaps
    // a block taller than the viewport to its top, which throws the caret out
    // of view and makes the document jump there and back. Show a block that
    // fits completely; of a taller one only bring the nearer edge in when it
    // is entirely off screen and leave the rest to the caret or selection
    // reveal. Only a row without a delegate is located through ListView.
    function revealBlock(blockIndex) {
        // Until ListView lays out pending model changes, itemAtIndex() still
        // answers with the delegate that used to be at that row, and a newly
        // inserted row has no editor to receive the pending focus request.
        editorView.forceLayout()
        const item = editorView.itemAtIndex(blockIndex)
        if (!item) {
            editorView.positionViewAtIndex(blockIndex, ListView.Contain)
            return
        }
        const top = item.mapToItem(editorView, 0, 0).y
        if (item.height <= editorView.height)
            revealViewportSpan(top, item.height, 0)
        else if (top >= editorView.height)
            revealViewportSpan(top, editorView.height, 0)
        else if (top + item.height <= 0)
            revealViewportSpan(top + item.height - editorView.height, editorView.height, 0)
    }

    function revealViewportSpan(top, height, margin) {
        const bottom = top + Math.max(1, height)
        let delta = 0
        if (top < margin)
            delta = top - margin
        else if (bottom > editorView.height - margin)
            delta = Math.min(top - margin, bottom - (editorView.height - margin))
        if (delta !== 0)
            editorView.contentY = clampedContentY(editorView.contentY + delta)
    }

    // The scrollable range includes ListView's top and bottom margins: at the
    // very top of a note contentY is originY - topMargin, not originY.
    function clampedContentY(value) {
        const minimum = editorView.originY - editorView.topMargin
        const maximum = Math.max(minimum, editorView.originY + editorView.contentHeight
                                          + editorView.bottomMargin - editorView.height)
        return Math.max(minimum, Math.min(maximum, value))
    }

    property Timer pendingFocusRetry: Timer {
        id: pendingFocusRetry
        interval: 10
        repeat: true
        onTriggered: if (editorView.tryPendingEditorFocus()) stop()
    }

    function stopPendingFocusRetry() {
        pendingFocusRetry.stop()
    }

    function orderedEditors() {
        return editors.filter(editor => editor !== null && editor.visible).sort((left, right) => {
            const lp = left.mapToItem(editorView, 0, 0)
            const rp = right.mapToItem(editorView, 0, 0)
            if (Math.abs(lp.y - rp.y) > 1)
                return lp.y - rp.y
            return lp.x - rp.x
        })
    }

    function editorAddress(editor, position) {
        if (!editor)
            return null
        const cursor = position === undefined ? editor.cursorPosition : position
        return {
            blockIndex: Number(editor.blockIndex),
            listItemIndex: Number(editor.listItemIndex),
            tableCellIndex: Number(editor.tableCellIndex),
            field: String(editor.editorField || "text"),
            cursorPosition: Number(cursor),
            selectionStart: Number(editor.selectionStart),
            selectionEnd: Number(editor.selectionEnd),
            atEnd: false
        }
    }

    function captureEditorState() {
        const focusAddress = pendingFocusAddress || editorAddress(activeEditor)
        const state = {
            active: focusAddress,
            activeTagLineIndex: activeTagLineIndex,
            selectedMediaIndex: editorView.selectedMediaIndex,
            selectedAttachmentIndex: editorView.selectedAttachmentIndex,
            wholeDocumentSelected: editorView.wholeDocumentSelected,
            selectionSpansEditors: editorView.selectionSpansEditors,
            selectionStart: null,
            selectionEnd: null
        }
        if (editorView.selectionSpansEditors && editorView.documentSelectionStartEditor && editorView.documentSelectionEndEditor) {
            state.selectionStart = editorAddress(editorView.documentSelectionStartEditor,
                                                 editorView.documentSelectionStartPosition)
            state.selectionEnd = editorAddress(editorView.documentSelectionEndEditor,
                                               editorView.documentSelectionEndPosition)
        }
        return state
    }

    function documentHistoryOwnsFocus() {
        // TextField editors keep their native local undo stacks while focused.
        // Do not route Ctrl+Z/Ctrl+Shift+Z into document history until their
        // URL or alt-text edit has been committed back to the model.
        return !editorView.linkEditorPopup.urlFieldFocused && !mediaTitleEditorFocused
    }

    function addressMatchesEditor(address, editor, exact) {
        if (!address || !editor || Number(address.blockIndex) !== editor.blockIndex)
            return false
        const listItem = Number(address.listItemIndex === undefined ? -1 : address.listItemIndex)
        const tableCell = Number(address.tableCellIndex === undefined ? -1 : address.tableCellIndex)
        if (listItem >= 0 && listItem !== editor.listItemIndex)
            return false
        if (tableCell >= 0 && tableCell !== editor.tableCellIndex)
            return false
        if (exact && address.field && String(address.field) !== String(editor.editorField || "text"))
            return false
        return listItem >= 0 || tableCell >= 0 || !exact
                || !address.field || String(address.field) === String(editor.editorField || "text")
    }

    function editorForAddress(address, candidate) {
        if (candidate && addressMatchesEditor(address, candidate, true))
            return candidate
        const ordered = orderedEditors()
        for (const editor of ordered)
            if (addressMatchesEditor(address, editor, true))
                return editor
        // A generic block request deliberately selects its first editor.
        const hasSpecificTarget = Number(address && address.listItemIndex) >= 0
                || Number(address && address.tableCellIndex) >= 0
                || Boolean(address && address.field)
        if (!hasSpecificTarget) {
            for (const editor of ordered)
                if (addressMatchesEditor(address, editor, false))
                    return editor
        }
        // Structural restore can legitimately remove a list item/table cell or
        // turn a structured block into plain text. Prefer the first surviving
        // editor in the same block, then the closest preceding block.
        for (const editor of ordered)
            if (Number(address.blockIndex) === editor.blockIndex)
                return editor
        let preceding = null
        for (const editor of ordered) {
            if (editor.blockIndex > Number(address.blockIndex))
                break
            preceding = editor
        }
        return preceding || (ordered.length > 0 ? ordered[0] : null)
    }

    function applyEditorAddress(editor, address) {
        if (!editor || !address)
            return false
        if (editor.sourceTextPending
                && typeof editor.applyPendingSourceText === "function")
            editor.applyPendingSourceText()
        editor.forceActiveFocus()
        let requested = Number(address.cursorPosition === undefined ? -1 : address.cursorPosition)
        if (address.markdownSourcePosition !== undefined
                && typeof editor.visualPositionForMarkdownSourcePosition === "function")
            requested = editor.visualPositionForMarkdownSourcePosition(Number(address.markdownSourcePosition))
        const position = requested >= 0 ? requested : (Boolean(address.atEnd) ? editor.length : 0)
        const cursor = Math.max(0, Math.min(editor.length, position))
        let selectionStart = Number(address.selectionStart === undefined ? cursor : address.selectionStart)
        let selectionEnd = Number(address.selectionEnd === undefined ? cursor : address.selectionEnd)
        if (address.markdownSelectionStart !== undefined
                && typeof editor.visualPositionForMarkdownSourcePosition === "function")
            selectionStart = editor.visualPositionForMarkdownSourcePosition(
                        Number(address.markdownSelectionStart))
        if (address.markdownSelectionEnd !== undefined
                && typeof editor.visualPositionForMarkdownSourcePosition === "function")
            selectionEnd = editor.visualPositionForMarkdownSourcePosition(
                        Number(address.markdownSelectionEnd))
        if (selectionStart !== selectionEnd)
            editorView.setEditorSelection(editor, Math.max(0, Math.min(editor.length, selectionStart)),
                                          Math.max(0, Math.min(editor.length, selectionEnd)))
        else
            editor.cursorPosition = cursor
        activeEditor = editor
        pendingFocusAddress = null
        if (address.preserveViewport)
            return true
        // Reveal the caret in this turn: revealBlock() may only have brought
        // the edge of a tall block into view. A structural change (notably a
        // table-cell edit) may settle its delegate's height after focus was
        // restored, so re-check on the next two turns as well; ListView's
        // scroll anchoring must not leave the cursor outside the viewport.
        ensureEditorCursorVisible(editor)
        scheduleCursorVisibility(editor)
        Qt.callLater(function() {
            if (editor !== editorView.activeEditor || !editor.activeFocus)
                return
            revealBlock(editor.blockIndex)
            editorView.scheduleCursorVisibility(editor)
            Qt.callLater(function() {
                if (editor === editorView.activeEditor && editor.activeFocus)
                    editorView.scheduleCursorVisibility(editor)
            })
        })
        return true
    }

    function preserveViewportAt(requestedY) {
        const generation = ++viewportRestoreGeneration
        suppressCursorVisibility = true
        cursorVisibilityRefresh.stop()
        function restore(finalPass) {
            if (generation !== editorView.viewportRestoreGeneration)
                return
            editorView.contentY = clampedContentY(requestedY)
            if (finalPass) {
                editorView.suppressCursorVisibility = false
            } else {
                Qt.callLater(function() { restore(true) })
            }
        }
        Qt.callLater(function() { restore(false) })
    }

    function tryRestorePendingEditorState() {
        const state = pendingEditorState
        if (!state)
            return false
        const tagLineIndex = Number(state.activeTagLineIndex === undefined ? -1 : state.activeTagLineIndex)
        if (tagLineIndex >= 0 && blockModel.blockTypeAt(tagLineIndex) === NoteBlockType.TagLine) {
            pendingEditorState = null
            focusTagLineBlock(tagLineIndex, true, true)
            return true
        }
        const mediaIndex = Number(state.selectedMediaIndex === undefined ? -1 : state.selectedMediaIndex)
        if (mediaIndex >= 0 && blockModel.blockTypeAt(mediaIndex) === NoteBlockType.Media) {
            pendingEditorState = null
            editorView.focusMediaBlock(mediaIndex)
            return true
        }
        const attachmentIndex = Number(state.selectedAttachmentIndex === undefined
                                       ? -1 : state.selectedAttachmentIndex)
        if (attachmentIndex >= 0 && blockModel.blockTypeAt(attachmentIndex) === NoteBlockType.Attachment) {
            pendingEditorState = null
            editorView.focusAttachmentBlock(attachmentIndex)
            return true
        }
        if (state.wholeDocumentSelected) {
            pendingEditorState = null
            editorView.selectAllDocument()
            if (state.active)
                focusEditorAddress(state.active)
            return true
        }
        if (state.selectionSpansEditors && state.selectionStart && state.selectionEnd) {
            const first = editorForAddress(state.selectionStart)
            const last = editorForAddress(state.selectionEnd)
            if (!first || !last)
                return false
            pendingEditorState = null
            editorView.applyDocumentSelection(first, Number(state.selectionStart.cursorPosition),
                                              last, Number(state.selectionEnd.cursorPosition))
            if (state.active) {
                const editor = editorForAddress(state.active)
                if (editor)
                    applyEditorAddress(editor, state.active)
            }
            return true
        }
        pendingEditorState = null
        if (state.active)
            focusEditorAddress(state.active)
        return true
    }

    function tryPendingEditorFocus(candidate) {
        if (pendingEditorState && tryRestorePendingEditorState())
            return pendingFocusAddress === null && pendingEditorState === null
        if (!pendingFocusAddress)
            return pendingEditorState === null
        const editor = editorForAddress(pendingFocusAddress, candidate)
        return editor ? applyEditorAddress(editor, pendingFocusAddress) : false
    }

    function focusEditorAddress(address) {
        if (!address || Number(address.blockIndex) < 0)
            return false
        const generation = ++focusRequestGeneration
        pendingFocusAddress = address
        if (address.preserveViewport) {
            const viewportY = address.viewportY === undefined
                    ? editorView.contentY : Number(address.viewportY)
            preserveViewportAt(viewportY)
        } else {
            ++viewportRestoreGeneration
            suppressCursorVisibility = false
            revealBlock(Number(address.blockIndex))
        }
        if (!tryPendingEditorFocus())
            pendingFocusRetry.restart()
        Qt.callLater(function() {
            if (generation !== editorView.focusRequestGeneration)
                return
            const editor = editorView.editorForAddress(address)
            if (editor)
                editorView.applyEditorAddress(editor, address)
            else {
                editorView.pendingFocusAddress = address
                pendingFocusRetry.restart()
            }
        })
        return true
    }

    function restoreEditorState(state) {
        // A scalar undo keeps the delegate alive. Its sourceText binding sees
        // the restored model value while the editor is still focused and, by
        // design, defers replacing the QTextDocument. Apply that one pending
        // value before restoring cursor/selection; otherwise the model and the
        // visible editor disagree until the next focus/navigation event.
        for (const editor of editors) {
            if (editor && editor.sourceTextPending
                    && typeof editor.applyPendingSourceText === "function")
                editor.applyPendingSourceText()
        }
        editorView.clearDocumentSelection()
        pendingEditorState = state || null
        if (!pendingEditorState)
            return false
        // Undo/redo reveals the restored caret or selection with the smallest
        // scroll. Restoring the absolute scroll offset recorded when the edit
        // began would move a document the user has scrolled since.
        const target = state.active || state.selectionStart || state.selectionEnd
        if (target && Number(target.blockIndex) >= 0)
            revealBlock(Number(target.blockIndex))
        if (!tryRestorePendingEditorState())
            pendingFocusRetry.restart()
        return true
    }

    function editorGeometry(index) {
        const ordered = orderedEditors()
        if (index < 0 || index >= ordered.length)
            return {}
        const editor = ordered[index]
        const position = editor.mapToItem(editorView, 0, 0)
        return { x: position.x, y: position.y, width: editor.width, height: editor.height }
    }

    function activeEditorIndex() { return orderedEditors().indexOf(activeEditor) }

    function editorIsBold(index) {
        const ordered = orderedEditors()
        return index >= 0 && index < ordered.length ? ordered[index].font.bold : false
    }

    function selectedEditorCount() {
        let visibleCount = 0
        for (const editor of editors) {
            if (!editor)
                continue
            if (editor.selectionStart !== editor.selectionEnd)
                ++visibleCount
        }
        return visibleCount
    }

    function editorAtPoint(x, y) {
        ++editorView.fullSelectionPasses
        const ordered = orderedEditors()
        let nearest = null
        let nearestDistance = Number.POSITIVE_INFINITY
        for (const editor of ordered) {
            const local = editor.mapFromItem(editorView, x, y)
            if (local.x >= 0 && local.y >= 0 && local.x <= editor.width && local.y <= editor.height)
                return { editor: editor, position: editor.positionAt(local.x, local.y) }
            const dx = local.x < 0 ? -local.x : (local.x > editor.width ? local.x - editor.width : 0)
            const dy = local.y < 0 ? -local.y : (local.y > editor.height ? local.y - editor.height : 0)
            const distance = dx * dx + dy * dy
            if (distance < nearestDistance) {
                nearestDistance = distance
                nearest = {
                    editor: editor,
                    x: Math.max(0, Math.min(editor.width, local.x)),
                    y: Math.max(0, Math.min(editor.height, local.y))
                }
            }
        }
        if (!nearest)
            return null
        return { editor: nearest.editor, position: nearest.editor.positionAt(nearest.x, nearest.y) }
    }

    function insertExternalTextAtPoint(value, x, y, codeLanguage, detectedCode) {
        const target = editorAtPoint(x, y)
        if (!target || !target.editor
                || typeof target.editor.insertExternalText !== "function") {
            return false
        }
        const language = String(codeLanguage || "")
        const treatAsCode = Boolean(detectedCode) || language.length > 0
        if (treatAsCode && blockModel && blockModel.markdown
                && !target.editor.codeDocument) {
            value = String(value || "").replace(/\r\n/g, "\n").replace(/\r/g, "\n")
            if (value.length === 0)
                return false
            return editorView.runEditTransaction("drop-code", function() {
                editorView.prepareForStructuralMutation()
                const row = Number(target.editor.blockIndex)
                const type = blockModel.blockTypeAt(row)
                let insertedRow = -1
                if (!target.editor.titleDocument && (type === 0 || type === 6)) {
                    insertedRow = editorBackend.insertDroppedCodeBlock(
                                row,
                                target.editor.markdownRange(0, target.position),
                                target.editor.markdownRange(target.position, target.editor.length),
                                value, language)
                } else {
                    insertedRow = Math.max(1, row + 1)
                    blockModel.insertCodeBlock(insertedRow, language)
                    blockModel.setBlockText(insertedRow, value)
                }
                if (insertedRow < 0)
                    return false
                focusBlock(insertedRow, true)
                return true
            })
        }
        return target.editor.insertExternalText(value, target.position)
    }

    function captureSpeechInsertionTarget() {
        const address = pendingFocusAddress || editorAddress(activeEditor)
        const fallbackRow = address && Number(address.blockIndex) >= 0
                ? Number(address.blockIndex) + 1 : editorView.insertionBlockIndex()
        if (!address) {
            return {
                blockIndex: -1,
                listItemIndex: -1,
                tableCellIndex: -1,
                field: "",
                cursorPosition: 0,
                selectionStart: 0,
                selectionEnd: 0,
                fallbackInsertionRow: fallbackRow
            }
        }
        return {
            blockIndex: Number(address.blockIndex),
            listItemIndex: Number(address.listItemIndex === undefined ? -1 : address.listItemIndex),
            tableCellIndex: Number(address.tableCellIndex === undefined ? -1 : address.tableCellIndex),
            field: String(address.field || "text"),
            cursorPosition: Number(address.cursorPosition === undefined ? 0 : address.cursorPosition),
            selectionStart: Number(address.selectionStart === undefined
                                   ? address.cursorPosition : address.selectionStart),
            selectionEnd: Number(address.selectionEnd === undefined
                                 ? address.cursorPosition : address.selectionEnd),
            fallbackInsertionRow: fallbackRow
        }
    }

    function insertTextIntoEditor(editor, value, address) {
        if (!editor)
            return false
        return editorView.runEditTransaction("insert-speech-text", function() {
            applyEditorAddress(editor, address)
            const start = Math.max(0, Math.min(editor.length,
                              Math.min(Number(address.selectionStart), Number(address.selectionEnd))))
            const end = Math.max(start, Math.min(editor.length,
                            Math.max(Number(address.selectionStart), Number(address.selectionEnd))))
            if (end > start)
                editor.remove(start, end)
            editor.insert(start, value)
            editor.cursorPosition = start + value.length
            activeEditor = editor
            editor.forceActiveFocus()
            scheduleCursorVisibility(editor)
            return true
        })
    }

    function insertTextAtTarget(value, target) {
        if (!value || value.length === 0 || !blockModel)
            return false
        const address = target || captureSpeechInsertionTarget()
        const editor = editorForAddress(address)
        if (editor && addressMatchesEditor(address, editor, true)) {
            return insertTextIntoEditor(editor, value, address)
        }

        const requestedRow = Number(address && address.fallbackInsertionRow !== undefined
                                    ? address.fallbackInsertionRow : editorView.insertionBlockIndex())
        const row = Math.max(0, Math.min(blockModel.rowCount(), requestedRow))
        return editorView.runEditTransaction("insert-speech-text-block", function() {
            editorView.prepareForStructuralMutation()
            blockModel.insertTextBlock(row)
            blockModel.setBlockText(row, value)
            focusBlock(row, true)
            return true
        })
    }

    function insertTextAtCursor(value) {
        if (!activeEditor)
            return false
        return insertTextIntoEditor(activeEditor, value, editorAddress(activeEditor))
    }

    function focusInitialEditor() {
        // Window activation hands focus back to the block that owned it (text,
        // image, tag line). Keep it: moving focus to the title would drop that
        // selection and scroll the document to its beginning.
        if (editorView.activeFocus && editorView.Window.activeFocusItem !== editorView)
            return true
        // A ListView delegate may have been replaced since a focus request was
        // queued. QML `var` properties retain a wrapper for a destroyed item,
        // which is truthy but no longer has callable methods.
        let editor = activeEditor && editors.indexOf(activeEditor) >= 0 ? activeEditor : null
        if (!editor) {
            activeEditor = null
            editor = firstVisibleEditor()
        }
        if (!editor)
            return false
        // Like window activation, restoring focus must not scroll to the caret.
        editor.forceActiveFocus(Qt.ActiveWindowFocusReason)
        return true
    }

    function firstVisibleEditor() {
        const ordered = orderedEditors()
        for (const editor of ordered) {
            if (editor.mapToItem(editorView, 0, editor.height).y > 0)
                return editor
        }
        return ordered.length > 0 ? ordered[0] : null
    }

    function focusTagLineBlock(blockIndex, atEnd, focusDraft) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== NoteBlockType.TagLine)
            return false
        const generation = ++focusRequestGeneration
        pendingFocusRetry.stop()
        pendingFocusAddress = null
        pendingEditorState = null
        editorView.clearDocumentSelection()
        editorView.clearMediaSelection()
        editorView.clearAttachmentSelection()
        activeEditor = null
        activeTagLineIndex = blockIndex
        revealBlock(blockIndex)

        function applyFocus(attempt) {
            if (generation !== editorView.focusRequestGeneration
                    || !blockModel || blockModel.blockTypeAt(blockIndex) !== NoteBlockType.TagLine)
                return
            const delegate = editorView.itemAtIndex(blockIndex)
            const editor = delegate && delegate.item ? delegate.item : null
            if (!editor) {
                if (attempt < 3) {
                    revealBlock(blockIndex)
                    Qt.callLater(function() { applyFocus(attempt + 1) })
                } else {
                    editorView.forceActiveFocus()
                }
                return
            }
            if (focusDraft && typeof editor.focusDraft === "function")
                editor.focusDraft()
            else if (typeof editor.editTag === "function")
                editor.editTag(atEnd ? Math.max(0, editor.tags.length - 1) : 0, Boolean(atEnd))
            else
                editor.forceActiveFocus()
        }
        Qt.callLater(function() { applyFocus(0) })
        return true
    }

    function focusBlock(blockIndex, atEnd, position) {
        const type = blockModel ? blockModel.blockTypeAt(blockIndex) : -1
        if (type === NoteBlockType.TagLine)
            return focusTagLineBlock(blockIndex, Boolean(atEnd), false)
        if (type === NoteBlockType.Media || type === NoteBlockType.Attachment)
            return editorView.focusMediaBlock(blockIndex)
        activeTagLineIndex = -1
        const listItemIndex = Boolean(atEnd)
                && (type === NoteBlockType.BulletList || type === NoteBlockType.CheckList
                    || type === NoteBlockType.NumberedList)
                ? Math.max(0, blockModel.listItemCountAt(blockIndex) - 1) : -1
        return focusEditorAddress({
            blockIndex: blockIndex,
            listItemIndex: listItemIndex,
            tableCellIndex: -1,
            cursorPosition: position === undefined ? -1 : position,
            selectionStart: position === undefined ? -1 : position,
            selectionEnd: position === undefined ? -1 : position,
            atEnd: Boolean(atEnd)
        })
    }

    function focusBlockAtMarkdownSourcePosition(blockIndex, sourcePosition) {
        return focusEditorAddress({
            blockIndex: blockIndex,
            listItemIndex: -1,
            tableCellIndex: -1,
            cursorPosition: -1,
            selectionStart: -1,
            selectionEnd: -1,
            markdownSourcePosition: sourcePosition,
            atEnd: false
        })
    }

    function firstEditorIn(item) {
        if (!item)
            return null
        if (item.objectName === "noteBlockTextArea")
            return item
        for (const child of item.children || []) {
            const editor = firstEditorIn(child)
            if (editor)
                return editor
        }
        return null
    }

    function focusPendingBlock() {
        return tryPendingEditorFocus()
    }

    function focusDocumentEnd() {
        // Clicking below a structural block means "continue after it". Only
        // a final ordinary text block should absorb the click into its own
        // last line.
        if (editorView.count > 0 && blockModel.blockTypeAt(editorView.count - 1) !== NoteBlockType.Text)
            return editorView.insertParagraphAtBoundary(editorView.count)
        const ordered = orderedEditors()
        if (ordered.length === 0)
            return false
        const editor = ordered[ordered.length - 1]
        editorView.clearDocumentSelection()
        editor.forceActiveFocus()
        editor.cursorPosition = editor.length
        activeEditor = editor
        return true
    }
}
