import QtQuick
import "../components" as Editor

Editor.NoteBlockTextArea {
    id: headingCell
    required property var block
    titleDocument: block.index === 0
    blockIndex: block.index
    editorField: "heading"
    width: block.width
    sourceText: editorView.markdownForRendering(block.blockText)
    font.bold: true
    font.family: editorView.editorFont.family
    font.pointSize: editorView.editorPointSize * (block.headingLevel === 1 ? 1.7
                    : block.headingLevel === 2 ? 1.5
                    : block.headingLevel === 3 ? 1.3
                    : block.headingLevel === 4 ? 1.15
                    : block.headingLevel === 5 ? 1.0 : 0.9)
    topPadding: Math.max(editorView.touchMode ? 8 : 4,
                         Math.round(editorView.editorFontMetricsHeight * 0.55))
    bottomPadding: Math.max(editorView.touchMode ? 4 : 2,
                            Math.round(editorView.editorFontMetricsHeight * 0.18))
    keyHandler: function(event) { return editorView.handleHeadingShortcut(event, headingCell) }
    commitText: function() {
        editorView.blockModel.setBlockText(
            block.index, editorView.editorBackend.markdownText(textDocument))
    }
    textFormat: TextEdit.MarkdownText
    onTextChanged: commitChangedText(activeFocus)
    onLinkActivated: link => Qt.openUrlExternally(link)
}
