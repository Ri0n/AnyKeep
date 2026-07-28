import QtQuick

Item {
    id: root
    property bool dragEnabled: true
    readonly property bool dragging: dragHandler.active
    signal dragStarted()
    signal dragMoved(real dx, real dy)
    signal dragFinished()
    signal tapped()

    HoverHandler {
        cursorShape: dragHandler.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onTapped: root.tapped()
    }

    DragHandler {
        id: dragHandler
        target: null
        enabled: root.dragEnabled
        onActiveTranslationChanged: {
            if (active)
                root.dragMoved(activeTranslation.x, activeTranslation.y)
        }
        onActiveChanged: {
            if (active)
                root.dragStarted()
            else
                root.dragFinished()
        }
    }
}
