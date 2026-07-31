pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    default property alias actions: actionRow.data
    property bool triggerHovered: false
    property int spacing: 4
    property int gapAfter: 6
    property int fadeDuration: 480
    property bool revealRequested: false

    readonly property bool hovered: stripHoverHandler.hovered

    implicitWidth: actionRow.implicitWidth + gapAfter
    implicitHeight: actionRow.implicitHeight
    width: implicitWidth
    height: implicitHeight
    opacity: revealRequested ? 1 : 0
    visible: revealRequested || opacity > 0.001
    enabled: revealRequested || opacity > 0.5

    function updateReveal() {
        if (triggerHovered) {
            hideTimer.stop()
            revealRequested = true
            return
        }
        if (stripHoverHandler.hovered) {
            hideTimer.stop()
            if (revealRequested || opacity > 0.001)
                revealRequested = true
            return
        }
        hideTimer.restart()
    }

    onTriggerHoveredChanged: updateReveal()

    Behavior on opacity {
        NumberAnimation {
            duration: root.fadeDuration
            easing.type: Easing.OutCubic
        }
    }

    Timer {
        id: hideTimer
        interval: 40
        onTriggered: {
            if (!root.triggerHovered && !stripHoverHandler.hovered)
                root.revealRequested = false
        }
    }

    Row {
        id: actionRow
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: root.spacing
    }

    HoverHandler {
        id: stripHoverHandler
        onHoveredChanged: root.updateReveal()
    }
}
