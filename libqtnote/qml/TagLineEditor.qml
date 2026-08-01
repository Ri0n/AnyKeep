pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "reorder" as Reorder

FocusScope {
    id: root

    required property var editorView
    required property var blockModel
    required property var tags
    property int blockIndex: -1
    property font editorFont
    property bool touchMode: false
    property int editingIndex: -1
    property bool committingEditor: false
    readonly property int chipSpacing: touchMode ? 8 : 6
    readonly property int chipVerticalPadding: touchMode ? 4 : 3
    readonly property int chipHeight: Math.max(touchMode ? 30 : 24,
                                               tagMetrics.height + chipVerticalPadding * 2)
    readonly property real chipRadius: Math.max(3, Math.round(chipHeight / 4))
    readonly property color chipFill: Qt.rgba(0.16, 0.62, 0.34, 0.26)
    readonly property color chipBorder: Qt.rgba(0.20, 0.72, 0.40, 0.78)
    readonly property color gripFill: Qt.rgba(0.18, 0.66, 0.36, 0.31)
    readonly property color gripHoverFill: Qt.rgba(0.20, 0.70, 0.38, 0.40)

    implicitHeight: Math.max(chipHeight, tagRow.implicitHeight) + 2
    activeFocusOnTab: true
    onActiveFocusChanged: {
        if (activeFocus) {
            editorView.clearImageSelection()
            editorView.activeEditor = null
            editorView.activeTagLineIndex = blockIndex
        } else if (editorView.activeTagLineIndex === blockIndex) {
            editorView.activeTagLineIndex = -1
        }
    }

    function chipItems() {
        const result = []
        for (let index = 0; index < tagRepeater.count; ++index) {
            const item = tagRepeater.itemAt(index)
            if (item)
                result.push(item)
        }
        return result
    }

    function commitCurrentTagWithoutRefocus() {
        if (editingIndex < 0)
            return true
        const currentIndex = editingIndex
        const item = tagRepeater.itemAt(currentIndex)
        if (!item || !item.editor) {
            editingIndex = -1
            return true
        }
        return finishExistingTag(currentIndex, item.editor.text, false,
                                 item.editor.cursorPosition, true)
    }

    function focusDraft() {
        if (!commitCurrentTagWithoutRefocus())
            return
        editingIndex = -1
        draftField.forceActiveFocus()
        draftField.cursorPosition = draftField.length
    }

    function editTag(index, atEnd) {
        if (index < 0 || index >= tagRepeater.count) {
            focusDraft()
            return false
        }
        if (editingIndex !== index && !commitCurrentTagWithoutRefocus())
            return false
        editingIndex = index
        Qt.callLater(function() {
            const item = tagRepeater.itemAt(index)
            if (!item || !item.editor)
                return
            item.editor.forceActiveFocus()
            item.editor.cursorPosition = atEnd === false ? 0 : item.editor.length
        })
        return true
    }

    function forceActiveFocus() { focusDraft() }

    function applyModelResult(result) {
        if (!result || !result.handled)
            return false
        if (!result.tagLine) {
            const targetRow = Number(result.focusRow === undefined ? blockIndex : result.focusRow)
            const position = Number(result.cursorPosition === undefined ? 0 : result.cursorPosition)
            editorView.focusBlock(targetRow, false, position)
            return true
        }
        editingIndex = -1
        if (result.focusDraft) {
            Qt.callLater(function() {
                const delegate = editorView.itemAtIndex(root.blockIndex)
                if (delegate && delegate.item && delegate.item.focusDraft)
                    delegate.item.focusDraft()
            })
        } else if (result.focusTag !== undefined) {
            const target = Number(result.focusTag)
            Qt.callLater(function() {
                const delegate = editorView.itemAtIndex(root.blockIndex)
                if (delegate && delegate.item && delegate.item.editTag)
                    delegate.item.editTag(target, true)
            })
        }
        return true
    }

    function finishExistingTag(index, value, focusDraftAfter, cursorPosition, suppressRefocus) {
        if (committingEditor || index < 0)
            return false
        committingEditor = true
        let result = null
        editorView.runEditTransaction("edit-tag", function() {
            result = blockModel.setTagLineTag(blockIndex, index, value, Number(cursorPosition))
            return Boolean(result && result.handled)
        })
        committingEditor = false
        if (suppressRefocus) {
            if (result && result.tagLine) {
                editingIndex = -1
                return true
            }
            applyModelResult(result)
            return false
        }
        if (result && result.tagLine && focusDraftAfter)
            result.focusDraft = true
        return applyModelResult(result)
    }

    function finishDraft(value, cursorPosition, focusFollowingAfter, suppressRefocus) {
        if (committingEditor)
            return false
        const token = String(value || "").trim()
        if (token.length === 0) {
            draftField.text = ""
            if (focusFollowingAfter)
                Qt.callLater(function() { editorView.focusFollowingBlock(blockIndex, true) })
            return Boolean(focusFollowingAfter)
        }
        committingEditor = true
        let result = null
        editorView.runEditTransaction("add-tag", function() {
            result = blockModel.appendTagLineTag(blockIndex, token, Number(cursorPosition))
            return Boolean(result && result.handled)
        })
        committingEditor = false
        draftField.text = ""
        if (result && result.tagLine && (focusFollowingAfter || suppressRefocus))
            result.focusDraft = false
        const applied = applyModelResult(result)
        if (applied && focusFollowingAfter)
            Qt.callLater(function() { editorView.focusFollowingBlock(blockIndex, true) })
        return applied
    }

    function removeTag(index) {
        let result = null
        editorView.runEditTransaction("remove-tag", function() {
            result = blockModel.removeTagLineTag(blockIndex, index)
            return Boolean(result && result.handled)
        })
        return applyModelResult(result)
    }

    FontMetrics {
        id: tagMetrics
        font: root.editorFont
    }

    Reorder.LinearReorderLayout {
        id: tagLayout
        geometryItem: root
        orientation: Qt.Horizontal
        sourceEntries: tagReorder.sourceEntries
        keyProvider: function(item) { return Number(item.tagIndex) }
        orderProvider: function(item) { return Number(item.tagIndex) }
        extentProvider: function(item) { return Number(item.naturalWidth) }
        offsetProvider: function(item) { return Number(item.reorderOffsetX) }
    }

    Reorder.GenericReorderController {
        id: tagReorder
        anchors.fill: parent
        geometryItem: root
        orientation: Qt.Horizontal
        compensateForScroll: false
        previewObjectName: "tagReorderPreview-" + root.blockIndex
        previewObjectNamePrefix: "tagReorderPreviewItem-"
        boundaryProvider: function() { return tagLayout.boundaries(root.chipItems()) }
        outsideDropProvider: function(controller) {
            if (root.touchMode || !root.editorView)
                return false
            const point = root.mapToItem(root.editorView,
                                         controller.currentPointerX,
                                         controller.currentPointerY)
            return point.x < 0 || point.y < 0
                    || point.x > root.editorView.width
                    || point.y > root.editorView.height
        }
        outsideDropHandler: function(payload) {
            if (!payload)
                return false
            const index = Number(payload.from)
            // Removing the final tag replaces this Loader item with a normal
            // text editor. Let GenericReorderController clear its preview and
            // drag state before a model mutation can destroy the controller.
            Qt.callLater(function() { root.removeTag(index) })
            return true
        }
        commitHandler: function(payload, boundary) {
            if (!payload || !boundary)
                return false
            const from = Number(payload.from)
            const to = Math.max(0, Math.min(Number(boundary.finalIndex), root.tags.length - 1))
            let moved = false
            root.editorView.runEditTransaction("move-tag", function() {
                moved = root.blockModel.moveTagLineTag(root.blockIndex, from, to)
                return moved
            })
            return moved
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) {
            const point = mapToItem(root.editorView, mouse.x, mouse.y)
            root.editorView.beginBlankAreaSelection(root.blockIndex + 1, point.x, point.y)
            mouse.accepted = true
        }
        onPositionChanged: function(mouse) {
            if (!(mouse.buttons & Qt.LeftButton))
                return
            const point = mapToItem(root.editorView, mouse.x, mouse.y)
            root.editorView.updateBlankAreaSelection(point.x, point.y)
        }
        onReleased: function(mouse) {
            if (mouse.button !== Qt.LeftButton)
                return
            if (!root.editorView.finishBlankAreaSelection())
                root.focusDraft()
        }
        onCanceled: root.editorView.cancelBlankAreaSelection()
    }

    Row {
        id: tagRow
        z: 1
        spacing: root.chipSpacing
        height: root.chipHeight

        Repeater {
            id: tagRepeater
            model: root.tags

            delegate: Rectangle {
                id: chip
                required property int index
                required property string modelData
                property int tagIndex: index
                readonly property bool editing: root.editingIndex === index
                property real reorderOffsetX: tagLayout.translationByOrder(
                                                  chip, tagReorder.targetBoundary,
                                                  tagReorder.draggedExtent)
                property real naturalWidth: width + root.chipSpacing
                property alias editor: tagEditor
                height: root.chipHeight
                width: chip.editing ? Math.max(70, tagEditor.implicitWidth + 16)
                                    : grip.width + tagLabel.implicitWidth + 18
                radius: root.chipRadius
                clip: true
                color: chip.editing ? "transparent" : root.chipFill
                border.width: chip.editing ? 0 : 1
                border.color: root.chipBorder
                opacity: tagReorder.containsSource(chip) ? 0 : 1
                transform: Translate { x: chip.reorderOffsetX }

                Behavior on reorderOffsetX {
                    enabled: tagReorder.dragging && !tagReorder.committingDrop
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }
                Behavior on width {
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }
                Behavior on color {
                    ColorAnimation { duration: 160; easing.type: Easing.OutCubic }
                }

                Item {
                    id: grip
                    visible: !chip.editing
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: root.touchMode ? 18 : 14
                    clip: true
                    readonly property bool activeVisual: gripDragHandle.hovered || gripDragHandle.dragging

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: parent.width + root.chipRadius
                        radius: root.chipRadius
                        color: grip.activeVisual ? root.gripHoverFill : root.gripFill

                        Behavior on color {
                            ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 1
                        color: root.chipBorder
                        opacity: grip.activeVisual ? 0.48 : 0.28

                        Behavior on opacity {
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                    }

                    Row {
                        anchors.centerIn: parent
                        spacing: 2
                        Repeater {
                            model: 2
                            Rectangle {
                                width: 1
                                height: Math.max(9, grip.height * 0.36)
                                color: root.chipBorder
                                opacity: grip.activeVisual ? 0.86 : 0.52

                                Behavior on opacity {
                                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                                }
                            }
                        }
                    }

                    Reorder.ReorderDragHandle {
                        id: gripDragHandle
                        anchors.fill: parent
                        dragEnabled: root.editingIndex < 0 && root.tags.length > 1
                        onTapped: root.editTag(chip.index, true)
                        onDragStarted: {
                            root.editingIndex = -1
                            tagReorder.beginDrag({
                                sources: [{
                                    item: chip,
                                    previewItem: chip,
                                    key: chip.index,
                                    order: chip.index,
                                    naturalExtent: chip.naturalWidth
                                }],
                                pointerItem: grip,
                                payload: { from: chip.index },
                                targetByDraggedLeading: true
                            })
                        }
                        onDragMoved: function(dx, dy) { tagReorder.moveDrag(dx, dy) }
                        onDragFinished: tagReorder.finishDrag()
                    }
                }

                Label {
                    id: tagLabel
                    visible: !chip.editing
                    anchors.left: grip.right
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: chip.modelData
                    font: root.editorFont
                    elide: Text.ElideRight

                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: root.editTag(chip.index, true)
                    }
                }

                TextField {
                    id: tagEditor
                    visible: chip.editing
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.rightMargin: 5
                    text: visible ? "*" + chip.modelData : ""
                    font: root.editorFont
                    selectByMouse: true
                    background: null
                    leftPadding: 3
                    rightPadding: 3
                    topPadding: root.chipVerticalPadding
                    bottomPadding: root.chipVerticalPadding
                    verticalAlignment: TextInput.AlignVCenter
                    onActiveFocusChanged: {
                        if (!activeFocus && visible && !root.committingEditor)
                            root.finishExistingTag(chip.index, text, false, cursorPosition, true)
                    }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Escape) {
                            root.editingIndex = -1
                            root.forceActiveFocus()
                            event.accepted = true
                        } else if ((event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter) && selectionStart === selectionEnd) {
                            root.finishExistingTag(chip.index, text, true, cursorPosition)
                            event.accepted = true
                        } else if ((event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)
                                   && text.length === 0) {
                            root.removeTag(chip.index)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up) {
                            root.editorView.focusPrecedingBlock(root.blockIndex)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Down) {
                            root.editorView.focusFollowingBlock(root.blockIndex, false)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Left && cursorPosition === 0) {
                            if (chip.index > 0)
                                root.editTag(chip.index - 1, true)
                            else
                                root.editorView.focusPrecedingBlock(root.blockIndex)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Right && cursorPosition === length) {
                            if (chip.index + 1 < root.tags.length)
                                root.editTag(chip.index + 1, false)
                            else
                                root.focusDraft()
                            event.accepted = true
                        }
                    }
                }
            }
        }

        TextField {
            id: draftField
            visible: root.editingIndex < 0
            enabled: visible
            height: root.chipHeight
            width: visible ? Math.max(34, Math.min(180, implicitWidth + 10)) : 0
            text: ""
            placeholderText: activeFocus ? qsTr("*tag") : "+"
            font: root.editorFont
            selectByMouse: true
            background: null
            leftPadding: 4
            rightPadding: 4
            onActiveFocusChanged: {
                if (!activeFocus && text.trim().length > 0 && !root.committingEditor)
                    root.finishDraft(text, cursorPosition, false, true)
            }
            onAccepted: root.finishDraft(text, cursorPosition, true)
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space && text.trim().length > 1) {
                    root.finishDraft(text, cursorPosition, false)
                    event.accepted = true
                } else if (event.key === Qt.Key_Backspace && text.length === 0 && root.tags.length > 0) {
                    root.editTag(root.tags.length - 1, true)
                    event.accepted = true
                } else if (event.key === Qt.Key_Left && cursorPosition === 0 && root.tags.length > 0) {
                    root.editTag(root.tags.length - 1, true)
                    event.accepted = true
                } else if (event.key === Qt.Key_Up) {
                    root.editorView.focusPrecedingBlock(root.blockIndex)
                    event.accepted = true
                } else if (event.key === Qt.Key_Down
                           || (event.key === Qt.Key_Right && cursorPosition === length && text.length === 0)) {
                    root.editorView.focusFollowingBlock(root.blockIndex, false)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    text = ""
                    root.editorView.focusBlock(Math.max(0, root.blockIndex - 1), true)
                    event.accepted = true
                }
            }
        }
    }

}
