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
    readonly property real headingScale: block.headingLevel === 1 ? 1.7
                                         : block.headingLevel === 2 ? 1.5
                                         : block.headingLevel === 3 ? 1.3
                                         : block.headingLevel === 4 ? 1.15
                                         : block.headingLevel === 5 ? 1.0 : 0.9
    // Assign the composite font in one binding. NoteBlockTextArea binds its
    // complete font to editorFont; overriding grouped font sub-properties on
    // top of that leaves some Qt versions with the old point size when a live
    // paragraph delegate turns into a heading. Keep the configured editor
    // font as the source and only scale its size for this structural role.
    font: Qt.font({
        family: editorView.editorFont.family,
        pointSize: editorView.editorPointSize * headingScale,
        bold: true,
        italic: editorView.editorFont.italic,
        underline: editorView.editorFont.underline,
        strikeout: editorView.editorFont.strikeout,
        capitalization: editorView.editorFont.capitalization,
        letterSpacing: editorView.editorFont.letterSpacing,
        wordSpacing: editorView.editorFont.wordSpacing,
        kerning: editorView.editorFont.kerning,
        styleName: editorView.editorFont.styleName
    })
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
