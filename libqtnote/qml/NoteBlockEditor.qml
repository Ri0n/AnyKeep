import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ListBlockBehavior.js" as ListBlockBehavior
import "TableBlockBehavior.js" as TableBlockBehavior
import "reorder" as Reorder

ListView {
    id: root
    property var blockModel: typeof noteBlockModel !== "undefined" ? noteBlockModel : null
    property var editorBackend: typeof noteEditor !== "undefined" ? noteEditor : null
    property var platformBackend: typeof qmlNoteEditor !== "undefined" ? qmlNoteEditor : null
    property var activeEditor: null
    property int selectedImageIndex: -1
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
    property var keyboardSelectionAnchorEditor: null
    property int keyboardSelectionAnchorPosition: 0
    property int editTransactionDepth: 0
    property bool suppressCursorVisibility: false
    property int viewportRestoreGeneration: 0
    property var currentFindResult: ({})
    property string currentFindText: ""
    signal findRequested()
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

    EditorReorderController {
        id: editorReorderController
        editorView: root
        blockModel: root.blockModel
    }

    function formattedLinkLabelRuns(label) {
        const runs = []
        let buffer = ""
        let bold = false
        let italic = false
        let strike = false
        let codeDelimiter = ""
        let foundFormatting = false

        function appendRun() {
            if (buffer.length === 0)
                return
            runs.push({
                text: buffer,
                bold: bold,
                italic: italic,
                strike: strike,
                code: codeDelimiter.length > 0
            })
            buffer = ""
        }

        function backtickRunAt(position) {
            let end = position
            while (end < label.length && label.charAt(end) === "`")
                ++end
            return label.substring(position, end)
        }

        function isWordCharacter(character) {
            if (!character)
                return false
            return /[0-9A-Za-z]/.test(character)
                    || character.toUpperCase() !== character.toLowerCase()
        }

        function underscoreIsIntraword(position, length) {
            const previous = position > 0 ? label.charAt(position - 1) : ""
            const next = position + length < label.length
                       ? label.charAt(position + length) : ""
            return isWordCharacter(previous) && isWordCharacter(next)
        }

        for (let index = 0; index < label.length;) {
            const character = label.charAt(index)
            if (character === "\\" && index + 1 < label.length) {
                buffer += label.substring(index, index + 2)
                index += 2
                continue
            }

            if (character === "`") {
                const delimiter = backtickRunAt(index)
                if (codeDelimiter.length === 0 || codeDelimiter === delimiter) {
                    appendRun()
                    codeDelimiter = codeDelimiter.length === 0 ? delimiter : ""
                    foundFormatting = true
                    index += delimiter.length
                    continue
                }
            }

            if (codeDelimiter.length === 0) {
                const pair = label.substring(index, index + 2)
                if (pair === "**" || (pair === "__" && !underscoreIsIntraword(index, 2))) {
                    appendRun()
                    bold = !bold
                    foundFormatting = true
                    index += 2
                    continue
                }
                if (pair === "~~") {
                    appendRun()
                    strike = !strike
                    foundFormatting = true
                    index += 2
                    continue
                }
                if (character === "*"
                        || (character === "_" && !underscoreIsIntraword(index, 1))) {
                    appendRun()
                    italic = !italic
                    foundFormatting = true
                    ++index
                    continue
                }
            }

            buffer += character
            ++index
        }

        appendRun()
        if (!foundFormatting || bold || italic || strike || codeDelimiter.length > 0)
            return null
        return runs
    }

    function codeSpanForRendering(text) {
        let maximumRun = 0
        let currentRun = 0
        for (let index = 0; index < text.length; ++index) {
            if (text.charAt(index) === "`") {
                ++currentRun
                maximumRun = Math.max(maximumRun, currentRun)
            } else {
                currentRun = 0
            }
        }
        let delimiter = ""
        for (let index = 0; index <= maximumRun; ++index)
            delimiter += "`"
        return delimiter + text + delimiter
    }

    function renderFormattedLinkRun(run) {
        let value = run.code ? codeSpanForRendering(run.text) : run.text
        if (run.italic)
            value = "*" + value + "*"
        if (run.bold)
            value = "**" + value + "**"
        if (run.strike)
            value = "~~" + value + "~~"
        return value
    }

    function markdownForRendering(source) {
        let rendered = underlineMarkupForRendering(source || "")
        if (!rendered || (rendered.indexOf("*") < 0 && rendered.indexOf("_") < 0
                          && rendered.indexOf("~") < 0 && rendered.indexOf("`") < 0))
            return rendered

        // QTextDocument can lose nested or intraword formatting inside a
        // link label. Give every uniform style run its own adjacent link.
        // The C++ serializer joins the runs back into one Markdown link.
        const linkPattern = /\[((?:\\.|[^\]\\\n])*)\]\(([^)\n]+)\)/g
        return rendered.replace(linkPattern, function(fullMatch, label, destination, offset, wholeText) {
            if (offset > 0 && wholeText.charAt(offset - 1) === "!")
                return fullMatch

            const runs = formattedLinkLabelRuns(label)
            if (runs === null)
                return fullMatch

            let result = ""
            for (const run of runs) {
                if (run.text.length === 0)
                    continue
                result += "[" + renderFormattedLinkRun(run) + "](" + destination + ")"
            }
            return result.length > 0 ? result : fullMatch
        })
    }

    function underlineMarkupForRendering(source) {
        const underlinePattern = /^<(ins|u)(?:\s[^>]*)?>([\s\S]*?)<\/\1\s*>/i
        let result = ""
        let codeDelimiter = ""
        for (let index = 0; index < source.length;) {
            const character = source.charAt(index)
            if (character === "\\" && index + 1 < source.length) {
                result += source.substring(index, index + 2)
                index += 2
                continue
            }
            if (character === "`") {
                let end = index
                while (end < source.length && source.charAt(end) === "`")
                    ++end
                const delimiter = source.substring(index, end)
                if (codeDelimiter.length === 0 || codeDelimiter === delimiter)
                    codeDelimiter = codeDelimiter.length === 0 ? delimiter : ""
                result += delimiter
                index = end
                continue
            }
            if (codeDelimiter.length === 0 && character === "<") {
                const match = source.substring(index).match(underlinePattern)
                if (match) {
                    result += "QTNOTEINSOPEN7F3A" + match[2] + "QTNOTEINSCLOSE7F3A"
                    index += match[0].length
                    continue
                }
            }
            result += character
            ++index
        }
        return result
    }

    function markdownTableCellForRendering(source) {
        // The model stores a table-cell hard break as a newline and writes it
        // as <br>. Recreate Qt's in-document line separator directly so the
        // editable value remains one QTextDocument block.
        return markdownForRendering(source).replace(/\n/g, "\u2028")
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
            selectedImageIndex: selectedImageIndex,
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
        const imageIndex = Number(state.selectedImageIndex === undefined ? -1 : state.selectedImageIndex)
        if (imageIndex >= 0 && blockModel.blockTypeAt(imageIndex) === 4) {
            pendingEditorState = null
            focusImageBlock(imageIndex)
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

    function clearDocumentSelection() {
        wholeDocumentSelected = false
        selectionSpansEditors = false
        documentSelectionStartEditor = null
        documentSelectionStartPosition = 0
        documentSelectionEndEditor = null
        documentSelectionEndPosition = 0
        documentSelectionAvailable = false
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

    function beginEditTransaction(kind) {
        if (editTransactionDepth === 0) {
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

    function runEditTransaction(kind, callback) {
        beginEditTransaction(kind)
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

    function applyDocumentSelection(anchorEditor, anchorPosition, focusEditor, focusPosition) {
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
        return ranges
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
                return true
            })
            if (titlePasted)
                return true
        }
        if (platformBackend && typeof platformBackend.insertClipboardImage === "function"
                && platformBackend.insertClipboardImage(insertionBlockIndex()))
            return true
        return runEditTransaction("paste", function() {
            if (!pasteStructuredSelection(activeEditor))
                activeEditor.paste()
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

    function deleteStructuredSelection() {
        // Ordinary selection inside one editor is handled by TextArea. Avoid
        // taking a structural before-state for every Backspace/Delete key.
        if (!wholeDocumentSelected && !selectionSpansEditors && selectedEditorCount() < 2)
            return false
        return runEditTransaction("delete-selection", function() {
            return deleteStructuredSelectionImpl()
        })
    }

    function deleteStructuredSelectionImpl() {
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

    function focusBlock(blockIndex, atEnd, position) {
        return focusEditorAddress({
            blockIndex: blockIndex,
            listItemIndex: -1,
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
        // Images deliberately have no TextArea. A click below a document
        // ending in one therefore means "continue after the image", not
        // "move to the end of the preceding text block".
        if (count > 0 && blockModel.blockTypeAt(count - 1) === 4) {
            const imageRow = count - 1
            return runEditTransaction("append-text-after-image", function() {
                blockModel.appendTextBlock()
                focusBlock(imageRow + 1)
                return true
            })
        }
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
        ++focusRequestGeneration
        pendingFocusRetry.stop()
        pendingFocusAddress = null
        pendingEditorState = null
        flushPendingEditorChanges()
        clearDocumentSelection()
        activeEditor = null
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

    function focusAfterImageRemoval(blockIndex) {
        if (blockIndex < count && blockModel.blockTypeAt(blockIndex) === 4)
            return focusImageBlock(blockIndex)
        if (blockIndex > 0 && blockModel.blockTypeAt(blockIndex - 1) === 4)
            return focusImageBlock(blockIndex - 1)
        for (let row = blockIndex; row < count; ++row) {
            if (blockModel.blockTypeAt(row) !== 4) {
                focusBlock(row)
                return true
            }
        }
        for (let row = Math.min(blockIndex - 1, count - 1); row >= 0; --row) {
            if (blockModel.blockTypeAt(row) !== 4) {
                focusBlock(row, true)
                return true
            }
        }
        return false
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
        // Images have no cursor of their own. Find the next actual editor,
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
        // pending-focus path load it; only image blocks are intentionally
        // skipped here.
        for (let row = blockIndex + 1; row < count; ++row) {
            if (blockModel.blockTypeAt(row) !== 4) {
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

    function hasOnlyImagesFollowing(blockIndex) {
        if (blockIndex + 1 >= count)
            return false
        for (let row = blockIndex + 1; row < count; ++row) {
            if (blockModel.blockTypeAt(row) !== 4)
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

    function handleAdjacentImageDeletion(event, editor) {
        if (!editor || event.modifiers || editor.selectionStart !== editor.selectionEnd)
            return false
        const backwards = event.key === Qt.Key_Backspace && editor.cursorPosition === 0
        const forwards = event.key === Qt.Key_Delete && editor.cursorPosition === editor.length
        if (!backwards && !forwards)
            return false
        const editorRow = editor.blockIndex
        const imageRow = backwards ? editorRow - 1 : editorRow + 1
        if (blockModel.blockTypeAt(imageRow) !== 4)
            return false
        const focusAddress = editorAddress(editor)
        return runEditTransaction("remove-adjacent-image", function() {
            prepareForStructuralMutation()
            blockModel.removeBlock(imageRow)
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
            focusFollowingBlock(
                editor.blockIndex,
                !event.isAutoRepeat
                    && (editor.codeDocument || hasOnlyImagesFollowing(editor.blockIndex)))
        }
        return true
    }

    function insertionBlockIndex() {
        if (pendingFocusAddress && Number(pendingFocusAddress.blockIndex) >= 0)
            return Number(pendingFocusAddress.blockIndex) + 1
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
        if (!platformBackend)
            return
        customDictionaryText.text = platformBackend.customSpellingDictionary().join("\n")
        customDictionaryDialog.open()
        customDictionaryText.forceActiveFocus()
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

    Dialog {
        id: customDictionaryDialog
        parent: Overlay.overlay
        title: qsTr("Custom Dictionary")
        modal: true
        standardButtons: Dialog.Save | Dialog.Cancel
        width: Math.min(480, Math.max(280, parent ? parent.width - 32 : 420))
        height: Math.min(520, Math.max(260, parent ? parent.height - 48 : 420))
        onAccepted: {
            if (root.platformBackend)
                root.platformBackend.setCustomSpellingDictionary(customDictionaryText.text.split(/\r?\n/))
        }

        ColumnLayout {
            anchors.fill: parent

            Label {
                Layout.fillWidth: true
                text: qsTr("One word per line")
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true

                TextArea {
                    id: customDictionaryText
                    objectName: "customDictionaryText"
                    wrapMode: TextEdit.NoWrap
                    selectByMouse: true
                }
            }
        }
    }

    Popup {
        id: linkEditor
        parent: Overlay.overlay
        modal: false
        focus: !hoverMode
        padding: root.touchMode ? 12 : 8
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: root.touchMode
               ? Math.max(160, parent ? parent.width - 24 : 360)
               : Math.min(420, Math.max(240, parent ? parent.width - 16 : 420))

        property var editor: null
        property int selectionStart: 0
        property int selectionEnd: 0
        property bool hadLink: false
        property bool hoverMode: false
        property bool pointerInside: false

        Timer {
            id: hoverLinkCloseTimer
            interval: 250
            onTriggered: {
                if (!linkEditor.hoverMode || linkEditor.pointerInside)
                    return
                if (linkEditor.editor && linkEditor.editor.linkHoverActive)
                    return
                linkEditor.close()
            }
        }

        function openFor(target, start, end, href, activate) {
            hoverLinkCloseTimer.stop()
            editor = target
            selectionStart = start
            selectionEnd = end
            urlField.text = href || ""
            hadLink = urlField.text.length > 0
            hoverMode = activate === false
            const rectangle = target.positionToRectangle(start)
            const point = target.mapToItem(Overlay.overlay, rectangle.x, rectangle.y)
            const inset = root.touchMode ? 12 : 8
            x = Math.max(inset, Math.min(Overlay.overlay.width - width - inset, point.x))
            y = Math.max(inset, point.y - implicitHeight - 6)
            open()
            if (!hoverMode) {
                Qt.callLater(function() {
                    urlField.forceActiveFocus()
                    urlField.selectAll()
                })
            }
        }

        function scheduleHoverClose(target) {
            if (hoverMode && editor === target)
                hoverLinkCloseTimer.restart()
        }

        function applyLink(href) {
            if (!editor)
                return
            root.runEditTransaction("set-link", function() {
                const end = root.editorBackend.setLink(editor.textDocument,
                                                  selectionStart,
                                                  selectionEnd,
                                                  href.trim())
                if (end >= 0) {
                    editor.select(selectionStart, end)
                    editor.commitText(false)
                }
            })
            close()
            editor.forceActiveFocus()
        }

        onClosed: {
            hoverLinkCloseTimer.stop()
            pointerInside = false
            if (!hoverMode && editor)
                editor.forceActiveFocus()
            hoverMode = false
        }

        contentItem: GridLayout {
            columns: root.touchMode ? 2 : 3
            columnSpacing: root.touchMode ? 8 : 6
            rowSpacing: root.touchMode ? 8 : 6

            HoverHandler {
                onHoveredChanged: {
                    linkEditor.pointerInside = hovered
                    if (hovered)
                        hoverLinkCloseTimer.stop()
                    else if (linkEditor.hoverMode)
                        hoverLinkCloseTimer.restart()
                }
            }

            TextField {
                id: urlField
                objectName: "noteLinkUrlField"
                Layout.columnSpan: root.touchMode ? 2 : 1
                Layout.fillWidth: true
                placeholderText: qsTr("Paste or type a link")
                selectByMouse: true
                font: root.editorFont
                onActiveFocusChanged: {
                    if (activeFocus) {
                        linkEditor.hoverMode = false
                        hoverLinkCloseTimer.stop()
                    }
                }
                onAccepted: linkEditor.applyLink(text)
            }
            Button {
                text: qsTr("Apply")
                Layout.fillWidth: root.touchMode
                Layout.minimumHeight: root.touchMode ? 44 : 0
                onClicked: linkEditor.applyLink(urlField.text)
            }
            ToolButton {
                text: qsTr("Remove")
                Layout.fillWidth: root.touchMode
                Layout.minimumHeight: root.touchMode ? 44 : 0
                enabled: linkEditor.hadLink
                onClicked: linkEditor.applyLink("")
            }
        }
    }

    // Text editors only occupy their implicit content height. Observe taps in
    // the unused viewport below the final block and treat that area as the
    // continuation of the document.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onTapped: function(eventPoint) {
            const ordered = root.orderedEditors()
            if (ordered.length === 0)
                return
            const lastBlock = root.itemAtIndex(root.count - 1)
            const last = ordered[ordered.length - 1]
            const bottom = lastBlock ? lastBlock.mapToItem(root, 0, lastBlock.height).y
                                     : last.mapToItem(root, 0, last.height).y
            if (eventPoint.position.y > bottom)
                root.focusDocumentEnd()
        }
    }

    delegate: FocusScope {
        id: blockDelegate
        required property int index
        required property int blockType
        required property string blockText
        required property var items
        required property var checkedItems
        required property var itemIndents
        required property var itemTypes
        required property int headingLevel
        required property string codeLanguage
        required property var table
        required property string url
        required property string alt
        required property int imageWidth
        required property string imageAlignment
        required property url previewUrl
        property alias item: blockLoader.item
        readonly property bool reorderSourceActive:
            editorReorderController.blockSourceActive(blockDelegate)
        readonly property bool reorderTargetBefore:
            editorReorderController.blockTargetBefore(blockDelegate)
        readonly property bool reorderTargetAfter:
            editorReorderController.blockTargetAfter(blockDelegate)
        readonly property real reorderOffset: blockDisplacement.displacement
        width: Math.max(0, root.width - root.scrollBarInset)
        height: blockLoader.height
        opacity: reorderSourceActive ? 0 : 1
        transform: Translate { y: blockDelegate.reorderOffset }

        Reorder.ReorderDisplacement {
            id: blockDisplacement

            animationEnabled: editorReorderController.blockAnimationActive
                              && !editorReorderController.committingDrop
            sourceActive: blockDelegate.reorderSourceActive
            targetBefore: blockDelegate.reorderTargetBefore
            targetAfter: blockDelegate.reorderTargetAfter
            naturalExtent: blockDelegate.height + root.spacing
            draggedExtent: editorReorderController.structuralDraggedHeight
            displacement: editorReorderController.blockTranslation(blockDelegate)
        }

        Reorder.BlockReorderHandle {
            id: structuralBlockHandle

            objectName: "blockReorderHandle-" + blockDelegate.index
            visible: !root.touchMode
                     && blockDelegate.blockType !== 1
                     && blockDelegate.blockType !== 2
                     && blockDelegate.blockType !== 4
                     && blockDelegate.blockType !== 5
            x: Math.max(0, blockLoader.x - width)
            y: 0
            width: root.listLevelHandleGutter
            height: Math.max(root.editorFontMetricsHeight, blockDelegate.height)
            z: 20
            fullHeight: true
            dragEnabled: !editorReorderController.dragging || dragging
            onDragStarted: editorReorderController.startBlockDrag(
                               blockDelegate, blockLoader.item || blockLoader)
            onDragMoved: function(dx, dy) {
                editorReorderController.moveBlockDrag(dx, dy)
            }
            onDragFinished: editorReorderController.finishBlockDrag()
        }

        Loader {
            id: blockLoader
            property int index: blockDelegate.index
            property int blockType: blockDelegate.blockType
            property string blockText: blockDelegate.blockText
            property var items: blockDelegate.items
            property var checkedItems: blockDelegate.checkedItems
            property var itemIndents: blockDelegate.itemIndents
            property var itemTypes: blockDelegate.itemTypes
            property int headingLevel: blockDelegate.headingLevel
            property string codeLanguage: blockDelegate.codeLanguage
            property var table: blockDelegate.table
            property string url: blockDelegate.url
            property string alt: blockDelegate.alt
            property int imageWidth: blockDelegate.imageWidth
            property string imageAlignment: blockDelegate.imageAlignment
            property url previewUrl: blockDelegate.previewUrl
            x: root.editorInset
            width: Math.max(0, blockDelegate.width - 2 * root.editorInset)
            height: item ? item.implicitHeight : 0
            onLoaded: {
                // Keep the editor address tied to the delegate's current model row.
                // A plain assignment here used to replace the component's own binding,
                // leaving list editors with a stale blockIndex after another block was
                // moved across them. Subsequent list-range drops then addressed the old
                // source row and were rejected by the model.
                if (item && item.blockIndex !== undefined) {
                    item.blockIndex = Qt.binding(function() {
                        return blockLoader.index
                    })
                }
                if (blockType === 0 && index === 0 && blockText.trim().length === 0)
                    item.forceActiveFocus()
            }
            sourceComponent: blockType === 1 || blockType === 2 || blockType === 5 ? listEditor
                           : blockType === 3 ? tableEditor
                           : blockType === 4 ? imageEditor
                           : blockType === 6 ? headingEditor
                           : blockType === 7 ? blockQuoteEditor
                           : blockType === 8 ? codeBlockEditor : textEditor
        }
    }

    component BlockTextArea: TextArea {
        id: blockArea
        objectName: "noteBlockTextArea"
        property bool titleDocument: false
        property bool codeDocument: false
        property string syntaxLanguage: ""
        property var spellingRanges: []
        property string contextWord: ""
        property string contextLink: ""
        property int contextStart: 0
        property int contextEnd: 0
        property var contextSuggestions: []
        property var commitText: function() {}
        property var keyHandler: null
        property int blockIndex: -1
        property int listItemIndex: -1
        property int tableRow: -1
        property int tableCellIndex: -1
        property string editorField: "text"
        property bool tableCell: false
        property bool canRemoveTableRow: false
        property bool canRemoveTableColumn: false
        property var insertRowAbove: null
        property var insertRowBelow: null
        property var removeRow: null
        property var insertColumnLeft: null
        property var insertColumnRight: null
        property var removeColumn: null
        readonly property bool renderedMarkdown: textFormat === TextEdit.MarkdownText
        readonly property bool linkHoverActive: editorMouseArea.containsMouse
                                                && editorMouseArea.hoveredRenderedLinkInfo !== null
        property bool primaryModifierDown: false
        property string sourceText: ""
        property bool syncingSourceText: false
        property bool sourceTextPending: false
        property string observedPlainText: ""
        property bool observedPlainTextInitialized: false
        font: root.editorFont
        wrapMode: TextEdit.Wrap
        verticalAlignment: TextEdit.AlignTop
        selectByMouse: !root.touchMode
        persistentSelection: true
        background: null
        leftPadding: root.touchMode ? 2 : 4
        rightPadding: root.touchMode ? 2 : 4
        topPadding: root.touchMode ? 4 : 0
        bottomPadding: root.touchMode ? 4 : 0
        function registerTextDocument() {
            if (!root.platformBackend)
                return
            if (codeDocument)
                root.platformBackend.registerCodeDocument(textDocument, syntaxLanguage)
            else
                root.platformBackend.registerTextDocument(textDocument, titleDocument)
        }

        function currentPlainText() {
            return getText(0, length)
        }

        function markdownRange(start, end) {
            return root.editorBackend.markdownSelection(textDocument, start, end)
        }

        function rememberPlainText() {
            observedPlainText = currentPlainText()
            observedPlainTextInitialized = true
        }

        function flushToModel() {
            if (syncingSourceText)
                return false
            const current = currentPlainText()
            if (observedPlainTextInitialized && current === observedPlainText)
                return false
            commitText(false)
            rememberPlainText()
            return true
        }

        function applySourceText(force) {
            sourceTextPending = false
            if (!force && text === sourceText) {
                rememberPlainText()
                return false
            }
            syncingSourceText = true
            text = sourceText
            if (renderedMarkdown && sourceText.indexOf("QTNOTEINSOPEN7F3A") >= 0
                    && typeof root.editorBackend !== "undefined" && root.editorBackend !== null)
                root.editorBackend.applyInlineHtmlFormatting(textDocument)
            syncingSourceText = false
            rememberPlainText()
            return true
        }

        function synchronizeSourceText(force) {
            if (activeFocus) {
                sourceTextPending = true
                return false
            }
            return applySourceText(force)
        }

        function applyPendingSourceText() {
            if (!sourceTextPending)
                return false
            return applySourceText(true)
        }

        function shouldCommitTextChange(allowed) {
            if (syncingSourceText)
                return false
            const current = currentPlainText()
            if (!observedPlainTextInitialized) {
                observedPlainText = current
                observedPlainTextInitialized = true
                return false
            }
            return Boolean(allowed) && current !== observedPlainText
        }

        function commitChangedText(allowed) {
            if (!shouldCommitTextChange(allowed))
                return false
            commitText(true)
            rememberPlainText()
            return true
        }

        onSourceTextChanged: {
            synchronizeSourceText()
            Qt.callLater(function() {
                if (blockArea)
                    blockArea.registerTextDocument()
            })
        }
        onTextFormatChanged: Qt.callLater(function() {
            if (blockArea)
                blockArea.registerTextDocument()
        })
        onTitleDocumentChanged: registerTextDocument()
        onSyntaxLanguageChanged: registerTextDocument()
        onActiveFocusChanged: {
            if (activeFocus) {
                root.clearImageSelection()
                root.activeEditor = this
                rememberPlainText()
                root.scheduleCursorVisibility(this)
            } else if (sourceTextPending) {
                synchronizeSourceText()
            }
        }
        onCursorPositionChanged: root.scheduleCursorVisibility(blockArea)
        Component.onCompleted: {
            synchronizeSourceText(true)
            root.registerEditor(blockArea)
            registerTextDocument()
            rememberPlainText()
            spellRefresh.restart()
        }
        Component.onDestruction: root.unregisterEditor(blockArea)

        Timer {
            id: spellRefresh
            interval: 0
            onTriggered: {
                blockArea.spellingRanges = root.platformBackend && !blockArea.codeDocument
                        ? root.platformBackend.spellCheckRanges(blockArea.textDocument) : []
                spellingCanvas.requestPaint()
            }
        }

        Connections {
            target: root
            function onPlatformBackendChanged() { blockArea.registerTextDocument() }
        }

        Connections {
            target: root.platformBackend
            ignoreUnknownSignals: true
            function onHighlightingChanged() { spellRefresh.restart() }
        }

        Connections {
            target: blockArea
            function onTextChanged() {
                spellRefresh.restart()
                plainLinkHoverCanvas.requestPaint()
            }
            function onSelectedTextChanged() {
                if (!root.mouseSelectionActive)
                    selectionStateRefresh.restart()
            }
        }

        // Structural paste must run before TextArea's built-in rich-text
        // importer; otherwise an office HTML table becomes an embedded
        // QTextDocument table instead of a NoteBlockModel table.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Control || event.key === Qt.Key_Meta) {
                primaryModifierDown = true
                editorMouseArea.refreshPlainLinkHover(event.modifiers)
                plainLinkHoverCanvas.requestPaint()
            }
            if (event.matches(StandardKey.Find)) {
                root.findRequested()
                event.accepted = true
            } else if (event.matches(StandardKey.Undo) && root.editorBackend.undo()) {
                event.accepted = true
            } else if (event.matches(StandardKey.Redo) && root.editorBackend.redo()) {
                event.accepted = true
            } else if (!blockArea.codeDocument && blockArea.handleLinkSpaceExit(event)) {
                event.accepted = true
            } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                    && root.deleteStructuredSelection()) {
                event.accepted = true
            } else if (root.handleAdjacentImageDeletion(event, blockArea)) {
                event.accepted = true
            } else if (root.handleAdjacentTextBlockMerge(event, blockArea)) {
                event.accepted = true
            } else if (!blockArea.codeDocument && root.handleInlineFormatting(event, blockArea)) {
                event.accepted = true
            } else if (root.handleKeyboardSelection(event, blockArea)) {
                event.accepted = true
            } else if (keyHandler && keyHandler(event)) {
                event.accepted = true
            } else if (root.handleBlockBoundaryNavigation(event, blockArea)) {
                event.accepted = true
            } else if (event.matches(StandardKey.SelectAll)) {
                root.selectAllDocument()
                event.accepted = true
            } else if (event.matches(StandardKey.Copy) && root.copyActiveSelection()) {
                event.accepted = true
            } else if (event.matches(StandardKey.Cut) && root.cutActiveSelection()) {
                event.accepted = true
            } else if (event.matches(StandardKey.Paste) && root.pasteClipboard()) {
                event.accepted = true
            }
        }

        Keys.onReleased: function(event) {
            if (event.key === Qt.Key_Control || event.key === Qt.Key_Meta) {
                primaryModifierDown = false
                editorMouseArea.refreshPlainLinkHover(event.modifiers)
                plainLinkHoverCanvas.requestPaint()
            }
        }

        function handleLinkSpaceExit(event) {
            if (!renderedMarkdown || event.key !== Qt.Key_Space
                    || event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
                    || selectionStart !== selectionEnd || cursorPosition <= 0
                    || getText(cursorPosition - 1, cursorPosition) !== " ") {
                return false
            }

            const position = cursorPosition
            const info = root.editorBackend.linkInfo(textDocument, position - 1, position)
            if (!info.valid || !info.href || info.end !== position)
                return false

            return root.runEditTransaction("leave-link", function() {
                const result = root.editorBackend.setLink(textDocument, position - 1, position, "")
                if (result < 0)
                    return false

                cursorPosition = position
                commitText(false)
                return true
            })
        }

        function isSpellingError(position) {
            for (const range of spellingRanges) {
                if (position >= range.start && position < range.start + range.length)
                    return true
            }
            return false
        }

        function replaceContextWord(replacement) {
            root.runEditTransaction("spell-replace", function() {
                remove(contextStart, contextEnd)
                insert(contextStart, replacement)
                cursorPosition = contextStart + replacement.length
                commitText(false)
            })
        }

        function refreshSpelling() { spellRefresh.restart() }

        function plainLinkInfoAtPosition(position) {
            if (codeDocument)
                return null
            const source = text
            let match
            const markdownLink = /\[[^\]]*\]\(([^)\s]+)\)/g
            while ((match = markdownLink.exec(source)) !== null) {
                const end = match.index + match[0].length
                if (position >= match.index && position < end)
                    return { href: match[1], start: match.index, end: end }
            }
            const rawUrl = /(?:(?:https?|ftp):\/\/|www\.)[^\s<>{}\[\]"']+/gi
            while ((match = rawUrl.exec(source)) !== null) {
                let visibleUrl = match[0]
                while (visibleUrl.length > 0 && ".,;:!?".indexOf(visibleUrl[visibleUrl.length - 1]) >= 0)
                    visibleUrl = visibleUrl.slice(0, -1)
                const end = match.index + visibleUrl.length
                if (position >= match.index && position < end) {
                    return {
                        href: visibleUrl.indexOf("www.") === 0 ? "https://" + visibleUrl : visibleUrl,
                        start: match.index,
                        end: end
                    }
                }
            }
            return null
        }

        function plainLinkAtPosition(position) {
            const info = plainLinkInfoAtPosition(position)
            return info ? info.href : ""
        }

        Timer {
            id: renderedLinkHoverTimer
            interval: 700
            onTriggered: {
                const info = editorMouseArea.hoveredRenderedLinkInfo
                if (!blockArea.renderedMarkdown || !editorMouseArea.containsMouse || !info)
                    return
                if (linkEditor.visible && !linkEditor.hoverMode)
                    return
                linkEditor.openFor(blockArea, info.start, info.end, info.href, false)
            }
        }

        Timer {
            id: modifierStatePoll
            interval: 50
            repeat: true
            running: editorMouseArea.containsMouse && !blockArea.renderedMarkdown
            onTriggered: {
                const pressed = root.editorBackend.primaryModifierPressed()
                if (blockArea.primaryModifierDown === pressed)
                    return
                blockArea.primaryModifierDown = pressed
                editorMouseArea.refreshPlainLinkHover(pressed ? Qt.ControlModifier : Qt.NoModifier)
                plainLinkHoverCanvas.requestPaint()
            }
        }

        MouseArea {
            id: editorMouseArea
            anchors.fill: parent
            z: 20
            enabled: !root.touchMode
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: (blockArea.renderedMarkdown && hoveredRenderedLink.length > 0)
                         || (!blockArea.renderedMarkdown && blockArea.primaryModifierDown
                             && hoveredPlainLinkInfo !== null)
                         ? Qt.PointingHandCursor : Qt.IBeamCursor
            property bool selecting: false
            property bool selectionMoved: false
            property real hoverX: -1
            property real hoverY: -1
            property string hoveredRenderedLink: ""
            property var hoveredRenderedLinkInfo: null
            property var hoveredPlainLinkInfo: null

            function clearPlainLinkHover() {
                renderedLinkHoverTimer.stop()
                hoveredRenderedLink = ""
                hoveredRenderedLinkInfo = null
                hoveredPlainLinkInfo = null
                plainLinkHoverCanvas.requestPaint()
            }

            function refreshPlainLinkHover(modifiers) {
                if (hoverX < 0 || hoverY < 0) {
                    clearPlainLinkHover()
                    return
                }
                if (blockArea.renderedMarkdown) {
                    hoveredPlainLinkInfo = null
                    const href = blockArea.linkAt(hoverX, hoverY)
                    const position = blockArea.positionAt(hoverX, hoverY)
                    const info = href.length > 0
                            ? root.editorBackend.linkInfo(blockArea.textDocument, position, position)
                            : null
                    const changed = !hoveredRenderedLinkInfo || !info
                            || hoveredRenderedLinkInfo.start !== info.start
                            || hoveredRenderedLinkInfo.end !== info.end
                            || hoveredRenderedLinkInfo.href !== info.href
                    hoveredRenderedLink = href
                    hoveredRenderedLinkInfo = info && info.valid ? info : null
                    if (hoveredRenderedLinkInfo && changed)
                        renderedLinkHoverTimer.restart()
                    else if (!hoveredRenderedLinkInfo)
                        renderedLinkHoverTimer.stop()
                    plainLinkHoverCanvas.requestPaint()
                    return
                }

                renderedLinkHoverTimer.stop()
                hoveredRenderedLink = ""
                hoveredRenderedLinkInfo = null
                const primaryModifier = blockArea.primaryModifierDown
                        || Boolean(modifiers & (Qt.ControlModifier | Qt.MetaModifier))
                if (!primaryModifier) {
                    clearPlainLinkHover()
                    return
                }
                const position = blockArea.positionAt(hoverX, hoverY)
                hoveredPlainLinkInfo = blockArea.plainLinkInfoAtPosition(position)
                plainLinkHoverCanvas.requestPaint()
            }
            onPressed: function(mouse) {
                root.editorBackend.updateHistoryViewState(root.captureEditorState(), true)
                const position = blockArea.positionAt(mouse.x, mouse.y)
                if (mouse.button === Qt.LeftButton) {
                    blockArea.forceActiveFocus()
                    root.activeEditor = blockArea
                    root.selectionAnchorEditor = blockArea
                    root.selectionAnchorPosition = position
                    root.clearDocumentSelection()
                    root.setEditorSelection(blockArea, position, position)
                    root.mouseSelectionActive = true
                    selecting = true
                    selectionMoved = false
                    mouse.accepted = true
                    return
                }
                blockArea.forceActiveFocus()
                root.openEditorContextMenu(blockArea, mouse.x, mouse.y)
                mouse.accepted = true
            }
            onPositionChanged: function(mouse) {
                hoverX = mouse.x
                hoverY = mouse.y
                if (mouse.buttons === Qt.NoButton)
                    refreshPlainLinkHover(mouse.modifiers)
                if (!selecting || !(mouse.buttons & Qt.LeftButton))
                    return
                const started = Date.now()
                selectionMoved = true
                if (mouse.x >= 0 && mouse.y >= 0 && mouse.x <= blockArea.width && mouse.y <= blockArea.height) {
                    root.applyDocumentSelection(root.selectionAnchorEditor, root.selectionAnchorPosition,
                                                blockArea, blockArea.positionAt(mouse.x, mouse.y))
                    const elapsed = Date.now() - started
                    if (elapsed >= 8)
                        console.info("QML selection move slow: path=local duration=" + elapsed
                                     + "ms editors=" + root.editors.length)
                    return
                }
                const globalPosition = blockArea.mapToItem(root, mouse.x, mouse.y)
                const hit = root.editorAtPoint(globalPosition.x, globalPosition.y)
                if (hit)
                    root.applyDocumentSelection(root.selectionAnchorEditor, root.selectionAnchorPosition,
                                                hit.editor, hit.position)
                const elapsed = Date.now() - started
                if (elapsed >= 8)
                    console.info("QML selection move slow: path=cross-editor duration=" + elapsed
                                 + "ms editors=" + root.editors.length)
            }
            onReleased: function(mouse) {
                if (mouse.button === Qt.LeftButton) {
                    if (!selectionMoved) {
                        const position = blockArea.positionAt(mouse.x, mouse.y)
                        if (blockArea.renderedMarkdown) {
                            const link = blockArea.linkAt(mouse.x, mouse.y)
                            if (link.length > 0)
                                Qt.openUrlExternally(link)
                        } else if (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) {
                            const link = blockArea.plainLinkAtPosition(position)
                            if (link.length > 0)
                                Qt.openUrlExternally(link)
                        }
                    }
                    selecting = false
                    root.mouseSelectionActive = false
                    if (selectionMoved)
                        root.copyDocumentSelectionToPrimary()
                    selectionStateRefresh.restart()
                    root.selectionAnchorEditor = null
                }
            }
            onExited: {
                hoverX = -1
                hoverY = -1
                clearPlainLinkHover()
                linkEditor.scheduleHoverClose(blockArea)
            }
            onDoubleClicked: function(mouse) {
                if (mouse.button !== Qt.LeftButton)
                    return
                blockArea.cursorPosition = blockArea.positionAt(mouse.x, mouse.y)
                blockArea.selectWord()
                root.wholeDocumentSelected = false
                Qt.callLater(function() { root.copyDocumentSelectionToPrimary() })
            }
        }

        TapHandler {
            id: touchContextHandler
            enabled: root.touchMode
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onLongPressed: {
                root.openEditorContextMenu(blockArea, point.position.x, point.position.y)
            }
        }

        Canvas {
            id: plainLinkHoverCanvas
            anchors.fill: parent
            z: 15
            visible: !blockArea.renderedMarkdown && blockArea.primaryModifierDown
                     && editorMouseArea.hoveredPlainLinkInfo !== null
            onVisibleChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                if (!visible || !editorMouseArea.hoveredPlainLinkInfo)
                    return

                const info = editorMouseArea.hoveredPlainLinkInfo
                ctx.strokeStyle = blockArea.palette.link
                ctx.lineWidth = 1.5
                let segmentLeft = -1
                let segmentY = 0
                for (let position = info.start; position < info.end; ++position) {
                    const current = blockArea.positionToRectangle(position)
                    const next = blockArea.positionToRectangle(position + 1)
                    let right = next.y === current.y && next.x > current.x
                            ? next.x
                            : current.x + Math.max(current.width, editorFontMetrics.averageCharacterWidth)
                    if (segmentLeft < 0) {
                        segmentLeft = current.x
                        segmentY = current.y + current.height - 1
                    }
                    if (next.y !== current.y || position === info.end - 1) {
                        ctx.beginPath()
                        ctx.moveTo(segmentLeft, segmentY)
                        ctx.lineTo(right, segmentY)
                        ctx.stroke()
                        segmentLeft = -1
                    }
                }
            }
        }

        Canvas {
            id: spellingCanvas
            anchors.fill: parent
            z: 10
            function drawWave(ctx, left, right, y) {
                if (right <= left)
                    return
                ctx.beginPath()
                ctx.moveTo(left, y)
                let up = true
                for (let x = left + 2; x < right; x += 2) {
                    ctx.lineTo(x, y + (up ? -1.5 : 1.5))
                    up = !up
                }
                ctx.lineTo(right, y)
                ctx.stroke()
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = "#d32f2f"
                ctx.lineWidth = 1.25
                for (const range of blockArea.spellingRanges) {
                    const end = range.start + range.length
                    let segmentLeft = -1
                    let segmentY = 0
                    for (let position = range.start; position < end; ++position) {
                        const current = blockArea.positionToRectangle(position)
                        const next = blockArea.positionToRectangle(position + 1)
                        let right = next.y === current.y ? next.x : current.x + current.width
                        if (right <= current.x)
                            right = current.x + Math.max(2, current.width)
                        const y = current.y + current.height - 1.5
                        if (segmentLeft < 0) {
                            segmentLeft = current.x
                            segmentY = y
                        }
                        if (next.y !== current.y || position === end - 1) {
                            drawWave(ctx, segmentLeft, right, segmentY)
                            segmentLeft = -1
                        }
                    }
                }
            }
        }
    }

    Component {
        id: textEditor
        BlockTextArea {
            id: textCell
            property var block: parent
            titleDocument: block.index === 0
            blockIndex: block.index
            width: block.width
            sourceText: root.blockModel && root.blockModel.markdown
                        ? root.markdownForRendering(block.blockText) : block.blockText
            keyHandler: function(event) { return root.handleHeadingShortcut(event, textCell) }
            commitText: function() {
                const value = root.blockModel && root.blockModel.markdown
                    ? root.editorBackend.markdownText(textDocument) : text
                root.blockModel.setBlockText(block.index, value)
            }
            textFormat: root.blockModel && root.blockModel.markdown ? TextEdit.MarkdownText : TextEdit.PlainText
            onTextChanged: commitChangedText(activeFocus)
            onLinkActivated: link => Qt.openUrlExternally(link)
        }
    }

    Component {
        id: codeBlockEditor

        Rectangle {
            id: codeRoot
            property var block: parent
            property alias blockIndex: codeCell.blockIndex
            width: block.width
            implicitHeight: Math.max(codeCell.implicitHeight + 12, root.editorFontMetricsHeight * 3)
            radius: 4
            clip: true
            color: Qt.rgba(codeCell.palette.base.r, codeCell.palette.base.g,
                           codeCell.palette.base.b, 0.82)
            border.width: 1
            border.color: codeCell.palette.midlight

            function forceActiveFocus() { codeCell.forceActiveFocus() }

            BlockTextArea {
                id: codeCell
                property var block: codeRoot.block
                anchors.fill: parent
                anchors.margins: 6
                blockIndex: block.index
                editorField: "code"
                codeDocument: true
                syntaxLanguage: block.codeLanguage
                sourceText: block.blockText
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.NoWrap
                font.family: Qt.platform.os === "windows" ? "Consolas" : "monospace"
                commitText: function() { root.blockModel.setBlockText(block.index, text) }
                onTextChanged: commitChangedText(activeFocus)
            }

            Item {
                id: codeOverlay
                anchors.top: parent.top
                anchors.right: parent.right
                width: languageSelector.width + codeActions.width
                height: languageSelector.height
                z: 2

                HoverActionStrip {
                    id: codeActions
                    anchors.right: languageSelector.left
                    anchors.verticalCenter: languageSelector.verticalCenter
                    triggerHovered: languageSelector.hovered
                    fadeDuration: 480
                    gapAfter: 6

                    ToolButton {
                        id: copyCodeButton
                        width: languageSelector.height
                        height: languageSelector.height
                        padding: 3
                        display: AbstractButton.IconOnly
                        contentItem: ThemedIcon {
                            themeName: "edit-copy"
                            fallbackName: "copy22.png"
                            pixelSize: Math.max(14, Math.round(copyCodeButton.height * 0.58))
                        }
                        Accessible.name: qsTr("Copy code")
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: {
                            if (root.editorBackend)
                                root.editorBackend.copyToClipboard(codeCell.text)
                        }

                        background: Rectangle {
                            radius: codeRoot.radius
                            color: copyCodeButton.pressed ? copyCodeButton.palette.mid
                                                          : copyCodeButton.palette.alternateBase
                            border.width: 1
                            border.color: codeCell.palette.midlight
                        }
                    }
                }

                CompactPopupSelector {
                    id: languageSelector
                    anchors.top: parent.top
                    anchors.right: parent.right
                    model: root.platformBackend ? root.platformBackend.codeLanguages : []
                    textRole: "name"
                    valueRole: "id"
                    implicitHeight: Math.max(24, root.editorFontMetricsHeight + 8)
                    minimumControlWidth: 48
                    minimumPopupWidth: 180
                    backgroundColor: palette.alternateBase
                    hoverColor: palette.alternateBase
                    borderColor: codeCell.palette.midlight

                    function synchronizeLanguage() {
                        const language = root.platformBackend
                                ? root.platformBackend.canonicalCodeLanguage(codeRoot.block.codeLanguage)
                                : codeRoot.block.codeLanguage
                        currentIndex = Math.max(0, indexOfValue(language))
                    }

                    Component.onCompleted: synchronizeLanguage()
                    onModelChanged: synchronizeLanguage()
                    onActivated: function(index, value) {
                        root.blockModel.setCodeLanguage(codeRoot.block.index, value || "")
                    }

                    Connections {
                        target: codeRoot.block
                        ignoreUnknownSignals: true
                        function onCodeLanguageChanged() { languageSelector.synchronizeLanguage() }
                    }
                }
            }
        }
    }

    Component {
        id: headingEditor
        BlockTextArea {
            id: headingCell
            property var block: parent
            titleDocument: block.index === 0
            blockIndex: block.index
            editorField: "heading"
            width: block.width
            sourceText: root.markdownForRendering(block.blockText)
            font.bold: true
            font.pointSize: root.editorPointSize * (block.headingLevel === 1 ? 1.7
                            : block.headingLevel === 2 ? 1.5
                            : block.headingLevel === 3 ? 1.3
                            : block.headingLevel === 4 ? 1.15
                            : block.headingLevel === 5 ? 1.0 : 0.9)
            topPadding: Math.max(root.touchMode ? 8 : 4,
                                 Math.round(root.editorFontMetricsHeight * 0.55))
            bottomPadding: Math.max(root.touchMode ? 4 : 2,
                                    Math.round(root.editorFontMetricsHeight * 0.18))
            keyHandler: function(event) { return root.handleHeadingShortcut(event, headingCell) }
            commitText: function() {
                root.blockModel.setBlockText(
                    block.index, root.editorBackend.markdownText(textDocument))
            }
            textFormat: TextEdit.MarkdownText
            onTextChanged: commitChangedText(activeFocus)
            onLinkActivated: link => Qt.openUrlExternally(link)
        }
    }

    Component {
        id: blockQuoteEditor

        Item {
            id: quoteRoot
            property var block: parent
            width: block.width
            implicitHeight: quoteCell.implicitHeight

            Rectangle {
                width: Math.max(3, Math.round(root.editorFontAverageCharacterWidth * 0.45))
                radius: width / 2
                color: quoteCell.palette.midlight
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
            }

            BlockTextArea {
                id: quoteCell
                property var block: quoteRoot.block
                x: Math.max(12, Math.round(root.editorFontAverageCharacterWidth * 1.6))
                width: Math.max(0, quoteRoot.width - x)
                blockIndex: block.index
                editorField: "blockquote"
                sourceText: root.markdownForRendering(block.blockText)
                textFormat: TextEdit.MarkdownText
                font.italic: true
                commitText: function() {
                    root.blockModel.setBlockText(
                        block.index, root.editorBackend.markdownText(textDocument))
                }
                onTextChanged: commitChangedText(activeFocus)
                onLinkActivated: link => Qt.openUrlExternally(link)
            }
        }
    }

    Component {
        id: listItemEditorDelegate

        BlockTextArea {
            id: listItemCell

            readonly property var listRow: parent.listRow
            readonly property var listBlock: parent.listBlock
            blockIndex: listBlock ? listBlock.blockIndex : -1
            listItemIndex: listRow ? listRow.index : -1
            editorField: "listItem"
            width: parent.width
            sourceText: root.markdownForRendering(listRow ? listRow.itemText : "")
            keyHandler: function(event) {
                return listBlock && listRow
                        ? listBlock.handleItemKey(event, listItemCell, listRow.index) : false
            }
            commitText: function() {
                if (!listBlock || !listRow)
                    return
                root.blockModel.setListItem(
                    listBlock.blockIndex,
                    listRow.index,
                    root.editorBackend.markdownText(textDocument))
            }
            textFormat: TextEdit.MarkdownText
            onTextChanged: commitChangedText(activeFocus && listBlock && !listBlock.syncingItems)
            onLinkActivated: link => Qt.openUrlExternally(link)
        }
    }

    Component {
        id: listEditor

        ListBlockEditor {
            block: parent
            editorView: root
            reorderController: editorReorderController
            editorDelegate: listItemEditorDelegate
        }
    }

    Component {
        id: tableEditor
        GridLayout {
            id: tableRoot
            property var block: parent
            property var tableData: block.table
            width: block.width
            columns: tableData.columns
            columnSpacing: 0
            rowSpacing: 0
            onTableDataChanged: syncCells()
            Component.onCompleted: syncCells()
            function syncCells() {
                const values = tableData.values || []
                while (cellModel.count > values.length)
                    cellModel.remove(cellModel.count - 1)
                while (cellModel.count < values.length)
                    cellModel.append({ cellText: "" })
                for (let index = 0; index < values.length; ++index) {
                    const value = values[index] || ""
                    if (cellModel.get(index).cellText !== value)
                        cellModel.setProperty(index, "cellText", value)
                }
            }
            ListModel { id: cellModel }
            function focusCell(cellIndex, position) {
                root.focusEditorAddress({
                    blockIndex: block.index,
                    listItemIndex: -1,
                    tableCellIndex: cellIndex,
                    field: "tableCell",
                    cursorPosition: position === undefined ? 0 : position
                })
            }
            function focusCellVertically(cellIndex, x, atBottom) {
                Qt.callLater(function() {
                    const cell = cellRepeater.itemAt(cellIndex)
                    if (!cell)
                        return
                    cell.forceActiveFocus()
                    const y = atBottom ? Math.max(0, cell.height - cell.bottomPadding - 1)
                                       : cell.topPadding + 1
                    cell.cursorPosition = cell.positionAt(x, y)
                    root.activeEditor = cell
                })
            }
            function insertRow(tableRow, focusColumn) {
                return root.runEditTransaction("insert-table-row", function() {
                    root.blockModel.insertTableRow(block.index, tableRow)
                    focusCell(tableRow * columns + focusColumn, 0)
                    return true
                })
            }
            function cellCount() { return cellModel.count }
            function rowCount() { return cellModel.count / columns }
            function cellLength(index) {
                const cell = cellRepeater.itemAt(index)
                return cell ? cell.length : 0
            }
            function rowEmpty(row) {
                for (let column = 0; column < columns; ++column)
                    if (String(tableData.values[row * columns + column] || "").trim().length > 0)
                        return false
                return true
            }
            function tableEmpty() {
                for (const value of tableData.values || [])
                    if (String(value || "").trim().length > 0)
                        return false
                return true
            }
            function handleCellKey(event, cell) {
                return TableBlockBehavior.handleKey(tableRoot, root, event, cell)
            }
            Repeater {
                id: cellRepeater
                model: cellModel
                BlockTextArea {
                    id: tableCell
                    blockIndex: tableRoot.block.index
                    required property int index
                    required property string cellText
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    font.bold: index < tableRoot.columns
                    sourceText: root.markdownTableCellForRendering(cellText)
                    textFormat: TextEdit.MarkdownText
                    tableCell: true
                    tableCellIndex: index
                    editorField: "tableCell"
                    tableRow: Math.floor(index / tableRoot.columns)
                    canRemoveTableRow: cellModel.count / tableRoot.columns > 1
                    canRemoveTableColumn: tableRoot.columns > 1
                    keyHandler: function(event) { return tableRoot.handleCellKey(event, tableCell) }
                    insertRowAbove: function() {
                        tableRoot.insertRow(Math.floor(index / tableRoot.columns), index % tableRoot.columns)
                    }
                    insertRowBelow: function() {
                        tableRoot.insertRow(Math.floor(index / tableRoot.columns) + 1, index % tableRoot.columns)
                    }
                    removeRow: function() {
                        root.runEditTransaction("remove-table-row", function() {
                            const tableRow = Math.floor(index / tableRoot.columns)
                            const targetRow = Math.min(tableRow, cellModel.count / tableRoot.columns - 2)
                            const target = targetRow * tableRoot.columns + index % tableRoot.columns
                            root.blockModel.removeTableRow(tableRoot.block.index, tableRow)
                            tableRoot.focusCell(target, 0)
                        })
                    }
                    insertColumnLeft: function() {
                        root.runEditTransaction("insert-table-column", function() {
                            const oldColumns = tableRoot.columns
                            const tableRow = Math.floor(index / oldColumns)
                            const column = index % oldColumns
                            root.blockModel.insertTableColumn(tableRoot.block.index, column)
                            tableRoot.focusCell(tableRow * (oldColumns + 1) + column, 0)
                        })
                    }
                    insertColumnRight: function() {
                        root.runEditTransaction("insert-table-column", function() {
                            const oldColumns = tableRoot.columns
                            const tableRow = Math.floor(index / oldColumns)
                            const column = index % oldColumns
                            root.blockModel.insertTableColumn(tableRoot.block.index, column + 1)
                            tableRoot.focusCell(tableRow * (oldColumns + 1) + column + 1, 0)
                        })
                    }
                    removeColumn: function() {
                        root.runEditTransaction("remove-table-column", function() {
                            const oldColumns = tableRoot.columns
                            const tableRow = Math.floor(index / oldColumns)
                            const column = index % oldColumns
                            root.blockModel.removeTableColumn(tableRoot.block.index, column)
                            tableRoot.focusCell(tableRow * (oldColumns - 1)
                                                + Math.min(column, oldColumns - 2), 0)
                        })
                    }
                    leftPadding: root.touchMode ? 8 : 6
                    rightPadding: root.touchMode ? 8 : 6
                    topPadding: root.touchMode ? 8 : 3
                    bottomPadding: root.touchMode ? 8 : 3
                    background: Rectangle {
                        color: Math.floor(tableCell.index / tableRoot.columns) % 2
                               ? tableCell.palette.alternateBase : tableCell.palette.base
                        border.width: 1
                        border.color: tableCell.palette.midlight
                    }
                    commitText: function() {
                        root.blockModel.setTableCell(
                            tableRoot.block.index, index, root.editorBackend.markdownTableCellText(textDocument))
                        // setTableCell echoes the canonical value through
                        // sourceText. The current QTextDocument already owns
                        // that edit; applying the echo on focus loss would
                        // parse its line separators for a second time.
                        sourceTextPending = false
                    }
                    onTextChanged: commitChangedText(activeFocus)
                    onLinkActivated: link => Qt.openUrlExternally(link)
                }
            }
        }
    }

    component ImageAlignmentGlyph: Item {
        required property string alignment
        required property color tint
        implicitWidth: 18
        implicitHeight: 16

        Repeater {
            model: [14, 10, 13]
            delegate: Rectangle {
                required property int index
                required property int modelData
                width: modelData
                height: 1.5
                radius: 0.75
                color: parent.tint
                y: 3 + index * 4
                x: parent.alignment === "left" ? 1
                   : parent.alignment === "right" ? parent.width - width - 1
                   : (parent.width - width) / 2
            }
        }
    }

    component ImageResizeHandle: Rectangle {
        required property var imageEditor
        required property int direction
        required property color fillColor
        required property color strokeColor
        width: root.touchMode ? 18 : 12
        height: width
        radius: 2
        color: fillColor
        border.width: 1
        border.color: strokeColor
        z: 5

        property real pressX: 0
        property bool resizing: false

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            cursorShape: Qt.SizeHorCursor
            preventStealing: true
            propagateComposedEvents: false

            onPressed: function(mouse) {
                mouse.accepted = true
                imageEditor.selectAndFocus()
                const point = mapToItem(imageEditor, mouse.x, mouse.y)
                parent.pressX = point.x
                parent.resizing = true
                imageEditor.beginResize(parent.direction)
            }
            onPositionChanged: function(mouse) {
                if (!parent.resizing)
                    return
                const point = mapToItem(imageEditor, mouse.x, mouse.y)
                imageEditor.updateResize(point.x - parent.pressX)
                mouse.accepted = true
            }
            onReleased: function(mouse) {
                mouse.accepted = true
                if (!parent.resizing)
                    return
                parent.resizing = false
                imageEditor.finishResize()
            }
            onCanceled: {
                if (!parent.resizing)
                    return
                parent.resizing = false
                imageEditor.finishResize()
            }
        }
    }

    Component {
        id: imageEditor

        FocusScope {
            id: imageRoot
            objectName: "imageBlockEditor-" + block.index
            property var block: parent
            property bool selected: root.selectedImageIndex === block.index
            property real transientWidth: -1
            property real transientX: -1
            property real resizeStartWidth: 0
            property real resizeStartX: 0
            property int resizeDirection: 1
            readonly property string normalizedAlignment:
                block.imageAlignment === "left" || block.imageAlignment === "right"
                ? block.imageAlignment : "center"
            readonly property real naturalWidth:
                sourceImage.implicitWidth > 0 ? sourceImage.implicitWidth : Math.min(width, 480)
            readonly property real requestedWidth:
                transientWidth >= 0 ? transientWidth
                                    : (block.imageWidth > 0 ? block.imageWidth : naturalWidth)
            readonly property real displayWidth: Math.max(1, Math.min(width, requestedWidth))
            readonly property real imageAspect:
                sourceImage.implicitWidth > 0 && sourceImage.implicitHeight > 0
                ? sourceImage.implicitHeight / sourceImage.implicitWidth : 0.75
            readonly property real displayHeight: Math.max(40, displayWidth * imageAspect)
            readonly property real imageY: selected ? altEditor.implicitHeight + 6 : 0
            readonly property real actionGap: root.touchMode ? 12 : 8
            readonly property real alignedX:
                normalizedAlignment === "left" ? 0
                : normalizedAlignment === "right" ? width - displayWidth
                : (width - displayWidth) / 2
            readonly property real displayX: transientX >= 0 ? transientX : alignedX

            width: block.width
            implicitHeight: imageY + displayHeight
                            + (selected ? actionGap + imageActions.height + 4 : 0)
            activeFocusOnTab: true

            function selectAndFocus() {
                root.selectImageBlock(block.index)
                imageRoot.forceActiveFocus()
            }

            function setAlignment(value) {
                if (normalizedAlignment === value)
                    return
                root.runEditTransaction("align-image", function() {
                    root.blockModel.setImageAlignment(block.index, value)
                })
            }

            function resetPresentation() {
                if (block.imageWidth === 0 && normalizedAlignment === "center")
                    return
                root.runEditTransaction("reset-image-presentation", function() {
                    root.blockModel.setImageWidth(block.index, 0)
                    root.blockModel.setImageAlignment(block.index, "center")
                })
            }

            function beginResize(direction) {
                resizeDirection = direction
                resizeStartWidth = displayWidth
                resizeStartX = sourceImage.x
                transientWidth = displayWidth
                transientX = sourceImage.x
                root.beginEditTransaction("resize-image")
            }

            function updateResize(delta) {
                if (transientWidth < 0)
                    return
                const maximum = resizeDirection > 0 ? width - resizeStartX
                                                    : resizeStartX + resizeStartWidth
                const minimum = Math.min(root.touchMode ? 72 : 48, maximum)
                if (resizeDirection > 0) {
                    transientWidth = Math.max(minimum, Math.min(maximum, resizeStartWidth + delta))
                    transientX = resizeStartX
                } else {
                    transientWidth = Math.max(minimum, Math.min(maximum, resizeStartWidth - delta))
                    transientX = resizeStartX + resizeStartWidth - transientWidth
                }
            }

            function finishResize() {
                if (transientWidth < 0)
                    return
                const width = Math.round(transientWidth)
                root.blockModel.setImageWidth(block.index, width)
                transientWidth = -1
                transientX = -1
                root.endEditTransaction()
            }

            Keys.onPressed: function(event) {
                if (altEditor.activeFocus)
                    return
                if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
                    root.removeImageBlock(block.index, true)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    root.clearImageSelection()
                    root.forceActiveFocus()
                    event.accepted = true
                }
            }

            TextField {
                id: altEditor
                objectName: "imageAltEditor-" + imageRoot.block.index
                visible: imageRoot.selected
                opacity: visible ? 1 : 0
                width: Math.min(imageRoot.width, Math.max(180, sourceImage.width))
                x: Math.max(0, Math.min(imageRoot.width - width,
                                        sourceImage.x + (sourceImage.width - width) / 2))
                placeholderText: qsTr("Alt text")
                text: imageRoot.block.alt
                selectByMouse: true
                onTextEdited: root.blockModel.setImageAlt(imageRoot.block.index, text)
                onActiveFocusChanged: root.imageAltEditorFocused = activeFocus
                Component.onDestruction: {
                    if (root.imageAltEditorFocused)
                        root.imageAltEditorFocused = false
                }
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }

            Image {
                id: sourceImage
                objectName: "imageBlockPreview-" + imageRoot.block.index
                x: imageRoot.displayX
                y: imageRoot.imageY
                width: imageRoot.displayWidth
                height: imageRoot.displayHeight
                source: imageRoot.block.previewUrl
                fillMode: Image.PreserveAspectFit
                smooth: true
                asynchronous: true
                cache: true
                ToolTip.visible: imageDescriptionHover.hovered && imageRoot.block.alt.length > 0
                                     && !imageRoot.selected
                ToolTip.text: imageRoot.block.alt

                Behavior on x {
                    enabled: imageRoot.transientWidth < 0
                    NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                }

                HoverHandler { id: imageDescriptionHover }
                Reorder.ReorderDragHandle {
                    anchors.fill: parent
                    dragEnabled: !root.touchMode && imageRoot.transientWidth < 0
                    hoverCursorShape: Qt.OpenHandCursor
                    dragCursorShape: Qt.ClosedHandCursor
                    onTapped: imageRoot.selectAndFocus()
                    onDragStarted: {
                        imageRoot.selectAndFocus()
                        editorReorderController.startBlockDrag(
                                    imageRoot.block.parent, imageRoot)
                    }
                    onDragMoved: function(dx, dy) {
                        editorReorderController.moveBlockDrag(dx, dy)
                    }
                    onDragFinished: editorReorderController.finishBlockDrag()
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: {
                        imageRoot.selectAndFocus()
                        imageContextMenu.popup()
                    }
                }
                TapHandler {
                    enabled: root.touchMode
                    acceptedButtons: Qt.LeftButton
                    gesturePolicy: TapHandler.DragThreshold
                    onLongPressed: {
                        imageRoot.selectAndFocus()
                        imageContextMenu.popup()
                    }
                }
            }

            Rectangle {
                x: sourceImage.x - 2
                y: sourceImage.y - 2
                width: sourceImage.width + 4
                height: sourceImage.height + 4
                visible: imageRoot.selected
                color: "transparent"
                border.width: 2
                border.color: altEditor.palette.highlight
                radius: 2
                z: 3
            }

            ImageResizeHandle {
                visible: imageRoot.selected
                imageEditor: imageRoot
                direction: -1
                fillColor: altEditor.palette.base
                strokeColor: altEditor.palette.highlight
                x: Math.max(0, sourceImage.x - width / 2)
                y: sourceImage.y + sourceImage.height - height / 2
            }

            ImageResizeHandle {
                visible: imageRoot.selected
                imageEditor: imageRoot
                direction: 1
                fillColor: altEditor.palette.base
                strokeColor: altEditor.palette.highlight
                x: Math.min(imageRoot.width - width,
                            sourceImage.x + sourceImage.width - width / 2)
                y: sourceImage.y + sourceImage.height - height / 2
            }

            Row {
                id: imageActions
                visible: imageRoot.selected
                spacing: 3
                height: root.touchMode ? 36 : 28
                y: sourceImage.y + sourceImage.height + imageRoot.actionGap
                x: Math.max(0, Math.min(imageRoot.width - width,
                                        sourceImage.x + (sourceImage.width - width) / 2))

                Repeater {
                    model: ["left", "center", "right"]
                    delegate: ToolButton {
                        id: alignmentButton
                        required property string modelData
                        width: imageActions.height
                        height: imageActions.height
                        padding: 4
                        checkable: true
                        checked: imageRoot.normalizedAlignment === modelData
                        display: AbstractButton.IconOnly
                        ToolTip.visible: hovered
                        ToolTip.text: modelData === "left" ? qsTr("Align left")
                                      : modelData === "right" ? qsTr("Align right")
                                      : qsTr("Align center")
                        contentItem: ImageAlignmentGlyph {
                            alignment: modelData
                            tint: alignmentButton.palette.buttonText
                        }
                        onClicked: imageRoot.setAlignment(modelData)
                    }
                }

                ToolButton {
                    id: resetImageButton
                    width: imageActions.height
                    height: imageActions.height
                    padding: 4
                    display: AbstractButton.IconOnly
                    enabled: imageRoot.block.imageWidth > 0 || imageRoot.normalizedAlignment !== "center"
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Reset image size and alignment")
                    contentItem: Label {
                        text: "↺"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: resetImageButton.palette.buttonText
                        opacity: resetImageButton.enabled ? 1 : 0.45
                        font.pixelSize: Math.max(15, resetImageButton.height * 0.62)
                    }
                    onClicked: imageRoot.resetPresentation()
                }
            }

            Menu {
                id: imageContextMenu
                MenuItem {
                    text: qsTr("Save Image As…")
                    visible: root.platformBackend !== null
                    enabled: visible && imageRoot.block.url.startsWith("qtnote-media:/")
                    onTriggered: root.platformBackend.saveImageAs(imageRoot.block.url)
                }
                MenuSeparator { }
                MenuItem {
                    text: qsTr("Align Left")
                    onTriggered: imageRoot.setAlignment("left")
                }
                MenuItem {
                    text: qsTr("Align Center")
                    onTriggered: imageRoot.setAlignment("center")
                }
                MenuItem {
                    text: qsTr("Align Right")
                    onTriggered: imageRoot.setAlignment("right")
                }
                MenuItem {
                    text: qsTr("Reset Size and Alignment")
                    enabled: imageRoot.block.imageWidth > 0 || imageRoot.normalizedAlignment !== "center"
                    onTriggered: imageRoot.resetPresentation()
                }
                MenuSeparator { }
                MenuItem {
                    text: qsTr("Remove Image")
                    onTriggered: root.removeImageBlock(imageRoot.block.index, true)
                }
            }
        }
    }

}
