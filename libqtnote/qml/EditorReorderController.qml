import QtQuick

Item {
    id: controller

    objectName: "editorReorderController"
    property var editorView: null
    property var blockModel: null
    property var listBlocks: []

    readonly property bool dragging: sourceBlock !== null
    property var sourceBlock: null
    property int sourceItem: -1
    property int sourceEnd: -1
    property var sourceRows: []
    property real startPointerX: 0
    property real startPointerY: 0
    property real startContentX: 0
    property real startContentY: 0
    property real startDraggedTopY: 0
    property bool targetByDraggedTop: false
    property real translationX: 0
    property real translationY: 0
    property real draggedHeight: 0
    property bool committingDrop: false

    property var targetBlock: null
    property int targetItem: -1
    property int targetIndent: 0

    parent: editorView
    anchors.fill: parent
    visible: dragging
    enabled: false
    z: 100000

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
            targetBlock = null
    }

    function startListDrag(listBlock, row) {
        if (!editorView || !blockModel || !listBlock || !row || dragging)
            return

        startListRangeDrag(listBlock, row.index, listBlock.subtreeEnd(row.index), row.markerItem)
    }

    function startListRangeDrag(listBlock, firstItem, endItem, pointerItem, logicalPointerX) {
        if (!editorView || !blockModel || !listBlock || dragging
                || firstItem < 0 || endItem <= firstItem || !pointerItem)
            return

        sourceBlock = listBlock
        sourceItem = firstItem
        sourceEnd = endItem
        const rows = []
        draggedHeight = 0
        let draggedTop = Number.POSITIVE_INFINITY
        for (let index = sourceItem; index < sourceEnd; ++index) {
            const sourceRow = listBlock.rowAt(index)
            if (!sourceRow)
                continue
            const contentOrigin = sourceRow.dragContent.mapToItem(editorView.contentItem, 0, 0)
            rows.push(sourceRow)
            draggedHeight += sourceRow.naturalHeight
            draggedTop = Math.min(draggedTop, contentOrigin.y)
        }
        sourceRows = rows
        dragPreview.capture(rows.map(row => row.dragContent))

        const markerCenter = pointerItem.mapToItem(
            editorView.contentItem, pointerItem.width / 2, pointerItem.height / 2)
        startPointerX = logicalPointerX === undefined ? markerCenter.x : Number(logicalPointerX)
        startPointerY = markerCenter.y
        startDraggedTopY = rows.length > 0 ? draggedTop : startPointerY
        targetByDraggedTop = rows.length > 1
        startContentX = editorView.contentX
        startContentY = editorView.contentY
        translationX = 0
        translationY = 0
        updateTarget()
    }

    function moveListDrag(dx, dy) {
        if (!dragging)
            return
        translationX = dx
        translationY = dy
        updateTarget()
    }

    function animatedDisplacementBeforeBlock(blockIndex) {
        let displacement = 0
        for (const listBlock of listBlocks) {
            if (listBlock && listBlock.blockIndex < blockIndex)
                displacement += listBlock.animatedLayoutDisplacement()
        }
        return displacement
    }

    function updateTarget() {
        if (!dragging || !editorView)
            return

        const pointerX = startPointerX + translationX + editorView.contentX - startContentX
        const pointerY = startPointerY + translationY + editorView.contentY - startContentY
        // Boundaries omit the source range. In that compressed geometry the preview top
        // makes its physical bottom cross a lower row's midpoint before the target advances.
        const targetProbeY = targetByDraggedTop
                ? startDraggedTopY + translationY + editorView.contentY - startContentY
                : pointerY
        let bestBlock = null
        let bestItem = -1
        let bestDistance = Number.POSITIVE_INFINITY

        for (const listBlock of listBlocks) {
            if (!listBlock || !listBlock.visible)
                continue
            const count = listBlock.remainingItemCount()
            for (let item = 0; item <= count; ++item) {
                const boundary = listBlock.boundaryPosition(item)
                const logicalY = boundary.y
                                 - animatedDisplacementBeforeBlock(listBlock.blockIndex)
                                 - listBlock.animatedDisplacementBeforeBoundary(item)
                const distance = Math.abs(targetProbeY - logicalY)
                if (distance < bestDistance) {
                    bestDistance = distance
                    bestBlock = listBlock
                    bestItem = item
                }
            }
        }

        targetBlock = bestBlock
        targetItem = bestItem
        if (!bestBlock || bestItem < 0) {
            targetIndent = 0
            return
        }

        const markerCenter = typeof bestBlock.markerCenterXForIndent === "function"
                ? bestBlock.markerCenterXForIndent(0)
                : bestBlock.mapToItem(editorView.contentItem, 0, 0).x + editorView.listMarkerWidth / 2
        const requestedIndent = Math.round((pointerX - markerCenter) / editorView.listIndent)
        const maximumIndent = bestItem === 0 ? 0 : bestBlock.remainingIndentAt(bestItem - 1) + 1
        targetIndent = Math.max(0, Math.min(maximumIndent, requestedIndent))
    }

    function finishListDrag() {
        if (!dragging)
            return

        const fromBlock = sourceBlock.blockIndex
        const fromItem = sourceItem
        const fromEnd = sourceEnd
        const toBlock = targetBlock ? targetBlock.blockIndex : -1
        const toItem = targetItem
        const toIndent = targetIndent
        const activeEditor = editorView.activeEditor
        const focusedOffset = activeEditor
                && activeEditor.blockIndex === fromBlock
                && activeEditor.listItemIndex >= fromItem
                && activeEditor.listItemIndex < fromEnd
                ? activeEditor.listItemIndex - fromItem : -1
        const focusedCursor = focusedOffset >= 0 ? activeEditor.cursorPosition : 0
        const focusedSelectionStart = focusedOffset >= 0 ? activeEditor.selectionStart : 0
        const focusedSelectionEnd = focusedOffset >= 0 ? activeEditor.selectionEnd : 0
        const removesSourceBlock = fromBlock !== toBlock
                && fromItem === 0 && fromEnd === sourceBlock.itemCount()
        const viewportX = startPointerX + translationX - editorView.contentX
        const viewportY = startPointerY + translationY - editorView.contentY
        const droppedOutside = !editorView.touchMode
                && (viewportX < 0 || viewportY < 0
                    || viewportX > editorView.width || viewportY > editorView.height)

        if (droppedOutside) {
            committingDrop = true
            try {
                editorView.runEditTransaction("delete-list-items", function() {
                    editorView.prepareForStructuralMutation()
                    blockModel.removeListItems(fromBlock, fromItem, fromEnd - 1)
                    editorView.focusBlock(Math.min(fromBlock, blockModel.rowCount() - 1))
                })
                clearVisualState()
            } finally {
                committingDrop = false
            }
            return
        }

        if (toBlock < 0 || toItem < 0) {
            clearVisualState()
            return
        }
        committingDrop = true
        try {
            editorView.runEditTransaction("move-list-item", function() {
                if (!blockModel.moveListRange(fromBlock, fromItem, fromEnd - 1,
                                              toBlock, toItem, toIndent)) {
                    return
                }
                if (focusedOffset >= 0) {
                    editorView.focusEditorAddress({
                        blockIndex: removesSourceBlock && fromBlock < toBlock ? toBlock - 1 : toBlock,
                        listItemIndex: toItem + focusedOffset,
                        tableCellIndex: -1,
                        field: "listItem",
                        cursorPosition: focusedCursor,
                        selectionStart: focusedSelectionStart,
                        selectionEnd: focusedSelectionEnd
                    })
                }
            })
            clearVisualState()
        } finally {
            committingDrop = false
        }
    }

    function cancelDrag() {
        if (dragging)
            clearVisualState()
    }

    function clearVisualState() {
        sourceBlock = null
        sourceItem = -1
        sourceEnd = -1
        sourceRows = []
        targetBlock = null
        targetItem = -1
        targetIndent = 0
        startContentX = 0
        startContentY = 0
        startDraggedTopY = 0
        targetByDraggedTop = false
        translationX = 0
        translationY = 0
        draggedHeight = 0
        committingDrop = false
        dragPreview.clear()
    }

    Connections {
        target: controller.editorView
        enabled: controller.dragging

        function onContentXChanged() {
            controller.updateTarget()
        }

        function onContentYChanged() {
            controller.updateTarget()
        }
    }

    DragPreviewLayer {
        id: dragPreview

        anchors.fill: parent
        objectNamePrefix: "listDragPreview-"
        translationX: controller.translationX
        translationY: controller.translationY
    }
}
