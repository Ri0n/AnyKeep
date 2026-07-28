pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    property bool animationEnabled: false
    property bool sourceActive: false
    property bool targetBefore: false
    property bool targetAfter: false
    property real naturalExtent: 0
    property real draggedExtent: 0
    property int animationDuration: 160
    property bool afterSide: false

    property real collapseSpace: sourceActive ? naturalExtent : 0
    property real dropSpace: targetBefore || targetAfter ? draggedExtent : 0
    readonly property real beforeSpace: afterSide ? 0 : dropSpace
    readonly property real afterSpace: afterSide ? dropSpace : 0
    readonly property real layoutExtent: Math.max(0, naturalExtent - collapseSpace)
                                               + dropSpace

    visible: false
    width: 0
    height: 0

    onTargetBeforeChanged: {
        if (targetBefore)
            afterSide = false
    }

    onTargetAfterChanged: {
        if (targetAfter)
            afterSide = true
    }

    Behavior on collapseSpace {
        enabled: root.animationEnabled

        NumberAnimation {
            duration: root.animationDuration
            easing.type: Easing.OutCubic
        }
    }

    Behavior on dropSpace {
        enabled: root.animationEnabled

        NumberAnimation {
            duration: root.animationDuration
            easing.type: Easing.OutCubic
        }
    }
}
