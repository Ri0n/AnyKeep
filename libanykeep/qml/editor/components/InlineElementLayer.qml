import QtQuick

Item {
    id: root
    required property var editor
    required property var editorView
    objectName: "inlineElementLayer"
    anchors.fill: parent
    z: 30

    property var elements: []
    property int layoutRevision: 0
    property date today: startOfDay(new Date())
    property int pendingDateShortcutStart: -1
    property bool datePickerAddsSeparator: false
    property int pendingDateContinuationPosition: -1
    property bool materializingDateSeparator: false
    property bool inlineCaretOn: true
    property bool normalizingCursor: false
    property int lastCursorPosition: -1

    function startOfDay(value) {
        return new Date(value.getFullYear(), value.getMonth(), value.getDate())
    }

    function dateFromIso(value) {
        const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(value || ""))
        if (!match)
            return null
        const year = Number(match[1])
        const month = Number(match[2])
        const day = Number(match[3])
        if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31)
            return null
        const result = new Date(0)
        result.setHours(0, 0, 0, 0)
        result.setFullYear(year, month - 1, day)
        if (result.getFullYear() !== year || result.getMonth() !== month - 1
                || result.getDate() !== day)
            return null
        return result
    }

    function paddedNumber(value, width) {
        let result = String(value)
        while (result.length < width)
            result = "0" + result
        return result
    }

    function isoDate(value) {
        return paddedNumber(value.getFullYear(), 4)
                + "-" + paddedNumber(value.getMonth() + 1, 2)
                + "-" + paddedNumber(value.getDate(), 2)
    }

    function isDateBoundary(character) {
        if (!character || character.length === 0)
            return true
        if (/\s/.test(character))
            return true
        return "()[]{}<>,.;:!?\"'“”‘’—–".indexOf(character) >= 0
    }

    function dateState(dateText) {
        const value = dateFromIso(dateText)
        if (!value)
            return "future"
        const dayNumber = function(date) {
            return Math.floor(Date.UTC(date.getFullYear(), date.getMonth(), date.getDate()) / 86400000)
        }
        const delta = dayNumber(value) - dayNumber(today)
        if (delta < 0)
            return "overdue"
        if (delta <= 7)
            return "soon"
        return "future"
    }

    function refresh() {
        if (!editor || !editor.renderedMarkdown || editor.codeDocument
                || !editorView || !editorView.editorBackend) {
            elements = []
            ++layoutRevision
            if (editor)
                lastCursorPosition = Number(editor.cursorPosition)
            return
        }

        const plain = editor.currentPlainText()
        const pattern = /\d{4}-\d{2}-\d{2}/g
        const discovered = []
        let match
        while ((match = pattern.exec(plain)) !== null) {
            const start = match.index
            const end = start + match[0].length
            const before = start > 0 ? plain.charAt(start - 1) : ""
            const after = end < plain.length ? plain.charAt(end) : ""
            if (!isDateBoundary(before) || !isDateBoundary(after))
                continue
            if (!dateFromIso(match[0]))
                continue

            const link = editorView.editorBackend.linkInfo(editor.textDocument, start, end)
            if (link && link.valid && link.href)
                continue
            // QTextCursor::charFormat() reports the format immediately before
            // an insertion position, so probe one character into the date.
            if (editorView.editorBackend.inlineFormatEnabled(
                        editor.textDocument, Math.min(end, start + 1), "code"))
                continue

            discovered.push({
                type: "date",
                start: start,
                end: end,
                dateText: match[0]
            })
        }
        elements = discovered
        ++layoutRevision
        if (lastCursorPosition < 0)
            lastCursorPosition = Number(editor.cursorPosition)
        Qt.callLater(root.normalizeCursorAndSelection)
    }

    function invalidateGeometry() {
        ++layoutRevision
    }

    function materializePendingDateSeparator() {
        if (!editor || materializingDateSeparator || pendingDateContinuationPosition < 0)
            return false
        if (!editor.activeFocus || editor.selectionStart !== editor.selectionEnd) {
            pendingDateContinuationPosition = -1
            return false
        }

        const plain = editor.currentPlainText()
        const position = pendingDateContinuationPosition
        if (position >= plain.length)
            return false

        const characterAfterDate = plain.charAt(position)
        if (isDateBoundary(characterAfterDate)) {
            // The user supplied a real separator (space/newline) or typed
            // punctuation that is already a valid standalone-date boundary.
            pendingDateContinuationPosition = -1
            return false
        }

        const cursor = Number(editor.cursorPosition)
        pendingDateContinuationPosition = -1
        materializingDateSeparator = true
        editor.insert(position, " ")
        if (cursor >= position)
            editor.cursorPosition = cursor + 1
        materializingDateSeparator = false
        return true
    }

    function dateAtCursor(position, backspace) {
        for (const element of elements) {
            if (!element || element.type !== "date")
                continue
            if (backspace) {
                if (position === element.end
                        || (position > element.start && position < element.end))
                    return element
                if (editor && position === element.end + 1
                        && editor.getText(element.end, element.end + 1) === " ")
                    return element
            } else if (position === element.start
                       || (position > element.start && position < element.end)) {
                return element
            }
        }
        return null
    }

    function dateContainingPosition(position) {
        const value = Number(position)
        for (const element of elements) {
            if (element && element.type === "date"
                    && value > element.start && value < element.end)
                return element
        }
        return null
    }

    function normalizedSelectionBoundary(position, startBoundary) {
        const element = dateContainingPosition(position)
        if (!element)
            return Number(position)
        return Boolean(startBoundary) ? element.start : element.end
    }

    function normalizeCursorAndSelection() {
        if (!editor || normalizingCursor)
            return false

        const current = Number(editor.cursorPosition)
        const selectionStart = Number(editor.selectionStart)
        const selectionEnd = Number(editor.selectionEnd)
        const previous = lastCursorPosition

        if (selectionStart !== selectionEnd) {
            const activeElement = dateContainingPosition(current)
            if (activeElement) {
                const activeAtStart = current === selectionStart
                const anchor = activeAtStart ? selectionEnd : selectionStart
                let target

                if (anchor <= activeElement.start) {
                    // Expanding from the left selects the entire widget at
                    // once. Contracting back into a previously selected date
                    // drops the entire widget at once instead of exposing its
                    // individual source characters.
                    target = previous >= activeElement.end && current < previous
                            ? activeElement.start : activeElement.end
                } else if (anchor >= activeElement.end) {
                    target = previous >= 0 && previous <= activeElement.start
                            && current > previous
                            ? activeElement.end : activeElement.start
                } else {
                    target = current - activeElement.start
                            < activeElement.end - current
                            ? activeElement.start : activeElement.end
                }

                normalizingCursor = true
                editor.select(anchor, target)
                lastCursorPosition = Number(editor.cursorPosition)
                normalizingCursor = false
                return true
            }

            // Programmatic selections and cross-editor mouse selection can
            // occasionally move the non-active boundary without first moving
            // the cursor. Never leave either endpoint inside a widget.
            const normalizedStart = normalizedSelectionBoundary(selectionStart, true)
            const normalizedEnd = normalizedSelectionBoundary(selectionEnd, false)
            if (normalizedStart !== selectionStart || normalizedEnd !== selectionEnd) {
                const activeAtStart = current === selectionStart
                normalizingCursor = true
                if (activeAtStart)
                    editor.select(normalizedEnd, normalizedStart)
                else
                    editor.select(normalizedStart, normalizedEnd)
                lastCursorPosition = Number(editor.cursorPosition)
                normalizingCursor = false
                return true
            }

            lastCursorPosition = current
            return false
        }

        const element = dateContainingPosition(current)
        if (!element) {
            lastCursorPosition = current
            return false
        }

        let target
        if (previous >= 0 && previous <= element.start)
            target = element.end
        else if (previous >= element.end)
            target = element.start
        else
            target = current - element.start < element.end - current
                    ? element.start : element.end

        normalizingCursor = true
        editor.cursorPosition = target
        lastCursorPosition = target
        normalizingCursor = false
        return true
    }

    function cursorTouchesDate() {
        if (!editor || !editor.activeFocus || editor.selectionStart !== editor.selectionEnd)
            return false
        const position = editor.cursorPosition
        for (const element of elements) {
            if (element && element.type === "date"
                    && position >= element.start && position <= element.end)
                return true
        }
        return false
    }

    function currentCaretRectangle() {
        const revision = layoutRevision
        void revision
        if (!editor)
            return Qt.rect(0, 0, 0, 0)
        return editor.positionToRectangle(editor.cursorPosition)
    }

    function deleteDateAtCursor(backspace) {
        if (!editor || editor.selectionStart !== editor.selectionEnd)
            return false
        const element = dateAtCursor(editor.cursorPosition, Boolean(backspace))
        if (!element)
            return false
        let deleteEnd = element.end
        if (Boolean(backspace) && editor.cursorPosition === element.end + 1
                && editor.getText(element.end, element.end + 1) === " ") {
            deleteEnd = element.end + 1
        }
        const handled = editorView.runEditTransaction("delete-date", function() {
            editor.remove(element.start, deleteEnd)
            editor.cursorPosition = element.start
            editor.commitText(false)
            editor.rememberPlainText()
            return true
        })
        if (handled)
            Qt.callLater(root.refresh)
        return Boolean(handled)
    }

    function replaceRangeWithDate(start, end, value, addSeparator) {
        const date = startOfDay(value)
        const replacement = isoDate(date)
        const plain = editor.currentPlainText()
        const boundedStart = Math.max(0, Math.min(plain.length, Number(start)))
        const boundedEnd = Math.max(boundedStart, Math.min(plain.length, Number(end)))
        const characterAfter = boundedEnd < plain.length ? plain.charAt(boundedEnd) : ""
        let suffix = ""
        let existingSeparatorAdvance = 0
        if (Boolean(addSeparator)) {
            if (characterAfter === " " || characterAfter === "\t")
                existingSeparatorAdvance = 1
            else if (characterAfter.length > 0
                     && characterAfter !== "\n" && characterAfter !== "\r"
                     && ".,;:!?)]}".indexOf(characterAfter) < 0)
                suffix = " "
        }

        pendingDateContinuationPosition = -1
        const handled = editorView.runEditTransaction("set-date", function() {
            editor.remove(boundedStart, boundedEnd)
            editor.insert(boundedStart, replacement + suffix)
            editor.cursorPosition = boundedStart + replacement.length
                    + suffix.length + existingSeparatorAdvance
            editor.commitText(false)
            editor.rememberPlainText()
            return true
        })
        if (handled) {
            // At EOL (or immediately before punctuation/newline) a literal
            // trailing space would be normalized away. Remember the widget
            // boundary instead and materialize a separator only if the next
            // actual input is ordinary text.
            if (Boolean(addSeparator) && suffix.length === 0 && existingSeparatorAdvance === 0)
                pendingDateContinuationPosition = boundedStart + replacement.length
            Qt.callLater(root.refresh)
        }
        return Boolean(handled)
    }

    function openDatePicker(start, end, dateText, addSeparator) {
        if (!editor || !editor.renderedMarkdown || editor.codeDocument)
            return false
        datePickerAddsSeparator = Boolean(addSeparator)
        datePicker.openFor(editor, start, end, dateText)
        return true
    }

    function armDateShortcut(event) {
        pendingDateShortcutStart = -1
        if (!editor || !editor.renderedMarkdown || editor.codeDocument
                || editor.selectionStart !== editor.selectionEnd || event.text !== "/")
            return false

        const position = editor.cursorPosition
        if (position <= 0 || editor.getText(position - 1, position) !== "/")
            return false
        const before = position > 1 ? editor.getText(position - 2, position - 1) : ""
        // Keep this deliberately stricter than date boundary detection. It
        // avoids URL schemes (https://) and path-like text without guessing.
        if (before.length > 0 && !/\s/.test(before))
            return false

        const link = editorView.editorBackend.linkInfo(editor.textDocument, position - 1, position)
        if (link && link.valid && link.href)
            return false
        if (editorView.editorBackend.inlineFormatEnabled(editor.textDocument, position, "code"))
            return false

        pendingDateShortcutStart = position - 1
        dateShortcutTimer.restart()
        return true
    }

    Timer {
        id: dateShortcutTimer
        interval: 0
        repeat: false
        onTriggered: {
            const start = root.pendingDateShortcutStart
            root.pendingDateShortcutStart = -1
            if (start < 0 || !root.editor || root.editor.selectionStart !== root.editor.selectionEnd)
                return
            if (root.editor.getText(start, start + 2) !== "//")
                return
            root.openDatePicker(start, start + 2, "", true)
        }
    }

    Timer {
        interval: 60000
        repeat: true
        running: root.elements.length > 0
        onTriggered: {
            const current = root.startOfDay(new Date())
            if (current.getTime() !== root.today.getTime())
                root.today = current
        }
    }

    Timer {
        id: inlineCaretTimer
        interval: 500
        repeat: true
        running: root.cursorTouchesDate()
        onRunningChanged: {
            if (running)
                root.inlineCaretOn = true
        }
        onTriggered: root.inlineCaretOn = !root.inlineCaretOn
    }

    Connections {
        target: root.editor
        ignoreUnknownSignals: true
        function onTextChanged() {
            if (!root.materializingDateSeparator && root.pendingDateContinuationPosition >= 0)
                Qt.callLater(root.materializePendingDateSeparator)
        }
        function onCursorPositionChanged() {
            if (!root.normalizingCursor)
                root.normalizeCursorAndSelection()
            root.inlineCaretOn = true
            if (inlineCaretTimer.running)
                inlineCaretTimer.restart()
        }
        function onActiveFocusChanged() {
            root.inlineCaretOn = true
            if (root.editor && root.editor.activeFocus) {
                root.lastCursorPosition = Number(root.editor.cursorPosition)
                Qt.callLater(root.normalizeCursorAndSelection)
            } else {
                root.pendingDateContinuationPosition = -1
            }
        }
    }

    Repeater {
        model: root.elements
        delegate: Item {
            required property var modelData
            anchors.fill: parent

            DateChip {
                anchors.fill: parent
                visible: parent.modelData && parent.modelData.type === "date"
                editor: root.editor
                element: parent.modelData
                urgencyState: root.dateState(parent.modelData.dateText)
                layoutRevision: root.layoutRevision
                onActivated: root.openDatePicker(element.start, element.end, element.dateText, false)
            }
        }
    }

    Rectangle {
        readonly property var caretGeometry: root.currentCaretRectangle()
        z: 100
        visible: root.cursorTouchesDate() && root.inlineCaretOn
        x: caretGeometry.x
        y: caretGeometry.y + 1
        width: 1
        height: Math.max(2, caretGeometry.height - 2)
        color: root.editor ? root.editor.palette.text : "white"
    }

    DatePickerPopup {
        id: datePicker
        editorView: root.editorView
        onDateChosen: function(value) {
            root.replaceRangeWithDate(datePicker.selectionStart, datePicker.selectionEnd,
                                      value, root.datePickerAddsSeparator)
        }
        onClosed: root.datePickerAddsSeparator = false
    }
}
