pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    id: policy

    function clampedDepth(requestedDepth, maximumDepth, rootOnly) {
        if (rootOnly)
            return 0
        return Math.max(0, Math.min(Number(maximumDepth),
                                   Math.round(Number(requestedDepth))))
    }

    // Task-list handles already live on the marker of their current level, so
    // their horizontal position can be converted directly into a depth.
    function depthFromMarker(pointerX, depthZeroMarkerX, indentWidth,
                             maximumDepth, rootOnly) {
        const step = Math.max(1, Number(indentWidth))
        const requested = (Number(pointerX) - Number(depthZeroMarkerX)) / step
        return clampedDepth(requested, maximumDepth, rootOnly)
    }

    // Folder rows can be grabbed anywhere across their width. Preserve the
    // source depth while moving vertically and interpret deliberate horizontal
    // movement in indent-sized steps.
    function depthFromDrag(pointerX, startPointerX, sourceDepth, indentWidth,
                           maximumDepth, rootOnly) {
        const step = Math.max(1, Number(indentWidth))
        const requested = Number(sourceDepth)
                + (Number(pointerX) - Number(startPointerX)) / step
        return clampedDepth(requested, maximumDepth, rootOnly)
    }

    // Resolve a preorder insertion point to the parent and next sibling used
    // by tree backends. `groups` must contain only the remaining group rows.
    function treeTarget(groups, insertionIndex, targetDepth,
                        idProvider, parentProvider, depthProvider) {
        const count = groups ? groups.length : 0
        const insertion = Math.max(0, Math.min(count,
                                               Number(insertionIndex)))
        let depth = Math.max(0, Number(targetDepth))
        let parentId = ""

        if (depth > 0) {
            for (let index = insertion - 1; index >= 0; --index) {
                const candidate = groups[index]
                if (Number(depthProvider(candidate)) !== depth - 1)
                    continue
                parentId = String(idProvider(candidate))
                break
            }
            if (parentId.length === 0)
                depth = 0
        }

        let beforeId = ""
        for (let index = insertion; index < count; ++index) {
            const candidate = groups[index]
            const candidateDepth = Number(depthProvider(candidate))
            if (candidateDepth < depth)
                break
            if (candidateDepth === depth
                    && String(parentProvider(candidate)) === parentId) {
                beforeId = String(idProvider(candidate))
                break
            }
        }
        return {
            depth: depth,
            parentId: parentId,
            beforeId: beforeId
        }
    }
}
