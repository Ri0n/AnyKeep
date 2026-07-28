import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ListBlockBehavior.js" as ListBlockBehavior

Column {
    id: listRoot

    required property var editorView
    required property var block
    required property var reorderController
    required property Component editorDelegate

    property var itemData: block.items
    property var checkedData: block.checkedItems
    property var indentData: block.itemIndents
    property var typeData: block.itemTypes
    property bool syncingItems: false
    property int blockIndex: block.index
    readonly property int itemSpacing: Math.max(5, Math.round(editorView.editorFontMetricsHeight * 0.3))

    width: block.width

    SystemPalette {
        id: listPalette
    }

    onItemDataChanged: syncItems()
    onCheckedDataChanged: syncItems()
    onIndentDataChanged: syncItems()
    onTypeDataChanged: syncItems()
    Component.onCompleted: {
        syncItems()
        reorderController.registerListBlock(listRoot)
    }
    Component.onDestruction: reorderController.unregisterListBlock(listRoot)

    function syncItems() {
        syncingItems = true
        const values = itemData || []
        while (listModel.count > values.length)
            listModel.remove(listModel.count - 1)
        while (listModel.count < values.length)
            listModel.append({ itemText: "", itemChecked: false, itemIndent: 0, itemType: 2 })
        for (let index = 0; index < values.length; ++index) {
            if (listModel.get(index).itemText !== values[index])
                listModel.setProperty(index, "itemText", values[index])
            const checked = Boolean(checkedData[index])
            if (listModel.get(index).itemChecked !== checked)
                listModel.setProperty(index, "itemChecked", checked)
            const indent = Number(indentData[index] || 0)
            if (listModel.get(index).itemIndent !== indent)
                listModel.setProperty(index, "itemIndent", indent)
            const type = Number(typeData[index] === undefined ? block.blockType : typeData[index])
            if (listModel.get(index).itemType !== type)
                listModel.setProperty(index, "itemType", type)
        }
        syncingItems = false
    }

    function rowAt(index) {
        return listRepeater.itemAt(index)
    }

    function itemCount() {
        return listModel.count
    }

    function itemText(index) {
        return listModel.get(index).itemText
    }

    function itemNumber(index) {
        const current = listModel.get(index)
        if (!current)
            return 1
        const level = current.itemIndent
        let number = 1
        for (let previous = index - 1; previous >= 0; --previous) {
            const item = listModel.get(previous)
            if (item.itemIndent < level)
                break
            if (item.itemIndent === level && item.itemType === 5)
                ++number
        }
        return number
    }

    function subtreeEnd(index) {
        const level = Number(listModel.get(index).itemIndent)
        let end = index + 1
        while (end < listModel.count && Number(listModel.get(end).itemIndent) > level)
            ++end
        return end
    }

    function isSourceIndex(index) {
        return reorderController.dragging
                && reorderController.sourceBlock === listRoot
                && index >= reorderController.sourceItem
                && index < reorderController.sourceEnd
    }

    function remainingItemCount() {
        return listModel.count - (reorderController.sourceBlock === listRoot
                                  ? reorderController.sourceEnd - reorderController.sourceItem : 0)
    }

    function originalIndexForRemaining(index) {
        if (reorderController.sourceBlock !== listRoot || index < reorderController.sourceItem)
            return index
        return index + reorderController.sourceEnd - reorderController.sourceItem
    }

    function remainingIndexForOriginal(index) {
        if (reorderController.sourceBlock !== listRoot || index < reorderController.sourceItem)
            return index
        if (index >= reorderController.sourceEnd)
            return index - (reorderController.sourceEnd - reorderController.sourceItem)
        return -1
    }

    function remainingIndentAt(index) {
        const original = originalIndexForRemaining(index)
        return original >= 0 && original < listModel.count
                ? Number(listModel.get(original).itemIndent) : 0
    }

    function boundaryPosition(index) {
        const count = remainingItemCount()
        if (count === 0)
            return listRoot.mapToItem(editorView.contentItem, 0, 0)
        let position
        if (index < count) {
            const row = rowAt(originalIndexForRemaining(index))
            position = row ? row.mapToItem(editorView.contentItem, 0, 0)
                           : listRoot.mapToItem(editorView.contentItem, 0, 0)
        } else {
            const lastRow = rowAt(originalIndexForRemaining(count - 1))
            position = lastRow ? lastRow.mapToItem(editorView.contentItem, 0, lastRow.naturalHeight)
                               : listRoot.mapToItem(editorView.contentItem, 0, 0)
        }

        // A visible drop gap must not move the hit-test boundary below it,
        // otherwise the target chases the pointer and the last position is
        // unreachable without dragging by an extra item height.
        if (reorderController.targetBlock === listRoot
                && index > reorderController.targetItem) {
            const target = reorderController.targetItem
            if (target >= 0 && target < count) {
                const targetRow = rowAt(originalIndexForRemaining(target))
                if (targetRow)
                    position.y -= Math.max(0, targetRow.height - targetRow.naturalHeight)
            } else if (target === count) {
                position.y -= endDropSpacer.height
            }
        }
        return position
    }

    function focusItem(itemIndex, position) {
        editorView.focusEditorAddress({
            blockIndex: block.index,
            listItemIndex: itemIndex,
            tableCellIndex: -1,
            field: "listItem",
            cursorPosition: position
        })
    }

    function focusItemVertically(itemIndex, x, atBottom) {
        Qt.callLater(function() {
            const row = rowAt(itemIndex)
            const cell = row ? row.listEditor : null
            if (!cell)
                return
            cell.forceActiveFocus()
            const y = atBottom ? Math.max(0, cell.height - cell.bottomPadding - 1)
                               : cell.topPadding + 1
            cell.cursorPosition = cell.positionAt(x, y)
            editorView.activeEditor = cell
        })
    }

    function selectedItemRange() {
        let first = -1
        let last = -1
        for (let index = 0; index < listModel.count; ++index) {
            const row = rowAt(index)
            if (row && row.listEditor
                    && row.listEditor.selectionStart !== row.listEditor.selectionEnd) {
                if (first < 0)
                    first = index
                last = index
            }
        }
        return { first: first, last: last }
    }

    function handleItemKey(event, cell, itemIndex) {
        return ListBlockBehavior.handleKey(listRoot, editorView, event, cell, itemIndex)
    }

    ListModel {
        id: listModel
    }

    Repeater {
        id: listRepeater
        model: listModel

        Item {
            id: rowWrapper

            required property int index
            required property string itemText
            required property bool itemChecked
            required property int itemIndent
            required property int itemType

            property alias listEditor: editorLoader.item
            property alias markerItem: markerSlot
            property alias dragContent: rowContent
            property alias dragTranslationX: dragTranslation.x
            property alias dragTranslationY: dragTranslation.y
            property real dragOriginX: 0
            property real dragOriginY: 0
            readonly property bool sourceRow: listRoot.isSourceIndex(index)
            readonly property int remainingIndex: listRoot.remainingIndexForOriginal(index)
            readonly property bool dropBefore: !sourceRow
                                                   && reorderController.targetBlock === listRoot
                                                   && reorderController.targetItem === remainingIndex
            readonly property real dropSpace: dropBefore ? reorderController.draggedHeight : 0
            readonly property real trailingSpace: index + 1 < listModel.count ? listRoot.itemSpacing : 0
            readonly property real naturalHeight: rowContent.implicitHeight + trailingSpace

            width: listRoot.width
            height: (sourceRow ? 0 : naturalHeight) + dropSpace
            z: sourceRow ? 1000 : 0

            Behavior on height {
                enabled: reorderController.dragging
                NumberAnimation {
                    duration: 140
                    easing.type: Easing.OutCubic
                }
            }

            Rectangle {
                visible: rowWrapper.dropSpace > 0
                x: reorderController.targetIndent * editorView.listIndent + editorView.listMarkerWidth
                y: Math.max(0, rowWrapper.dropSpace / 2 - height / 2)
                width: Math.max(12, rowWrapper.width - x)
                height: 2
                color: listPalette.highlight
            }

            RowLayout {
                id: rowContent

                x: rowWrapper.itemIndent * editorView.listIndent
                y: rowWrapper.dropSpace
                width: Math.max(0, listRoot.width - x)
                height: implicitHeight
                z: rowWrapper.sourceRow ? 1000 : 0

                transform: Translate {
                    id: dragTranslation
                }

                Item {
                    id: markerSlot

                    objectName: "listMarker-" + listRoot.blockIndex + "-" + rowWrapper.index
                    readonly property real indicatorCenterX: taskMarker.indicator
                                                                    ? taskMarker.indicator.x
                                                                      + taskMarker.indicator.width / 2
                                                                    : width / 2
                    Layout.preferredWidth: editorView.listMarkerWidth
                    Layout.minimumWidth: editorView.listMarkerWidth
                    Layout.preferredHeight: editorView.touchMode ? 44 : editorView.editorFontMetricsHeight
                    Layout.alignment: Qt.AlignTop

                    CheckBox {
                        id: taskMarker

                        objectName: "taskMarker-" + listRoot.blockIndex + "-" + rowWrapper.index
                        visible: rowWrapper.itemType === 2
                        checked: rowWrapper.itemChecked
                        anchors.top: parent.top
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width
                        height: editorView.editorFontMetricsHeight
                        padding: 0
                        onClicked: editorView.runEditTransaction("toggle-task", function() {
                            editorView.blockModel.setChecked(listRoot.blockIndex, rowWrapper.index, checked)
                        })
                    }

                    Label {
                        objectName: "listGlyph-" + listRoot.blockIndex + "-" + rowWrapper.index
                        visible: rowWrapper.itemType !== 2
                        text: rowWrapper.itemType === 5
                              ? listRoot.itemNumber(rowWrapper.index) + "." : "●"
                        anchors.top: parent.top
                        x: Math.round(markerSlot.indicatorCenterX - width / 2)
                        width: implicitWidth
                        height: editorView.editorFontMetricsHeight
                        font.family: editorView.editorFont.family
                        font.pointSize: rowWrapper.itemType === 1
                                        ? editorView.editorPointSize * 0.72
                                        : editorView.editorPointSize
                        verticalAlignment: Text.AlignVCenter
                    }

                    ReorderDragHandle {
                        objectName: "listReorderHandle-" + listRoot.blockIndex + "-" + rowWrapper.index
                        anchors.fill: parent
                        onDragStarted: reorderController.startListDrag(listRoot, rowWrapper)
                        onDragMoved: function(dx, dy) { reorderController.moveListDrag(dx, dy) }
                        onDragFinished: reorderController.finishListDrag()
                        onTapped: {
                            if (rowWrapper.itemType !== 2)
                                return
                            editorView.runEditTransaction("toggle-task", function() {
                                editorView.blockModel.setChecked(
                                    listRoot.blockIndex, rowWrapper.index, !rowWrapper.itemChecked)
                            })
                        }
                    }
                }

                Loader {
                    id: editorLoader

                    property var listRow: rowWrapper
                    property var listBlock: listRoot
                    sourceComponent: listRoot.editorDelegate
                    Layout.fillWidth: true
                    Layout.preferredHeight: item ? item.implicitHeight : 0
                }
            }
        }
    }

    Item {
        id: endDropSpacer

        readonly property bool active: reorderController.targetBlock === listRoot
                                       && reorderController.targetItem === listRoot.remainingItemCount()
        width: listRoot.width
        height: active ? reorderController.draggedHeight : 0

        Behavior on height {
            enabled: reorderController.dragging
            NumberAnimation {
                duration: 140
                easing.type: Easing.OutCubic
            }
        }

        Rectangle {
            visible: endDropSpacer.height > 0
            x: reorderController.targetIndent * editorView.listIndent + editorView.listMarkerWidth
            y: Math.max(0, parent.height / 2 - height / 2)
            width: Math.max(12, parent.width - x)
            height: 2
            color: listPalette.highlight
        }
    }
}
