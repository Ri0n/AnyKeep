import QtQuick
import AnyKeep.Editor 1.0
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
    required property Component mediaEditorComponent
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
    required property int mediaDisplayWidth
    required property string mediaAlignment
    required property url previewUrl
    required property var tags
    required property string mediaType
    required property real mediaDuration
    required property int mediaWidth
    required property int mediaHeight
    required property string mediaTranscript
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
        visible: dragging || (!editorView.blockInReorderRange(blockDelegate.index)
                 && blockDelegate.blockType !== NoteBlockType.BulletList
                 && blockDelegate.blockType !== NoteBlockType.CheckList
                 && blockDelegate.blockType !== NoteBlockType.NumberedList
                 && blockDelegate.blockType !== NoteBlockType.TagLine
                 && (editorView.touchMode ? blockDelegate.activeFocus && editorView.count > 1
                                           : blockDelegate.blockType !== NoteBlockType.Media))
        forceVisible: editorView.touchMode
        x: editorView.rightToLeft ? blockLoader.x + blockLoader.width
                                  : Math.max(0, blockLoader.x - width)
        y: editorView.touchMode
           ? Math.max(0, Math.min(blockDelegate.height - height, editorView.contentY - blockDelegate.y))
           : 0
        width: editorView.reorderHandleWidth
        height: editorView.touchMode ? 44 : Math.max(editorView.editorFontMetricsHeight, blockDelegate.height)
        z: 20
        fullHeight: !editorView.touchMode
        dragEnabled: !reorderController.dragging || dragging
        onDragStarted: reorderController.startBlockDrag(
                           blockDelegate, blockLoader.item || blockLoader,
                           editorView.touchMode ? structuralBlockHandle : undefined)
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
        property int mediaDisplayWidth: blockDelegate.mediaDisplayWidth
        property string mediaAlignment: blockDelegate.mediaAlignment
        property url previewUrl: blockDelegate.previewUrl
        property var tags: blockDelegate.tags
        property string mediaType: blockDelegate.mediaType
        property real mediaDuration: blockDelegate.mediaDuration
        property int mediaWidth: blockDelegate.mediaWidth
        property int mediaHeight: blockDelegate.mediaHeight
        property string mediaTranscript: blockDelegate.mediaTranscript
        property string attachmentMediaType: blockDelegate.attachmentMediaType
        property real attachmentSize: blockDelegate.attachmentSize
        x: editorView.rightToLeft ? editorView.editorInset : editorView.leadingEditorInset
        width: Math.max(0, blockDelegate.width - editorView.editorInset - editorView.leadingEditorInset)
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
                       : blockType === NoteBlockType.Media ? mediaEditorComponent
                       : blockType === NoteBlockType.Heading ? headingEditorComponent
                       : blockType === NoteBlockType.BlockQuote ? blockQuoteEditorComponent
                       : blockType === NoteBlockType.CodeBlock ? codeBlockEditorComponent
                       : blockType === NoteBlockType.TagLine ? tagLineEditorComponent
                       : blockType === NoteBlockType.Attachment ? attachmentEditorComponent : textEditorComponent
    }

}
