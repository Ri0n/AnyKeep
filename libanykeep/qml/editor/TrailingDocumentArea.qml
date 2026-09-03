import QtQuick

Item {
    id: trailingDocumentArea
    required property var editorView
    required property var reorderController
    parent: editorView
    x: 0
    y: editorView.trailingViewportTop()
    width: Math.max(0, editorView.width - editorView.scrollBarInset)
    height: Math.max(0, editorView.height - y)
    z: 40000
    enabled: !reorderController.dragging && editorView.count > 0

    MouseArea {
        anchors.fill: parent
        enabled: !editorView.touchMode
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) {
            const point = mapToItem(editorView, mouse.x, mouse.y)
            editorView.beginBlankAreaSelection(editorView.count, point.x, point.y)
            mouse.accepted = true
        }
        onPositionChanged: function(mouse) {
            if (!(mouse.buttons & Qt.LeftButton))
                return
            const point = mapToItem(editorView, mouse.x, mouse.y)
            editorView.updateBlankAreaSelection(point.x, point.y)
        }
        onReleased: function(mouse) {
            if (mouse.button !== Qt.LeftButton)
                return
            if (!editorView.finishBlankAreaSelection())
                editorView.focusDocumentEnd()
        }
        onCanceled: editorView.cancelBlankAreaSelection()
    }

    TapHandler {
        enabled: editorView.touchMode
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onTapped: editorView.insertParagraphAtBoundary(editorView.count)
    }
}
