import QtQuick
import "../components" as Editor
import "../support/ListBlockBehavior.js" as ListBlockBehavior

Editor.ListBlockEditor {
    id: listRoot
    required property var linkEditorPopup
    editorDelegate: listItemEditorDelegate

    Component {
        id: listItemEditorDelegate

        Editor.NoteBlockTextArea {
            editorView: listRoot.editorView
            linkPopup: listRoot.linkEditorPopup
            id: listItemCell

            readonly property var listRow: parent.listRow
            readonly property var listBlock: parent.listBlock
            blockIndex: listBlock ? listBlock.blockIndex : -1
            listItemIndex: listRow ? listRow.index : -1
            editorField: "listItem"
            width: parent.width
            sourceText: listRoot.editorView.markdownForRendering(listRow ? listRow.itemText : "")
            keyHandler: function(event) {
                return listBlock && listRow
                        ? listBlock.handleItemKey(event, listItemCell, listRow.index) : false
            }
            property bool listImeReturnQueued: false
            finalizeInputMethodEdit: function() { return scheduleListImeReturn() }

            function normalizeListPlainText(value) {
                return String(value || "").replace(/\r\n/g, "\n")
                                          .replace(/[\r\u2028\u2029]/g, "\n")
            }

            function scheduleListImeReturn() {
                if (!androidImeWorkaround || !activeFocus || syncingSourceText
                        || inputMethodComposing || !listBlock || !listRow
                        || !observedPlainTextInitialized || !editorView.blockModel)
                    return false
                const current = normalizeListPlainText(currentPlainText())
                const previous = normalizeListPlainText(observedPlainText)
                const countBreaks = value => (value.match(/\n/g) || []).length
                if (countBreaks(current) <= countBreaks(previous))
                    return false
                if (listImeReturnQueued)
                    return true

                // Locate the newly committed separator, not an older newline
                // in an already-multiline item.
                let prefix = 0
                while (prefix < previous.length && prefix < current.length
                        && previous.charAt(prefix) === current.charAt(prefix))
                    ++prefix
                const separator = current.indexOf("\n", prefix)
                if (separator < 0)
                    return false
                const priorBreak = current.lastIndexOf("\n", separator - 1)
                const lineBefore = current.substring(priorBreak + 1, separator)
                const exit = listBlock.itemIndent(listRow.index) === 0
                        && separator === current.length - 1
                        && lineBefore.trim().length === 0
                const before = markdownRange(0, separator)
                const after = markdownRange(separator + 1, length)
                const caret = Math.max(0, cursorPosition - separator - 1)
                const targetRow = listRow.index
                listImeReturnQueued = true
                Qt.callLater(function() {
                    listImeReturnQueued = false
                    if (!listItemCell.activeFocus || listItemCell.inputMethodComposing
                            || !listBlock || !listRow || listRow.index !== targetRow)
                        return
                    ListBlockBehavior.splitCommittedImeLine(
                                listBlock, editorView, listItemCell, targetRow,
                                separator, before, after, exit, caret)
                })
                return true
            }
            commitText: function() {
                if (!listBlock || !listRow)
                    return
                listRoot.editorView.blockModel.setListItem(
                    listBlock.blockIndex,
                    listRow.index,
                    listRoot.editorView.editorBackend.markdownText(textDocument))
            }
            textFormat: TextEdit.MarkdownText
            onTextChanged: {
                if (androidImeWorkaround && scheduleListImeReturn())
                    return
                commitChangedText(activeFocus && listBlock && !listBlock.syncingItems)
            }
            onLinkActivated: link => Qt.openUrlExternally(link)
        }
    }
}
