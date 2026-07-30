pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    id: layout

    property Item geometryItem: null
    property var sourceEntries: []
    property var keyProvider: function(item) { return item }
    property var orderProvider: function(item) {
        if (!item)
            return -1
        if (item.row !== undefined)
            return Number(item.row)
        if (item.index !== undefined)
            return Number(item.index)
        return -1
    }
    property var extentProvider: function(item) {
        if (!item)
            return 0
        if (item.naturalHeight !== undefined)
            return Number(item.naturalHeight)
        return Number(item.height)
    }
    property var offsetProvider: function(item) {
        return item && item.reorderOffset !== undefined
                ? Number(item.reorderOffset) : 0
    }

    function orderOf(item) {
        return typeof orderProvider === "function"
                ? Number(orderProvider(item)) : -1
    }

    function extentOf(item) {
        return typeof extentProvider === "function"
                ? Number(extentProvider(item)) : 0
    }

    function keyOf(item) {
        return typeof keyProvider === "function" ? keyProvider(item) : item
    }

    function containsSource(item) {
        const itemKey = keyOf(item)
        for (const entry of sourceEntries || []) {
            if (!entry)
                continue
            // Reusable views can repurpose the same delegate object for a
            // different model row while a drag is active. Once a stable key
            // was snapshotted, identity must follow that key, not the QObject.
            if (entry.key !== undefined
                    ? entry.key === itemKey : entry.item === item)
                return true
        }
        return false
    }

    function sourceExtentBefore(item) {
        const itemOrder = orderOf(item)
        let extent = 0
        for (const entry of sourceEntries || []) {
            if (!entry)
                continue
            const sourceOrder = entry.order !== undefined
                    ? Number(entry.order) : orderOf(entry.item)
            if (sourceOrder >= itemOrder)
                continue
            extent += entry.naturalExtent !== undefined
                    ? Number(entry.naturalExtent) : extentOf(entry.item)
        }
        return extent
    }

    function sourceCountBefore(item) {
        const itemOrder = orderOf(item)
        let count = 0
        for (const entry of sourceEntries || []) {
            if (!entry)
                continue
            const sourceOrder = entry.order !== undefined
                    ? Number(entry.order) : orderOf(entry.item)
            if (sourceOrder < itemOrder)
                ++count
        }
        return count
    }

    // Dense model rows use an absolute order, while a reusable view exposes
    // only the delegates currently around its viewport.  Drop boundaries
    // must stay in that absolute order as well; a viewport-local index would
    // make rows above a scrolled source animate toward an unrelated gap.
    function compactOrder(item) {
        return orderOf(item) - sourceCountBefore(item)
    }

    // Returns a boundary in the coordinate system used by GenericReorderController.
    // positionCorrection removes animated flow-layout space already reflected in
    // mapToItem(). Translated lists normally leave it at zero.
    function logicalPosition(item, afterItem, positionCorrection, subtractSources) {
        if (!item || !geometryItem)
            return 0
        const localY = afterItem ? extentOf(item) : 0
        const mapped = item.mapToItem(geometryItem, 0, localY)
        const correction = positionCorrection === undefined
                ? 0 : Number(positionCorrection)
        const removedSourceExtent = subtractSources === false
                ? 0 : sourceExtentBefore(item)
        return mapped.y - Number(offsetProvider(item))
                - removedSourceExtent - correction
    }

    function remainingItems(items) {
        const result = []
        for (const item of items || [])
            if (item && !containsSource(item))
                result.push(item)
        return result
    }

    function remainingIndex(item, items) {
        const remaining = remainingItems(items)
        for (let index = 0; index < remaining.length; ++index)
            if (remaining[index] === item)
                return index
        return -1
    }

    function boundaryAt(item, afterItem, finalIndex, payload,
                        positionCorrection, subtractSources) {
        return Object.assign({
            position: logicalPosition(item, afterItem, positionCorrection,
                                      subtractSources),
            owner: item,
            ownerKey: keyOf(item),
            ownerOrder: orderOf(item),
            afterOwner: Boolean(afterItem),
            finalIndex: Number(finalIndex)
        }, payload || {})
    }

    function boundary(item, afterItem, items, payload, positionCorrection,
                      subtractSources) {
        const itemIndex = remainingIndex(item, items)
        if (itemIndex < 0)
            return null
        return boundaryAt(item, afterItem, itemIndex + (afterItem ? 1 : 0),
                          payload, positionCorrection, subtractSources)
    }

    function boundaryByOrder(item, afterItem, payload, positionCorrection,
                             subtractSources) {
        if (!item || containsSource(item))
            return null
        const itemIndex = orderOf(item) - sourceCountBefore(item)
        return boundaryAt(item, afterItem, itemIndex + (afterItem ? 1 : 0),
                          payload, positionCorrection, subtractSources)
    }

    // Produces "before every remaining item, after the final item" boundaries.
    // payloadProvider may attach domain-specific fields to every boundary.
    function boundaries(items, payloadProvider) {
        const remaining = remainingItems(items)
        if (remaining.length === 0)
            return []

        const result = []
        for (let index = 0; index < remaining.length; ++index) {
            const item = remaining[index]
            let boundary = layout.boundaryAt(item, false, compactOrder(item),
                                             null, 0, true)
            if (typeof payloadProvider === "function")
                boundary = Object.assign(boundary,
                                         payloadProvider(item, false, index) || {})
            result.push(boundary)
        }

        const last = remaining[remaining.length - 1]
        let boundary = layout.boundaryAt(last, true, compactOrder(last) + 1,
                                         null, 0, true)
        if (typeof payloadProvider === "function")
            boundary = Object.assign(boundary,
                                     payloadProvider(last, true, remaining.length) || {})
        result.push(boundary)
        return result
    }

    // Produces a leading boundary followed by a boundary after every
    // remaining item. This is the natural representation for hierarchical
    // drops: once the animation opens a gap, the item immediately above that
    // gap determines its group or prospective parent.
    function trailingBoundaries(items, payloadProvider, includeLeading) {
        const remaining = remainingItems(items)
        if (remaining.length === 0)
            return []

        const result = []
        if (includeLeading !== false) {
            const first = remaining[0]
            let leading = layout.boundaryAt(first, false, compactOrder(first),
                                            null, 0, true)
            if (typeof payloadProvider === "function")
                leading = Object.assign(leading,
                                        payloadProvider(null, false, 0) || {})
            result.push(leading)
        }
        for (let index = 0; index < remaining.length; ++index) {
            const item = remaining[index]
            let boundary = layout.boundaryAt(item, true,
                                             compactOrder(item) + 1,
                                             null, 0, true)
            if (typeof payloadProvider === "function")
                boundary = Object.assign(
                            boundary,
                            payloadProvider(item, true, index + 1) || {})
            result.push(boundary)
        }
        return result
    }

    // Translation for views which keep fixed row geometry and animate with a
    // Translate transform. Flow layouts bind ReorderDisplacement.layoutExtent.
    function translation(item, items, boundary, draggedExtent) {
        if (!item || containsSource(item) || !boundary)
            return 0
        const index = remainingIndex(item, items)
        if (index < 0)
            return 0
        const insertionIndex = Number(boundary.finalIndex)
        const insertionExtent = index >= insertionIndex ? Number(draggedExtent) : 0
        return insertionExtent - sourceExtentBefore(item)
    }

    // Faster and resilient to delegate recycling when order values are dense
    // integer rows. Boundaries retain their finalIndex snapshot from drag time.
    function translationByOrder(item, boundary, draggedExtent) {
        if (!item || containsSource(item) || !boundary)
            return 0
        const index = orderOf(item) - sourceCountBefore(item)
        const insertionIndex = Number(boundary.finalIndex)
        const insertionExtent = index >= insertionIndex ? Number(draggedExtent) : 0
        return insertionExtent - sourceExtentBefore(item)
    }
}
