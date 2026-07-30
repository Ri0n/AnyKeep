pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    id: range

    property var countProvider: null
    property var depthProvider: null

    function count() {
        return typeof countProvider === "function" ? Number(countProvider()) : 0
    }

    function depthAt(index) {
        return typeof depthProvider === "function" ? Number(depthProvider(index)) : 0
    }

    // Returns the exclusive end of the subtree rooted at index.
    function subtreeEnd(index) {
        const total = count()
        if (index < 0 || index >= total)
            return index
        const depth = depthAt(index)
        let end = index + 1
        while (end < total && depthAt(end) > depth)
            ++end
        return end
    }

    // Returns the contiguous range belonging to one indentation level around
    // index. Task-list level handles and grouped-note subtree drags share this
    // exact depth rule.
    function levelRange(index) {
        const total = count()
        if (index < 0 || index >= total)
            return { start: -1, end: -1, level: -1 }

        const level = depthAt(index)
        let start = index
        while (start > 0 && depthAt(start - 1) >= level)
            --start

        let end = index + 1
        while (end < total && depthAt(end) >= level)
            ++end
        return { start: start, end: end, level: level }
    }
}
