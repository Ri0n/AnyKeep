pragma ComponentBehavior: Bound

import QtQuick

/*
 * Adapter shared by fixed-height, flat ListView reorder implementations.
 *
 * The view supplies delegate identity, payload and the domain commit callback.
 * Geometry compression, target boundaries, preview lifetime and model-reset
 * cancellation stay here so every flat list gets the same animation.
 */
Item {
    id: root

    property Item geometryItem: parent
    property var listView: null
    property var model: null
    property var keyProvider: function(item) {
        return item && item.itemId !== undefined ? String(item.itemId) : ""
    }
    property var extentProvider: function(item) {
        return item ? Number(item.height) : 0
    }
    property var payloadProvider: function(item) {
        return {
            sourceRow: item ? Number(item.index) : -1,
            itemId: item && item.itemId !== undefined ? String(item.itemId) : ""
        }
    }
    property var commitHandler: null
    property bool compensateForScroll: false
    property string previewObjectName: "flatListDragPreview"
    property string previewObjectNamePrefix: "flatListDragPreviewItem-"
    property bool previewHideSources: false
    property bool previewLive: false

    readonly property var sourcePayload: genericController.sourcePayload
    readonly property var activePayload: genericController.sourcePayload
    readonly property var sourceEntries: genericController.sourceEntries
    readonly property var targetBoundary: genericController.targetBoundary
    readonly property bool dragging: genericController.dragging
    readonly property bool committingDrop: genericController.committingDrop
    readonly property real draggedExtent: genericController.draggedExtent
    readonly property int previewCount: genericController.previewCount

    enabled: false

    function keyOf(item) {
        return typeof keyProvider === "function" ? keyProvider(item) : item
    }

    function extentOf(item) {
        return typeof extentProvider === "function"
                ? Number(extentProvider(item)) : Number(item ? item.height : 0)
    }

    function itemAt(row) {
        return listView && typeof listView.itemAtIndex === "function"
                ? listView.itemAtIndex(row) : null
    }

    function rowItems() {
        const result = []
        if (!listView)
            return result
        for (let row = 0; row < listView.count; ++row) {
            const item = itemAt(row)
            if (item)
                result.push(item)
        }
        return result
    }

    function boundaries() {
        if (!activePayload)
            return []
        const rows = rowItems()
        if (linearLayout.remainingItems(rows).length === 0) {
            return [{
                position: genericController.startDraggedTopY,
                owner: null,
                finalRow: 0,
                finalIndex: 0,
                afterOwner: false
            }]
        }
        return linearLayout.boundaries(rows, function(item, after, index) {
            return { finalRow: index }
        })
    }

    function rowTranslation(item) {
        return linearLayout.translationByOrder(
                    item, genericController.targetBoundary,
                    genericController.draggedExtent)
    }

    function sourceActive(item) {
        return linearLayout.containsSource(item)
    }

    function targetBefore(item) {
        return Boolean(targetBoundary
                       && targetBoundary.owner === item
                       && !targetBoundary.afterOwner)
    }

    function targetAfter(item) {
        return Boolean(targetBoundary
                       && targetBoundary.owner === item
                       && targetBoundary.afterOwner)
    }

    function beginDrag(item) {
        if (!item || !model || dragging)
            return false
        let payload = typeof payloadProvider === "function"
                ? payloadProvider(item) : null
        if (!payload)
            payload = {}
        if (payload.sourceRow === undefined)
            payload.sourceRow = Number(item.index)

        const extent = extentOf(item)
        const started = genericController.beginDrag({
            sources: [{
                item: item,
                key: keyOf(item),
                order: Number(item.index),
                previewItem: item,
                geometryItem: item,
                naturalExtent: extent,
                previewWidth: item.width,
                previewHeight: extent
            }],
            payload: payload,
            pointerItem: item,
            pointerLocalX: item.width / 2,
            pointerLocalY: extent / 2,
            targetByDraggedTop: true
        })
        item.internalDragActive = started
        return started
    }

    function moveDrag(item, dx, dy) {
        if (item && item.internalDragActive)
            genericController.moveDrag(dx, dy)
    }

    function finishDrag(item) {
        if (!item || !item.internalDragActive)
            return false
        item.internalDragActive = false
        return genericController.finishDrag()
    }

    function clearSourceFlags() {
        for (const entry of genericController.sourceEntries || [])
            if (entry && entry.item)
                entry.item.internalDragActive = false
    }

    function cancelDrag() {
        clearSourceFlags()
        genericController.cancelDrag()
    }

    LinearReorderLayout {
        id: linearLayout

        geometryItem: root.geometryItem
        sourceEntries: genericController.sourceEntries
        keyProvider: function(item) { return root.keyOf(item) }
        orderProvider: function(item) { return Number(item.index) }
        extentProvider: function(item) { return root.extentOf(item) }
    }

    GenericReorderController {
        id: genericController

        anchors.fill: parent
        geometryItem: root.geometryItem
        scrollItem: root.listView
        compensateForScroll: root.compensateForScroll
        previewObjectName: root.previewObjectName
        previewObjectNamePrefix: root.previewObjectNamePrefix
        previewHideSources: root.previewHideSources
        previewLive: root.previewLive
        boundaryProvider: function() { return root.boundaries() }
        commitHandler: function(payload, boundary, controller) {
            if (!payload || !boundary || !root.model
                    || typeof root.commitHandler !== "function") {
                return false
            }
            const destination = Number(boundary.finalRow)
            if (destination === Number(payload.sourceRow))
                return false
            return Boolean(root.commitHandler(payload, destination,
                                              boundary, controller))
        }
    }

    Connections {
        target: root.model
        enabled: root.dragging
        ignoreUnknownSignals: true

        function onModelAboutToBeReset() {
            if (!root.committingDrop)
                root.cancelDrag()
        }

        function onRowsAboutToBeRemoved() {
            if (!root.committingDrop)
                root.cancelDrag()
        }
    }
}
