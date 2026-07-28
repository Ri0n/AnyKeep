import QtQuick

Item {
    id: controller

    objectName: "editorReorderController"
    property var editorView: null
    property var blockModel: null
    property var listBlocks: []

    readonly property bool dragging: reorderCore.dragging
    property var sourceBlock: null
    property int sourceItem: -1
    property int sourceEnd: -1
    property var sourceRows: []
    readonly property real startPointerX: reorderCore.startPointerX
    readonly property real startPointerY: reorderCore.startPointerY
    readonly property real startContentX: reorderCore.startScrollX
    readonly property real startContentY: reorderCore.startScrollY
    readonly property real startDraggedTopY: reorderCore.startDraggedTopY
    readonly property bool targetByDraggedTop: reorderCore.targetByDraggedTop
    readonly property real translationX: reorderCore.translationX
    readonly property real translationY: reorderCore.translationY
    readonly property real draggedHeight: reorderCore.draggedExtent
    readonly property bool committingDrop: reorderCore.committingDrop

    property var targetBlock: null
    property int targetItem: -1
    property int targetIndent: 0

    parent: editorView
    anchors.fill: parent
    visible: dragging
    enabled: false
    z: 100000

    GenericReorderController {
        id: reorderCore

        anchors.fill: parent
        geometryItem: controller.editorView ? controller.editorView.contentItem : null
        scrollItem: controller.editorView
        compensateForScroll: true
        previewObjectNamePrefix: "listDragPreview-"
        boundaryProvider: function() {
            return controller.insertionBoundaries()
        }
        targetChangedHandler: function(boundary, pointerX) {
            controller.applyTarget(boundary, pointerX)
        }
        commitHandler: function(payload, boundary, core) {
            return controller.commitListDrop(boundary, core)
        }
        resetHandler: function() {
            controller.resetAdapterState()
        }
    }

    function registerListBlock(listBlock) {
        if (!listBlock || listBlocks.indexOf(listBlock) >= 0)
            return
        listBlocks = listBlocks.concat([listBlock])
    }

    function unregisterListBlock(listBlock) {
        const remaining = []
        for (const candidate of listBlocks)
            if (candidate && candidate !== listBlock)
                remaining.push(candidate)
        listBlocks = remaining
        if (sourceBlock === listBlock)
            cancelDrag()
        else if (targetBlock === listBlock)
            reorderCore.updateTarget()
    }

    function startListDrag(listBlock, row) {
        if (!editorView || !blockModel || !listBlock || !row || dragging)
            return
        startListRangeDrag(listBlock, row.index, listBlock.subtreeEnd(row.index),
                           row.markerItem)
    }

    function startListRangeDrag(listBlock, firstItem, endItem, pointerItem,
                                logicalPointerX) {
        if (!editorView || !blockModel || !listBlock || dragging
                || firstItem < 0 || endItem <= firstItem || !pointerItem)
            return

        sourceBlock = listBlock
        sourceItem = firstItem
        sourceEnd = endItem
        const rows = []
        const sources = []
        for (let index = sourceItem; index < sourceEnd; ++index) {
            const sourceRow = listBlock.rowAt(index)
            if (!sourceRow)
                continue
            rows.push(sourceRow)
            sources.push({
                item: sourceRow,
                previewItem: sourceRow.dragContent,
                geometryItem: sourceRow.dragContent,
                naturalExtent: sourceRow.naturalHeight
            })
        }
        sourceRows = rows

        const markerCenter = pointerItem.mapToItem(
            editorView.contentItem, pointerItem.width / 2, pointerItem.height / 2)
        const started = reorderCore.beginDrag({
            sources: sources,
            payload: {
                block: listBlock,
                firstItem: firstItem,
                endItem: endItem
            },
            pointerX: logicalPointerX === undefined
                      ? markerCenter.x : Number(logicalPointerX),
            pointerY: markerCenter.y,
            targetByDraggedTop: true
        })
        if (!started)
            resetAdapterState()
    }

    function moveListDrag(dx, dy) {
        reorderCore.moveDrag(dx, dy)
    }

    function animatedDisplacementBeforeBlock(blockIndex) {
        let displacement = 0
        for (const listBlock of listBlocks) {
            if (listBlock && listBlock.blockIndex < blockIndex)
                displacement += listBlock.animatedLayoutDisplacement()
        }
        return displacement
    }

    function insertionBoundaries() {
        const boundaries = []
        for (const listBlock of listBlocks) {
            if (!listBlock || !listBlock.visible)
                continue
            const count = listBlock.remainingItemCount()
            for (let item = 0; item <= count; ++item) {
                const boundary = listBlock.boundaryPosition(item)
                boundaries.push({
                    position: boundary.y
                              - animatedDisplacementBeforeBlock(listBlock.blockIndex)
                              - listBlock.animatedDisplacementBeforeBoundary(item),
                    block: listBlock,
                    item: item
                })
            }
        }
        return boundaries
    }

    function applyTarget(boundary, pointerX) {
        targetBlock = boundary ? boundary.block : null
        targetItem = boundary ? boundary.item : -1
        if (!targetBlock || targetItem < 0) {
            targetIndent = 0
            return
        }

        const markerCenter = typeof targetBlock.markerCenterXForIndent === "function"
                ? targetBlock.markerCenterXForIndent(0)
                : targetBlock.mapToItem(editorView.contentItem, 0, 0).x
                  + editorView.listMarkerWidth / 2
        const requestedIndent = Math.round((pointerX - markerCenter)
                                           / editorView.listIndent)
        const maximumIndent = targetItem === 0
                ? 0 : targetBlock.remainingIndentAt(targetItem - 1) + 1
        targetIndent = Math.max(0, Math.min(maximumIndent, requestedIndent))
    }

    function finishListDrag() {
        reorderCore.finishDrag()
    }

    function commitListDrop(boundary, core) {
        if (!sourceBlock)
            return false

        const fromBlock = sourceBlock.blockIndex
        const fromItem = sourceItem
        const fromEnd = sourceEnd
        const toBlock = boundary && boundary.block ? boundary.block.blockIndex : -1
        const toItem = boundary ? boundary.item : -1
        const toIndent = targetIndent
        const activeEditor = editorView.activeEditor
        const focusedOffset = activeEditor
                && activeEditor.blockIndex === fromBlock
                && activeEditor.listItemIndex >= fromItem
                && activeEditor.listItemIndex < fromEnd
                ? activeEditor.listItemIndex - fromItem : -1
        const focusedCursor = focusedOffset >= 0 ? activeEditor.cursorPosition : 0
        const focusedSelectionStart = focusedOffset >= 0
                ? activeEditor.selectionStart : 0
        const focusedSelectionEnd = focusedOffset >= 0
                ? activeEditor.selectionEnd : 0
        const removesSourceBlock = fromBlock !== toBlock
                && fromItem === 0 && fromEnd === sourceBlock.itemCount()
        const viewportX = core.currentPointerX - editorView.contentX
        const viewportY = core.currentPointerY - editorView.contentY
        const droppedOutside = !editorView.touchMode
                && (viewportX < 0 || viewportY < 0
                    || viewportX > editorView.width || viewportY > editorView.height)

        if (droppedOutside) {
            editorView.runEditTransaction("delete-list-items", function() {
                editorView.prepareForStructuralMutation()
                blockModel.removeListItems(fromBlock, fromItem, fromEnd - 1)
                editorView.focusBlock(Math.min(fromBlock, blockModel.rowCount() - 1))
            })
            return true
        }
        if (toBlock < 0 || toItem < 0)
            return false

        let moved = false
        editorView.runEditTransaction("move-list-item", function() {
            if (!blockModel.moveListRange(fromBlock, fromItem, fromEnd - 1,
                                          toBlock, toItem, toIndent)) {
                return
            }
            moved = true
            if (focusedOffset >= 0) {
                editorView.focusEditorAddress({
                    blockIndex: removesSourceBlock && fromBlock < toBlock
                                ? toBlock - 1 : toBlock,
                    listItemIndex: toItem + focusedOffset,
                    tableCellIndex: -1,
                    field: "listItem",
                    cursorPosition: focusedCursor,
                    selectionStart: focusedSelectionStart,
                    selectionEnd: focusedSelectionEnd
                })
            }
        })
        return moved
    }

    function cancelDrag() {
        reorderCore.cancelDrag()
    }

    function clearVisualState() {
        reorderCore.cancelDrag()
    }

    function resetAdapterState() {
        sourceBlock = null
        sourceItem = -1
        sourceEnd = -1
        sourceRows = []
        targetBlock = null
        targetItem = -1
        targetIndent = 0
    }
}
