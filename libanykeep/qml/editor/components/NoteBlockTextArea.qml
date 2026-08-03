import QtQuick
import QtQuick.Controls

TextArea {
    id: blockArea
    required property var editorView
    required property var linkPopup
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
    property bool discardEmptyBlockOnFocusLoss: false
    // -1 follows the format at the cursor, 0 forces the next input off,
    // and 1 forces it on.  Keeping this state in the editor avoids
    // inserting placeholder text when a format button is pressed without
    // a selection.
    property int pendingBold: -1
    property int pendingItalic: -1
    property int pendingStrike: -1
    property int pendingUnderline: -1
    property int pendingCode: -1
    property int pendingFormatGeneration: 0
    font: editorView.editorFont
    wrapMode: TextEdit.Wrap
    verticalAlignment: TextEdit.AlignTop
    selectByMouse: !editorView.touchMode
    persistentSelection: true
    background: null
    leftPadding: editorView.touchMode ? 2 : 4
    rightPadding: editorView.touchMode ? 2 : 4
    topPadding: editorView.touchMode ? 4 : 0
    bottomPadding: editorView.touchMode ? 4 : 0
    function registerTextDocument() {
        if (!editorView.platformBackend)
            return
        if (codeDocument)
            editorView.platformBackend.registerCodeDocument(textDocument, syntaxLanguage)
        else
            editorView.platformBackend.registerTextDocument(textDocument, titleDocument)
    }

    function currentPlainText() {
        return getText(0, length)
    }

    function pendingInlineValue(style) {
        if (style === "bold")
            return pendingBold
        if (style === "italic")
            return pendingItalic
        if (style === "strike")
            return pendingStrike
        if (style === "underline")
            return pendingUnderline
        if (style === "code")
            return pendingCode
        return -1
    }

    function setPendingInlineValue(style, value) {
        if (style === "bold")
            pendingBold = value
        else if (style === "italic")
            pendingItalic = value
        else if (style === "strike")
            pendingStrike = value
        else if (style === "underline")
            pendingUnderline = value
        else if (style === "code")
            pendingCode = value
    }

    function hasPendingInlineStyle() {
        return pendingBold >= 0 || pendingItalic >= 0 || pendingStrike >= 0
                || pendingUnderline >= 0 || pendingCode >= 0
    }

    function clearPendingInlineStyles() {
        pendingBold = -1
        pendingItalic = -1
        pendingStrike = -1
        pendingUnderline = -1
        pendingCode = -1
        ++pendingFormatGeneration
    }

    function togglePendingInlineStyle(style) {
        if (!editorView.editorBackend
                || ["bold", "italic", "strike", "underline", "code"].indexOf(style) < 0)
            return false
        const pending = pendingInlineValue(style)
        const enabled = pending >= 0
                ? pending === 1
                : editorView.editorBackend.inlineFormatEnabled(textDocument, cursorPosition, style)
        setPendingInlineValue(style, enabled ? 0 : 1)
        forceActiveFocus()
        return true
    }

    function applyPendingInlineStyle(start, end) {
        if (!editorView.editorBackend || start < 0 || end <= start)
            return false
        let changed = false
        for (const style of ["bold", "italic", "strike", "underline", "code"]) {
            const value = pendingInlineValue(style)
            if (value >= 0)
                changed = editorView.editorBackend.setInlineFormat(textDocument, start, end,
                                                              style, value === 1) || changed
        }
        if (changed) {
            select(end, end)
            commitText(false)
            rememberPlainText()
        }
        return changed
    }

    function armPendingInlineFormatting(event) {
        if (!hasPendingInlineStyle() || selectionStart !== selectionEnd
                || event.text.length === 0
                || /[\r\n\u2028\u2029]/.test(event.text))
            return
        const start = cursorPosition
        const generation = pendingFormatGeneration
        Qt.callLater(function() {
            if (!blockArea || generation !== blockArea.pendingFormatGeneration
                    || !blockArea.activeFocus)
                return
            blockArea.applyPendingInlineStyle(start, blockArea.cursorPosition)
        })
    }

    function markdownRange(start, end) {
        return editorView.editorBackend.markdownSelection(textDocument, start, end)
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
        if (renderedMarkdown && sourceText.indexOf("ANYKEEPINSOPEN7F3A") >= 0
                && typeof editorView.editorBackend !== "undefined" && editorView.editorBackend !== null)
            editorView.editorBackend.applyInlineHtmlFormatting(textDocument)
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
            editorView.clearImageSelection()
            editorView.clearAudioSelection()
            editorView.clearAttachmentSelection()
            editorView.activeTagLineIndex = -1
            editorView.activeEditor = this
            rememberPlainText()
            editorView.scheduleCursorVisibility(this)
        } else {
            clearPendingInlineStyles()
            if (discardEmptyBlockOnFocusLoss)
                editorView.scheduleDiscardEmptyInsertedParagraph(this)
            if (sourceTextPending)
                synchronizeSourceText()
        }
    }
    onCursorPositionChanged: editorView.scheduleCursorVisibility(blockArea)
    Component.onCompleted: {
        synchronizeSourceText(true)
        editorView.registerEditor(blockArea)
        registerTextDocument()
        rememberPlainText()
        spellRefresh.restart()
    }
    Component.onDestruction: editorView.unregisterEditor(blockArea)

    Timer {
        id: spellRefresh
        interval: 0
        onTriggered: {
            blockArea.spellingRanges = editorView.platformBackend && !blockArea.codeDocument
                    ? editorView.platformBackend.spellCheckRanges(blockArea.textDocument) : []
            spellingCanvas.requestPaint()
        }
    }

    Connections {
        target: editorView
        function onPlatformBackendChanged() { blockArea.registerTextDocument() }
    }

    Connections {
        target: editorView.platformBackend
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
            if (!editorView.mouseSelectionActive)
                editorView.scheduleSelectionStateRefresh()
        }
    }

    // Structural paste must run before TextArea's built-in rich-text
    // importer; otherwise an office HTML table becomes an embedded
    // QTextDocument table instead of a NoteBlockModel table.
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: function(event) {
        armPendingInlineFormatting(event)
        if (event.key === Qt.Key_Control || event.key === Qt.Key_Meta) {
            primaryModifierDown = true
            editorMouseArea.refreshPlainLinkHover(event.modifiers)
            plainLinkHoverCanvas.requestPaint()
        }
        if (event.matches(StandardKey.Find)) {
            editorView.findRequested()
            event.accepted = true
        } else if (event.matches(StandardKey.Undo) && editorView.editorBackend.undo()) {
            event.accepted = true
        } else if (event.matches(StandardKey.Redo) && editorView.editorBackend.redo()) {
            event.accepted = true
        } else if (!blockArea.codeDocument && blockArea.handleLinkSpaceExit(event)) {
            event.accepted = true
        } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                && editorView.deleteStructuredSelection(event.key === Qt.Key_Backspace)) {
            event.accepted = true
        } else if (editorView.handleEmptyTextBlockDeletion(event, blockArea)) {
            event.accepted = true
        } else if (editorView.handleAdjacentImageDeletion(event, blockArea)) {
            event.accepted = true
        } else if (editorView.handleAdjacentTextBlockMerge(event, blockArea)) {
            event.accepted = true
        } else if (editorView.handleStructuredEnter(event, blockArea)) {
            event.accepted = true
        } else if (!blockArea.codeDocument && editorView.handleInlineFormatting(event, blockArea)) {
            event.accepted = true
        } else if (editorView.handleKeyboardSelection(event, blockArea)) {
            event.accepted = true
        } else if (keyHandler && keyHandler(event)) {
            event.accepted = true
        } else if (editorView.handleBlockBoundaryNavigation(event, blockArea)) {
            event.accepted = true
        } else if (event.matches(StandardKey.SelectAll)) {
            editorView.selectAllDocument()
            event.accepted = true
        } else if (event.matches(StandardKey.Copy) && editorView.copyActiveSelection()) {
            event.accepted = true
        } else if (event.matches(StandardKey.Cut) && editorView.cutActiveSelection()) {
            event.accepted = true
        } else if (event.matches(StandardKey.Paste) && editorView.pasteClipboard()) {
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
        const info = editorView.editorBackend.linkInfo(textDocument, position - 1, position)
        if (!info.valid || !info.href || info.end !== position)
            return false

        return editorView.runEditTransaction("leave-link", function() {
            const result = editorView.editorBackend.setLink(textDocument, position - 1, position, "")
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
        editorView.runEditTransaction("spell-replace", function() {
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
            if (linkPopup.visible && !linkPopup.hoverMode)
                return
            linkPopup.openFor(blockArea, info.start, info.end, info.href, false)
        }
    }

    Timer {
        id: modifierStatePoll
        interval: 50
        repeat: true
        running: editorMouseArea.containsMouse && !blockArea.renderedMarkdown
        onTriggered: {
            const pressed = editorView.editorBackend.primaryModifierPressed()
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
        enabled: !editorView.touchMode
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
                        ? editorView.editorBackend.linkInfo(blockArea.textDocument, position, position)
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
            editorView.editorBackend.updateHistoryViewState(editorView.captureEditorState(), true)
            const position = blockArea.positionAt(mouse.x, mouse.y)
            if (mouse.button === Qt.LeftButton) {
                blockArea.forceActiveFocus()
                editorView.activeEditor = blockArea
                editorView.selectionAnchorEditor = blockArea
                editorView.selectionAnchorPosition = position
                editorView.clearDocumentSelection()
                editorView.setEditorSelection(blockArea, position, position)
                editorView.mouseSelectionActive = true
                selecting = true
                selectionMoved = false
                mouse.accepted = true
                return
            }
            blockArea.forceActiveFocus()
            editorView.openEditorContextMenu(blockArea, mouse.x, mouse.y)
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
                editorView.applyDocumentSelection(editorView.selectionAnchorEditor, editorView.selectionAnchorPosition,
                                            blockArea, blockArea.positionAt(mouse.x, mouse.y))
                const elapsed = Date.now() - started
                if (elapsed >= 8)
                    console.info("QML selection move slow: path=local duration=" + elapsed
                                 + "ms editors=" + editorView.editors.length)
                return
            }
            const globalPosition = blockArea.mapToItem(editorView, mouse.x, mouse.y)
            const hit = editorView.editorAtPoint(globalPosition.x, globalPosition.y)
            if (hit)
                editorView.applyDocumentSelection(editorView.selectionAnchorEditor, editorView.selectionAnchorPosition,
                                            hit.editor, hit.position)
            const elapsed = Date.now() - started
            if (elapsed >= 8)
                console.info("QML selection move slow: path=cross-editor duration=" + elapsed
                             + "ms editors=" + editorView.editors.length)
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
                editorView.mouseSelectionActive = false
                if (selectionMoved)
                    editorView.copyDocumentSelectionToPrimary()
                editorView.scheduleSelectionStateRefresh()
                editorView.selectionAnchorEditor = null
            }
        }
        onExited: {
            hoverX = -1
            hoverY = -1
            clearPlainLinkHover()
            linkPopup.scheduleHoverClose(blockArea)
        }
        onDoubleClicked: function(mouse) {
            if (mouse.button !== Qt.LeftButton)
                return
            blockArea.cursorPosition = blockArea.positionAt(mouse.x, mouse.y)
            blockArea.selectWord()
            editorView.wholeDocumentSelected = false
            Qt.callLater(function() { editorView.copyDocumentSelectionToPrimary() })
        }
    }

    TapHandler {
        id: touchContextHandler
        enabled: editorView.touchMode
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onLongPressed: {
            editorView.openEditorContextMenu(blockArea, point.position.x, point.position.y)
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
                        : current.x + Math.max(current.width, editorView.editorFontAverageCharacterWidth)
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
