import QtQuick
import "../components" as Editor

Editor.TagLineEditor {
    required property var block
    blockModel: editorView.blockModel
    blockIndex: block.index
    tags: block.tags
    editorFont: editorView.editorFont
    touchMode: editorView.touchMode
    width: block.width
}
