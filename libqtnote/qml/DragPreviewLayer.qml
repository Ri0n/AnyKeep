pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    property var entries: []
    property real translationX: 0
    property real translationY: 0
    property bool hideSources: true
    property real previewOpacity: 0.9
    property string objectNamePrefix: "dragPreview-"
    readonly property int previewCount: entries.length
    readonly property real totalHeight: {
        let result = 0
        for (const entry of entries)
            result += Number(entry.height)
        return result
    }

    visible: previewCount > 0
    enabled: false

    function capture(sourceItems) {
        const captured = []
        for (const source of sourceItems || []) {
            const descriptor = source && source.sourceItem ? source : null
            const sourceItem = descriptor ? descriptor.sourceItem : source
            if (!sourceItem)
                continue
            const sourceX = descriptor && descriptor.sourceX !== undefined
                    ? Number(descriptor.sourceX) : 0
            const sourceY = descriptor && descriptor.sourceY !== undefined
                    ? Number(descriptor.sourceY) : 0
            const origin = sourceItem.mapToItem(root, sourceX, sourceY)
            captured.push({
                sourceItem: sourceItem,
                x: origin.x,
                y: origin.y,
                sourceX: sourceX,
                sourceY: sourceY,
                width: descriptor && descriptor.width !== undefined
                       ? Number(descriptor.width) : sourceItem.width,
                height: descriptor && descriptor.height !== undefined
                        ? Number(descriptor.height) : sourceItem.height
            })
        }
        entries = captured
    }

    function clear() {
        entries = []
    }

    Repeater {
        model: root.entries

        ShaderEffectSource {
            required property int index
            required property var modelData

            objectName: root.objectNamePrefix + index
            sourceItem: modelData.sourceItem
            hideSource: root.hideSources
            live: true
            recursive: true
            smooth: true
            x: Number(modelData.x) + root.translationX
            y: Number(modelData.y) + root.translationY
            width: Number(modelData.width)
            height: Number(modelData.height)
            sourceRect: Qt.rect(Number(modelData.sourceX), Number(modelData.sourceY),
                                width, height)
            opacity: root.previewOpacity
        }
    }
}
