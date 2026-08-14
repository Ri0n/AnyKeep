import QtQuick
import QtQuick.Controls
import "support/EditorMarkdownRendering.js" as MarkdownRendering
import "controllers" as Controllers

ListView {
    id: root
    property var blockModel: typeof noteBlockModel !== "undefined" ? noteBlockModel : null
    property var editorBackend: typeof noteEditor !== "undefined" ? noteEditor : null
    property var platformBackend: typeof qmlNoteEditor !== "undefined" ? qmlNoteEditor : null
    property var audioTranscriptionController: null
    property alias activeEditor: focusController.activeEditor
    property alias activeTagLineIndex: focusController.activeTagLineIndex
    property alias selectedImageIndex: mediaNavigationController.selectedImageIndex
    property alias selectedAudioIndex: mediaNavigationController.selectedAudioIndex
    property alias selectedAttachmentIndex: mediaNavigationController.selectedAttachmentIndex
    property alias imageAltEditorFocused: focusController.imageAltEditorFocused
    property alias pendingFocusAddress: focusController.pendingFocusAddress
    property alias pendingEditorState: focusController.pendingEditorState
    property alias focusRequestGeneration: focusController.focusRequestGeneration
    property alias editors: focusController.editors
    property alias selectionAnchorEditor: selectionController.selectionAnchorEditor
    property alias selectionAnchorPosition: selectionController.selectionAnchorPosition
    property alias wholeDocumentSelected: selectionController.wholeDocumentSelected
    property alias editorRegistrations: focusController.editorRegistrations
    property alias fullSelectionPasses: selectionController.fullSelectionPasses
    property alias selectionSpansEditors: selectionController.selectionSpansEditors
    property alias documentSelectionStartEditor: selectionController.documentSelectionStartEditor
    property alias documentSelectionStartPosition: selectionController.documentSelectionStartPosition
    property alias documentSelectionEndEditor: selectionController.documentSelectionEndEditor
    property alias documentSelectionEndPosition: selectionController.documentSelectionEndPosition
    property alias documentSelectionAvailable: selectionController.documentSelectionAvailable
    property alias contextEditor: selectionController.contextEditor
    property alias mouseSelectionActive: selectionController.mouseSelectionActive
    property alias blankSelectionBoundary: selectionController.blankSelectionBoundary
    property alias blankSelectionPressX: selectionController.blankSelectionPressX
    property alias blankSelectionPressY: selectionController.blankSelectionPressY
    property alias blankSelectionMoved: selectionController.blankSelectionMoved
    property alias blankSelectionAnchorEditor: selectionController.blankSelectionAnchorEditor
    property alias blankSelectionAnchorPosition: selectionController.blankSelectionAnchorPosition
    property alias documentSelectionBlankBoundary: selectionController.documentSelectionBlankBoundary
    property alias documentSelectionBlankDirection: selectionController.documentSelectionBlankDirection
    property alias keyboardSelectionAnchorEditor: selectionController.keyboardSelectionAnchorEditor
    property alias keyboardSelectionAnchorPosition: selectionController.keyboardSelectionAnchorPosition
    property alias pendingInsertionBoundary: selectionController.pendingInsertionBoundary
    property alias editTransactionDepth: selectionController.editTransactionDepth
    property alias suppressCursorVisibility: focusController.suppressCursorVisibility
    property alias viewportRestoreGeneration: focusController.viewportRestoreGeneration

    property var currentFindResult: ({})
    property string currentFindText: ""
    signal findRequested()
    readonly property alias linkEditorPopup: linkEditor
    readonly property bool touchMode: Qt.platform.os === "android" || Qt.platform.os === "ios"
    readonly property font editorFont: typeof mobileApp !== "undefined"
                                       ? Qt.font({ family: Application.font.family,
                                                   pointSize: mobileApp.editorFontSize })
                                       : platformBackend && platformBackend.editorFont !== undefined
                                         ? platformBackend.editorFont : Application.font
    readonly property real editorPointSize: editorFont.pointSize > 0
                                            ? editorFont.pointSize : Application.font.pointSize
    onEditorFontChanged: {
        Qt.callLater(function() {
            if (root.platformBackend)
                root.platformBackend.rehighlight()
        })
    }
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


    Controllers.EditorFocusController {
        id: focusController
        editorView: root
        blockModel: root.blockModel
        editorBackend: root.editorBackend
    }

    Controllers.EditorSelectionController {
        id: selectionController
        editorView: root
        blockModel: root.blockModel
        editorBackend: root.editorBackend
        platformBackend: root.platformBackend
        focusCoordinator: focusController
    }

    Controllers.EditorMediaNavigationController {
        id: mediaNavigationController
        editorView: root
        blockModel: root.blockModel
        editorBackend: root.editorBackend
        focusCoordinator: focusController
    }

    EditorContextMenu {
        id: sharedContextMenu
        controller: root
        editorBackend: root.editorBackend
        platformBackend: root.platformBackend
    }

    function registerEditor(editor) { return focusController.registerEditor(editor) }
    function unregisterEditor(editor) { return focusController.unregisterEditor(editor) }
    function scheduleCursorVisibility(editor) { return focusController.scheduleCursorVisibility(editor) }
    function ensureEditorCursorVisible(editor) { return focusController.ensureEditorCursorVisible(editor) }
    function orderedEditors() { return focusController.orderedEditors() }
    function editorAddress(editor, position) { return focusController.editorAddress(editor, position) }
    function captureEditorState() { return focusController.captureEditorState() }
    function documentHistoryOwnsFocus() { return focusController.documentHistoryOwnsFocus() }
    function addressMatchesEditor(address, editor, exact) { return focusController.addressMatchesEditor(address, editor, exact) }
    function editorForAddress(address, candidate) { return focusController.editorForAddress(address, candidate) }
    function applyEditorAddress(editor, address) { return focusController.applyEditorAddress(editor, address) }
    function preserveViewportAt(requestedY) { return focusController.preserveViewportAt(requestedY) }
    function tryRestorePendingEditorState() { return focusController.tryRestorePendingEditorState() }
    function tryPendingEditorFocus(candidate) { return focusController.tryPendingEditorFocus(candidate) }
    function focusEditorAddress(address) { return focusController.focusEditorAddress(address) }
    function restoreEditorState(state) { return focusController.restoreEditorState(state) }
    function editorGeometry(index) { return focusController.editorGeometry(index) }
    function activeEditorIndex() { return focusController.activeEditorIndex() }
    function editorIsBold(index) { return focusController.editorIsBold(index) }
    function selectedEditorCount() { return focusController.selectedEditorCount() }
    function editorAtPoint(x, y) { return focusController.editorAtPoint(x, y) }
    function insertExternalTextAtPoint(value, x, y, codeLanguage) { return focusController.insertExternalTextAtPoint(value, x, y, codeLanguage) }
    function captureSpeechInsertionTarget() { return focusController.captureSpeechInsertionTarget() }
    function insertTextIntoEditor(editor, value, address) { return focusController.insertTextIntoEditor(editor, value, address) }
    function insertTextAtTarget(value, target) { return focusController.insertTextAtTarget(value, target) }
    function insertTextAtCursor(value) { return focusController.insertTextAtCursor(value) }
    function focusInitialEditor() { return focusController.focusInitialEditor() }
    function focusTagLineBlock(blockIndex, atEnd, focusDraft) { return focusController.focusTagLineBlock(blockIndex, atEnd, focusDraft) }
    function focusBlock(blockIndex, atEnd, position) { return focusController.focusBlock(blockIndex, atEnd, position) }
    function focusBlockAtMarkdownSourcePosition(blockIndex, sourcePosition) {
        return focusController.focusBlockAtMarkdownSourcePosition(blockIndex, sourcePosition)
    }
    function firstEditorIn(item) { return focusController.firstEditorIn(item) }
    function focusPendingBlock() { return focusController.focusPendingBlock() }
    function focusDocumentEnd() { return focusController.focusDocumentEnd() }
    function refreshSelectionState() { return selectionController.refreshSelectionState() }
    function scheduleSelectionStateRefresh() { return selectionController.scheduleSelectionStateRefresh() }
    function selectionAnchorAtBoundary(boundary, direction) { return selectionController.selectionAnchorAtBoundary(boundary, direction) }
    function beginBlankAreaSelection(boundary, x, y) { return selectionController.beginBlankAreaSelection(boundary, x, y) }
    function updateBlankAreaSelection(x, y) { return selectionController.updateBlankAreaSelection(x, y) }
    function applyMouseDocumentSelection(anchorEditor, anchorPosition, x, y) {
        return selectionController.applyMouseDocumentSelection(anchorEditor, anchorPosition, x, y)
    }
    function finishBlankAreaSelection() { return selectionController.finishBlankAreaSelection() }
    function cancelBlankAreaSelection() { return selectionController.cancelBlankAreaSelection() }
    function insertParagraphAtBoundary(row) { return selectionController.insertParagraphAtBoundary(row) }
    function scheduleDiscardEmptyInsertedParagraph(editor) { return selectionController.scheduleDiscardEmptyInsertedParagraph(editor) }
    function clearPendingInsertionBoundary() { return selectionController.clearPendingInsertionBoundary() }
    function handleEmptyTextBlockDeletion(event, editor) { return selectionController.handleEmptyTextBlockDeletion(event, editor) }
    function clearDocumentSelection() { return selectionController.clearDocumentSelection() }
    function flushPendingEditorChanges() { return selectionController.flushPendingEditorChanges() }
    function beginEditTransaction(kind, flushEditors) { return selectionController.beginEditTransaction(kind, flushEditors) }
    function endEditTransaction() { return selectionController.endEditTransaction() }
    function runEditTransaction(kind, callback, flushEditors) { return selectionController.runEditTransaction(kind, callback, flushEditors) }
    function prepareForStructuralMutation() { return selectionController.prepareForStructuralMutation() }
    function prepareForHistoryRestore() { return selectionController.prepareForHistoryRestore() }
    function setEditorSelection(editor, start, end) { return selectionController.setEditorSelection(editor, start, end) }
    function handleKeyboardSelection(event, editor) { return selectionController.handleKeyboardSelection(event, editor) }
    function applyDocumentSelection(anchorEditor, anchorPosition, focusEditor, focusPosition, preserveBlankBoundary) { return selectionController.applyDocumentSelection(anchorEditor, anchorPosition, focusEditor, focusPosition, preserveBlankBoundary) }
    function hasDocumentSelection() { return selectionController.hasDocumentSelection() }
    function structuralBlockSelected(blockIndex) { return selectionController.structuralBlockSelected(blockIndex) }
    function selectedDocumentText() { return selectionController.selectedDocumentText() }
    function selectedDocumentMarkdown() { return selectionController.selectedDocumentMarkdown() }
    function selectionNeedsStructure(ranges) { return selectionController.selectionNeedsStructure(ranges) }
    function structuredSelectionRanges(includeBoundaryEditors) { return selectionController.structuredSelectionRanges(includeBoundaryEditors) }
    function fullySelectedTableBlock(ranges) { return selectionController.fullySelectedTableBlock(ranges) }
    function copyDocumentSelection() { return selectionController.copyDocumentSelection() }
    function copyDocumentSelectionAsMarkdown() { return selectionController.copyDocumentSelectionAsMarkdown() }
    function copyDocumentSelectionToPrimary() { return selectionController.copyDocumentSelectionToPrimary() }
    function copyActiveSelection() { return selectionController.copyActiveSelection() }
    function renameAudioBlock(blockIndex, title) { return selectionController.renameAudioBlock(blockIndex, title) }
    function pasteStructuredSelection(editor) { return selectionController.pasteStructuredSelection(editor) }
    function pasteClipboard() { return selectionController.pasteClipboard() }
    function cutDocumentSelection() { return selectionController.cutDocumentSelection() }
    function cutActiveSelection() { return selectionController.cutActiveSelection() }
    function deleteStructuredSelection(backwards) { return selectionController.deleteStructuredSelection(backwards) }
    function deleteStructuredSelectionImpl(backwards) { return selectionController.deleteStructuredSelectionImpl(backwards) }
    function selectAllDocument() { return selectionController.selectAllDocument() }
    function selectImageBlock(blockIndex) { return mediaNavigationController.selectImageBlock(blockIndex) }
    function focusImageBlock(blockIndex) { return mediaNavigationController.focusImageBlock(blockIndex) }
    function clearImageSelection() { return mediaNavigationController.clearImageSelection() }
    function selectAudioBlock(blockIndex) { return mediaNavigationController.selectAudioBlock(blockIndex) }
    function focusAudioBlock(blockIndex) { return mediaNavigationController.focusAudioBlock(blockIndex) }
    function clearAudioSelection() { return mediaNavigationController.clearAudioSelection() }
    function selectAttachmentBlock(blockIndex) { return mediaNavigationController.selectAttachmentBlock(blockIndex) }
    function focusAttachmentBlock(blockIndex) { return mediaNavigationController.focusAttachmentBlock(blockIndex) }
    function clearAttachmentSelection() { return mediaNavigationController.clearAttachmentSelection() }
    function isMediaBlockType(type) { return mediaNavigationController.isMediaBlockType(type) }
    function focusMediaBlock(blockIndex) { return mediaNavigationController.focusMediaBlock(blockIndex) }
    function focusAfterMediaRemoval(blockIndex) { return mediaNavigationController.focusAfterMediaRemoval(blockIndex) }
    function focusAfterImageRemoval(blockIndex) { return mediaNavigationController.focusAfterImageRemoval(blockIndex) }
    function removeTableBlock(blockIndex, backwards) { return mediaNavigationController.removeTableBlock(blockIndex, backwards) }
    function focusFollowingBlock(blockIndex, appendIfMissing) { return mediaNavigationController.focusFollowingBlock(blockIndex, appendIfMissing) }
    function focusPrecedingBlock(blockIndex) { return mediaNavigationController.focusPrecedingBlock(blockIndex) }
    function hasOnlyMediaFollowing(blockIndex) { return mediaNavigationController.hasOnlyMediaFollowing(blockIndex) }
    function removeImageBlock(blockIndex, focusAfter) { return mediaNavigationController.removeImageBlock(blockIndex, focusAfter) }
    function removeAudioBlock(blockIndex, focusAfter) { return mediaNavigationController.removeAudioBlock(blockIndex, focusAfter) }
    function removeAttachmentBlock(blockIndex, focusAfter) { return mediaNavigationController.removeAttachmentBlock(blockIndex, focusAfter) }
    function handleAdjacentImageDeletion(event, editor) { return mediaNavigationController.handleAdjacentImageDeletion(event, editor) }
    function handleAdjacentTextBlockMerge(event, editor) { return mediaNavigationController.handleAdjacentTextBlockMerge(event, editor) }
    function handleStructuredEnter(event, editor) { return mediaNavigationController.handleStructuredEnter(event, editor) }
    function handleBlockBoundaryNavigation(event, editor) { return mediaNavigationController.handleBlockBoundaryNavigation(event, editor) }

    function insertionBlockIndex() {
        if (pendingInsertionBoundary >= 0)
            return Math.max(0, Math.min(pendingInsertionBoundary, count))
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
            if (pendingInsertionBoundary >= 0 && row < blockModel.rowCount()
                    && blockModel.isExplicitEmptyTextBlock(row))
                blockModel.removeBlock(row)
            pendingInsertionBoundary = -1
            blockModel.insertCodeBlock(row, language || "")
            focusBlock(row)
            return true
        })
    }

    function insertBlockQuoteBlock() {
        if (hasDocumentSelection())
            return convertActiveToQuote(true)
        return runEditTransaction("insert-or-convert-blockquote", function() {
            if (activeEditor && activeEditor.blockIndex >= 0 && !cursorTouchesTitle(activeEditor)) {
                const sourcePosition = activeEditor.markdownSourcePosition()
                const focusSourcePosition
                        = blockModel.sourcePositionInParagraph(activeEditor.blockIndex, sourcePosition)
                const converted = blockModel.convertTextBlockToQuote(activeEditor.blockIndex,
                                                                     sourcePosition, true)
                if (converted >= 0) {
                    focusBlockAtMarkdownSourcePosition(converted, focusSourcePosition)
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
            const sourcePosition = activeEditor.markdownSourcePosition()
            const focusSourcePosition = level === 0
                    ? blockModel.sourcePositionAfterTextCoalesce(activeEditor.blockIndex, sourcePosition)
                    : blockModel.sourcePositionInParagraph(activeEditor.blockIndex, sourcePosition)
            const row = blockModel.convertTextBlockToHeading(activeEditor.blockIndex, sourcePosition, level)
            if (row < 0)
                return false
            focusBlockAtMarkdownSourcePosition(row, focusSourcePosition)
            return true
        })
    }

    function convertActiveToQuote(quote) {
        if (!activeEditor || activeEditor.blockIndex < 0 || activeEditor.codeDocument
                || cursorTouchesTitle(activeEditor))
            return false
        return runEditTransaction("convert-blockquote", function() {
            if (quote && activeEditor.selectionStart !== activeEditor.selectionEnd
                    && !selectionSpansEditors) {
                const visualStart = activeEditor.selectionStart
                const visualEnd = activeEditor.selectionEnd
                const sourceStart = activeEditor.markdownRange(0, visualStart).length
                const sourceEnd = activeEditor.markdownRange(0, visualEnd).length
                const cursorAtStart = activeEditor.cursorPosition === visualStart
                const result = blockModel.convertTextRangeToQuote(activeEditor.blockIndex,
                                                                  sourceStart, sourceEnd)
                if (!result.handled)
                    return false
                focusEditorAddress({
                    blockIndex: Number(result.row),
                    markdownSourcePosition: cursorAtStart ? Number(result.selectionStart)
                                                          : Number(result.selectionEnd),
                    markdownSelectionStart: Number(result.selectionStart),
                    markdownSelectionEnd: Number(result.selectionEnd)
                })
                return true
            }
            const sourcePosition = activeEditor.markdownSourcePosition()
            const focusSourcePosition = !quote
                    ? blockModel.sourcePositionAfterTextCoalesce(activeEditor.blockIndex, sourcePosition)
                    : blockModel.sourcePositionInParagraph(activeEditor.blockIndex, sourcePosition)
            const row = blockModel.convertTextBlockToQuote(activeEditor.blockIndex, sourcePosition, Boolean(quote))
            if (row < 0)
                return false
            focusBlockAtMarkdownSourcePosition(row, focusSourcePosition)
            return true
        })
    }

    function applyActiveInlineStyle(style) {
        if (!editorBackend)
            return false
        if (style === "code" && !hasDocumentSelection()
                && (activeEditor || pendingInsertionBoundary >= 0))
            return insertCodeBlock("")
        if (!activeEditor || activeEditor.editorField === "code")
            return false
        if (selectionTouchesTitle(activeEditor))
            return false
        if (style === "code" && hasDocumentSelection()) {
            const selectedText = selectedDocumentText().replace(/\r\n/g, "\n")
                                                       .replace(/[\r\u2028\u2029]/g, "\n")
            // Inline code is deliberately a one-line operation. Multiline
            // selections either become one code block or remain unchanged if
            // they contain a structural/non-text block.
            if (selectedText.indexOf("\n") >= 0)
                return convertTextSelectionToCodeBlock(selectedText)
        }
        return runEditTransaction("inline-" + style, function() {
            const editor = activeEditor
            const selectionStart = editor.selectionStart
            const selectionEnd = editor.selectionEnd
            applyInlineStyle(editor, style)
            if (selectionStart !== selectionEnd) {
                Qt.callLater(function() {
                    if (!editor)
                        return
                    editor.forceActiveFocus()
                    editor.select(Math.min(selectionStart, editor.length),
                                  Math.min(selectionEnd, editor.length))
                    root.activeEditor = editor
                    root.scheduleCursorVisibility(editor)
                })
            }
            return true
        })
    }

    function activeInlineStyleEnabled(style) {
        if (!activeEditor || !editorBackend || activeEditor.codeDocument
                || selectionTouchesTitle(activeEditor))
            return false
        const pending = activeEditor.pendingInlineValue(style)
        return pending >= 0 ? pending === 1
                            : editorBackend.inlineFormatEnabled(activeEditor.textDocument,
                                                                activeEditor.cursorPosition, style)
    }

    function convertTextSelectionToCodeBlock(plainText) {
        if (!editorBackend.markdown || !hasDocumentSelection() || wholeDocumentSelected)
            return false
        const ranges = structuredSelectionRanges(true)
        if (ranges.length === 0)
            return false
        for (const range of ranges) {
            if (Number(range.blockIndex) <= 0 || blockModel.blockTypeAt(Number(range.blockIndex)) !== 0
                    || Number(range.listItemIndex) >= 0 || Number(range.tableCellIndex) >= 0) {
                return false
            }
        }
        if (plainText.indexOf("\n") < 0)
            return false
        return runEditTransaction("selection-to-code-block", function() {
            prepareForStructuralMutation()
            const converted = editorBackend.convertSelectionToCodeBlock(ranges, plainText, "")
            if (!converted.handled)
                return false
            focusBlock(converted.focusRow, true)
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
            const sourcePosition = editor.markdownSourcePosition()
            const focusSourcePosition = level === 0
                    ? blockModel.sourcePositionAfterTextCoalesce(editor.blockIndex, sourcePosition)
                    : blockModel.sourcePositionInParagraph(editor.blockIndex, sourcePosition)
            const row = blockModel.convertTextBlockToHeading(editor.blockIndex, sourcePosition, level)
            if (row < 0)
                return false
            focusBlockAtMarkdownSourcePosition(row, focusSourcePosition)
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
        if (style === "code") {
            if (editor && activeEditor !== editor)
                activeEditor = editor
            applyActiveInlineStyle("code")
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
