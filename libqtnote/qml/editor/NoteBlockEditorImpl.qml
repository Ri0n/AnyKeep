import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "support/ListBlockBehavior.js" as ListBlockBehavior
import "support/TableBlockBehavior.js" as TableBlockBehavior
import "support/EditorMarkdownRendering.js" as MarkdownRendering
import "../reorder" as Reorder

ListView {
    id: root
    property var blockModel: typeof noteBlockModel !== "undefined" ? noteBlockModel : null
    property var editorBackend: typeof noteEditor !== "undefined" ? noteEditor : null
    property var platformBackend: typeof qmlNoteEditor !== "undefined" ? qmlNoteEditor : null
    property var audioTranscriptionController: null
    property var activeEditor: null
    property int activeTagLineIndex: -1
    property int selectedImageIndex: -1
    property int selectedAudioIndex: -1
    property int selectedAttachmentIndex: -1
    property bool imageAltEditorFocused: false
    property var pendingFocusAddress: null
    property var pendingEditorState: null
    property int focusRequestGeneration: 0
    property var editors: []
    property var selectionAnchorEditor: null
    property int selectionAnchorPosition: 0
    property bool wholeDocumentSelected: false
    property int editorRegistrations: 0
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
    property int editTransactionDepth: 0
    property bool suppressCursorVisibility: false
    property int viewportRestoreGeneration: 0
    property var currentFindResult: ({})
    property string currentFindText: ""
    signal findRequested()
    readonly property alias linkEditorPopup: linkEditor
    readonly property bool touchMode: Qt.platform.os === "android" || Qt.platform.os === "ios"
    readonly property real editorPointSize: typeof mobileApp !== "undefined"
                                            ? mobileApp.editorFontSize : Application.font.pointSize
    readonly property font editorFont: Qt.font({
        family: Application.font.family,
        pointSize: editorPointSize
    })
    readonly property int blockSpacing: Math.max(touchMode ? 8 : 1,
                                                 Math.round(editorFontMetrics.height * (touchMode ? 0.8 : 0.6)))
    readonly property int baseEditorInset: Math.max(touchMode ? 14 : 8,
                                                    Math.round(editorFontMetrics.height * (touchMode ? 0.9 : 0.67)))
    readonly property int listLevelHandleGutter: touchMode ? 18 : 14
    readonly property int editorInset: baseEditorInset + Math.max(0, listLevelHandleGutter - 5)
    readonly property int listIndent: Math.max(touchMode ? 24 : 20,
                                               Math.round(editorFontMetrics.averageCharacterWidth * 4))
    readonly property int listMarkerWidth: Math.max(touchMode ? 44 : 24,
                                                    Math.round(editorFontMetrics.averageCharacterWidth
                                                               * (touchMode ? 4 : 3)))
    readonly property real editorFontMetricsHeight: editorFontMetrics.height
    readonly property real editorFontAverageCharacterWidth: editorFontMetrics.averageCharacterWidth
    readonly property int scrollBarInset: !touchMode && verticalScrollBar.visible
                                           ? Math.ceil(verticalScrollBar.width) : 0
    model: blockModel
    // ListView estimates contentHeight from instantiated delegates. Editor
    // blocks vary from one-line paragraphs to very tall code blocks, so the
    // estimate otherwise changes while scrolling and makes the desktop
    // scrollbar jump. Keep every realistic desktop note instantiated; retain
    // a smaller cache on touch devices where memory pressure matters more.
    cacheBuffer: touchMode ? Math.max(4096, height * 4) : 1000000
    spacing: blockSpacing
    clip: true
    topMargin: baseEditorInset
    bottomMargin: baseEditorInset
    boundsBehavior: touchMode ? Flickable.DragAndOvershootBounds : Flickable.StopAtBounds
    activeFocusOnTab: true
    focus: true

    function resetFind() {
        currentFindResult = ({})
        currentFindText = ""
    }

    function findNext(text, backwards) {
        if (!blockModel || !text || text.length === 0)
            return false
        flushPendingEditorChanges()
        if (currentFindText !== text) {
            currentFindText = text
            currentFindResult = ({})
        }
        const result = blockModel.findText(text, currentFindResult, Boolean(backwards), false)
        if (!result || result.blockIndex === undefined)
            return false
        currentFindResult = result
        positionViewAtIndex(Number(result.blockIndex), ListView.Contain)
        return focusEditorAddress({
            blockIndex: Number(result.blockIndex),
            listItemIndex: Number(result.listItemIndex),
            tableCellIndex: Number(result.tableCellIndex),
            field: String(result.field),
            cursorPosition: Number(result.start) + Number(result.length),
            selectionStart: Number(result.start),
            selectionEnd: Number(result.start) + Number(result.length),
            atEnd: false
        })
    }

    function registerEditorBackendView() {
        if (editorBackend && typeof editorBackend.registerEditorView === "function")
            editorBackend.registerEditorView(root)
    }

    Component.onCompleted: registerEditorBackendView()
    onEditorBackendChanged: registerEditorBackendView()

    ScrollBar.vertical: ScrollBar {
        id: verticalScrollBar
        policy: root.touchMode ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
    }

    FontMetrics {
        id: editorFontMetrics
        font: root.editorFont
    }

    SystemPalette {
        id: editorPalette
    }

    EditorReorderController {
        id: editorReorderController
        editorView: root
        blockModel: root.blockModel
    }

    function markdownForRendering(source) {
        return MarkdownRendering.markdownForRendering(source)
    }

    function markdownTableCellForRendering(source) {
        return MarkdownRendering.markdownTableCellForRendering(source)
    }

    function registerEditor(editor) {
        if (editors.indexOf(editor) < 0) {
            editors = editors.concat([editor])
            ++editorRegistrations
        }
        tryPendingEditorFocus(editor)
    }

    function unregisterEditor(editor) {
        editors = editors.filter(candidate => candidate !== editor)
        selectionStateRefresh.restart()
    }

    function refreshSelectionState() {
        if (wholeDocumentSelected) {
            documentSelectionAvailable = true
            return
        }
        for (const editor of editors) {
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

    Timer {
        id: selectionStateRefresh
        interval: 0
        onTriggered: root.refreshSelectionState()
    }

    // TextArea knows how to keep a cursor visible inside itself, but the
    // structured editor is an outer ListView. Coalesce cursor moves into one
    // outer scroll adjustment per event-loop turn.
    Timer {
        id: cursorVisibilityRefresh
        interval: 0
        property var editor: null
        onTriggered: root.ensureEditorCursorVisible(editor)
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
        const point = editor.mapToItem(root, rectangle.x, rectangle.y)
        const margin = Math.max(4, Math.round(editorFontMetrics.height / 2))
        const top = point.y
        const bottom = point.y + Math.max(1, rectangle.height)
        if (top < margin) {
            contentY = Math.max(originY, contentY + top - margin)
        } else if (bottom > height - margin) {
            const maximum = originY + Math.max(0, contentHeight - height)
            contentY = Math.min(maximum, contentY + bottom - (height - margin))
        }
    }

    Timer {
        id: pendingFocusRetry
        interval: 10
        repeat: true
        onTriggered: if (root.tryPendingEditorFocus()) stop()
    }

    EditorContextMenu {
        id: sharedContextMenu
        controller: root
        editorBackend: root.editorBackend
        platformBackend: root.platformBackend
    }

    function orderedEditors() {
        return editors.filter(editor => editor !== null && editor.visible).sort((left, right) => {
            const lp = left.mapToItem(root, 0, 0)
            const rp = right.mapToItem(root, 0, 0)
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
            selectedImageIndex: selectedImageIndex,
            selectedAudioIndex: selectedAudioIndex,
            selectedAttachmentIndex: selectedAttachmentIndex,
            wholeDocumentSelected: wholeDocumentSelected,
            selectionSpansEditors: selectionSpansEditors,
            selectionStart: null,
            selectionEnd: null,
            contentY: contentY
        }
        if (selectionSpansEditors && documentSelectionStartEditor && documentSelectionEndEditor) {
            state.selectionStart = editorAddress(documentSelectionStartEditor,
                                                 documentSelectionStartPosition)
            state.selectionEnd = editorAddress(documentSelectionEndEditor,
                                               documentSelectionEndPosition)
        }
        return state
    }

    function documentHistoryOwnsFocus() {
        // TextField editors keep their native local undo stacks while focused.
        // Do not route Ctrl+Z/Ctrl+Shift+Z into document history until their
        // URL or alt-text edit has been committed back to the model.
        return !urlField.activeFocus && !imageAltEditorFocused
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
            setEditorSelection(editor, Math.max(0, Math.min(editor.length, selectionStart)),
                               Math.max(0, Math.min(editor.length, selectionEnd)))
        else
            editor.cursorPosition = cursor
        activeEditor = editor
        pendingFocusAddress = null
        if (address.preserveViewport)
            return true
        // A structural change (notably a table-cell edit) may settle its
        // delegate's height after focus was restored.  Re-check on the next
        // two turns so ListView's scroll anchoring cannot leave the cursor
        // outside the viewport.
        scheduleCursorVisibility(editor)
        Qt.callLater(function() {
            if (editor !== root.activeEditor || !editor.activeFocus)
                return
            root.positionViewAtIndex(editor.blockIndex, ListView.Contain)
            root.scheduleCursorVisibility(editor)
            Qt.callLater(function() {
                if (editor === root.activeEditor && editor.activeFocus)
                    root.scheduleCursorVisibility(editor)
            })
        })
        return true
    }

    function preserveViewportAt(requestedY) {
        const generation = ++viewportRestoreGeneration
        suppressCursorVisibility = true
        cursorVisibilityRefresh.stop()
        function restore(finalPass) {
            if (generation !== root.viewportRestoreGeneration)
                return
            root.contentY = Math.max(root.originY,
                                     Math.min(root.originY + Math.max(0, root.contentHeight - root.height),
                                              requestedY))
            if (finalPass) {
                root.suppressCursorVisibility = false
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
            focusImageBlock(imageIndex)
            return true
        }
        const audioIndex = Number(state.selectedAudioIndex === undefined ? -1 : state.selectedAudioIndex)
        if (audioIndex >= 0 && blockModel.blockTypeAt(audioIndex) === 10) {
            pendingEditorState = null
            focusAudioBlock(audioIndex)
            return true
        }
        const attachmentIndex = Number(state.selectedAttachmentIndex === undefined
                                       ? -1 : state.selectedAttachmentIndex)
        if (attachmentIndex >= 0 && blockModel.blockTypeAt(attachmentIndex) === 11) {
            pendingEditorState = null
            focusAttachmentBlock(attachmentIndex)
            return true
        }
        if (state.wholeDocumentSelected) {
            pendingEditorState = null
            selectAllDocument()
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
            applyDocumentSelection(first, Number(state.selectionStart.cursorPosition),
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
            positionViewAtIndex(Number(address.blockIndex), ListView.Contain)
        }
        if (!tryPendingEditorFocus())
            pendingFocusRetry.restart()
        Qt.callLater(function() {
            if (generation !== root.focusRequestGeneration)
                return
            const editor = root.editorForAddress(address)
            if (editor)
                root.applyEditorAddress(editor, address)
            else {
                root.pendingFocusAddress = address
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
        clearDocumentSelection()
        pendingEditorState = state || null
        if (!pendingEditorState)
            return false
        const target = state.active || state.selectionStart || state.selectionEnd
        if (target && Number(target.blockIndex) >= 0)
            positionViewAtIndex(Number(target.blockIndex), ListView.Contain)
        if (!tryRestorePendingEditorState())
            pendingFocusRetry.restart()
        if (state.contentY !== undefined) {
            const requestedY = Number(state.contentY)
            Qt.callLater(function() {
                root.contentY = Math.max(root.originY,
                                         Math.min(root.originY + Math.max(0, root.contentHeight - root.height),
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
        const position = editor.mapToItem(root, 0, 0)
        return { x: position.x, y: position.y, width: editor.width, height: editor.height }
    }

    function activeEditorIndex() { return orderedEditors().indexOf(activeEditor) }

    function editorIsBold(index) {
        const ordered = orderedEditors()
        return index >= 0 && index < ordered.length ? ordered[index].font.bold : false
    }

    function selectedEditorCount() {
        let count = 0
        for (const editor of editors) {
            if (!editor)
                continue
            if (editor.selectionStart !== editor.selectionEnd)
                ++count
        }
        return count
    }

    function editorAtPoint(x, y) {
        ++fullSelectionPasses
        const ordered = orderedEditors()
        let nearest = null
        let nearestDistance = Number.POSITIVE_INFINITY
        for (const editor of ordered) {
            const local = editor.mapFromItem(root, x, y)
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

    function selectionAnchorAtBoundary(boundary, direction) {
        const ordered = orderedEditors()
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
            editorBackend.updateHistoryViewState(captureEditorState(), true)
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
        const hit = editorAtPoint(x, y)
        if (hit)
            applyDocumentSelection(blankSelectionAnchorEditor, blankSelectionAnchorPosition,
                                   hit.editor, hit.position, true)
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
        selectionStateRefresh.restart()
    }

    function insertParagraphAtBoundary(row) {
        if (!blockModel)
            return false
        row = Math.max(0, Math.min(row, blockModel.rowCount()))
        return runEditTransaction("insert-text-block", function() {
            prepareForStructuralMutation()
            blockModel.insertTextBlock(row)
            focusBlock(row)
            return true
        })
    }

    function scheduleDiscardEmptyInsertedParagraph(editor) {
        Qt.callLater(function() {
            if (!editor || editor.activeFocus || !blockModel || count <= 1)
                return
            const row = Number(editor.blockIndex)
            if (row <= 0 || editor.currentPlainText().length !== 0
                    || !blockModel.isExplicitEmptyTextBlock(row))
                return
            runEditTransaction("discard-empty-text-block", function() {
                if (!editor || editor.activeFocus)
                    return false
                const currentRow = Number(editor.blockIndex)
                if (currentRow <= 0 || editor.currentPlainText().length !== 0
                        || !blockModel.isExplicitEmptyTextBlock(currentRow))
                    return false
                if (activeEditor === editor)
                    activeEditor = null
                if (selectedImageIndex > currentRow)
                    --selectedImageIndex
                if (selectedAudioIndex > currentRow)
                    --selectedAudioIndex
                if (selectedAttachmentIndex > currentRow)
                    --selectedAttachmentIndex
                blockModel.removeBlock(currentRow)
                return true
            })
        })
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
        if (emptyTextBlock && (count <= 1
                || (row === 0 && !blockModel.isExplicitEmptyTextBlock(row))))
            return false

        const backwards = event.key === Qt.Key_Backspace
        return runEditTransaction(emptyBlockQuote ? "remove-empty-blockquote"
                                                       : "remove-empty-text-block", function() {
            prepareForStructuralMutation()
            if (count <= 1) {
                // A structurally empty quote still has to leave one editable
                // paragraph behind when it is the only block.
                blockModel.convertTextBlockToQuote(row, 0, false)
                focusBlock(row)
                return true
            }
            blockModel.removeBlock(row)
            const hasPreceding = row > 0
            const hasFollowing = row < count
            const focusPrevious = backwards ? hasPreceding : !hasFollowing
            const target = focusPrevious ? Math.max(0, row - 1)
                                         : Math.min(row, count - 1)
            if (isMediaBlockType(blockModel.blockTypeAt(target)))
                focusMediaBlock(target)
            else
                focusBlock(target, focusPrevious)
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
        for (const editor of editors) {
            if (!editor)
                continue
            if (editor.selectionStart !== editor.selectionEnd)
                editor.select(editor.cursorPosition, editor.cursorPosition)
        }
    }

    function flushPendingEditorChanges() {
        const candidates = []
        if (activeEditor)
            candidates.push(activeEditor)
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
            root.editorBackend.beginHistoryTransaction(kind, captureEditorState())
        }
        ++editTransactionDepth
    }

    function endEditTransaction() {
        if (editTransactionDepth <= 0)
            return
        --editTransactionDepth
        if (editTransactionDepth === 0)
            root.editorBackend.endHistoryTransaction(captureEditorState())
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
        selectedImageIndex = -1
        selectedAudioIndex = -1
        selectedAttachmentIndex = -1
        activeTagLineIndex = -1
        activeEditor = null
    }

    function prepareForHistoryRestore() {
        // History restore replaces model state and may destroy every current
        // delegate. Commit once while addresses still refer to the old state,
        // then make it impossible for a delayed focus request or selection
        // callback to write through a stale delegate.
        flushPendingEditorChanges()
        ++focusRequestGeneration
        pendingFocusRetry.stop()
        pendingFocusAddress = null
        pendingEditorState = null
        clearDocumentSelection()
        selectedImageIndex = -1
        selectedAudioIndex = -1
        selectedAttachmentIndex = -1
        activeTagLineIndex = -1
        keyboardSelectionAnchorEditor = null
        selectionAnchorEditor = null
        contextEditor = null
        forceActiveFocus()
        activeEditor = null
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
                                && selectedEditorCount() < 2)) {
                return false
            }
            const backwards = event.key === Qt.Key_Left || event.key === Qt.Key_Up
                           || event.key === Qt.Key_Home || event.key === Qt.Key_PageUp
            const ordered = orderedEditors()
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
            activeEditor = target
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
        const ordered = orderedEditors()
        const index = ordered.indexOf(editor)
        const targetIndex = index + direction
        if (targetIndex < 0 || targetIndex >= ordered.length)
            return true
        const target = ordered[targetIndex]
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
            if (selectionSpansEditors) {
                for (const editor of editors) {
                    if (editor && editor !== focusEditor && editor.selectionStart !== editor.selectionEnd)
                        editor.select(editor.cursorPosition, editor.cursorPosition)
                }
            }
            selectionSpansEditors = false
            documentSelectionStartEditor = null
            documentSelectionStartPosition = 0
            documentSelectionEndEditor = null
            documentSelectionEndPosition = 0
            setEditorSelection(focusEditor, anchorPosition, focusPosition)
            documentSelectionAvailable = anchorPosition !== focusPosition
            if (activeEditor !== focusEditor) {
                focusEditor.forceActiveFocus()
                activeEditor = focusEditor
            }
            return
        }
        const ordered = orderedEditors()
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
        if (activeEditor !== focusEditor) {
            focusEditor.forceActiveFocus()
            activeEditor = focusEditor
        }
    }

    function hasDocumentSelection() {
        if (wholeDocumentSelected)
            return true
        // documentSelectionAvailable is updated by a zero-delay timer. A
        // quick Ctrl+C immediately after mouse release must still see the
        // actual selection instead of falling through to TextArea.copy().
        for (const editor of editors) {
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

    function selectedDocumentText() {
        if (wholeDocumentSelected)
            return blockModel ? blockModel.contents : ""
        const parts = []
        for (const editor of orderedEditors()) {
            if (editor.selectionStart !== editor.selectionEnd)
                parts.push(editor.selectedText)
        }
        return parts.join("\n")
    }

    function selectedDocumentMarkdown() {
        if (wholeDocumentSelected)
            return blockModel ? blockModel.contents : ""
        const parts = []
        for (const editor of orderedEditors()) {
            if (editor.selectionStart !== editor.selectionEnd)
                parts.push(root.editorBackend.markdownSelection(editor.textDocument,
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
        const ordered = orderedEditors()
        let first = -1
        let last = -1
        const useDocumentBoundaries = Boolean(includeBoundaryEditors) && selectionSpansEditors
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
                markdown: selected ? root.editorBackend.markdownSelection(
                                         editor.textDocument, rangeStart, rangeEnd) : "",
                wholeEditor: !boundaryOnly && rangeStart === 0 && rangeEnd === editor.length,
                boundaryOnly: boundaryOnly,
                selectionStart: rangeStart,
                before: selected ? root.editorBackend.markdownSelection(
                                       editor.textDocument, 0, rangeStart)
                                 : (boundaryOnly && rangeStart > 0
                                    ? root.editorBackend.markdownSelection(editor.textDocument, 0, rangeStart) : ""),
                after: selected ? root.editorBackend.markdownSelection(
                                      editor.textDocument, rangeEnd, editor.length)
                                : (boundaryOnly && rangeEnd < editor.length
                                   ? root.editorBackend.markdownSelection(editor.textDocument,
                                                                     rangeEnd, editor.length) : "")
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
        const tableEditors = orderedEditors().filter(function(editor) {
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
            root.editorBackend.copyDocumentToClipboard()
            return
        }
        if (root.editorBackend.markdown) {
            const ranges = structuredSelectionRanges(false)
            if (selectionNeedsStructure(ranges)
                    && root.editorBackend.copySelectionToClipboard(ranges))
                return
        }
        if (root.editorBackend.markdown) {
            const markdown = selectedDocumentMarkdown()
            if (markdown.length > 0)
                root.editorBackend.copyMarkdownToClipboard(markdown)
        } else {
            const text = selectedDocumentText()
            if (text.length > 0)
                root.editorBackend.copyToClipboard(text)
        }
    }


    function copyDocumentSelectionToPrimary() {
        if (!hasDocumentSelection() || !root.editorBackend)
            return false
        if (root.editorBackend.markdown) {
            const ranges = structuredSelectionRanges(false)
            if (selectionNeedsStructure(ranges)
                    && root.editorBackend.copySelectionToPrimarySelection(ranges))
                return true
        }
        if (root.editorBackend.markdown)
            return root.editorBackend.copyMarkdownToPrimarySelection(selectedDocumentMarkdown())
        return root.editorBackend.copyTextToPrimarySelection(selectedDocumentText())
    }

    function copyActiveSelection() {
        if (!hasDocumentSelection())
            return false
        copyDocumentSelection()
        return true
    }

    function pasteStructuredSelection(editor) {
        if (!editor)
            return false
        editor.commitText(false)
        if (editor.listItemIndex >= 0) {
            const listPasted = root.editorBackend.pasteListFromClipboard(editor.textDocument, editor.blockIndex,
                                                                      editor.listItemIndex, editor.selectionStart,
                                                                      editor.selectionEnd)
            if (!listPasted.handled)
                return false
            clearDocumentSelection()
            focusEditorAddress({
                blockIndex: editor.blockIndex,
                listItemIndex: listPasted.focusItem,
                tableCellIndex: -1,
                field: "listItem",
                cursorPosition: 0
            })
            return true
        }
        if (editor.tableCell) {
            const tablePasted = root.editorBackend.pasteTableFromClipboard(editor.blockIndex, editor.tableCellIndex)
            return tablePasted.handled
        }
        const pasted = root.editorBackend.pasteStructuredFromClipboard(editor.textDocument, editor.blockIndex,
                                                                    editor.selectionStart, editor.selectionEnd)
        if (!pasted.handled)
            return false
        clearDocumentSelection()
        focusBlock(pasted.focusRow)
        return true
    }

    function pasteClipboard() {
        if (!activeEditor)
            return false
        // Code blocks are literal text containers. Never let image or rich
        // structured clipboard import turn their contents into other block
        // types or pass them through QTextDocument's Markdown importer.
        if (activeEditor.codeDocument) {
            return runEditTransaction("paste-code", function() {
                activeEditor.paste()
                return true
            })
        }
        // The first line is the note title and deliberately does not support
        // inline styling. Paste its plain-text clipboard representation so
        // HTML/RTF formatting cannot become persistent Markdown markup.
        if (selectionTouchesTitle(activeEditor) && root.editorBackend
                && typeof root.editorBackend.pastePlainText === "function") {
            const titlePasted = runEditTransaction("paste", function() {
                const position = root.editorBackend.pastePlainText(activeEditor.textDocument,
                                                                   activeEditor.selectionStart,
                                                                   activeEditor.selectionEnd)
                if (position < 0)
                    return false
                activeEditor.cursorPosition = position
                activeEditor.commitText(false)
                activeEditor.rememberPlainText()
                if (typeof activeEditor.tryPromoteTagLine === "function")
                    activeEditor.tryPromoteTagLine(true)
                return true
            })
            if (titlePasted)
                return true
        }
        if (platformBackend && typeof platformBackend.insertClipboardImage === "function"
                && platformBackend.insertClipboardImage(insertionBlockIndex()))
            return true
        return runEditTransaction("paste", function() {
            const editor = activeEditor
            if (!pasteStructuredSelection(editor)) {
                editor.paste()
                if (typeof editor.tryPromoteTagLine === "function")
                    editor.tryPromoteTagLine(true)
            }
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
            for (const editor of orderedEditors()) {
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
        if (!wholeDocumentSelected && !selectionSpansEditors && selectedEditorCount() < 2) {
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
            focusBlock(0)
            return true
        }
        const ranges = structuredSelectionRanges(true)
        const selectedTableBlock = fullySelectedTableBlock(ranges)
        if (selectedTableBlock >= 0) {
            prepareForStructuralMutation()
            return removeTableBlock(selectedTableBlock, backwards)
        }
        if (ranges.length > 1
                && ranges[0].blockIndex !== ranges[ranges.length - 1].blockIndex) {
            prepareForStructuralMutation()
            const removed = root.editorBackend.deleteSelection(ranges)
            if (removed.handled) {
                if (removed.focusPosition !== undefined) {
                    focusBlock(removed.focusRow, false, removed.focusPosition)
                } else {
                    focusBlock(removed.focusRow)
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
                    const blockEditors = orderedEditors().filter(editor => editor.blockIndex === block
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
                        focusBlock(block)
                    else
                        focusEditorAddress({
                            blockIndex: block,
                            listItemIndex: focusItem,
                            tableCellIndex: -1,
                            field: "listItem",
                            cursorPosition: focusPosition
                        })
                    return true
                }

                const tableCells = ranges.filter(range => range.tableCellIndex >= 0)
                if (tableCells.length === ranges.length
                        && tableCells.every(range => range.blockIndex === block)
                        && tableCells.every(range => range.tableRow === tableCells[0].tableRow)) {
                    // A selection crossing neighbouring cells in one table
                    // row is still a text selection, not a request to remove
                    // the row.  Native TextArea only knows the focused cell,
                    // so apply both boundary fragments explicitly.
                    const start = tableCells[0]
                    prepareForStructuralMutation()
                    for (const cell of tableCells) {
                        blockModel.setTableCell(block, cell.tableCellIndex, cell.before + cell.after)
                    }
                    focusEditorAddress({
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
        const selected = orderedEditors().filter(editor => editor.selectionStart !== editor.selectionEnd)
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
            focusBlock(block)
            return true
        }
        const tableCells = selected.filter(editor => editor.tableRow >= 0)
        if (tableCells.length === selected.length) {
            const rows = tableCells.map(editor => editor.tableRow)
            if (Math.min(...rows) === Math.max(...rows))
                return false
            prepareForStructuralMutation()
            blockModel.removeTableRows(block, Math.min(...rows), Math.max(...rows))
            focusBlock(block)
            return true
        }
        return false
    }

    function selectAllDocument() {
        const ordered = orderedEditors()
        if (ordered.length > 0 && ordered.indexOf(activeEditor) < 0) {
            activeEditor = ordered[0]
            activeEditor.forceActiveFocus()
        }
        wholeDocumentSelected = true
        wholeDocumentSelected = true
        documentSelectionAvailable = true
        for (const editor of ordered)
            setEditorSelection(editor, 0, editor.length)
    }

    function insertTextAtCursor(value) {
        if (!activeEditor)
            return false
        return runEditTransaction("insert-text", function() {
            const position = activeEditor.cursorPosition
            activeEditor.insert(position, value)
            activeEditor.cursorPosition = position + value.length
            return true
        })
    }

    function focusInitialEditor() {
        if (activeEditor) {
            activeEditor.forceActiveFocus()
            return true
        }
        const loader = itemAtIndex(0)
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
        clearDocumentSelection()
        clearImageSelection()
        clearAudioSelection()
        clearAttachmentSelection()
        activeEditor = null
        activeTagLineIndex = blockIndex
        positionViewAtIndex(blockIndex, ListView.Contain)

        function applyFocus(attempt) {
            if (generation !== root.focusRequestGeneration
                    || !root.blockModel || root.blockModel.blockTypeAt(blockIndex) !== 9)
                return
            const delegate = root.itemAtIndex(blockIndex)
            const editor = delegate && delegate.item ? delegate.item : null
            if (!editor) {
                if (attempt < 3) {
                    root.positionViewAtIndex(blockIndex, ListView.Contain)
                    Qt.callLater(function() { applyFocus(attempt + 1) })
                } else {
                    root.forceActiveFocus()
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
            return focusImageBlock(blockIndex)
        if (blockModel && blockModel.blockTypeAt(blockIndex) === 10)
            return focusAudioBlock(blockIndex)
        if (blockModel && blockModel.blockTypeAt(blockIndex) === 11)
            return focusAttachmentBlock(blockIndex)
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
        if (count > 0 && blockModel.blockTypeAt(count - 1) !== 0)
            return insertParagraphAtBoundary(count)
        const ordered = orderedEditors()
        if (ordered.length === 0)
            return false
        const editor = ordered[ordered.length - 1]
        clearDocumentSelection()
        editor.forceActiveFocus()
        editor.cursorPosition = editor.length
        activeEditor = editor
        return true
    }

    function selectImageBlock(blockIndex) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== 4)
            return false
        activeTagLineIndex = -1
        ++focusRequestGeneration
        pendingFocusRetry.stop()
        pendingFocusAddress = null
        pendingEditorState = null
        flushPendingEditorChanges()
        clearDocumentSelection()
        activeEditor = null
        selectedAudioIndex = -1
        selectedAttachmentIndex = -1
        selectedImageIndex = blockIndex
        positionViewAtIndex(blockIndex, ListView.Contain)
        return true
    }

    function focusImageBlock(blockIndex) {
        if (!selectImageBlock(blockIndex))
            return false
        Qt.callLater(function() {
            const delegate = root.itemAtIndex(blockIndex)
            if (delegate && delegate.item
                    && typeof delegate.item.forceActiveFocus === "function") {
                delegate.item.forceActiveFocus()
            } else {
                root.forceActiveFocus()
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
        activeTagLineIndex = -1
        ++focusRequestGeneration
        pendingFocusRetry.stop()
        pendingFocusAddress = null
        pendingEditorState = null
        flushPendingEditorChanges()
        clearDocumentSelection()
        activeEditor = null
        selectedImageIndex = -1
        selectedAttachmentIndex = -1
        selectedAudioIndex = blockIndex
        positionViewAtIndex(blockIndex, ListView.Contain)
        return true
    }

    function focusAudioBlock(blockIndex) {
        if (!selectAudioBlock(blockIndex))
            return false
        Qt.callLater(function() {
            const delegate = root.itemAtIndex(blockIndex)
            if (delegate && delegate.item
                    && typeof delegate.item.forceActiveFocus === "function")
                delegate.item.forceActiveFocus()
            else
                root.forceActiveFocus()
        })
        return true
    }

    function clearAudioSelection() {
        selectedAudioIndex = -1
    }

    function selectAttachmentBlock(blockIndex) {
        if (!blockModel || blockModel.blockTypeAt(blockIndex) !== 11)
            return false
        activeTagLineIndex = -1
        ++focusRequestGeneration
        pendingFocusRetry.stop()
        pendingFocusAddress = null
        pendingEditorState = null
        flushPendingEditorChanges()
        clearDocumentSelection()
        activeEditor = null
        selectedImageIndex = -1
        selectedAudioIndex = -1
        selectedAttachmentIndex = blockIndex
        positionViewAtIndex(blockIndex, ListView.Contain)
        return true
    }

    function focusAttachmentBlock(blockIndex) {
        if (!selectAttachmentBlock(blockIndex))
            return false
        Qt.callLater(function() {
            const delegate = root.itemAtIndex(blockIndex)
            if (delegate && delegate.item
                    && typeof delegate.item.forceActiveFocus === "function")
                delegate.item.forceActiveFocus()
            else
                root.forceActiveFocus()
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
        if (blockIndex < count && isMediaBlockType(blockModel.blockTypeAt(blockIndex)))
            return focusMediaBlock(blockIndex)
        if (blockIndex > 0 && isMediaBlockType(blockModel.blockTypeAt(blockIndex - 1)))
            return focusMediaBlock(blockIndex - 1)
        if (blockIndex < count) {
            focusBlock(blockIndex)
            return true
        }
        if (count > 0) {
            focusBlock(count - 1, true)
            return true
        }
        return false
    }

    function focusAfterImageRemoval(blockIndex) {
        return focusAfterMediaRemoval(blockIndex)
    }

    function removeTableBlock(blockIndex, backwards) {
        return runEditTransaction("remove-table", function() {
            const oldCount = count
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                focusBlock(0)
                return true
            }
            const target = backwards ? Math.max(0, blockIndex - 1)
                                     : Math.min(blockIndex, oldCount - 2)
            focusBlock(target, backwards)
            return true
        })
    }

    function focusFollowingBlock(blockIndex, appendIfMissing) {
        if (blockIndex + 1 < count && blockModel.blockTypeAt(blockIndex + 1) === 9)
            return focusTagLineBlock(blockIndex + 1, false, false)
        // Media blocks have no text cursor of their own. Find the next actual editor,
        // rather than pretending the immediately following block is one.
        const ordered = orderedEditors()
        for (const editor of ordered) {
            if (editor.blockIndex <= blockIndex)
                continue
            editor.forceActiveFocus()
            editor.cursorPosition = 0
            activeEditor = editor
            return true
        }
        // The next delegate may not be instantiated yet when it is outside
        // ListView's cache. Locate it from the model and let the normal
        // pending-focus path load it; structural media blocks are intentionally
        // skipped here.
        for (let row = blockIndex + 1; row < count; ++row) {
            if (!isMediaBlockType(blockModel.blockTypeAt(row))) {
                focusBlock(row)
                return true
            }
        }
        if (!appendIfMissing)
            return false
        runEditTransaction("append-following-text-block", function() {
            blockModel.appendTextBlock()
            focusBlock(count - 1)
        })
        return true
    }

    function focusPrecedingBlock(blockIndex) {
        if (blockIndex <= 0)
            return
        if (blockModel.blockTypeAt(blockIndex - 1) === 9) {
            focusTagLineBlock(blockIndex - 1, true, false)
            return
        }
        const ordered = orderedEditors()
        const activeIndex = ordered.indexOf(activeEditor)
        if (activeIndex > 0) {
            const editor = ordered[activeIndex - 1]
            editor.forceActiveFocus()
            editor.cursorPosition = editor.length
            activeEditor = editor
            return
        }
        focusBlock(blockIndex - 1)
    }

    function hasOnlyMediaFollowing(blockIndex) {
        if (blockIndex + 1 >= count)
            return false
        for (let row = blockIndex + 1; row < count; ++row) {
            if (!isMediaBlockType(blockModel.blockTypeAt(row)))
                return false
        }
        return true
    }

    function removeImageBlock(blockIndex, focusAfter) {
        if (blockModel.blockTypeAt(blockIndex) !== 4)
            return false
        const restoreFocus = focusAfter === undefined ? true : Boolean(focusAfter)
        return runEditTransaction("remove-image", function() {
            const oldCount = count
            prepareForStructuralMutation()
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                focusBlock(0)
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
        return runEditTransaction("remove-audio", function() {
            const oldCount = count
            if (editorBackend && editorBackend.audioPlayback)
                editorBackend.audioPlayback.stop()
            prepareForStructuralMutation()
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                focusBlock(0)
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
        return runEditTransaction("remove-attachment", function() {
            const oldCount = count
            prepareForStructuralMutation()
            blockModel.removeBlock(blockIndex)
            if (oldCount === 1) {
                blockModel.appendTextBlock()
                focusBlock(0)
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
        const focusAddress = editorAddress(editor)
        return runEditTransaction("remove-adjacent-media", function() {
            prepareForStructuralMutation()
            blockModel.removeBlock(mediaRow)
            focusAddress.blockIndex = backwards ? editorRow - 1 : editorRow
            focusAddress.selectionStart = focusAddress.cursorPosition
            focusAddress.selectionEnd = focusAddress.cursorPosition
            focusEditorAddress(focusAddress)
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
            for (const candidate of orderedEditors()) {
                if (candidate.blockIndex === mergeRow) {
                    cursorPosition = candidate.length
                    break
                }
            }
        }
        return runEditTransaction("merge-text-blocks", function() {
            prepareForStructuralMutation()
            if (!blockModel.mergeTextBlockWithNext(mergeRow))
                return false
            if (cursorPosition >= 0)
                focusBlock(mergeRow, false, cursorPosition)
            else
                focusBlock(mergeRow, true)
            return true
        })
    }

    function handleStructuredEnter(event, editor) {
        const modifiers = event.modifiers
                & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
        if (!editor || editor.codeDocument
                || (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
                || modifiers)
            return false
        if (blockModel.blockTypeAt(editor.blockIndex) !== 6)
            return false

        const row = editor.blockIndex
        const before = editor.markdownRange(0, editor.selectionStart)
        const after = editor.markdownRange(editor.selectionEnd, editor.length)
        return runEditTransaction("exit-heading", function() {
            prepareForStructuralMutation()
            if (!blockModel.splitStructuredBlockToText(row, before, after))
                return false
            focusBlock(row + 1, false, 0)
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

    function insertionBlockIndex() {
        if (pendingFocusAddress && Number(pendingFocusAddress.blockIndex) >= 0)
            return Number(pendingFocusAddress.blockIndex) + 1
        if (activeTagLineIndex >= 0)
            return activeTagLineIndex + 1
        if (selectedImageIndex >= 0)
            return selectedImageIndex + 1
        if (selectedAudioIndex >= 0)
            return selectedAudioIndex + 1
        if (selectedAttachmentIndex >= 0)
            return selectedAttachmentIndex + 1
        return activeEditor && activeEditor.blockIndex >= 0 ? activeEditor.blockIndex + 1 : count
    }

    function insertionRowAtPoint(x, y) {
        let lastVisibleRow = 0
        for (let row = 0; row < count; ++row) {
            const block = itemAtIndex(row)
            if (!block)
                continue
            const position = block.mapToItem(root, 0, 0)
            if (y < position.y + block.height / 2)
                return row
            lastVisibleRow = row + 1
        }
        return Math.min(count, lastVisibleRow)
    }

    function insertTableBlock() {
        return runEditTransaction("insert-table", function() {
            const row = insertionBlockIndex()
            blockModel.insertTable(row)
            focusBlock(row)
            return true
        })
    }

    function insertCodeBlock(language) {
        return runEditTransaction("insert-code-block", function() {
            const row = insertionBlockIndex()
            blockModel.insertCodeBlock(row, language || "")
            focusBlock(row)
            return true
        })
    }

    function insertBlockQuoteBlock() {
        return runEditTransaction("insert-or-convert-blockquote", function() {
            if (activeEditor && activeEditor.blockIndex >= 0 && !cursorTouchesTitle(activeEditor)) {
                const converted = blockModel.convertTextBlockToQuote(activeEditor.blockIndex,
                                                                     activeEditor.cursorPosition, true)
                if (converted >= 0) {
                    focusBlock(converted)
                    return true
                }
            }
            const row = insertionBlockIndex()
            blockModel.insertBlockQuote(row)
            focusBlock(row)
            return true
        })
    }

    function insertListBlock(type) {
        return runEditTransaction("insert-or-convert-list", function() {
            if (activeEditor && activeEditor.blockIndex >= 0 && !activeEditor.titleDocument) {
                const activeBlock = activeEditor.blockIndex
                if (blockModel.convertListLevel(activeBlock, activeEditor.listItemIndex, type))
                    return true
            }
            const row = insertionBlockIndex()
            blockModel.insertList(row, type)
            focusBlock(row)
            return true
        })
    }

    function titleEnd(editor) {
        if (!editor || !editor.titleDocument)
            return -1
        const text = editor.currentPlainText()
        let end = text.length
        for (const separator of ["\n", "\r", "\u2028", "\u2029"]) {
            const position = text.indexOf(separator)
            if (position >= 0)
                end = Math.min(end, position)
        }
        return end
    }

    function cursorTouchesTitle(editor) {
        const end = titleEnd(editor)
        return end >= 0 && editor.cursorPosition <= end
    }

    function selectionTouchesTitle(editor) {
        const end = titleEnd(editor)
        if (end < 0)
            return false
        if (editor.selectionStart === editor.selectionEnd)
            return editor.cursorPosition <= end
        return editor.selectionStart < end
    }

    function convertActiveToHeading(level) {
        if (!activeEditor || activeEditor.blockIndex < 0 || activeEditor.codeDocument
                || cursorTouchesTitle(activeEditor))
            return false
        if (level === 0 && activeEditor.editorField === "blockquote")
            return convertActiveToQuote(false)
        return runEditTransaction("convert-heading", function() {
            const row = blockModel.convertTextBlockToHeading(activeEditor.blockIndex,
                                                              activeEditor.cursorPosition, level)
            if (row < 0)
                return false
            focusBlock(row)
            return true
        })
    }

    function convertActiveToQuote(quote) {
        if (!activeEditor || activeEditor.blockIndex < 0 || activeEditor.codeDocument
                || cursorTouchesTitle(activeEditor))
            return false
        return runEditTransaction("convert-blockquote", function() {
            const row = blockModel.convertTextBlockToQuote(activeEditor.blockIndex,
                                                            activeEditor.cursorPosition,
                                                            Boolean(quote))
            if (row < 0)
                return false
            focusBlock(row)
            return true
        })
    }

    function applyActiveInlineStyle(style) {
        if (!activeEditor || !editorBackend || activeEditor.editorField === "code"
                || selectionTouchesTitle(activeEditor))
            return false
        return runEditTransaction("inline-" + style, function() {
            applyInlineStyle(activeEditor, style)
            return true
        })
    }

    function editActiveLink() {
        if (!activeEditor || activeEditor.codeDocument || selectionTouchesTitle(activeEditor))
            return false
        openLinkEditor(activeEditor)
        return true
    }

    function handleHeadingShortcut(event, editor) {
        if (!editor || editor.codeDocument)
            return false
        const modifiers = event.modifiers
        if (!(modifiers & Qt.ControlModifier) || modifiers & (Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier)
                || event.key < Qt.Key_0 || event.key > Qt.Key_6)
            return false
        if (cursorTouchesTitle(editor))
            return true
        return runEditTransaction("convert-heading", function() {
            const level = event.key - Qt.Key_0
            const row = blockModel.convertTextBlockToHeading(editor.blockIndex, editor.cursorPosition, level)
            if (row < 0)
                return false
            focusBlock(row)
            return true
        })
    }

    function inlineMarkers(event) {
        const primary = event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)
        if (!primary || event.modifiers & Qt.AltModifier)
            return null
        if (event.key === Qt.Key_B && !(event.modifiers & Qt.ShiftModifier))
            return "bold"
        if (event.key === Qt.Key_I && !(event.modifiers & Qt.ShiftModifier))
            return "italic"
        if (event.key === Qt.Key_U && !(event.modifiers & Qt.ShiftModifier))
            return "underline"
        if (event.key === Qt.Key_S && event.modifiers & Qt.ShiftModifier)
            return "strike"
        if (event.key === Qt.Key_QuoteLeft && !(event.modifiers & Qt.ShiftModifier))
            return "code"
        if (event.key === Qt.Key_K && !(event.modifiers & Qt.ShiftModifier))
            return "link"
        return null
    }

    function applyInlineStyle(editor, style) {
        if (!editor || editor.codeDocument)
            return
        const start = editor.selectionStart
        const end = editor.selectionEnd
        if (start === end) {
            editor.togglePendingInlineStyle(style)
            return
        }
        const formattedEnd = root.editorBackend.applyInlineFormat(editor.textDocument, start, end, style)
        if (formattedEnd < 0)
            return
        editor.select(start, formattedEnd)
        editor.commitText(false)
    }

    function openLinkEditor(editor) {
        if (!editor || editor.codeDocument || selectionTouchesTitle(editor)
                || editor.textFormat !== TextEdit.MarkdownText)
            return
        const info = root.editorBackend.linkInfo(editor.textDocument,
                                            editor.selectionStart,
                                            editor.selectionEnd)
        if (!info.valid)
            return
        editor.select(info.start, info.end)
        linkEditor.openFor(editor, info.start, info.end, info.href)
    }

    function openEditorContextMenu(editor, localX, localY) {
        if (!editor)
            return
        editor.forceActiveFocus()
        activeEditor = editor
        const position = editor.positionAt(localX, localY)
        editor.contextWord = ""
        editor.contextSuggestions = []
        const renderedLink = editor.codeDocument ? ({ valid: false })
                                                   : root.editorBackend.linkInfo(editor.textDocument,
                                                                                 position, position)
        const plainLink = editor.codeDocument ? null : editor.plainLinkInfoAtPosition(position)
        editor.contextLink = renderedLink.valid && renderedLink.href.length > 0
                ? renderedLink.href : (plainLink ? plainLink.href : "")
        if (editor.isSpellingError(position)) {
            editor.cursorPosition = position
            editor.selectWord()
            editor.contextWord = editor.selectedText
            editor.contextStart = editor.selectionStart
            editor.contextEnd = editor.selectionEnd
            editor.contextSuggestions = root.platformBackend
                    ? root.platformBackend.spellingSuggestions(editor.contextWord) : []
        } else if (editor.selectionStart === editor.selectionEnd
                   || position < editor.selectionStart || position > editor.selectionEnd) {
            clearDocumentSelection()
            editor.cursorPosition = position
        }
        contextEditor = editor
        const globalPosition = editor.mapToItem(root, localX, localY)
        sharedContextMenu.popup(root, globalPosition.x, globalPosition.y)
    }

    function openCustomDictionaryEditor() {
        customDictionaryDialog.openEditor()
    }

    function handleInlineFormatting(event, editor) {
        const style = inlineMarkers(event)
        if (!style)
            return false
        if (style === "link") {
            if (!editor || editor.textFormat !== TextEdit.MarkdownText)
                return false
            const selected = orderedEditors().filter(candidate => candidate.selectionStart !== candidate.selectionEnd
                                                      && !selectionTouchesTitle(candidate))
            openLinkEditor(selected.length === 1 ? selected[0] : editor)
            return true
        }
        const selected = orderedEditors().filter(candidate => candidate.selectionStart !== candidate.selectionEnd
                                                  && !selectionTouchesTitle(candidate))
        runEditTransaction("inline-format", function() {
            if (selected.length > 1) {
                for (const candidate of selected)
                    applyInlineStyle(candidate, style)
                refreshSelectionState()
            } else if (selected.length === 1) {
                applyInlineStyle(selected[0], style)
            } else if (!selectionTouchesTitle(editor)) {
                applyInlineStyle(editor, style)
            }
        })
        return true
    }

    CustomDictionaryDialog {
        id: customDictionaryDialog
        editorView: root
    }

    LinkEditorPopup {
        id: linkEditor
        editorView: root
    }

    function trailingViewportTop() {
        // mapToItem() is not itself a bindable property. Reading contentY and
        // contentHeight makes the geometry below update while scrolling or
        // while the final delegate changes height.
        const scrollDependency = contentY + contentHeight
        if (count <= 0)
            return 0
        const lastBlock = itemAtIndex(count - 1)
        if (!lastBlock)
            return height
        const bottom = lastBlock.mapToItem(root, 0, lastBlock.height).y
        return Math.max(0, Math.min(height, bottom))
    }

    // The ListView spacing is a real document boundary. A click inserts a
    // session-only empty text block there; dragging from the same area starts
    // selection at the boundary instead of being ignored.
    InterBlockHitLayer {
        editorView: root
        reorderController: editorReorderController
    }

    TrailingDocumentArea {
        editorView: root
        reorderController: editorReorderController
    }

    EditorBlockFactories {
        id: blockFactories
        editorView: root
        reorderController: editorReorderController
    }

    delegate: EditorBlockDelegate {
        editorView: root
        reorderController: editorReorderController
        textEditorComponent: blockFactories.textEditor
        tagLineEditorComponent: blockFactories.tagLineEditor
        codeBlockEditorComponent: blockFactories.codeBlockEditor
        headingEditorComponent: blockFactories.headingEditor
        blockQuoteEditorComponent: blockFactories.blockQuoteEditor
        listEditorComponent: blockFactories.listEditor
        tableEditorComponent: blockFactories.tableEditor
        imageEditorComponent: blockFactories.imageEditor
        audioEditorComponent: blockFactories.audioEditor
        attachmentEditorComponent: blockFactories.attachmentEditor
    }

}
