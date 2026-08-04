import QtQuick
import "../reorder" as Reorder

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
    readonly property bool wholeListBlockDrag: sourceKind === "list"
                                                && sourceRemovesWholeBlock
    // A source range ending at the bottom of its list has no trailing item
    // spacing. Every insertion into a non-empty destination list does have
    // one adjacency, so include that spacing in the animated placeholder.
    readonly property real listDraggedHeight: sourceKind === "list"
                                               ? (sourceRemovesWholeBlock
                                                  ? draggedHeight
                                                  : sourceListRowsExtent()
                                                    + (sourceBlock && sourceRows.length > 0
                                                       && Number(sourceRows[sourceRows.length - 1].trailingSpace) <= 0
                                                       ? Number(sourceBlock.itemSpacing) : 0))
                                               : draggedHeight
    // A partial list range remains the same flattened sequence of rows while
    // crossing list and ordinary-block boundaries. Changing its extent from
    // list spacing to document spacing mid-drag retargets every displaced item
    // and produces a second animation. The model restores canonical outer
    // block spacing when the range is committed as a standalone list.
    readonly property real structuralDraggedHeight: sourceKind === "list"
                                                    && !wholeListBlockDrag
                                                    ? listDraggedHeight : draggedHeight
    readonly property bool committingDrop: reorderCore.committingDrop
    readonly property bool sourceListRemovesWholeBlock: sourceKind === "list"
                                                        && sourceRemovesWholeBlock
    readonly property bool structuralTargetActive: dragging && targetKind === "block"
    readonly property bool wholeListTargetActive: dragging && wholeListBlockDrag
                                                  && targetKind === "list"
    // Keep Behaviors enabled for the entire structural drag. Enabling them only
    // when targetKind changes makes the first block-to-list displacement land
    // in the same binding update as `enabled`, which Qt applies without animation.
    readonly property bool blockAnimationActive: dragging

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
        sourceEntries: controller.sourceKind === "list" && !controller.wholeListBlockDrag
                       ? reorderCore.sourceEntries : []
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
                    : controller.listDocumentBoundaries()
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

        let dragSources = sources
        if (sourceRemovesWholeBlock) {
            const blockItem = editorView.itemAtIndex(sourceBlockRow)
            if (!blockItem) {
                resetAdapterState()
                return
            }
            dragSources = [{
                item: blockItem,
                key: sourceBlockRow,
                order: sourceBlockRow,
                previewItem: listBlock,
                geometryItem: blockItem,
                naturalExtent: Number(blockItem.height) + Number(editorView.spacing)
            }]
        }

        // A level handle must keep the height captured before its source rows
        // collapse and move with the same overlay as their contents. Leaving
        // the live handle inside the first source row makes its lower edge move
        // upward while the pointer moves in the opposite direction.
        const previewHandle = pointerItem.fullHeight === true ? pointerItem : null
        let previewItems = undefined
        if (sourceRemovesWholeBlock || previewHandle) {
            previewItems = []
            for (const sourceRow of rows) {
                previewItems.push({
                    sourceItem: sourceRow.dragContent,
                    sourceX: 0,
                    sourceY: 0,
                    width: sourceRow.dragContent.width,
                    height: sourceRow.dragContent.height
                })
            }
            if (previewHandle) {
                previewItems.push({
                    sourceItem: previewHandle,
                    sourceX: 0,
                    sourceY: 0,
                    width: previewHandle.width,
                    height: previewHandle.height
                })
            }
        }

        const markerCenter = pointerItem.mapToItem(
            editorView.contentItem, pointerItem.width / 2, pointerItem.height / 2)
        const started = reorderCore.beginDrag({
            sources: dragSources,
            previewItems: previewItems,
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
        if (sourceKind === "block" || wholeListBlockDrag)
            return reorderCore.sourceEntries
        return []
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
        if (sourceKind === "block" || wholeListBlockDrag) {
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

    function listDocumentBoundaries() {
        // Project every destination list row into the same structural sequence
        // as ordinary document blocks. The model remains grouped into lists;
        // boundary metadata keeps enough information to rebuild that grouping
        // on commit. Never use the centre of a multi-row drag to choose between
        // list and block layouts: its leading row must cross every boundary in
        // exactly the same way as an ordinary structural item.
        const boundaries = []
        for (const boundary of blockInsertionBoundaries()) {
            let listOwner = false
            for (const listBlock of listBlocks) {
                if (listBlock && Number(listBlock.blockIndex) === Number(boundary.ownerKey)) {
                    listOwner = true
                    break
                }
            }
            if (!listOwner)
                boundaries.push(boundary)
        }
        return boundaries.concat(listInsertionBoundaries())
    }

    function applyBlockTarget(boundary) {
        targetKind = boundary ? "block" : ""
        targetBlock = null
        targetItem = -1
        targetIndent = 0
    }

    function commitBlockDrop(boundary, core) {
        if (sourceKind !== "block" || sourceBlockRow < 0)
            return false

        const viewportX = core.currentPointerX - editorView.contentX
        const viewportY = core.currentPointerY - editorView.contentY
        const droppedOutside = !editorView.touchMode
                && (viewportX < 0 || viewportY < 0
                    || viewportX > editorView.width || viewportY > editorView.height)
        if (droppedOutside) {
            editorView.runEditTransaction("delete-block", function() {
                editorView.prepareForStructuralMutation()
                const oldCount = editorView.count
                blockModel.removeBlock(sourceBlockRow)
                if (oldCount === 1) {
                    blockModel.appendTextBlock()
                    editorView.focusBlock(0)
                    return
                }
                const targetRow = Math.min(sourceBlockRow, blockModel.rowCount() - 1)
                const backwards = targetRow < sourceBlockRow
                if (blockModel.blockTypeAt(targetRow) === 4)
                    editorView.focusImageBlock(targetRow)
                else
                    editorView.focusBlock(targetRow, backwards)
            })
            return true
        }

        if (!boundary || boundary.kind !== "block")
            return false
        const destination = Number(boundary.finalIndex)
        if (destination < 0 || destination >= editorView.count)
            return false

        let moved = false
        let resolvedRow = sourceBlockRow
        editorView.runEditTransaction("move-block", function() {
            editorView.prepareForStructuralMutation()
            resolvedRow = blockModel.moveBlockResolved(sourceBlockRow, destination)
            moved = resolvedRow >= 0
            const selectedRow = moved ? resolvedRow : sourceBlockRow
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
        return (sourceKind === "block" || wholeListBlockDrag)
                && blockLayout.containsSource(item)
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
        if (wholeListTargetActive) {
            // A whole list attached inside another list uses row transforms,
            // not an expanded destination-list height. Translate only blocks
            // between source and destination into the compact post-removal
            // geometry; blocks below both endpoints must remain stationary.
            const row = Number(item.index)
            const destinationRow = targetBlock ? Number(targetBlock.blockIndex) : -1
            if (sourceBlockRow < destinationRow)
                return row > sourceBlockRow && row <= destinationRow
                        ? -structuralDraggedHeight : 0
            if (destinationRow < sourceBlockRow)
                return row > destinationRow && row < sourceBlockRow
                        ? structuralDraggedHeight : 0
            return 0
        }
        if (!structuralTargetActive)
            return 0
        return blockLayout.translationByOrder(
                    item, reorderCore.targetBoundary, structuralDraggedHeight)
    }

    function animatedDisplacementBeforeBlock(blockIndex) {
        let displacement = 0
        for (const listBlock of listBlocks) {
            if (listBlock && listBlock.blockIndex < blockIndex)
                displacement += listBlock.animatedLayoutDisplacement()
        }
        return displacement
    }

    function sourceStructuralExtentBeforeBlock(blockIndex) {
        if (!editorView || sourceBlockRow >= Number(blockIndex))
            return 0
        if (wholeListBlockDrag)
            return structuralDraggedHeight
        return sourceListRemovesWholeBlock ? Number(editorView.spacing) : 0
    }

    function structuralOffsetForBlock(blockIndex) {
        if (!editorView)
            return 0
        const blockDelegate = editorView.itemAtIndex(Number(blockIndex))
        return blockDelegate && blockDelegate.reorderOffset !== undefined
                ? Number(blockDelegate.reorderOffset) : 0
    }

    function sourceListRowsExtent() {
        let extent = 0
        for (const row of sourceRows || []) {
            if (row)
                extent += Number(row.naturalHeight)
        }
        return extent
    }

    function listInsertionBoundaries(onlyBlock) {
        const boundaries = []
        for (const listBlock of listBlocks) {
            if (!listBlock || !listBlock.visible
                    || (wholeListBlockDrag && listBlock === sourceBlock)
                    || (onlyBlock && listBlock !== onlyBlock))
                continue
            const count = listBlock.remainingItemCount()
            for (let item = 0; item <= count; ++item) {
                const boundary = listBlock.boundaryDescriptor(item)
                if (!boundary || !boundary.owner)
                    continue
                let position = 0
                if (wholeListBlockDrag) {
                    // Derive structural row boundaries from immutable natural
                    // extents. Reading an animated Column row through mapToItem()
                    // races its next polish pass and can briefly reverse the
                    // selected item during a very slow drag.
                    const mapped = listBlock.mapToItem(editorView.contentItem, 0, 0)
                    position = mapped.y
                            - structuralOffsetForBlock(listBlock.blockIndex)
                            - animatedDisplacementBeforeBlock(listBlock.blockIndex)
                            - sourceStructuralExtentBeforeBlock(listBlock.blockIndex)
                            + listBlock.naturalBoundaryOffset(item)
                } else {
                    const correction = structuralOffsetForBlock(listBlock.blockIndex)
                            + animatedDisplacementBeforeBlock(listBlock.blockIndex)
                            + sourceStructuralExtentBeforeBlock(listBlock.blockIndex)
                            + listBlock.animatedDisplacementBeforeBoundary(item)
                    position = listLayout.logicalPosition(
                                boundary.owner, boundary.afterOwner,
                                correction, false)
                }
                boundaries.push({
                    kind: "list",
                    position: position,
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
        let moved = false
        editorView.runEditTransaction("move-list-item", function() {
            editorView.prepareForStructuralMutation()
            const resolvedTargetRow = blockModel.moveListRangeResolved(
                        fromBlock, fromItem, fromEnd - 1,
                        toBlock, toItem, toIndent)
            if (resolvedTargetRow < 0) {
                return
            }
            moved = true
            editorView.focusEditorAddress({
                blockIndex: resolvedTargetRow,
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
