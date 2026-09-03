import QtQuick
import QtQuick.Controls

Item {
    id: interBlockHitLayer
    objectName: "interBlockHitLayer"
    required property var editorView
    required property var reorderController
    readonly property bool formatEnabled: Boolean(editorView.editorBackend
                                                   && editorView.editorBackend.markdown)

    SystemPalette {
        id: editorPalette
    }
    parent: editorView.contentItem
    width: Math.max(0, editorView.width - editorView.scrollBarInset)
    height: editorView.contentHeight
    z: 50000
    visible: formatEnabled && !reorderController.dragging
    enabled: formatEnabled && !reorderController.dragging

    Repeater {
        model: Math.max(0, editorView.count - 1)

        delegate: Item {
            id: gapTarget
            property bool revealIndicator: false
            readonly property var precedingBlock: {
                const geometryDependency = editorView.contentY + editorView.contentHeight + editorView.editorRegistrations
                return editorView.itemAtIndex(index)
            }
            readonly property var followingBlock: {
                const geometryDependency = editorView.contentY + editorView.contentHeight + editorView.editorRegistrations
                return editorView.itemAtIndex(index + 1)
            }
            readonly property int boundaryRow: index + 1
            x: 0
            y: precedingBlock ? precedingBlock.y + precedingBlock.height : 0
            width: interBlockHitLayer.width
            height: editorView.spacing
            visible: precedingBlock && followingBlock && height > 0

            MouseArea {
                id: gapMouse
                anchors.fill: parent
                enabled: !editorView.touchMode
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                preventStealing: true
                cursorShape: Qt.IBeamCursor
                onContainsMouseChanged: {
                    if (containsMouse) {
                        gapRevealTimer.restart()
                    } else {
                        gapRevealTimer.stop()
                        gapTarget.revealIndicator = false
                    }
                }
                onEnabledChanged: {
                    if (!enabled) {
                        gapRevealTimer.stop()
                        gapTarget.revealIndicator = false
                    }
                }
                onPressed: function(mouse) {
                    const point = mapToItem(editorView, mouse.x, mouse.y)
                    editorView.beginBlankAreaSelection(gapTarget.boundaryRow, point.x, point.y)
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
                        editorView.insertParagraphAtBoundary(gapTarget.boundaryRow)
                }
                onCanceled: editorView.cancelBlankAreaSelection()
            }

            TapHandler {
                enabled: editorView.touchMode
                acceptedButtons: Qt.LeftButton
                gesturePolicy: TapHandler.DragThreshold
                onTapped: editorView.insertParagraphAtBoundary(gapTarget.boundaryRow)
            }

            Timer {
                id: gapRevealTimer
                interval: 500
                repeat: false
                onTriggered: {
                    if (gapMouse.containsMouse)
                        gapTarget.revealIndicator = true
                }
            }

            Item {
                anchors.fill: parent
                opacity: gapTarget.revealIndicator ? 1 : 0

                Behavior on opacity {
                    NumberAnimation {
                        duration: 480
                        easing.type: Easing.OutCubic
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: editorView.editorInset
                    anchors.rightMargin: editorView.editorInset
                    anchors.verticalCenter: parent.verticalCenter
                    height: 1
                    color: editorPalette.highlight
                    opacity: 0.55
                }

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: Math.max(1, editorView.baseEditorInset / 3)
                    anchors.verticalCenter: parent.verticalCenter
                    text: "+"
                    color: editorPalette.highlight
                    opacity: 0.9
                    font.bold: true
                }
            }
        }
    }
}
