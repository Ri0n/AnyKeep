import QtQuick

Rectangle {
    required property var imageEditor
    required property int direction
    required property color fillColor
    required property color strokeColor
    width: imageEditor.editorView.touchMode ? 18 : 12
    height: width
    radius: 2
    color: fillColor
    border.width: 1
    border.color: strokeColor
    z: 5

    property real pressX: 0
    property bool resizing: false

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        propagateComposedEvents: false

        onPressed: function(mouse) {
            mouse.accepted = true
            imageEditor.selectAndFocus()
            const point = mapToItem(imageEditor, mouse.x, mouse.y)
            parent.pressX = point.x
            parent.resizing = true
            imageEditor.beginResize(parent.direction)
        }
        onPositionChanged: function(mouse) {
            if (!parent.resizing)
                return
            const point = mapToItem(imageEditor, mouse.x, mouse.y)
            imageEditor.updateResize(point.x - parent.pressX)
            mouse.accepted = true
        }
        onReleased: function(mouse) {
            mouse.accepted = true
            if (!parent.resizing)
                return
            parent.resizing = false
            imageEditor.finishResize()
        }
        onCanceled: {
            if (!parent.resizing)
                return
            parent.resizing = false
            imageEditor.finishResize()
        }
    }
}
