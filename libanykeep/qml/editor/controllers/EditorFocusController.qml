import QtQuick

QtObject {
    id: controller
    required property var editorView
    required property var blockModel
    required property var editorBackend
    property var activeEditor: null
    property int activeTagLineIndex: -1
    property bool imageAltEditorFocused: false
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
        tryPendingEditorFocus(editor)
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
        const rectangle = editor.positionToRectangle(editor.cursorPosition)
        const point = editor.mapToItem(editorView, rectangle.x, rectangle.y)
        const margin = Math.max(4, Math.round(editorView.editorFontMetricsHeight / 2))
        const top = point.y
        const bottom = point.y + Math.max(1, rectangle.height)
        if (top < margin) {
            editorView.contentY = Math.max(editorView.originY, editorView.contentY + top - margin)
        } else if (bottom > editorView.height - margin) {
            const maximum = editorView.originY + Math.max(0, editorView.contentHeight - editorView.height)
            editorView.contentY = Math.min(maximum, editorView.contentY + bottom - (editorView.height - margin))
        }
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
            selectedImageIndex: editorView.selectedImageIndex,
            selectedAudioIndex: editorView.selectedAudioIndex,
            selectedAttachmentIndex: editorView.selectedAttachmentIndex,
            wholeDocumentSelected: editorView.wholeDocumentSelected,
            selectionSpansEditors: editorView.selectionSpansEditors,
            selectionStart: null,
            selectionEnd: null,
            contentY: editorView.contentY
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
        return !editorView.linkEditorPopup.urlFieldFocused && !imageAltEditorFocused
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
        const requested = Number(address.cursorPosition === undefined ? -1 : address.cursorPosition)
        const position = requested >= 0 ? requested : (Boolean(address.atEnd) ? editor.length : 0)
        const cursor = Math.max(0, Math.min(editor.length, position))
        const selectionStart = Number(address.selectionStart === undefined ? cursor : address.selectionStart)
        const selectionEnd = Number(address.selectionEnd === undefined ? cursor : address.selectionEnd)
        if (selectionStart !== selectionEnd)
            editorView.setEditorSelection(editor, Math.max(0, Math.min(editor.length, selectionStart)),
                                          Math.max(0, Math.min(editor.length, selectionEnd)))
        else
            editor.cursorPosition = cursor
        activeEditor = editor
        pendingFocusAddress = null
        if (address.preserveViewport)
            return true
        // A structural change (notably a table-cell edit) may settle its
        // delegate's height after focus was restored. Re-check on the next
        // two turns so ListView's scroll anchoring cannot leave the cursor
        // outside the viewport.
        scheduleCursorVisibility(editor)
        Qt.callLater(function() {
            if (editor !== editorView.activeEditor || !editor.activeFocus)
                return
            editorView.positionViewAtIndex(editor.blockIndex, ListView.Contain)
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
            editorView.contentY = Math.max(editorView.originY,
                                     Math.min(editorView.originY + Math.max(0, editorView.contentHeight - editorView.height),
                                              requestedY))
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
        if (tagLineIndex >= 0 && blockModel.blockTypeAt(tagLineIndex) === 9) {
            pendingEditorState = null
            focusTagLineBlock(tagLineIndex, true, true)
            return true
        }
        const imageIndex = Number(state.selectedImageIndex === undefined ? -1 : state.selectedImageIndex)
        if (imageIndex >= 0 && blockModel.blockTypeAt(imageIndex) === 4) {
            pendingEditorState = null
            editorView.focusImageBlock(imageIndex)
            return true
        }
        const audioIndex = Number(state.selectedAudioIndex === undefined ? -1 : state.selectedAudioIndex)
        if (audioIndex >= 0 && blockModel.blockTypeAt(audioIndex) === 10) {
            pendingEditorState = null
            editorView.focusAudioBlock(audioIndex)
            return true
        }
        const attachmentIndex = Number(state.selectedAttachmentIndex === undefined
                                       ? -1 : state.selectedAttachmentIndex)
        if (attachmentIndex >= 0 && blockModel.blockTypeAt(attachmentIndex) === 11) {
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
                    ? contentY : Number(address.viewportY)
            preserveViewportAt(viewportY)
        } else {
            ++viewportRestoreGeneration
            suppressCursorVisibility = false
            editorView.positionViewAtIndex(Number(address.blockIndex), ListView.Contain)
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
        const target = state.active || state.selectionStart || state.selectionEnd
        if (target && Number(target.blockIndex) >= 0)
            editorView.positionViewAtIndex(Number(target.blockIndex), ListView.Contain)
        if (!tryRestorePendingEditorState())
            pendingFocusRetry.restart()
        if (state.contentY !== undefined) {
            const requestedY = Number(state.contentY)
            Qt.callLater(function() {
                editorView.contentY = Math.max(editorView.originY,
                                         Math.min(editorView.originY + Math.max(0, editorView.contentHeight - editorView.height),
                                                  requestedY))
            })
        }
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

    function insertExternalTextAtPoint(value, x, y, codeLanguage) {
        const target = editorAtPoint(x, y)
        if (!target || !target.editor
                || typeof target.editor.insertExternalText !== "function") {
            return false
        }
        const language = String(codeLanguage || "")
        if (language.length > 0 && blockModel && blockModel.markdown
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
        // A ListView delegate may have been replaced since a focus request was
        // queued. QML `var` properties retain a wrapper for a destroyed item,
        // which is truthy but no longer has callable methods.
        if (activeEditor && editors.indexOf(activeEditor) >= 0) {
            activeEditor.forceActiveFocus()
            return true
        }
        activeEditor = null
        const loader = editorView.itemAtIndex(0)
        if (!loader || !loader.item)
            return false
        loader.item.forceActiveFocus()
        return true
    }

    function focusTagLineBlock(blockIndex, atEnd, focusDraft) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== 9)
            return false
        const generation = ++focusRequestGeneration
        pendingFocusRetry.stop()
        pendingFocusAddress = null
        pendingEditorState = null
        editorView.clearDocumentSelection()
        editorView.clearImageSelection()
        editorView.clearAudioSelection()
        editorView.clearAttachmentSelection()
        activeEditor = null
        activeTagLineIndex = blockIndex
        editorView.positionViewAtIndex(blockIndex, ListView.Contain)

        function applyFocus(attempt) {
            if (generation !== editorView.focusRequestGeneration
                    || !blockModel || blockModel.blockTypeAt(blockIndex) !== 9)
                return
            const delegate = editorView.itemAtIndex(blockIndex)
            const editor = delegate && delegate.item ? delegate.item : null
            if (!editor) {
                if (attempt < 3) {
                    editorView.positionViewAtIndex(blockIndex, ListView.Contain)
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
        if (blockModel && blockModel.blockTypeAt(blockIndex) === 9)
            return focusTagLineBlock(blockIndex, Boolean(atEnd), false)
        if (blockModel && blockModel.blockTypeAt(blockIndex) === 4)
            return editorView.focusImageBlock(blockIndex)
        if (blockModel && blockModel.blockTypeAt(blockIndex) === 10)
            return editorView.focusAudioBlock(blockIndex)
        if (blockModel && blockModel.blockTypeAt(blockIndex) === 11)
            return editorView.focusAttachmentBlock(blockIndex)
        activeTagLineIndex = -1
        const type = blockModel ? blockModel.blockTypeAt(blockIndex) : -1
        const listItemIndex = Boolean(atEnd) && (type === 1 || type === 2 || type === 5)
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
        if (editorView.count > 0 && blockModel.blockTypeAt(editorView.count - 1) !== 0)
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
