import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ListBlockBehavior.js" as ListBlockBehavior
import "reorder" as Reorder

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
    property int itemsRevision: 0
    property int handleHoverLevel: -1
    property int handleHoverItem: -1
    readonly property int itemSpacing: Math.max(5, Math.round(editorView.editorFontMetricsHeight * 0.3))
    readonly property int focusedItem: editorView.activeEditor
                                       && editorView.activeEditor.blockIndex === blockIndex
                                       && editorView.activeEditor.listItemIndex >= 0
                                       ? editorView.activeEditor.listItemIndex : -1
    readonly property int handleAnchorItem: handleHoverItem >= 0 ? handleHoverItem
                                          : (editorView.touchMode ? focusedItem : -1)
    readonly property int activeLevel: handleHoverLevel >= 0 ? handleHoverLevel
                                  : (handleAnchorItem >= 0 ? itemIndent(handleAnchorItem) : -1)
    readonly property var activeLevelRange: {
        itemsRevision
        return levelRangeForItem(handleAnchorItem)
    }
    readonly property bool showFocusedLevelHandle: editorView.touchMode && activeLevelRange.start >= 0
    readonly property int levelHandleGutter: editorView.listLevelHandleGutter

    width: block.width

    onItemDataChanged: syncItems(reorderController.committingDrop)
    onCheckedDataChanged: syncItems()
    onIndentDataChanged: syncItems()
    onTypeDataChanged: syncItems()
    Component.onCompleted: {
        syncItems()
        reorderController.registerListBlock(listRoot)
    }
    Component.onDestruction: reorderController.unregisterListBlock(listRoot)

    function syncItems(forceEditorText) {
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
        // A focused editor normally defers source updates. Reordering changes which item
        // its row represents, so keeping the old text would display a duplicate until blur.
        if (forceEditorText) {
            for (let index = 0; index < listModel.count; ++index) {
                const row = rowAt(index)
                const cell = row ? row.listEditor : null
                if (cell && typeof cell.applySourceText === "function")
                    cell.applySourceText(true)
            }
        }
        syncingItems = false
        ++itemsRevision
    }

    function rowAt(index) {
        return listRepeater.itemAt(index)
    }

    function itemCount() {
        return listModel.count
    }

    function itemIndent(index) {
        const item = index >= 0 && index < listModel.count ? listModel.get(index) : null
        return item ? Number(item.itemIndent) : 0
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
        return hierarchyRange.subtreeEnd(index)
    }

    function levelRangeForItem(index) {
        return hierarchyRange.levelRange(index)
    }

    function markerCenterXForIndent(indent) {
        return listRoot.mapToItem(editorView.contentItem,
                                  Math.max(0, Number(indent)) * editorView.listIndent
                                  + editorView.listMarkerWidth / 2, 0).x
    }

    function levelHandleHeight(range) {
        if (!range || range.start < 0)
            return 0
        const first = rowAt(range.start)
        const last = rowAt(range.end - 1)
        return first && last ? Math.max(0, last.y + last.naturalHeight - first.y) : 0
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

    function animatedRowDisplacement(row) {
        if (!row)
            return 0
        const sourceDisplacement = row.sourceRow
                ? Math.max(0, row.naturalHeight - row.collapseSpace) : 0
        return sourceDisplacement + row.dropSpace
    }

    function animatedLayoutDisplacement() {
        let displacement = endDropSpacer.dropSpace
        for (let index = 0; index < listModel.count; ++index)
            displacement += animatedRowDisplacement(rowAt(index))
        return displacement
    }

    function animatedDisplacementBeforeBoundary(index) {
        const count = remainingItemCount()
        if (count === 0)
            return 0
        const boundaryOriginal = index < count
                ? originalIndexForRemaining(index)
                : originalIndexForRemaining(count - 1)
        let displacement = 0
        for (let original = 0; original < boundaryOriginal; ++original)
            displacement += animatedRowDisplacement(rowAt(original))
        return displacement
    }

    function boundaryDescriptor(index) {
        const count = remainingItemCount()
        if (count === 0)
            return { owner: listRoot, afterOwner: false }
        if (index < count) {
            const row = rowAt(originalIndexForRemaining(index))
            return { owner: row || listRoot, afterOwner: false }
        }
        const lastRow = rowAt(originalIndexForRemaining(count - 1))
        return { owner: lastRow || listRoot, afterOwner: Boolean(lastRow) }
    }

    function focusItem(itemIndex, position, preserveViewport, viewportY) {
        editorView.focusEditorAddress({
            blockIndex: block.index,
            listItemIndex: itemIndex,
            tableCellIndex: -1,
            field: "listItem",
            cursorPosition: position,
            preserveViewport: Boolean(preserveViewport),
            viewportY: viewportY
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

    Reorder.HierarchyRange {
        id: hierarchyRange

        countProvider: function() { return listRoot.itemCount() }
        depthProvider: function(index) { return listRoot.itemIndent(index) }
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
            property real swipeOffset: 0
            readonly property bool sourceRow: listRoot.isSourceIndex(index)
            readonly property var ownLevelRange: {
                listRoot.itemsRevision
                return listRoot.levelRangeForItem(index)
            }
            readonly property bool startsLevelRange: ownLevelRange.start === index
            readonly property bool ownsLevelHandle: listRoot.showFocusedLevelHandle
                                                   && index === listRoot.activeLevelRange.start
            readonly property int remainingIndex: listRoot.remainingIndexForOriginal(index)
            readonly property bool dropBefore: !sourceRow
                                                   && reorderController.targetBlock === listRoot
                                                   && reorderController.targetItem === remainingIndex
            readonly property alias collapseSpace: rowDisplacement.collapseSpace
            readonly property alias dropSpace: rowDisplacement.beforeSpace
            readonly property real trailingSpace: index + 1 < listModel.count ? listRoot.itemSpacing : 0
            readonly property real naturalHeight: rowContent.implicitHeight + trailingSpace

            objectName: "listRow-" + listRoot.blockIndex + "-" + index
            width: listRoot.width
            height: rowDisplacement.layoutExtent
            z: ownsLevelHandle || (!editorView.touchMode && startsLevelRange) ? 20 : 0

            Reorder.ReorderDisplacement {
                id: rowDisplacement

                animationEnabled: reorderController.dragging && !reorderController.committingDrop
                sourceActive: rowWrapper.sourceRow
                targetBefore: rowWrapper.dropBefore
                naturalExtent: rowWrapper.naturalHeight
                draggedExtent: reorderController.draggedHeight
            }

            function removeBySwipe() {
                const firstItem = index
                const endItem = listRoot.subtreeEnd(firstItem)
                const removesWholeList = firstItem === 0 && endItem === listRoot.itemCount()
                const focusItem = Math.min(firstItem,
                                           listRoot.itemCount() - (endItem - firstItem) - 1)
                editorView.runEditTransaction("delete-list-items", function() {
                    editorView.prepareForStructuralMutation()
                    editorView.blockModel.removeListItems(
                                listRoot.blockIndex, firstItem, endItem - 1)
                    if (removesWholeList) {
                        editorView.focusBlock(
                                    Math.min(listRoot.blockIndex,
                                             editorView.blockModel.rowCount() - 1))
                    } else {
                        editorView.focusEditorAddress({
                            blockIndex: listRoot.blockIndex,
                            listItemIndex: focusItem,
                            tableCellIndex: -1,
                            field: "listItem",
                            cursorPosition: 0
                        })
                    }
                })
            }

            Rectangle {
                anchors.fill: parent
                visible: editorView.touchMode && Math.abs(rowWrapper.swipeOffset) > 0
                color: "#b3261e"

                Label {
                    anchors.left: rowWrapper.swipeOffset > 0 ? parent.left : undefined
                    anchors.right: rowWrapper.swipeOffset < 0 ? parent.right : undefined
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Delete")
                    color: "white"
                    font.bold: true
                }
            }

            RowLayout {
                id: rowContent

                x: rowWrapper.itemIndent * editorView.listIndent + rowWrapper.swipeOffset
                y: rowWrapper.dropSpace
                width: Math.max(0, listRoot.width
                                  - rowWrapper.itemIndent * editorView.listIndent)
                height: implicitHeight
                spacing: 0

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

                    Reorder.ReorderDragHandle {
                        objectName: "listReorderHandle-" + listRoot.blockIndex + "-" + rowWrapper.index
                        anchors.fill: parent
                        hoverCursorShape: rowWrapper.itemType === 2 ? Qt.PointingHandCursor : Qt.OpenHandCursor
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

            DragHandler {
                id: swipeHandler

                parent: editorLoader
                enabled: editorView.touchMode && !reorderController.dragging
                target: null
                acceptedDevices: PointerDevice.TouchScreen
                xAxis.enabled: true
                yAxis.enabled: false

                onActiveTranslationChanged: {
                    rowWrapper.swipeOffset = activeTranslation.x
                }
                onActiveChanged: {
                    if (active)
                        return
                    const removeThreshold = Math.min(120, rowWrapper.width * 0.35)
                    if (Math.abs(rowWrapper.swipeOffset) >= removeThreshold) {
                        rowWrapper.removeBySwipe()
                    } else {
                        rowWrapper.swipeOffset = 0
                    }
                }
                onCanceled: rowWrapper.swipeOffset = 0
            }

            Behavior on swipeOffset {
                enabled: !swipeHandler.active

                NumberAnimation {
                    duration: 160
                    easing.type: Easing.OutCubic
                }
            }

            Reorder.ReorderDragHandle {
                id: levelHandle

                property bool hovered: false
                readonly property var handleRange: editorView.touchMode ? listRoot.activeLevelRange
                                                                         : rowWrapper.ownLevelRange
                readonly property int handleLevel: editorView.touchMode ? listRoot.activeLevel
                                                                        : rowWrapper.itemIndent
                readonly property bool active: editorView.touchMode ? rowWrapper.ownsLevelHandle
                                                                    : rowWrapper.startsLevelRange
                readonly property bool shown: active && !reorderController.dragging
                                              && (editorView.touchMode || hovered)

                objectName: "listLevelReorderHandle-" + listRoot.blockIndex + "-"
                            + handleLevel + "-" + rowWrapper.index
                visible: active
                opacity: shown ? 1 : 0
                x: handleLevel * editorView.listIndent - levelHandleGutter
                y: 0
                width: Math.max(12, levelHandleGutter - 2)
                height: listRoot.levelHandleHeight(handleRange)
                z: 10

                Behavior on opacity {
                    NumberAnimation {
                        duration: 480
                        easing.type: Easing.OutCubic
                    }
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    radius: width / 2
                    color: "#808080"
                    opacity: 0.85
                }

                HoverHandler {
                    onHoveredChanged: {
                        levelHandle.hovered = hovered
                        if (hovered) {
                            listRoot.handleHoverLevel = levelHandle.handleLevel
                            listRoot.handleHoverItem = levelHandle.handleRange.start
                        } else if (listRoot.handleHoverLevel === levelHandle.handleLevel) {
                            listRoot.handleHoverLevel = -1
                            listRoot.handleHoverItem = -1
                        }
                    }
                }

                onDragStarted: reorderController.startListRangeDrag(
                                   listRoot,
                                   levelHandle.handleRange.start,
                                   levelHandle.handleRange.end,
                                   levelHandle,
                                   listRoot.markerCenterXForIndent(levelHandle.handleLevel))
                onDragMoved: function(dx, dy) { reorderController.moveListDrag(dx, dy) }
                onDragFinished: reorderController.finishListDrag()
            }
        }
    }

    Item {
        id: endDropSpacer

        readonly property bool active: reorderController.targetBlock === listRoot
                                       && reorderController.targetItem === listRoot.remainingItemCount()
        readonly property alias dropSpace: endDisplacement.beforeSpace
        width: listRoot.width
        height: dropSpace

        Reorder.ReorderDisplacement {
            id: endDisplacement

            animationEnabled: reorderController.dragging && !reorderController.committingDrop
            targetBefore: endDropSpacer.active
            draggedExtent: reorderController.draggedHeight
        }
    }
}
