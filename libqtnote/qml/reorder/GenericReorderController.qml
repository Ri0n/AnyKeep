pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: controller

    property Item geometryItem: parent
    property var scrollItem: null
    property bool compensateForScroll: true
    property var boundaryProvider: null
    property var targetChangedHandler: null
    property var commitHandler: null
    property var resetHandler: null
    property string previewObjectName: ""
    property string previewObjectNamePrefix: "reorderPreview-"
    property bool previewHideSources: true
    property bool previewLive: true
    property bool previewCompact: false

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
    property real startDraggedTopY: 0
    property bool targetByDraggedTop: false
    property real translationX: 0
    property real translationY: 0
    property real draggedExtent: 0
    property real currentPointerX: 0
    property real currentPointerY: 0
    property real targetProbeY: 0
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
        let draggedTop = Number.POSITIVE_INFINITY
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
                    : Number(item.naturalHeight !== undefined
                             ? item.naturalHeight : item.height)
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
            draggedTop = Math.min(draggedTop, origin.y)
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

        dragPreview.capture(previewItems)
        sourceEntries = entries
        sourcePayload = configuration.payload
        draggedExtent = extent
        startPointerX = pointer.x
        startPointerY = pointer.y
        startDraggedTopY = Number.isFinite(draggedTop) ? draggedTop : pointer.y
        targetByDraggedTop = configuration.targetByDraggedTop !== undefined
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
        // Boundaries describe the layout after removing every source row. Comparing
        // the preview top with them advances the target only after half of the next
        // row is covered, for both a single row and a multi-row block.
        targetProbeY = (targetByDraggedTop ? startDraggedTopY : startPointerY)
                       + translationY + scrollDeltaY
        const boundaries = typeof boundaryProvider === "function"
                ? boundaryProvider(controller) : []
        targetBoundary = reorderGeometry.nearestBoundary(targetProbeY, boundaries)
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
            if (typeof commitHandler === "function")
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
        startDraggedTopY = 0
        targetByDraggedTop = false
        translationX = 0
        translationY = 0
        draggedExtent = 0
        currentPointerX = 0
        currentPointerY = 0
        targetProbeY = 0
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
        translationX: controller.translationX
        translationY: controller.translationY
    }
}
