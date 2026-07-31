import QtQuick
import "reorder" as Reorder

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
    property bool sourceRemovesWholeBlock: false
    property string sourceKind: ""
    property int sourceBlockRow: -1
    property int sourceBlockType: -1
    property var sourceFocusAddress: null
    readonly property real startPointerX: reorderCore.startPointerX
    readonly property real startPointerY: reorderCore.startPointerY
    readonly property real startContentX: reorderCore.startScrollX
    readonly property real startContentY: reorderCore.startScrollY
    readonly property real startDraggedTopY: reorderCore.startDraggedTopY
    readonly property bool targetByDraggedTop: reorderCore.targetByDraggedTop
    readonly property real translationX: reorderCore.translationX
    readonly property real translationY: reorderCore.translationY
    readonly property real draggedHeight: reorderCore.draggedExtent
    readonly property real structuralDraggedHeight: draggedHeight
                                                    + (sourceKind === "list" && editorView
                                                       ? Number(editorView.spacing) : 0)
    readonly property bool committingDrop: reorderCore.committingDrop
    readonly property bool sourceListRemovesWholeBlock: sourceKind === "list"
                                                        && sourceRemovesWholeBlock
    readonly property bool structuralTargetActive: dragging && targetKind === "block"
    readonly property bool blockAnimationActive: dragging
                                                 && (structuralTargetActive
                                                     || sourceListRemovesWholeBlock)

    property string targetKind: ""
    property var targetBlock: null
    property int targetItem: -1
    property int targetIndent: 0

    parent: editorView
    anchors.fill: parent
    visible: dragging
    enabled: false
    z: 100000

    Reorder.LinearReorderLayout {
        id: blockLayout

        geometryItem: controller.editorView ? controller.editorView.contentItem : null
        sourceEntries: controller.blockLayoutSourceEntries()
        keyProvider: function(item) { return item ? Number(item.index) : -1 }
        orderProvider: function(item) { return item ? Number(item.index) : -1 }
        extentProvider: function(item) {
            return item && controller.editorView
                    ? Number(item.height) + Number(controller.editorView.spacing) : 0
        }
        offsetProvider: function(item) {
            return item && item.reorderOffset !== undefined
                    ? Number(item.reorderOffset) : 0
        }
    }

    Reorder.LinearReorderLayout {
        id: listLayout

        geometryItem: controller.editorView ? controller.editorView.contentItem : null
        sourceEntries: controller.sourceKind === "list" ? reorderCore.sourceEntries : []
        keyProvider: function(item) { return item }
        orderProvider: function(item) {
            if (!item)
                return -1
            if (item.row !== undefined)
                return Number(item.row)
            if (item.index !== undefined)
                return Number(item.index)
            return -1
        }
        extentProvider: function(item) {
            if (!item)
                return 0
            return item.naturalHeight !== undefined
                    ? Number(item.naturalHeight) : Number(item.height)
        }
    }

    Reorder.HierarchyDropPolicy {
        id: hierarchyDropPolicy
    }

    Reorder.GenericReorderController {
        id: reorderCore

        anchors.fill: parent
        geometryItem: controller.editorView ? controller.editorView.contentItem : null
        scrollItem: controller.editorView
        compensateForScroll: true
        previewObjectNamePrefix: "editorDragPreview-"
        boundaryProvider: function() {
            return controller.sourceKind === "block"
                    ? controller.blockInsertionBoundaries()
                    : controller.listOrBlockInsertionBoundaries()
        }
        targetChangedHandler: function(boundary, pointerX) {
            if (boundary && boundary.kind === "list")
                controller.applyListTarget(boundary, pointerX)
            else
                controller.applyBlockTarget(boundary)
        }
        commitHandler: function(payload, boundary, core) {
            return payload && payload.kind === "block"
                    ? controller.commitBlockDrop(boundary, core)
                    : controller.commitListDrop(boundary, core)
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
        if (sourceBlock === listBlock) {
            if (!committingDrop)
                cancelDrag()
        } else if (targetBlock === listBlock && !committingDrop) {
            reorderCore.updateTarget()
        }
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

        sourceKind = "list"
        sourceBlock = listBlock
        sourceBlockRow = Number(listBlock.blockIndex)
        sourceItem = firstItem
        sourceEnd = endItem
        sourceRemovesWholeBlock = firstItem === 0 && endItem === listBlock.itemCount()
        const rows = []
        const sources = []
        for (let index = sourceItem; index < sourceEnd; ++index) {
            const sourceRow = listBlock.rowAt(index)
            if (!sourceRow)
                continue
            rows.push(sourceRow)
            sources.push({
                item: sourceRow,
                key: listBlock.blockIndex + ":" + index,
                order: listBlock.blockIndex * 1000000 + index,
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
                kind: "list",
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

    function startBlockDrag(blockItem, previewItem) {
        if (!editorView || !blockModel || !blockItem || dragging)
            return false
        const row = Number(blockItem.index)
        if (row < 0 || row >= editorView.count)
            return false

        sourceKind = "block"
        sourceBlockRow = row
        sourceBlockType = Number(blockItem.blockType)
        sourceFocusAddress = editorView.activeEditor
                && Number(editorView.activeEditor.blockIndex) === row
                ? Object.assign({}, editorView.editorAddress(editorView.activeEditor)) : null
        const pointerSource = previewItem || blockItem
        const pointer = pointerSource.mapToItem(
                    editorView.contentItem, pointerSource.width / 2,
                    pointerSource.height / 2)
        const started = reorderCore.beginDrag({
            sources: [{
                item: blockItem,
                key: row,
                order: row,
                previewItem: pointerSource,
                geometryItem: blockItem,
                naturalExtent: Number(blockItem.height) + Number(editorView.spacing)
            }],
            payload: {
                kind: "block",
                sourceRow: row
            },
            pointerX: pointer.x,
            pointerY: pointer.y,
            targetByDraggedTop: true
        })
        if (!started)
            resetAdapterState()
        return started
    }

    function moveBlockDrag(dx, dy) {
        reorderCore.moveDrag(dx, dy)
    }

    function finishBlockDrag() {
        return reorderCore.finishDrag()
    }

    function blockLayoutSourceEntries() {
        if (sourceKind === "block")
            return reorderCore.sourceEntries
        if (!sourceListRemovesWholeBlock || !editorView)
            return []
        const item = editorView.itemAtIndex(sourceBlockRow)
        return item ? [{
            item: item,
            key: sourceBlockRow,
            order: sourceBlockRow,
            naturalExtent: Number(editorView.spacing)
        }] : []
    }

    function visibleBlockItems() {
        const result = []
        if (!editorView)
            return result
        for (let row = 0; row < editorView.count; ++row) {
            const item = editorView.itemAtIndex(row)
            if (item)
                result.push(item)
        }
        return result
    }

    function blockInsertionBoundaries() {
        const items = visibleBlockItems()
        if (sourceKind === "block") {
            return blockLayout.boundaries(items, function(item, after) {
                return {
                    kind: "block",
                    insertRow: item ? Number(item.index) + (after ? 1 : 0) : -1
                }
            })
        }
        const remaining = blockLayout.remainingItems(items)
        if (remaining.length === 0)
            return []

        const boundaries = []
        for (const item of remaining) {
            const row = Number(item.index)
            boundaries.push({
                kind: "block",
                position: blockLayout.logicalPosition(
                              item, false, animatedDisplacementBeforeBlock(row), true),
                owner: item,
                ownerKey: row,
                ownerOrder: row,
                afterOwner: false,
                finalIndex: blockLayout.compactOrder(item),
                insertRow: row
            })
        }
        const last = remaining[remaining.length - 1]
        const afterRow = Number(last.index) + 1
        boundaries.push({
            kind: "block",
            position: blockLayout.logicalPosition(
                          last, true, animatedDisplacementBeforeBlock(afterRow), true),
            owner: last,
            ownerKey: Number(last.index),
            ownerOrder: Number(last.index),
            afterOwner: true,
            finalIndex: blockLayout.compactOrder(last) + 1,
            insertRow: afterRow
        })
        return boundaries
    }

    function listBlockAtProbe() {
        if (!editorView)
            return null
        const probeY = reorderCore.currentPointerY
        const pointerX = reorderCore.currentPointerX
        let best = null
        let bestDistance = Number.POSITIVE_INFINITY
        for (const listBlock of listBlocks) {
            if (!listBlock || !listBlock.visible || listBlock.remainingItemCount() <= 0)
                continue
            const mapped = listBlock.mapToItem(editorView.contentItem, 0, 0)
            const blockDelegate = editorView.itemAtIndex(listBlock.blockIndex)
            // A structural target gap is rendered with a Translate on the whole
            // delegate.  Hit-testing that translated rectangle makes the target
            // list run away from a dragged standalone list whenever another block
            // sits between them.  Use the post-source-removal logical rectangle,
            // just like listInsertionBoundaries(), so list attachment remains
            // possible while the surrounding structural blocks animate.
            const structuralOffset = blockDelegate
                    && blockDelegate.reorderOffset !== undefined
                    ? Number(blockDelegate.reorderOffset) : 0
            const top = mapped.y - structuralOffset
                    - animatedDisplacementBeforeBlock(listBlock.blockIndex)
                    - sourceBlockSpacingBefore(listBlock.blockIndex)
            const bottom = top + Number(listBlock.height)
            if (probeY < top || probeY > bottom)
                continue
            const markerCenter = listBlock.markerCenterXForIndent(0)
            if (pointerX < markerCenter - Number(editorView.listMarkerWidth) / 2)
                continue
            const distance = Math.abs(probeY - (top + bottom) / 2)
            if (distance < bestDistance) {
                best = listBlock
                bestDistance = distance
            }
        }
        return best
    }

    function listOrBlockInsertionBoundaries() {
        const listBlock = listBlockAtProbe()
        return listBlock ? listInsertionBoundaries(listBlock)
                         : blockInsertionBoundaries()
    }

    function applyBlockTarget(boundary) {
        targetKind = boundary ? "block" : ""
        targetBlock = null
        targetItem = -1
        targetIndent = 0
    }

    function commitBlockDrop(boundary) {
        if (sourceKind !== "block" || sourceBlockRow < 0 || !boundary
                || boundary.kind !== "block")
            return false
        const destination = Number(boundary.finalIndex)
        if (destination < 0 || destination >= editorView.count)
            return false

        let moved = false
        editorView.runEditTransaction("move-block", function() {
            editorView.prepareForStructuralMutation()
            moved = blockModel.moveBlock(sourceBlockRow, destination)
            const selectedRow = moved ? destination : sourceBlockRow
            if (sourceBlockType === 4) {
                editorView.focusImageBlock(selectedRow)
            } else if (sourceFocusAddress) {
                const address = Object.assign({}, sourceFocusAddress)
                address.blockIndex = selectedRow
                editorView.focusEditorAddress(address)
            } else {
                editorView.focusBlock(selectedRow)
            }
        })
        return moved
    }

    function blockSourceActive(item) {
        return sourceKind === "block" && blockLayout.containsSource(item)
    }

    function blockTargetBefore(item) {
        return structuralTargetActive && reorderCore.targetBoundary
                && reorderCore.targetBoundary.ownerKey === Number(item.index)
                && !Boolean(reorderCore.targetBoundary.afterOwner)
    }

    function blockTargetAfter(item) {
        return structuralTargetActive && reorderCore.targetBoundary
                && reorderCore.targetBoundary.ownerKey === Number(item.index)
                && Boolean(reorderCore.targetBoundary.afterOwner)
    }

    function blockTranslation(item) {
        if (structuralTargetActive) {
            return blockLayout.translationByOrder(
                        item, reorderCore.targetBoundary, structuralDraggedHeight)
        }
        if (sourceListRemovesWholeBlock && item
                && Number(item.index) > sourceBlockRow) {
            return -Number(editorView.spacing)
        }
        return 0
    }

    function animatedDisplacementBeforeBlock(blockIndex) {
        let displacement = 0
        for (const listBlock of listBlocks) {
            if (listBlock && listBlock.blockIndex < blockIndex)
                displacement += listBlock.animatedLayoutDisplacement()
        }
        return displacement
    }

    function sourceBlockSpacingBefore(blockIndex) {
        return sourceListRemovesWholeBlock && editorView
                && sourceBlockRow < Number(blockIndex)
                ? Number(editorView.spacing) : 0
    }

    function listInsertionBoundaries(onlyBlock) {
        const boundaries = []
        for (const listBlock of listBlocks) {
            if (!listBlock || !listBlock.visible
                    || (onlyBlock && listBlock !== onlyBlock))
                continue
            const count = listBlock.remainingItemCount()
            for (let item = 0; item <= count; ++item) {
                const boundary = listBlock.boundaryDescriptor(item)
                if (!boundary || !boundary.owner)
                    continue
                const correction =
                        animatedDisplacementBeforeBlock(listBlock.blockIndex)
                        + sourceBlockSpacingBefore(listBlock.blockIndex)
                        + listBlock.animatedDisplacementBeforeBoundary(item)
                boundaries.push({
                    kind: "list",
                    position: listLayout.logicalPosition(
                                  boundary.owner, boundary.afterOwner,
                                  correction, false),
                    block: listBlock,
                    item: item
                })
            }
        }
        return boundaries
    }

    function applyListTarget(boundary, pointerX) {
        targetKind = boundary ? "list" : ""
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
        const maximumIndent = targetItem === 0
                ? 0 : targetBlock.remainingIndentAt(targetItem - 1) + 1
        targetIndent = hierarchyDropPolicy.depthFromMarker(
                    pointerX, markerCenter, editorView.listIndent,
                    maximumIndent, false)
    }

    function finishListDrag() {
        reorderCore.finishDrag()
    }

    function movedListFocus(fromBlock, fromItem, fromEnd) {
        const activeEditor = editorView.activeEditor
        const offset = activeEditor
                && activeEditor.blockIndex === fromBlock
                && activeEditor.listItemIndex >= fromItem
                && activeEditor.listItemIndex < fromEnd
                ? activeEditor.listItemIndex - fromItem : -1
        return {
            offset: offset,
            cursor: offset >= 0 ? activeEditor.cursorPosition : 0,
            selectionStart: offset >= 0 ? activeEditor.selectionStart : 0,
            selectionEnd: offset >= 0 ? activeEditor.selectionEnd : 0
        }
    }

    function commitListAsBlock(boundary, focus) {
        if (!boundary || boundary.kind !== "block")
            return false
        const insertRow = Number(boundary.insertRow)
        if (insertRow < 0 || insertRow > blockModel.rowCount())
            return false

        let finalRow = -1
        editorView.runEditTransaction("move-list-block", function() {
            editorView.prepareForStructuralMutation()
            finalRow = blockModel.moveListRangeToBlock(
                        sourceBlockRow, sourceItem, sourceEnd - 1, insertRow)
            if (finalRow < 0)
                return
            editorView.focusEditorAddress({
                blockIndex: finalRow,
                listItemIndex: Math.max(0, focus.offset),
                tableCellIndex: -1,
                field: "listItem",
                cursorPosition: focus.cursor,
                selectionStart: focus.selectionStart,
                selectionEnd: focus.selectionEnd
            })
        })
        return finalRow >= 0
    }

    function commitListDrop(boundary, core) {
        if (!sourceBlock || sourceBlockRow < 0)
            return false

        const fromBlock = sourceBlockRow
        const fromItem = sourceItem
        const fromEnd = sourceEnd
        const focus = movedListFocus(fromBlock, fromItem, fromEnd)
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
        if (!boundary)
            return false
        if (boundary.kind === "block")
            return commitListAsBlock(boundary, focus)
        if (boundary.kind !== "list" || !boundary.block)
            return false

        const toBlock = boundary.block.blockIndex
        const toItem = boundary.item
        const toIndent = targetIndent
        const removesSourceBlock = fromBlock !== toBlock
                && sourceRemovesWholeBlock
        let moved = false
        editorView.runEditTransaction("move-list-item", function() {
            editorView.prepareForStructuralMutation()
            if (!blockModel.moveListRange(fromBlock, fromItem, fromEnd - 1,
                                          toBlock, toItem, toIndent)) {
                return
            }
            moved = true
            editorView.focusEditorAddress({
                blockIndex: removesSourceBlock && fromBlock < toBlock
                            ? toBlock - 1 : toBlock,
                listItemIndex: toItem + Math.max(0, focus.offset),
                tableCellIndex: -1,
                field: "listItem",
                cursorPosition: focus.cursor,
                selectionStart: focus.selectionStart,
                selectionEnd: focus.selectionEnd
            })
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
        sourceKind = ""
        sourceBlock = null
        sourceItem = -1
        sourceEnd = -1
        sourceRows = []
        sourceRemovesWholeBlock = false
        sourceBlockRow = -1
        sourceBlockType = -1
        sourceFocusAddress = null
        targetKind = ""
        targetBlock = null
        targetItem = -1
        targetIndent = 0
    }
}
