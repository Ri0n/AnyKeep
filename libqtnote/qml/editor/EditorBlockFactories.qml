pragma ComponentBehavior: Bound

import QtQuick
import "blocks" as Blocks

QtObject {
    id: factories

    required property var editorView
    required property var reorderController

    property Component textEditor: Component {
        Blocks.TextBlock {
            block: parent
            editorView: factories.editorView
            linkPopup: factories.editorView.linkEditorPopup
        }
    }

    property Component tagLineEditor: Component {
        Blocks.TagLineBlock {
            block: parent
            editorView: factories.editorView
        }
    }

    property Component codeBlockEditor: Component {
        Blocks.CodeBlock {
            block: parent
            editorView: factories.editorView
            linkEditorPopup: factories.editorView.linkEditorPopup
        }
    }

    property Component headingEditor: Component {
        Blocks.HeadingBlock {
            block: parent
            editorView: factories.editorView
            linkPopup: factories.editorView.linkEditorPopup
        }
    }

    property Component blockQuoteEditor: Component {
        Blocks.BlockQuoteBlock {
            block: parent
            editorView: factories.editorView
            linkEditorPopup: factories.editorView.linkEditorPopup
        }
    }

    property Component listEditor: Component {
        Blocks.ListBlock {
            block: parent
            editorView: factories.editorView
            reorderController: factories.reorderController
            linkEditorPopup: factories.editorView.linkEditorPopup
        }
    }

    property Component tableEditor: Component {
        Blocks.TableBlock {
            block: parent
            editorView: factories.editorView
            linkEditorPopup: factories.editorView.linkEditorPopup
        }
    }

    property Component imageEditor: Component {
        Blocks.ImageBlock {
            block: parent
            editorView: factories.editorView
            reorderController: factories.reorderController
        }
    }

    property Component audioEditor: Component {
        Blocks.AudioBlock {
            block: parent
            editorView: factories.editorView
        }
    }

    property Component attachmentEditor: Component {
        Blocks.AttachmentBlock {
            block: parent
            editorView: factories.editorView
        }
    }
}
