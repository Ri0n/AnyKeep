import QtQuick

Item {
    id: root

    property bool dragEnabled: true
    property bool forceVisible: false
    property bool fullHeight: false
    property color handleColor: "#808080"
    property int fadeDuration: 480
    readonly property bool hovered: hoverHandler.hovered
    readonly property bool dragging: dragHandle.dragging

    signal dragStarted()
    signal dragMoved(real dx, real dy)
    signal dragFinished()

    opacity: forceVisible || hovered || dragging ? 1 : 0

    Behavior on opacity {
        NumberAnimation {
            duration: root.fadeDuration
            easing.type: Easing.OutCubic
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.fullHeight ? 0 : Math.round((parent.height - height) / 2)
        width: 3
        height: root.fullHeight ? parent.height
                                : Math.min(parent.height,
                                           Math.min(32, Math.max(18, parent.height * 0.35)))
        radius: width / 2
        color: root.handleColor
        opacity: 0.85
    }

    HoverHandler {
        id: hoverHandler
    }

    ReorderDragHandle {
        id: dragHandle
        anchors.fill: parent
        dragEnabled: root.dragEnabled
        onDragStarted: root.dragStarted()
        onDragMoved: function(dx, dy) { root.dragMoved(dx, dy) }
        onDragFinished: root.dragFinished()
    }
}
