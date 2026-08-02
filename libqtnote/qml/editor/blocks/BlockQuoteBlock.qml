import QtQuick
import "../components" as Editor

Item {
    id: quoteRoot
    required property var editorView
    required property var linkEditorPopup
    required property var block
    width: block.width
    implicitHeight: quoteCell.implicitHeight

    Rectangle {
        width: Math.max(3, Math.round(quoteRoot.editorView.editorFontAverageCharacterWidth * 0.45))
        radius: width / 2
        color: quoteCell.palette.midlight
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }

    Editor.NoteBlockTextArea {
        editorView: quoteRoot.editorView
        linkPopup: quoteRoot.linkEditorPopup
        id: quoteCell
        property var block: quoteRoot.block
        x: Math.max(12, Math.round(quoteRoot.editorView.editorFontAverageCharacterWidth * 1.6))
        width: Math.max(0, quoteRoot.width - x)
        blockIndex: block.index
        editorField: "blockquote"
        sourceText: quoteRoot.editorView.markdownForRendering(block.blockText)
        textFormat: TextEdit.MarkdownText
        font.italic: true
        commitText: function() {
            quoteRoot.editorView.blockModel.setBlockText(
                block.index, quoteRoot.editorView.editorBackend.markdownText(textDocument))
        }
        onTextChanged: commitChangedText(activeFocus)
        onLinkActivated: link => Qt.openUrlExternally(link)
    }
}
