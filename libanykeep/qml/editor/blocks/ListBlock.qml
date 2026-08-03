import QtQuick
import "../components" as Editor

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
            commitText: function() {
                if (!listBlock || !listRow)
                    return
                listRoot.editorView.blockModel.setListItem(
                    listBlock.blockIndex,
                    listRow.index,
                    listRoot.editorView.editorBackend.markdownText(textDocument))
            }
            textFormat: TextEdit.MarkdownText
            onTextChanged: commitChangedText(activeFocus && listBlock && !listBlock.syncingItems)
            onLinkActivated: link => Qt.openUrlExternally(link)
        }
    }
}
