import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../reorder" as Reorder

FocusScope {
    id: imageRoot
    required property var editorView
    required property var reorderController
    objectName: "imageBlockEditor-" + block.index
    required property var block
    property bool selected: imageRoot.editorView.selectedImageIndex === block.index
    property real transientWidth: -1
    property real transientX: -1
    property real resizeStartWidth: 0
    property real resizeStartX: 0
    property int resizeDirection: 1
    readonly property string normalizedAlignment:
        block.imageAlignment === "left" || block.imageAlignment === "right"
        ? block.imageAlignment : "center"
    readonly property real naturalWidth:
        sourceImage.implicitWidth > 0 ? sourceImage.implicitWidth : Math.min(width, 480)
    readonly property real requestedWidth:
        transientWidth >= 0 ? transientWidth
                            : (block.imageWidth > 0 ? block.imageWidth : naturalWidth)
    readonly property real displayWidth: Math.max(1, Math.min(width, requestedWidth))
    readonly property real imageAspect:
        sourceImage.implicitWidth > 0 && sourceImage.implicitHeight > 0
        ? sourceImage.implicitHeight / sourceImage.implicitWidth : 0.75
    readonly property real displayHeight: Math.max(40, displayWidth * imageAspect)
    readonly property real imageY: selected ? altEditor.implicitHeight + 6 : 0
    readonly property real actionGap: imageRoot.editorView.touchMode ? 12 : 8
    readonly property real alignedX:
        normalizedAlignment === "left" ? 0
        : normalizedAlignment === "right" ? width - displayWidth
        : (width - displayWidth) / 2
    readonly property real displayX: transientX >= 0 ? transientX : alignedX

    width: block.width
    implicitHeight: imageY + displayHeight
                    + (selected ? actionGap + imageActions.height + 4 : 0)
    activeFocusOnTab: true

    function selectAndFocus() {
        imageRoot.editorView.selectImageBlock(block.index)
        imageRoot.forceActiveFocus()
    }

    function setAlignment(value) {
        if (normalizedAlignment === value)
            return
        imageRoot.editorView.runEditTransaction("align-image", function() {
            imageRoot.editorView.blockModel.setImageAlignment(block.index, value)
        })
    }

    function resetPresentation() {
        if (block.imageWidth === 0 && normalizedAlignment === "center")
            return
        imageRoot.editorView.runEditTransaction("reset-image-presentation", function() {
            imageRoot.editorView.blockModel.setImageWidth(block.index, 0)
            imageRoot.editorView.blockModel.setImageAlignment(block.index, "center")
        })
    }

    function beginResize(direction) {
        resizeDirection = direction
        resizeStartWidth = displayWidth
        resizeStartX = sourceImage.x
        transientWidth = displayWidth
        transientX = sourceImage.x
        imageRoot.editorView.beginEditTransaction("resize-image")
    }

    function updateResize(delta) {
        if (transientWidth < 0)
            return
        const maximum = resizeDirection > 0 ? width - resizeStartX
                                            : resizeStartX + resizeStartWidth
        const minimum = Math.min(imageRoot.editorView.touchMode ? 72 : 48, maximum)
        if (resizeDirection > 0) {
            transientWidth = Math.max(minimum, Math.min(maximum, resizeStartWidth + delta))
            transientX = resizeStartX
        } else {
            transientWidth = Math.max(minimum, Math.min(maximum, resizeStartWidth - delta))
            transientX = resizeStartX + resizeStartWidth - transientWidth
        }
    }

    function finishResize() {
        if (transientWidth < 0)
            return
        const width = Math.round(transientWidth)
        imageRoot.editorView.blockModel.setImageWidth(block.index, width)
        transientWidth = -1
        transientX = -1
        imageRoot.editorView.endEditTransaction()
    }

    function beginMarginSelection(area, mouse) {
        const point = area.mapToItem(imageRoot.editorView, mouse.x, mouse.y)
        imageRoot.editorView.beginBlankAreaSelection(block.index + 1, point.x, point.y)
        mouse.accepted = true
    }

    function updateMarginSelection(area, mouse) {
        if (!(mouse.buttons & Qt.LeftButton))
            return
        const point = area.mapToItem(imageRoot.editorView, mouse.x, mouse.y)
        imageRoot.editorView.updateBlankAreaSelection(point.x, point.y)
    }

    function finishMarginSelection(mouse) {
        if (mouse.button !== Qt.LeftButton)
            return
        if (!imageRoot.editorView.finishBlankAreaSelection())
            selectAndFocus()
    }

    Keys.onPressed: function(event) {
        if (altEditor.activeFocus)
            return
        if (event.matches(StandardKey.Copy)) {
            event.accepted = imageRoot.editorView.copyActiveSelection()
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            imageRoot.editorView.removeImageBlock(block.index, true)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            imageRoot.editorView.clearImageSelection()
            imageRoot.editorView.forceActiveFocus()
            event.accepted = true
        }
    }

    TextField {
        id: altEditor
        objectName: "imageAltEditor-" + imageRoot.block.index
        visible: imageRoot.selected
        opacity: visible ? 1 : 0
        width: Math.min(imageRoot.width, Math.max(180, sourceImage.width))
        x: Math.max(0, Math.min(imageRoot.width - width,
                                sourceImage.x + (sourceImage.width - width) / 2))
        placeholderText: qsTr("Alt text")
        text: imageRoot.block.alt
        selectByMouse: true
        onTextEdited: imageRoot.editorView.blockModel.setImageAlt(imageRoot.block.index, text)
        onActiveFocusChanged: imageRoot.editorView.imageAltEditorFocused = activeFocus
        Component.onDestruction: {
            if (imageRoot.editorView.imageAltEditorFocused)
                imageRoot.editorView.imageAltEditorFocused = false
        }
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    Image {
        id: sourceImage
        objectName: "imageBlockPreview-" + imageRoot.block.index
        x: imageRoot.displayX
        y: imageRoot.imageY
        width: imageRoot.displayWidth
        height: imageRoot.displayHeight
        source: imageRoot.block.previewUrl
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        cache: true
        ToolTip.visible: imageDescriptionHover.hovered && imageRoot.block.alt.length > 0
                             && !imageRoot.selected
        ToolTip.text: imageRoot.block.alt

        Behavior on x {
            enabled: imageRoot.transientWidth < 0
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

        HoverHandler { id: imageDescriptionHover }
        Reorder.ReorderDragHandle {
            anchors.fill: parent
            dragEnabled: !imageRoot.editorView.touchMode && imageRoot.transientWidth < 0
            hoverCursorShape: Qt.OpenHandCursor
            dragCursorShape: Qt.ClosedHandCursor
            onTapped: imageRoot.selectAndFocus()
            onDragStarted: {
                imageRoot.selectAndFocus()
                imageRoot.reorderController.startBlockDrag(
                            imageRoot.block.parent, imageRoot)
            }
            onDragMoved: function(dx, dy) {
                imageRoot.reorderController.moveBlockDrag(dx, dy)
            }
            onDragFinished: imageRoot.reorderController.finishBlockDrag()
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.DragThreshold
            onTapped: {
                imageRoot.selectAndFocus()
                imageContextMenu.popup()
            }
        }
        TapHandler {
            enabled: imageRoot.editorView.touchMode
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onLongPressed: {
                imageRoot.selectAndFocus()
                imageContextMenu.popup()
            }
        }
    }

    // The image itself owns the reorder gesture, but the unused row
    // width is still part of the document. Let an upward drag from
    // either horizontal margin start selection at the boundary after
    // the image (especially important when it is the final block).
    MouseArea {
        id: imageLeftMarginSelectionArea
        x: 0
        y: sourceImage.y
        width: Math.max(0, sourceImage.x)
        height: sourceImage.height
        z: 1
        enabled: !imageRoot.editorView.touchMode && !imageRoot.reorderController.dragging && width > 0
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) { imageRoot.beginMarginSelection(imageLeftMarginSelectionArea, mouse) }
        onPositionChanged: function(mouse) { imageRoot.updateMarginSelection(imageLeftMarginSelectionArea, mouse) }
        onReleased: function(mouse) { imageRoot.finishMarginSelection(mouse) }
        onCanceled: imageRoot.editorView.cancelBlankAreaSelection()
    }

    MouseArea {
        id: imageRightMarginSelectionArea
        x: sourceImage.x + sourceImage.width
        y: sourceImage.y
        width: Math.max(0, imageRoot.width - x)
        height: sourceImage.height
        z: 1
        enabled: !imageRoot.editorView.touchMode && !imageRoot.reorderController.dragging && width > 0
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) { imageRoot.beginMarginSelection(imageRightMarginSelectionArea, mouse) }
        onPositionChanged: function(mouse) { imageRoot.updateMarginSelection(imageRightMarginSelectionArea, mouse) }
        onReleased: function(mouse) { imageRoot.finishMarginSelection(mouse) }
        onCanceled: imageRoot.editorView.cancelBlankAreaSelection()
    }

    Rectangle {
        x: sourceImage.x - 2
        y: sourceImage.y - 2
        width: sourceImage.width + 4
        height: sourceImage.height + 4
        visible: imageRoot.selected
        color: "transparent"
        border.width: 2
        border.color: altEditor.palette.highlight
        radius: 2
        z: 3
    }

    ImageResizeHandle {
        visible: imageRoot.selected
        imageEditor: imageRoot
        direction: -1
        fillColor: altEditor.palette.base
        strokeColor: altEditor.palette.highlight
        x: Math.max(0, sourceImage.x - width / 2)
        y: sourceImage.y + sourceImage.height - height / 2
    }

    ImageResizeHandle {
        visible: imageRoot.selected
        imageEditor: imageRoot
        direction: 1
        fillColor: altEditor.palette.base
        strokeColor: altEditor.palette.highlight
        x: Math.min(imageRoot.width - width,
                    sourceImage.x + sourceImage.width - width / 2)
        y: sourceImage.y + sourceImage.height - height / 2
    }

    Row {
        id: imageActions
        visible: imageRoot.selected
        spacing: 3
        height: imageRoot.editorView.touchMode ? 36 : 28
        y: sourceImage.y + sourceImage.height + imageRoot.actionGap
        x: Math.max(0, Math.min(imageRoot.width - width,
                                sourceImage.x + (sourceImage.width - width) / 2))

        Repeater {
            model: ["left", "center", "right"]
            delegate: ToolButton {
                id: alignmentButton
                required property string modelData
                width: imageActions.height
                height: imageActions.height
                padding: 4
                checkable: true
                checked: imageRoot.normalizedAlignment === modelData
                display: AbstractButton.IconOnly
                ToolTip.visible: hovered
                ToolTip.text: modelData === "left" ? qsTr("Align left")
                              : modelData === "right" ? qsTr("Align right")
                              : qsTr("Align center")
                contentItem: ImageAlignmentGlyph {
                    alignment: modelData
                    tint: alignmentButton.palette.buttonText
                }
                onClicked: imageRoot.setAlignment(modelData)
            }
        }

        ToolButton {
            id: resetImageButton
            width: imageActions.height
            height: imageActions.height
            padding: 4
            display: AbstractButton.IconOnly
            enabled: imageRoot.block.imageWidth > 0 || imageRoot.normalizedAlignment !== "center"
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Reset image size and alignment")
            contentItem: Label {
                text: "↺"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: resetImageButton.palette.buttonText
                opacity: resetImageButton.enabled ? 1 : 0.45
                font.pixelSize: Math.max(15, resetImageButton.height * 0.62)
            }
            onClicked: imageRoot.resetPresentation()
        }
    }

    Menu {
        id: imageContextMenu
        MenuItem {
            text: qsTr("Save Image As…")
            visible: imageRoot.editorView.platformBackend !== null
            enabled: visible && imageRoot.block.url.startsWith("anykeep-media:/")
            onTriggered: imageRoot.editorView.platformBackend.saveImageAs(imageRoot.block.url)
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Align Left")
            onTriggered: imageRoot.setAlignment("left")
        }
        MenuItem {
            text: qsTr("Align Center")
            onTriggered: imageRoot.setAlignment("center")
        }
        MenuItem {
            text: qsTr("Align Right")
            onTriggered: imageRoot.setAlignment("right")
        }
        MenuItem {
            text: qsTr("Reset Size and Alignment")
            enabled: imageRoot.block.imageWidth > 0 || imageRoot.normalizedAlignment !== "center"
            onTriggered: imageRoot.resetPresentation()
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Remove Image")
            onTriggered: imageRoot.editorView.removeImageBlock(imageRoot.block.index, true)
        }
    }
}
