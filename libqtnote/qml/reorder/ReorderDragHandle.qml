import QtQuick

Item {
    id: root
    property bool dragEnabled: true
    property int hoverCursorShape: Qt.OpenHandCursor
    property int dragCursorShape: Qt.ClosedHandCursor
    property bool overrideCursorApplied: false
    readonly property bool dragging: dragHandler.active
    readonly property bool hovered: cursorArea.containsMouse
    signal dragStarted()
    signal dragMoved(real dx, real dy)
    signal dragFinished()
    signal tapped()

    function hasCursorController() {
        return typeof qtnoteCursor !== "undefined" && qtnoteCursor
    }

    function applyDragCursor() {
        cursorArea.cursorShape = root.dragCursorShape
        dragHandler.cursorShape = root.dragCursorShape
        if (hasCursorController()) {
            qtnoteCursor.setOverrideCursor(root.dragCursorShape)
            root.overrideCursorApplied = true
        }
    }

    function restoreDragCursorOverride() {
        if (hasCursorController() && root.overrideCursorApplied) {
            qtnoteCursor.restoreOverrideCursor()
            root.overrideCursorApplied = false
        }
    }

    function releaseDragCursor() {
        restoreDragCursorOverride()
        cursorArea.cursorShape = root.hoverCursorShape
        dragHandler.cursorShape = root.hoverCursorShape
    }

    onHoverCursorShapeChanged: {
        if (!dragging)
            releaseDragCursor()
    }

    onDragCursorShapeChanged: {
        if (dragging)
            applyDragCursor()
    }

    Component.onDestruction: restoreDragCursorOverride()

    MouseArea {
        id: cursorArea

        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        cursorShape: root.dragging ? root.dragCursorShape : root.hoverCursorShape
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
        cursorShape: active ? root.dragCursorShape : root.hoverCursorShape
        onActiveTranslationChanged: {
            if (active)
                root.dragMoved(activeTranslation.x, activeTranslation.y)
        }
        onActiveChanged: {
            if (active) {
                root.applyDragCursor()
                root.dragStarted()
            } else {
                root.releaseDragCursor()
                root.dragFinished()
            }
        }
    }
}
