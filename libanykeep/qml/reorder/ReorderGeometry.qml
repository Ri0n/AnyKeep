pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    function nearestBoundary(probe, boundaries) {
        let result = null
        let bestDistance = Number.POSITIVE_INFINITY
        for (const boundary of boundaries || []) {
            if (!boundary)
                continue
            const distance = Math.abs(Number(probe) - Number(boundary.position))
            if (distance < bestDistance) {
                bestDistance = distance
                result = boundary
            }
        }
        return result
    }
}
