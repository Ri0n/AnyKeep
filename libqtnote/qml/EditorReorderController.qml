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
    property real translationX: 0
    property real translationY: 0
    property real draggedHeight: 0

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

        sourceBlock = listBlock
        sourceItem = row.index
        sourceEnd = listBlock.subtreeEnd(sourceItem)
        const rows = []
        draggedHeight = 0
        for (let index = sourceItem; index < sourceEnd; ++index) {
            const sourceRow = listBlock.rowAt(index)
            if (!sourceRow)
                continue
            const origin = sourceRow.dragContent.mapToItem(controller, 0, 0)
            sourceRow.dragOriginX = origin.x
            sourceRow.dragOriginY = origin.y
            rows.push(sourceRow)
            draggedHeight += sourceRow.naturalHeight
        }
        sourceRows = rows

        const markerCenter = row.markerItem.mapToItem(
            editorView.contentItem, row.markerItem.width / 2, row.markerItem.height / 2)
        startPointerX = markerCenter.x
        startPointerY = markerCenter.y
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
                const distance = Math.abs(pointerY - logicalY)
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
        sourceBlock = null
        sourceItem = -1
        sourceEnd = -1
        sourceRows = []
        targetBlock = null
        targetItem = -1
        targetIndent = 0
        startContentX = 0
        startContentY = 0
        translationX = 0
        translationY = 0
        draggedHeight = 0
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

    Repeater {
        model: controller.sourceRows

        ShaderEffectSource {
            required property int index
            required property var modelData

            readonly property var sourceRow: modelData

            objectName: "listDragPreview-" + index
            sourceItem: sourceRow ? sourceRow.dragContent : null
            hideSource: true
            live: true
            recursive: true
            smooth: true
            x: sourceRow ? sourceRow.dragOriginX + controller.translationX : 0
            y: sourceRow ? sourceRow.dragOriginY + controller.translationY : 0
            width: sourceItem ? sourceItem.width : 0
            height: sourceItem ? sourceItem.height : 0
            sourceRect: Qt.rect(0, 0, width, height)
            opacity: 0.9
        }
    }
}
