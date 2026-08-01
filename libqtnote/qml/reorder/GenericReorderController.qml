pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: controller

    property Item geometryItem: parent
    property int orientation: Qt.Vertical
    property var scrollItem: null
    property bool compensateForScroll: true
    property var boundaryProvider: null
    property var targetChangedHandler: null
    property var commitHandler: null
    property var outsideDropProvider: null
    property var outsideDropHandler: null
    property var resetHandler: null
    property string previewObjectName: ""
    property string previewObjectNamePrefix: "reorderPreview-"
    property bool previewHideSources: true
    property bool previewLive: true
    property bool previewCompact: false
    property bool previewLockCrossAxis: false

    readonly property bool dragging: active
    property bool active: false
    property bool committingDrop: false
    property var sourcePayload: null
    property var sourceEntries: []
    property var targetBoundary: null
    property real startPointerX: 0
    property real startPointerY: 0
    property real startScrollX: 0
    property real startScrollY: 0
    property real startDraggedLeading: 0
    property bool targetByDraggedLeading: false
    readonly property real startDraggedTopY: startDraggedLeading
    readonly property bool targetByDraggedTop: targetByDraggedLeading
    property real translationX: 0
    property real translationY: 0
    property real draggedExtent: 0
    property real currentPointerX: 0
    property real currentPointerY: 0
    property real targetProbe: 0
    readonly property real targetProbeY: targetProbe
    readonly property int previewCount: dragPreview.previewCount

    visible: dragging
    enabled: false
    z: 100000

    ReorderGeometry {
        id: reorderGeometry
    }

    function scrollX() {
        return scrollItem && scrollItem.contentX !== undefined
                ? Number(scrollItem.contentX) : 0
    }

    function scrollY() {
        return scrollItem && scrollItem.contentY !== undefined
                ? Number(scrollItem.contentY) : 0
    }

    function containsSource(item) {
        for (const entry of sourceEntries)
            if (entry.item === item)
                return true
        return false
    }

    function beginDrag(configuration) {
        if (dragging || !configuration || !geometryItem)
            return false

        const requestedSources = configuration.sources || []
        if (requestedSources.length === 0)
            return false

        const entries = []
        const previewItems = []
        let extent = 0
        let draggedLeading = Number.POSITIVE_INFINITY
        for (const requested of requestedSources) {
            const descriptor = requested && requested.item !== undefined
                    ? requested : { item: requested }
            const item = descriptor.item
            if (!item)
                continue
            const previewItem = descriptor.previewItem || item
            const geometrySource = descriptor.geometryItem || previewItem
            const geometryX = descriptor.geometryX !== undefined
                    ? Number(descriptor.geometryX) : 0
            const geometryY = descriptor.geometryY !== undefined
                    ? Number(descriptor.geometryY) : 0
            const naturalExtent = descriptor.naturalExtent !== undefined
                    ? Number(descriptor.naturalExtent)
                    : controller.orientation === Qt.Horizontal
                      ? Number(item.naturalWidth !== undefined ? item.naturalWidth : item.width)
                      : Number(item.naturalHeight !== undefined ? item.naturalHeight : item.height)
            const origin = geometrySource.mapToItem(geometryItem, geometryX, geometryY)
            entries.push({
                item: item,
                key: descriptor.key,
                order: descriptor.order,
                naturalExtent: naturalExtent
            })
            previewItems.push({
                sourceItem: previewItem,
                sourceX: descriptor.previewX !== undefined
                         ? Number(descriptor.previewX) : 0,
                sourceY: descriptor.previewY !== undefined
                         ? Number(descriptor.previewY) : 0,
                width: descriptor.previewWidth !== undefined
                       ? Number(descriptor.previewWidth) : previewItem.width,
                height: descriptor.previewHeight !== undefined
                        ? Number(descriptor.previewHeight) : previewItem.height
            })
            extent += naturalExtent
            draggedLeading = Math.min(draggedLeading,
                                      controller.orientation === Qt.Horizontal ? origin.x : origin.y)
        }
        if (entries.length === 0)
            return false

        let pointer
        if (configuration.pointerX !== undefined
                && configuration.pointerY !== undefined) {
            pointer = Qt.point(Number(configuration.pointerX),
                               Number(configuration.pointerY))
        } else {
            const pointerItem = configuration.pointerItem
            if (!pointerItem)
                return false
            const localX = configuration.pointerLocalX !== undefined
                    ? Number(configuration.pointerLocalX) : pointerItem.width / 2
            const localY = configuration.pointerLocalY !== undefined
                    ? Number(configuration.pointerLocalY) : pointerItem.height / 2
            pointer = pointerItem.mapToItem(geometryItem, localX, localY)
        }

        // A logical source can be represented by several visual fragments.
        // Table-column reorder uses one header cell for compressed horizontal
        // geometry, but previews and hides every cell in that column.
        const configuredPreviewItems = configuration.previewItems
        dragPreview.capture(configuredPreviewItems === undefined
                            ? previewItems : configuredPreviewItems)
        sourceEntries = entries
        sourcePayload = configuration.payload
        draggedExtent = extent
        startPointerX = pointer.x
        startPointerY = pointer.y
        startDraggedLeading = Number.isFinite(draggedLeading)
                ? draggedLeading
                : (controller.orientation === Qt.Horizontal ? pointer.x : pointer.y)
        targetByDraggedLeading = configuration.targetByDraggedLeading !== undefined
                ? Boolean(configuration.targetByDraggedLeading)
                : configuration.targetByDraggedTop !== undefined
                  ? Boolean(configuration.targetByDraggedTop) : true
        startScrollX = scrollX()
        startScrollY = scrollY()
        translationX = 0
        translationY = 0
        active = true
        updateTarget()
        return true
    }

    function moveDrag(dx, dy) {
        if (!dragging)
            return
        translationX = Number(dx)
        translationY = Number(dy)
        updateTarget()
    }

    function updateTarget() {
        if (!dragging)
            return
        const scrollDeltaX = compensateForScroll ? scrollX() - startScrollX : 0
        const scrollDeltaY = compensateForScroll ? scrollY() - startScrollY : 0
        currentPointerX = startPointerX + translationX + scrollDeltaX
        currentPointerY = startPointerY + translationY + scrollDeltaY
        // Boundaries describe the layout after removing every source item. Compare
        // along the configured main axis so the same controller can drive rows,
        // tags and table columns.
        const startPointer = controller.orientation === Qt.Horizontal ? startPointerX : startPointerY
        const translation = controller.orientation === Qt.Horizontal ? translationX : translationY
        const scrollDelta = controller.orientation === Qt.Horizontal ? scrollDeltaX : scrollDeltaY
        targetProbe = (targetByDraggedLeading ? startDraggedLeading : startPointer)
                      + translation + scrollDelta
        const boundaries = typeof boundaryProvider === "function"
                ? boundaryProvider(controller) : []
        targetBoundary = reorderGeometry.nearestBoundary(targetProbe, boundaries)
        if (typeof targetChangedHandler === "function")
            targetChangedHandler(targetBoundary, currentPointerX,
                                 currentPointerY, controller)
    }

    function finishDrag() {
        if (!dragging)
            return false
        let result = false
        committingDrop = true
        try {
            const outside = typeof outsideDropProvider === "function"
                    && Boolean(outsideDropProvider(controller))
            if (outside && typeof outsideDropHandler === "function")
                result = Boolean(outsideDropHandler(sourcePayload, controller))
            else if (typeof commitHandler === "function")
                result = Boolean(commitHandler(sourcePayload, targetBoundary, controller))
        } finally {
            try {
                resetState()
            } finally {
                committingDrop = false
            }
        }
        return result
    }

    function cancelDrag() {
        if (!dragging)
            return
        committingDrop = true
        try {
            resetState()
        } finally {
            committingDrop = false
        }
    }

    function resetState() {
        active = false
        sourcePayload = null
        sourceEntries = []
        targetBoundary = null
        startPointerX = 0
        startPointerY = 0
        startScrollX = 0
        startScrollY = 0
        startDraggedLeading = 0
        targetByDraggedLeading = false
        translationX = 0
        translationY = 0
        draggedExtent = 0
        currentPointerX = 0
        currentPointerY = 0
        targetProbe = 0
        dragPreview.clear()
        if (typeof resetHandler === "function")
            resetHandler(controller)
    }

    Connections {
        target: controller.scrollItem
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
        objectName: controller.previewObjectName
        objectNamePrefix: controller.previewObjectNamePrefix
        hideSources: controller.previewHideSources
        liveSources: controller.previewLive
        compactEntries: controller.previewCompact
        translationX: controller.previewLockCrossAxis
                      && controller.orientation === Qt.Vertical
                      ? 0 : controller.translationX
        translationY: controller.previewLockCrossAxis
                      && controller.orientation === Qt.Horizontal
                      ? 0 : controller.translationY
    }
}
