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
    property real translationX: 0
    property real translationY: 0
    property real draggedHeight: 0

    property var targetBlock: null
    property int targetItem: -1
    property int targetIndent: 0

    visible: false
    width: 0
    height: 0

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

        sourceBlock = listBlock
        sourceItem = row.index
        sourceEnd = listBlock.subtreeEnd(sourceItem)
        sourceRows = []
        draggedHeight = 0
        for (let index = sourceItem; index < sourceEnd; ++index) {
            const sourceRow = listBlock.rowAt(index)
            if (!sourceRow)
                continue
            const origin = sourceRow.dragContent.mapToItem(editorView.contentItem, 0, 0)
            sourceRow.dragOriginX = origin.x
            sourceRow.dragOriginY = origin.y
            sourceRows.push(sourceRow)
            draggedHeight += sourceRow.naturalHeight
        }

        const markerCenter = row.markerItem.mapToItem(
            editorView.contentItem, row.markerItem.width / 2, row.markerItem.height / 2)
        startPointerX = markerCenter.x
        startPointerY = markerCenter.y
        translationX = 0
        translationY = 0
        updateTarget()
        Qt.callLater(positionSourceRows)
    }

    function moveListDrag(dx, dy) {
        if (!dragging)
            return
        translationX = dx
        translationY = dy
        updateTarget()
        positionSourceRows()
    }

    function positionSourceRows() {
        if (!dragging || !editorView)
            return
        for (const row of sourceRows) {
            if (!row || !row.dragContent)
                continue
            const mapped = row.dragContent.mapToItem(editorView.contentItem, 0, 0)
            const baseX = mapped.x - row.dragTranslationX
            const baseY = mapped.y - row.dragTranslationY
            row.dragTranslationX = row.dragOriginX + translationX - baseX
            row.dragTranslationY = row.dragOriginY + translationY - baseY
        }
    }

    function updateTarget() {
        if (!dragging || !editorView)
            return

        const pointerX = startPointerX + translationX
        const pointerY = startPointerY + translationY
        let bestBlock = null
        let bestItem = -1
        let bestDistance = Number.POSITIVE_INFINITY

        for (const listBlock of listBlocks) {
            if (!listBlock || !listBlock.visible)
                continue
            const count = listBlock.remainingItemCount()
            for (let item = 0; item <= count; ++item) {
                const boundary = listBlock.boundaryPosition(item)
                const distance = Math.abs(pointerY - boundary.y)
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

        const blockOrigin = bestBlock.mapToItem(editorView.contentItem, 0, 0)
        const markerCenter = blockOrigin.x + editorView.listMarkerWidth / 2
        const requestedIndent = Math.round((pointerX - markerCenter) / editorView.listIndent)
        const maximumIndent = bestItem === 0 ? 0 : bestBlock.remainingIndentAt(bestItem - 1) + 1
        targetIndent = Math.max(0, Math.min(maximumIndent, requestedIndent))
    }

    function finishListDrag() {
        if (!dragging)
            return

        const fromBlock = sourceBlock.blockIndex
        const fromItem = sourceItem
        const toBlock = targetBlock ? targetBlock.blockIndex : -1
        const toItem = targetItem
        const toIndent = targetIndent
        clearVisualState()

        if (toBlock < 0 || toItem < 0)
            return
        editorView.runEditTransaction("move-list-item", function() {
            blockModel.moveListSubtree(fromBlock, fromItem, toBlock, toItem, toIndent)
        })
    }

    function cancelDrag() {
        if (dragging)
            clearVisualState()
    }

    function clearVisualState() {
        const rows = sourceRows.slice()
        sourceBlock = null
        sourceItem = -1
        sourceEnd = -1
        sourceRows = []
        targetBlock = null
        targetItem = -1
        targetIndent = 0
        translationX = 0
        translationY = 0
        draggedHeight = 0
        for (const row of rows) {
            if (!row)
                continue
            row.dragTranslationX = 0
            row.dragTranslationY = 0
        }
    }

    Timer {
        interval: 16
        repeat: true
        running: controller.dragging
        onTriggered: controller.positionSourceRows()
    }
}
