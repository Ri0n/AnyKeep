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
        for (const sourceItem of sourceItems || []) {
            if (!sourceItem)
                continue
            const origin = sourceItem.mapToItem(root, 0, 0)
            captured.push({
                sourceItem: sourceItem,
                x: origin.x,
                y: origin.y,
                width: sourceItem.width,
                height: sourceItem.height
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
            sourceRect: Qt.rect(0, 0, width, height)
            opacity: root.previewOpacity
        }
    }
}
