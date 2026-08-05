pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as App
import "../reorder" as Reorder

SwipeDelegate {
    id: row

    required property var collection
    property int rowIndex: -1
    property int itemDepth: 0
    property int itemType: 1
    property int rowKind: itemType
    property string storageId: ""
    property string noteId: ""
    property string title: ""
    property string preview: ""
    property string storageName: ""
    property string iconSource: ""
    property string groupKind: ""
    property string groupId: ""
    property string parentGroupId: ""
    property string folderId: ""
    property string parentFolderId: ""
    property bool groupExpanded: false
    property bool groupHasChildren: false
    property bool groupCollapsed: !groupExpanded
    property bool favorite: false
    property bool archived: false
    property bool systemFolder: false
    property bool loading: false
    property string errorString: ""
    property int noteCount: 0
    property bool internalDragActive: false
    property bool inReusePool: false
    property double suppressClickUntil: 0

    readonly property bool noteRow: itemType === collection.noteItemType
    readonly property bool groupRow: !noteRow
    readonly property bool compactFlatNoteRow: noteRow && collection.flatNoteRows
    readonly property real leadingInset: compactFlatNoteRow ? 8 : 8 + itemDepth * 18
    readonly property real baseHeight: collection.rowHeight
    readonly property bool noteSelected: !collection.dragSelectionSuppressed
                                         && noteRow
                                         && collection.noteIsSelected(storageId, noteId)
    readonly property bool currentNote: noteRow
                                        && collection.currentStorageId === storageId
                                        && collection.currentNoteId === noteId
    // The current editor and a selected group are distinct states.  A group
    // selection takes visual precedence, otherwise the two full-row
    // highlights look like a stale note selection after a folder is reopened.
    readonly property bool currentNoteHighlighted: currentNote
                                                   && collection.selectedGroupId.length === 0
    readonly property bool selectedGroup: groupRow
                                          && collection.selectedGroupId === groupId
    readonly property bool sourceActive: collection.sourceContains(row)
    readonly property bool directTarget: collection.directTargetContains(row)
    readonly property bool dropBefore: collection.boundaryTargets(row, false)
    readonly property bool dropAfter: collection.boundaryTargets(row, true)
    readonly property bool hierarchyGroupDrop: (dropBefore || dropAfter)
                                                 && collection.activePayload
                                                 && collection.activePayload.kind === "group"
                                                 && collection.groupDropTargetDepth >= 0
    readonly property real hierarchyDropMarkerX: 8
                                                 + Math.max(0, collection.groupDropTargetDepth) * 18
    readonly property bool dragHovered: directTarget || dropBefore || dropAfter
    readonly property bool storageDropHovered: directTarget && groupKind === "storage"
    readonly property bool selectionCheckBoxVisible: collection.touchActions && noteRow
    readonly property bool swipeDeleteAvailable: collection.touchActions
                                                  && collection.swipeDeleteEnabled
                                                  && noteRow
    readonly property bool partOfActiveDrag: sourceActive
    // TreeView keeps pooled delegates alive with their last model roles. Only
    // the presented delegate may participate in inline editing; otherwise a
    // hidden duplicate can receive/lose focus and commit the visible editor.
    readonly property bool editing: !inReusePool && visible && groupRow
                                    && collection.editingGroupId === groupId
    readonly property real reorderOffset: displacement.displacement
    readonly property real collapseSpace: displacement.collapseSpace
    readonly property real dropSpace: displacement.beforeSpace
    readonly property real dropAfterSpace: displacement.afterSpace
    readonly property string displayTitle: collection.displayTitle(row)

    objectName: collection.rowObjectName(row)
    width: collection.viewWidth
    implicitHeight: baseHeight
    leftPadding: 0
    rightPadding: collection.rowContentRightPadding
    topPadding: 0
    bottomPadding: 0
    opacity: sourceActive ? 0 : 1
    hoverEnabled: !collection.dragSelectionSuppressed
    highlighted: !collection.dragSelectionSuppressed
                 && (currentNoteHighlighted || selectedGroup)
    transform: Translate { y: row.reorderOffset }

    ToolTip.visible: !collection.dragSelectionSuppressed && hovered
                     && (errorString.length > 0 || preview.length > 0)
    ToolTip.text: errorString.length > 0 ? errorString
                  : preview

    Component.onCompleted: collection.registerRow(row)
    TableView.onPooled: row.inReusePool = true
    TableView.onReused: row.inReusePool = false
    onEditingChanged: {
        if (editing) {
            // TreeView can recycle the initially focused rename delegate
            // after the page-level focus retry has already succeeded. The
            // replacement delegate owns the same editing identity and must
            // reclaim focus once its updated roles and visibility settle.
            Qt.callLater(function() {
                if (row.editing)
                    row.focusRenameField()
            })
        }
    }
    onNoteIdChanged: closeDeleteSwipe()
    onSwipeDeleteAvailableChanged: {
        if (!swipeDeleteAvailable)
            closeDeleteSwipe()
    }
    Component.onDestruction: {
        if (internalDragActive)
            collection.cancelDrag("source-destroyed")
        collection.unregisterRow(row)
    }

    Reorder.ReorderDisplacement {
        id: displacement

        animationEnabled: collection.dragging && !collection.committingDrop
        sourceActive: row.sourceActive
        targetBefore: row.dropBefore
        targetAfter: row.dropAfter
        naturalExtent: row.baseHeight
        draggedExtent: collection.draggedExtent
        displacement: collection.rowTranslation(row)
    }

    background: Rectangle {
        radius: 4
        color: row.directTarget
               ? Qt.rgba(0.30, 0.76, 0.38, 0.32)
               : row.highlighted
               ? row.palette.highlight
               : row.noteSelected
               ? Qt.rgba(row.palette.highlight.r, row.palette.highlight.g,
                         row.palette.highlight.b, 0.38)
               : row.hovered
               ? Qt.rgba(row.palette.button.r, row.palette.button.g,
                         row.palette.button.b, 0.45)
               : "transparent"
        border.width: row.directTarget ? 2 : 0
        border.color: Qt.rgba(0.22, 0.68, 0.30, 0.95)

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: 0
            height: 3
            visible: row.dropBefore && !row.hierarchyGroupDrop
            color: row.palette.highlight
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: parent.height - height
            height: 3
            visible: row.dropAfter && !row.hierarchyGroupDrop
            color: row.palette.highlight
        }

        // Folders resolve their destination depth from horizontal movement.
        // Indenting this marker makes "becomes a child" and "stays a
        // sibling" unambiguous before the drop.  A note move deliberately
        // keeps the familiar full-width marker above.
        Rectangle {
            x: row.hierarchyDropMarkerX
            y: 0
            width: Math.max(0, parent.width - x - 6)
            height: 3
            radius: 1.5
            visible: row.dropBefore && row.hierarchyGroupDrop
            color: row.palette.highlight
        }

        Rectangle {
            x: row.hierarchyDropMarkerX
            y: parent.height - height
            width: Math.max(0, parent.width - x - 6)
            height: 3
            radius: 1.5
            visible: row.dropAfter && row.hierarchyGroupDrop
            color: row.palette.highlight
        }
    }

    contentItem: RowLayout {
        spacing: 8

        Item {
            Layout.preferredWidth: row.leadingInset
            Layout.fillHeight: true
        }

        ToolButton {
            visible: row.groupRow && collection.groupCanCollapse(row)
            enabled: visible && !row.editing
            Layout.preferredWidth: visible ? 20 : 0
            Layout.preferredHeight: 20
            padding: 2
            display: AbstractButton.IconOnly
            Accessible.name: row.groupExpanded
                             ? qsTr("Collapse %1").arg(row.title)
                             : qsTr("Expand %1").arg(row.title)
            onClicked: collection.toggleGroup(row)

            contentItem: App.ThemedIcon {
                themeName: "go-next-symbolic"
                fallbackName: "go-next-symbolic"
                recolorFallback: true
                fallbackTintMode: String(row.highlighted
                                         ? row.palette.highlightedText
                                         : row.palette.text)
                pixelSize: 15
                rotation: row.groupExpanded ? 90 : 0
            }
        }

        Item {
            visible: !row.compactFlatNoteRow
                     && !(row.groupRow && collection.groupCanCollapse(row))
            Layout.preferredWidth: visible ? 20 : 0
            Layout.preferredHeight: 20
        }

        Item {
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            Layout.alignment: Qt.AlignVCenter

            Image {
                id: suppliedIcon

                anchors.fill: parent
                source: row.iconSource
                sourceSize.width: 20
                sourceSize.height: 20
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            App.ThemedIcon {
                anchors.centerIn: parent
                visible: suppliedIcon.status !== Image.Ready
                themeName: row.systemFolder ? "__bundled__" : collection.fallbackThemeName(row)
                fallbackName: row.systemFolder ? "user-trash-full-symbolic.svg"
                                              : collection.fallbackIconName(row)
                recolorFallback: true
                fallbackTintMode: String(row.highlighted
                                         ? row.palette.highlightedText
                                         : row.palette.text)
                pixelSize: 20
            }
        }

        TextField {
            id: renameField

            objectName: collection.renameObjectName(row)
            Layout.fillWidth: true
            visible: row.editing
            enabled: visible
            text: row.title
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter
            onAccepted: row.commitRename()
            onEditingFinished: row.commitRename()
            Keys.onEscapePressed: function(event) {
                collection.cancelGroupRename(row)
                event.accepted = true
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !row.editing
            text: row.displayTitle
            font.bold: row.groupRow
            color: row.highlighted
                   ? row.palette.highlightedText
                   : (row.archived ? row.palette.placeholderText : row.palette.text)
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        App.ThemedIcon {
            visible: row.groupRow && row.favorite
            Layout.preferredWidth: visible ? 18 : 0
            Layout.preferredHeight: 18
            Layout.rightMargin: visible ? 6 : 0
            themeName: "emblem-favorite-symbolic"
            fallbackName: "pin"
            recolorFallback: true
            fallbackTintMode: String(row.highlighted
                                     ? row.palette.highlightedText
                                     : row.palette.highlight)
            pixelSize: 16
            Accessible.name: qsTr("Favorite group")
        }

        CheckBox {
            objectName: "noteSelectionCheckBox-" + row.storageId + "-" + row.noteId
            visible: collection.touchActions && row.noteRow
            checked: collection.noteIsSelected(row.storageId, row.noteId)
            Accessible.name: qsTr("Select %1").arg(row.title)
            onClicked: collection.setNoteSelected(row, checked)
        }
    }

    swipe.left: Button {
        objectName: "noteSwipeDelete-" + row.storageId + "-" + row.noteId
        // Qt 6.4 can leave a SwipeDelegate action item painted after a
        // reverse swipe even though the content has returned to its origin.
        // Keep the action instantiated for SwipeDelegate's width calculation,
        // but tie painting and input to the actual swipe position.
        visible: row.swipeDeleteAvailable
        opacity: Math.max(0, Math.min(1, row.swipe.position))
        enabled: opacity > 0.01
        width: row.swipeDeleteAvailable ? Math.max(92, implicitWidth) : 0
        height: row.height
        text: qsTr("Delete")
        onClicked: {
            row.swipe.close()
            collection.deleteNote(row)
        }
    }

    function focusRenameField() {
        if (!row.editing || !renameField.visible || !row.visible)
            return false
        renameField.forceActiveFocus()
        renameField.selectAll()
        return renameField.activeFocus
    }

    function openDeleteSwipe() {
        if (swipeDeleteAvailable)
            swipe.open(SwipeDelegate.Left)
    }

    function closeDeleteSwipe() {
        swipe.close()
    }

    function claimInteractionFocus() {
        // Pointer navigation is disabled on the owning TreeView because row
        // selection is handled explicitly. Mirror the missing focus transfer
        // so clicks still finish inline editors and leave search fields.
        if (!row.editing)
            row.forceActiveFocus(Qt.MouseFocusReason)
    }

    function commitRename() {
        // Model roles and bindings are updated in separate steps when a
        // TreeView delegate is reused. Never commit an editing value carried
        // over from its previous row identity.
        if (!editing || String(groupId) !== String(collection.editingGroupId))
            return
        const name = renameField.text.trim()
        if (name.length === 0) {
            focusRenameField()
            return
        }
        if (!collection.commitGroupRename(row, name))
            focusRenameField()
    }

    DragHandler {
        id: rowDrag

        target: null
        cursorShape: row.internalDragActive && row.collection
                     && row.collection.pointerOutsideWindow()
                     ? Qt.ForbiddenCursor : Qt.ClosedHandCursor
        enabled: Boolean(row.collection)
                 && typeof row.collection.dragEnabled === "function"
                 && row.collection.dragEnabled(row) && !row.editing
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.TouchScreen
        onActiveTranslationChanged: {
            if (active && row.collection)
                row.collection.moveDrag(row, activeTranslation.x, activeTranslation.y)
        }
        onActiveChanged: {
            if (!row.collection) {
                row.internalDragActive = false
                return
            }
            if (active) {
                row.claimInteractionFocus()
                // pressPosition is in this delegate's local coordinates.
                // Do not reconstruct it from activeTranslation: TreeView can
                // scroll between the press and its drag threshold, making the
                // two values refer to different coordinate spaces.
                row.collection.beginDrag(row,
                                         centroid.pressPosition.x,
                                         centroid.pressPosition.y)
            }
            else if (row.internalDragActive)
                row.collection.finishDrag(row)
        }
    }

    TapHandler {
        id: desktopSelectionHandler

        acceptedButtons: Qt.LeftButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: row.noteRow && !collection.touchActions
                 && !collection.dragSelectionSuppressed && !row.editing
        gesturePolicy: TapHandler.DragThreshold
        onTapped: function(eventPoint, button) {
            row.claimInteractionFocus()
            row.suppressClickUntil = Date.now() + 100
            collection.selectDesktopNote(row, desktopSelectionHandler.point.modifiers)
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        acceptedDevices: PointerDevice.Mouse
        enabled: row.noteRow && collection.embeddedEditor
                 && !collection.dragSelectionSuppressed
        onDoubleTapped: collection.openStandalone(row)
    }

    onClicked: {
        if (collection.dragSelectionSuppressed || Date.now() < suppressClickUntil) {
            suppressClickUntil = 0
            return
        }
        row.claimInteractionFocus()
        if (row.groupRow)
            collection.activateGroup(row)
        else if (collection.touchActions)
            collection.activateNote(row)
    }

    MouseArea {
        id: contextArea

        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        preventStealing: true
        onClicked: function(mouse) {
            row.claimInteractionFocus()
            collection.requestContextMenu(
                        row, contextArea.mapToItem(collection,
                                                   Qt.point(mouse.x, mouse.y)))
        }
    }

    TapHandler {
        enabled: collection.touchActions
        acceptedButtons: Qt.LeftButton
        acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
        gesturePolicy: TapHandler.DragThreshold
        onLongPressed: {
            row.claimInteractionFocus()
            row.suppressClickUntil = Date.now() + 1000
            collection.requestContextMenu(row)
        }
    }
}
