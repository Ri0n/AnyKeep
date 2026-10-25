import QtQuick

// Observe activity above the video without taking gestures from its controls.
Item {
    id: root
    property bool playing: false
    property bool interacting: false
    property int idleDelay: 2500
    property bool idle: false
    property point lastPointerPosition: Qt.point(-1, -1)
    readonly property bool busy: interacting || pointer.active
    readonly property bool controlsVisible: !enabled || !playing || busy || !idle

    function reveal() {
        idle = false
        if (enabled && playing && !busy)
            hideTimer.restart()
        else
            hideTimer.stop()
    }

    function observeMovement(position) {
        if (position.x === lastPointerPosition.x && position.y === lastPointerPosition.y)
            return
        lastPointerPosition = position
        reveal()
    }

    onPlayingChanged: reveal()
    onBusyChanged: reveal()
    onEnabledChanged: reveal()
    onIdleDelayChanged: reveal()
    Component.onCompleted: reveal()

    Timer {
        id: hideTimer
        interval: root.idleDelay
        onTriggered: root.idle = true
    }

    HoverHandler {
        blocking: false
        onHoveredChanged: {
            if (hovered)
                root.reveal()
        }
        onPointChanged: root.observeMovement(point.scenePosition)
    }

    PointHandler {
        id: pointer
        acceptedButtons: Qt.AllButtons
        onActiveChanged: root.reveal()
        onPointChanged: {
            if (active)
                root.reveal()
        }
    }
}
