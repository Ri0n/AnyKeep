import QtQuick
import "../reorder" as Reorder

FocusScope {
    id: blockDelegate
    required property var editorView
    required property var reorderController
    required property Component textEditorComponent
    required property Component tagLineEditorComponent
    required property Component codeBlockEditorComponent
    required property Component headingEditorComponent
    required property Component blockQuoteEditorComponent
    required property Component listEditorComponent
    required property Component tableEditorComponent
    required property Component imageEditorComponent
    required property Component audioEditorComponent
    required property Component attachmentEditorComponent
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
    required property var tags
    required property real audioDuration
    required property string audioTranscript
    required property string attachmentMediaType
    required property real attachmentSize
    property alias item: blockLoader.item
    readonly property bool reorderSourceActive:
        reorderController.blockSourceActive(blockDelegate)
    readonly property bool reorderTargetBefore:
        reorderController.blockTargetBefore(blockDelegate)
    readonly property bool reorderTargetAfter:
        reorderController.blockTargetAfter(blockDelegate)
    readonly property real reorderOffset: blockDisplacement.displacement
    width: Math.max(0, editorView.width - editorView.scrollBarInset)
    height: blockLoader.height
    opacity: reorderSourceActive ? 0 : 1
    transform: Translate { y: blockDelegate.reorderOffset }

    Reorder.ReorderDisplacement {
        id: blockDisplacement

        animationEnabled: reorderController.blockAnimationActive
                          && !reorderController.committingDrop
        sourceActive: blockDelegate.reorderSourceActive
        targetBefore: blockDelegate.reorderTargetBefore
        targetAfter: blockDelegate.reorderTargetAfter
        naturalExtent: blockDelegate.height + editorView.spacing
        draggedExtent: reorderController.structuralDraggedHeight
        displacement: reorderController.blockTranslation(blockDelegate)
    }

    Reorder.BlockReorderHandle {
        id: structuralBlockHandle

        objectName: "blockReorderHandle-" + blockDelegate.index
        visible: !editorView.touchMode
                 && blockDelegate.blockType !== NoteBlockType.BulletList
                 && blockDelegate.blockType !== NoteBlockType.CheckList
                 && blockDelegate.blockType !== NoteBlockType.Image
                 && blockDelegate.blockType !== NoteBlockType.NumberedList
                 && blockDelegate.blockType !== NoteBlockType.TagLine
        x: Math.max(0, blockLoader.x - width)
        y: 0
        width: editorView.listLevelHandleGutter
        height: Math.max(editorView.editorFontMetricsHeight, blockDelegate.height)
        z: 20
        fullHeight: true
        dragEnabled: !reorderController.dragging || dragging
        onDragStarted: reorderController.startBlockDrag(
                           blockDelegate, blockLoader.item || blockLoader)
        onDragMoved: function(dx, dy) {
            reorderController.moveBlockDrag(dx, dy)
        }
        onDragFinished: reorderController.finishBlockDrag()
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
        property var tags: blockDelegate.tags
        property real audioDuration: blockDelegate.audioDuration
        property string audioTranscript: blockDelegate.audioTranscript
        property string attachmentMediaType: blockDelegate.attachmentMediaType
        property real attachmentSize: blockDelegate.attachmentSize
        readonly property bool structurallySelected:
            blockDelegate.editorView.structuralBlockSelected(index)
        x: editorView.editorInset
        width: Math.max(0, blockDelegate.width - 2 * editorView.editorInset)
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
            if (blockType === NoteBlockType.Text && index === 0 && blockText.trim().length === 0)
                item.forceActiveFocus()
        }
        sourceComponent: blockType === NoteBlockType.BulletList || blockType === NoteBlockType.CheckList || blockType === NoteBlockType.NumberedList ? listEditorComponent
                       : blockType === NoteBlockType.Table ? tableEditorComponent
                       : blockType === NoteBlockType.Image ? imageEditorComponent
                       : blockType === NoteBlockType.Heading ? headingEditorComponent
                       : blockType === NoteBlockType.BlockQuote ? blockQuoteEditorComponent
                       : blockType === NoteBlockType.CodeBlock ? codeBlockEditorComponent
                       : blockType === NoteBlockType.TagLine ? tagLineEditorComponent
                       : blockType === NoteBlockType.Audio ? audioEditorComponent
                       : blockType === NoteBlockType.Attachment ? attachmentEditorComponent : textEditorComponent
    }

}
