pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "../reorder" as Reorder

Item {
    id: collection

    required property var model
    property var selectionController: localSelection
    // All note collections use the same TreeView implementation.  This flag
    // describes the shape of the source model only: true for a native model
    // hierarchy, false for an already flattened projection with a depth role.
    property bool nativeModelHierarchy: false
    // Recent contains only independent note rows, unlike the flattened
    // folders projection which still has group indentation.
    property bool flatNoteRows: false
    property bool touchActions: false
    property bool embeddedEditor: true
    property int noteItemType: 1
    property string defaultGroupKind: "storage"
    property string currentStorageId: ""
    property string currentNoteId: ""
    property string selectedGroupId: ""
    property string editingGroupId: ""
    property string viewObjectName: "noteCollectionView"
    property string rowObjectNamePrefix: "noteCollectionRow-"
    property string renameObjectNamePrefix: "groupRenameField-"
    property string previewObjectName: "noteCollectionDragPreview"
    property string previewObjectNamePrefix: "noteCollectionDragPreviewItem-"
    property bool allowNoteDrag: true
    property bool allowGroupDrag: true
    property bool swipeDeleteEnabled: false
    property var displayTitleProvider: null
    property var rowObjectNameProvider: null
    property var groupCanCollapseProvider: null
    property var groupToggleHandler: null
    property var groupActivateHandler: null
    property var noteActivateHandler: null
    property var noteSelectionHandler: null
    property var noteStandaloneHandler: null
    property var noteContextHandler: null
    property var noteDeleteHandler: null
    property var groupContextHandler: null
    property var dragPayloadProvider: null
    property var groupSourceProvider: null
    property var boundaryProvider: null
    property var directTargetProvider: null
    property var targetUpdateHandler: null
    property var commitHandler: null
    // Called only when the pointer leaves the window, not merely when it
    // crosses this collection's bounds (for example into the editor pane).
    property var outsideDropHandler: null
    property var groupRenameHandler: null
    property var groupRenameCancelHandler: null
    property var fallbackThemeProvider: null
    property var fallbackIconProvider: null
    property var dragEnabledProvider: null
    property var diagnosticHandler: null

    readonly property real rowHeight: touchActions ? 44 : 34
    property real rowContentRightPadding: 8
    readonly property int verticalScrollBarInset:
        treeView.contentHeight > treeView.height
        ? Math.ceil(Math.max(verticalScrollBar.width, verticalScrollBar.implicitWidth)) : 0
    readonly property real viewWidth: Math.max(0, width - verticalScrollBarInset)
    onVerticalScrollBarInsetChanged: Qt.callLater(function() { treeView.forceLayout() })
    readonly property var activePayload: reorderController.sourcePayload
    readonly property var dropBoundary: reorderController.targetBoundary
    readonly property bool dragging: reorderController.dragging
    readonly property bool dragSelectionSuppressed: dragging
    readonly property bool committingDrop: reorderController.committingDrop
    readonly property real draggedExtent: reorderController.draggedExtent
    readonly property real dragTranslationX: reorderController.translationX
    readonly property real dragTranslationY: reorderController.translationY
    readonly property real dragStartPointerX: reorderController.startPointerX
    readonly property int previewCount: reorderController.previewCount
    readonly property var selectedNotes: selectionController.selectedNotes
    property var directDropTarget: null
    // A hierarchy-aware view may expose the resolved target depth while a
    // group is being reordered.  Rows use it only for the insertion marker;
    // ordinary note moves continue to have a simple full-width gap.
    property int groupDropTargetDepth: -1
    property int pendingFinishGeneration: 0
    property var liveRows: []
    property bool dragCursorOverridden: false

    NoteSelectionController {
        id: localSelection
    }

    Reorder.HierarchyRange {
        id: hierarchyRange

        countProvider: function() { return collection.rowCount() }
        depthProvider: function(index) {
            const item = collection.itemAtRow(index)
            return item ? Number(item.itemDepth) : 0
        }
    }

    Reorder.LinearReorderLayout {
        id: reorderLayout

        geometryItem: collection
        sourceEntries: reorderController.sourceEntries
        keyProvider: function(item) { return collection.rowKey(item) }
        orderProvider: function(item) { return Number(item.rowIndex) }
        extentProvider: function(item) { return item ? Number(item.baseHeight) : collection.rowHeight }
    }

    Reorder.GenericReorderController {
        id: reorderController

        anchors.fill: parent
        geometryItem: collection
        scrollItem: treeView
        compensateForScroll: false
        previewObjectName: collection.previewObjectName
        previewObjectNamePrefix: collection.previewObjectNamePrefix
        previewHideSources: false
        previewLive: false
        previewCompact: true
        boundaryProvider: function() {
            return typeof collection.boundaryProvider === "function"
                    ? collection.boundaryProvider(collection, collection.activePayload,
                                                  collection.visibleItems()) : []
        }
        targetChangedHandler: function(boundary, pointerX, pointerY) {
            collection.directDropTarget = typeof collection.directTargetProvider === "function"
                    ? collection.directTargetProvider(collection, collection.activePayload,
                                                      pointerX, pointerY)
                    : collection.itemAtPoint(pointerX, pointerY)
            if (typeof collection.targetUpdateHandler === "function")
                collection.targetUpdateHandler(collection, collection.activePayload,
                                               boundary, pointerX, pointerY)
        }
        commitHandler: function(payload, boundary) {
            return typeof collection.commitHandler === "function"
                    ? collection.commitHandler(payload, boundary,
                                               collection.directDropTarget, collection)
                    : false
        }
        outsideDropProvider: function() { return collection.pointerOutsideWindow() }
        outsideDropHandler: function(payload) {
            return typeof collection.outsideDropHandler === "function"
                    ? collection.outsideDropHandler(payload, collection)
                    : false
        }
        resetHandler: function() {
            collection.directDropTarget = null
            collection.groupDropTargetDepth = -1
        }
    }

    Connections {
        target: collection.model
        enabled: collection.dragging

        function onModelAboutToBeReset() {
            if (!collection.committingDrop) {
                collection.diagnose("model-reset", {
                    phase: "about-to-reset"
                })
                collection.cancelDrag("model-reset")
            }
        }
        function onRowsAboutToBeRemoved() {
            if (!collection.committingDrop) {
                collection.diagnose("model-reset", {
                    phase: "rows-about-to-be-removed"
                })
                collection.cancelDrag("rows-removed")
            }
        }
        function onLayoutAboutToBeChanged() {
            if (!collection.committingDrop) {
                collection.diagnose("model-reset", {
                    phase: "layout-about-to-change"
                })
                collection.cancelDrag("layout-change")
            }
        }
    }

    TreeView {
        id: treeView

        readonly property var collectionOwner: collection

        objectName: collection.viewObjectName
        anchors.fill: parent
        clip: true
        model: collection.model
        pointerNavigationEnabled: false
        reuseItems: true
        rowSpacing: 1
        boundsBehavior: Flickable.StopAtBounds
        bottomMargin: collection.touchActions ? 88 : 0
        ScrollBar.vertical: ScrollBar { id: verticalScrollBar }
        columnWidthProvider: function(column) { return Math.floor(collection.viewWidth) }
        rowHeightProvider: function(row) { return collection.rowHeight }
        onWidthChanged: Qt.callLater(function() { treeView.forceLayout() })
        Component.onCompleted: Qt.callLater(function() {
            if (collection.nativeModelHierarchy && treeView.model)
                expandRecursively(-1, 1)
        })
        delegate: TreeNoteListRow {
            collection: treeView.collectionOwner
        }
    }

    function role(data, name, fallback) {
        if (!data || data[name] === undefined || data[name] === null)
            return fallback
        return data[name]
    }

    function stringRole(data, name, fallback) {
        const value = role(data, name, fallback === undefined ? "" : fallback)
        return String(value === undefined || value === null ? "" : value)
    }

    function numberRole(data, name, fallback) {
        return Number(role(data, name, fallback === undefined ? 0 : fallback))
    }

    function boolRole(data, name, fallback) {
        return Boolean(role(data, name, fallback === undefined ? false : fallback))
    }

    function rowCount() {
        return treeView.rows
    }

    function itemAtRow(row) {
        for (const item of liveRows) {
            if (item && Number(item.rowIndex) === Number(row))
                return item
        }
        return treeView.itemAtCell(Qt.point(0, row))
    }

    function revealRow(row) {
        const targetRow = Number(row)
        if (targetRow < 0 || targetRow >= rowCount())
            return false
        treeView.positionViewAtCell(Qt.point(0, targetRow), Qt.AlignVCenter)
        return true
    }

    function expandGroups(depth) {
        if (nativeModelHierarchy && treeView.model)
            treeView.expandRecursively(-1, depth === undefined ? 1 : Number(depth))
    }

    function visibleItems() {
        const result = []
        for (const item of liveRows) {
            if (item && item.visible && item.width > 0 && item.height > 0)
                result.push(item)
        }
        result.sort(function(left, right) { return Number(left.rowIndex) - Number(right.rowIndex) })
        return result
    }

    function registerRow(item) {
        if (!item || liveRows.indexOf(item) >= 0)
            return
        const copy = liveRows.slice()
        copy.push(item)
        liveRows = copy
    }

    function diagnose(event, details) {
        if (typeof diagnosticHandler === "function")
            diagnosticHandler(String(event), details || {})
    }

    function unregisterRow(item) {
        const index = liveRows.indexOf(item)
        if (index < 0)
            return
        const copy = liveRows.slice()
        copy.splice(index, 1)
        liveRows = copy
    }

    function itemAtPoint(pointerX, pointerY) {
        for (const item of visibleItems()) {
            const local = item.mapFromItem(collection, pointerX, pointerY)
            // ReorderDisplacement translates delegates to open and close the
            // insertion gap. Hit testing that animated geometry makes a
            // direct target disappear underneath a stationary pointer once
            // the animation catches up. Translate the point back into the
            // row's stable layout geometry while a drag is active.
            const logicalY = local.y + (dragging ? item.reorderOffset : 0)
            if (local.x >= 0 && logicalY >= 0
                    && local.x < item.width && logicalY < item.baseHeight) {
                return item
            }
        }
        return null
    }

    function rowKey(item) {
        if (!item)
            return ""
        return item.noteRow
                ? "note:" + String(item.storageId) + "\n" + String(item.noteId)
                : "group:" + String(item.groupKind) + ":" + String(item.groupId)
    }

    function itemForKey(key) {
        for (const item of visibleItems()) {
            if (rowKey(item) === String(key))
                return item
        }
        return null
    }

    function rowObjectName(item) {
        return typeof rowObjectNameProvider === "function"
                ? String(rowObjectNameProvider(item))
                : rowObjectNamePrefix + rowKey(item).replace("\n", "-")
    }

    function renameObjectName(item) {
        return renameObjectNamePrefix + String(item ? item.groupId : "")
    }

    function displayTitle(item) {
        if (typeof displayTitleProvider === "function")
            return String(displayTitleProvider(item))
        if (!item.groupRow)
            return item.title
        return item.loading
                ? qsTr("%1 — loading…").arg(item.title)
                : qsTr("%1 (%2)").arg(item.title).arg(item.noteCount)
    }

    function fallbackThemeName(item) {
        if (typeof fallbackThemeProvider === "function")
            return String(fallbackThemeProvider(item))
        if (item.groupKind === "folder" || item.groupKind === "unsorted")
            return "folder-symbolic"
        if (item.groupKind === "storage")
            return "drive-harddisk-symbolic"
        return "text-x-generic-symbolic"
    }

    function fallbackIconName(item) {
        if (typeof fallbackIconProvider === "function")
            return String(fallbackIconProvider(item))
        return item.groupKind === "folder" || item.groupKind === "unsorted"
                ? "folder-symbolic" : "anykeep-symbolic"
    }

    function groupCanCollapse(item) {
        if (!item || !item.groupRow)
            return false
        return typeof groupCanCollapseProvider === "function"
                ? Boolean(groupCanCollapseProvider(item)) : item.groupHasChildren
    }

    function toggleGroup(item) {
        if (typeof groupToggleHandler === "function") {
            groupToggleHandler(item)
            return
        }
        if (nativeModelHierarchy)
            treeView.toggleExpanded(item.rowIndex)
    }

    function activateGroup(item) {
        // Group and note selections are mutually exclusive.  Leaving a note
        // selected while its parent group is activated paints both states and
        // makes the current action ambiguous.
        if (selectionController && typeof selectionController.clear === "function")
            selectionController.clear()
        if (typeof groupActivateHandler === "function")
            groupActivateHandler(item)
        else
            toggleGroup(item)
    }

    function activateNote(item) {
        selectionController.select(item, Qt.NoModifier, visibleItems())
        if (typeof noteSelectionHandler === "function")
            noteSelectionHandler(item)
        if (typeof noteActivateHandler === "function")
            noteActivateHandler(item)
    }

    function selectDesktopNote(item, modifiers) {
        const activate = selectionController.select(item, modifiers, visibleItems())
        if (typeof noteSelectionHandler === "function")
            noteSelectionHandler(item)
        if (activate && typeof noteActivateHandler === "function")
            noteActivateHandler(item)
    }

    function noteIsSelected(storageId, noteId) {
        return selectionController.isSelected(storageId, noteId)
    }

    function setNoteSelected(item, selected) {
        selectionController.setSelected(item, selected)
        if (typeof noteSelectionHandler === "function")
            noteSelectionHandler(item)
    }

    function openStandalone(item) {
        if (typeof noteStandaloneHandler === "function")
            noteStandaloneHandler(item)
    }

    function requestContextMenu(item, position) {
        if (item.noteRow && typeof noteContextHandler === "function") {
            // Match the desktop file-manager convention: opening a context
            // menu on an unselected row first makes that row the sole
            // selection, while right-clicking any member of an existing
            // multi-selection preserves the whole selection.
            if (!selectionController.isSelected(item.storageId, item.noteId)) {
                selectionController.select(item, Qt.NoModifier, visibleItems())
                if (typeof noteSelectionHandler === "function")
                    noteSelectionHandler(item)
            }
            noteContextHandler(item, position)
        } else if (item.groupRow && typeof groupContextHandler === "function") {
            groupContextHandler(item, position)
        }
    }

    function deleteNote(item) {
        if (item && item.noteRow && typeof noteDeleteHandler === "function")
            noteDeleteHandler(item)
    }

    function commitGroupRename(item, name) {
        return typeof groupRenameHandler === "function"
                ? Boolean(groupRenameHandler(item, name)) : false
    }

    function cancelGroupRename(item) {
        if (typeof groupRenameCancelHandler === "function")
            groupRenameCancelHandler(item)
    }

    function dragEnabled(item) {
        if (!item)
            return false
        const allowed = item.noteRow ? allowNoteDrag : allowGroupDrag
        return allowed && (typeof dragEnabledProvider !== "function"
                           || Boolean(dragEnabledProvider(item)))
    }

    function subtreeItems(item) {
        if (!item)
            return []
        const end = hierarchyRange.subtreeEnd(item.rowIndex)
        const result = []
        for (let row = item.rowIndex; row < end; ++row) {
            const candidate = itemAtRow(row)
            if (candidate)
                result.push(candidate)
        }
        return result
    }

    function subtreeEndRow(index) {
        return hierarchyRange.subtreeEnd(index)
    }

    function beginDrag(item, pointerLocalX, pointerLocalY) {
        if (!item || dragging || !dragEnabled(item))
            return false

        let sourceItems = item.groupRow
                ? (typeof groupSourceProvider === "function"
                   ? groupSourceProvider(item, visibleItems()) : subtreeItems(item))
                : selectionController.sourceItemsForDrag(item, visibleItems())
        sourceItems.sort(function(left, right) { return Number(left.rowIndex) - Number(right.rowIndex) })
        const sources = []
        for (const sourceItem of sourceItems) {
            sources.push({
                item: sourceItem,
                key: rowKey(sourceItem),
                order: sourceItem.rowIndex,
                previewItem: sourceItem,
                geometryItem: sourceItem,
                naturalExtent: sourceItem.baseHeight,
                previewWidth: sourceItem.width,
                previewHeight: sourceItem.baseHeight
            })
        }

        let payload = item.noteRow ? {
            kind: "notes",
            sourceItem: item,
            notes: selectionController.notesForDrag(item, visibleItems())
        } : {
            kind: "group",
            sourceItem: item,
            groupKind: item.groupKind,
            groupId: item.groupId,
            sourceRows: sourceItems.map(function(source) { return source.rowIndex }),
            descendantGroupIds: sourceItems.filter(function(source) { return source.groupRow })
                                           .map(function(source) { return source.groupId })
        }
        if (typeof dragPayloadProvider === "function")
            payload = dragPayloadProvider(item, payload, sourceItems) || payload

        const started = reorderController.beginDrag({
            sources: sources,
            payload: payload,
            pointerItem: item,
            pointerLocalX: pointerLocalX === undefined ? item.width / 2 : pointerLocalX,
            pointerLocalY: pointerLocalY === undefined ? item.baseHeight / 2 : pointerLocalY,
            targetByDraggedTop: true
        })
        item.internalDragActive = started
        diagnose("begin", {
            started: started,
            sourceKey: rowKey(item),
            kind: payload.kind,
            sourceCount: sources.length,
            noteCount: payload.notes ? payload.notes.length : 0
        })
        if (started) {
            item.suppressClickUntil = Date.now() + 500
            selectionController.clear()
            updateDragCursor()
        }
        return started
    }

    function moveDrag(item, dx, dy) {
        if (item && item.internalDragActive) {
            reorderController.moveDrag(dx, dy)
            updateDragCursor()
        }
    }

    function finishDrag(item) {
        if (!item || !item.internalDragActive)
            return
        item.internalDragActive = false
        restoreDragCursor()
        const generation = ++pendingFinishGeneration
        diagnose("finish-scheduled", {
            generation: generation,
            boundaryKey: dropBoundary ? String(dropBoundary.ownerKey || "") : "",
            directTargetKey: directDropTarget ? rowKey(directDropTarget) : "",
            outsideWindow: pointerOutsideWindow()
        })
        // A folder assignment resets its projection model synchronously. Do
        // not destroy the source delegate from inside DragHandler's own
        // activeChanged callback; commit after pointer delivery unwinds.
        Qt.callLater(function() {
            if (generation === pendingFinishGeneration && reorderController.dragging) {
                const committed = reorderController.finishDrag()
                collection.diagnose("finish-completed", {
                    generation: generation,
                    committed: committed
                })
            } else {
                collection.diagnose("finish-skipped", {
                    generation: generation,
                    currentGeneration: pendingFinishGeneration,
                    dragging: reorderController.dragging
                })
            }
        })
    }

    function cancelDrag(reason) {
        diagnose("cancel", {
            reason: String(reason || "requested"),
            dragging: reorderController.dragging
        })
        ++pendingFinishGeneration
        if (activePayload && activePayload.sourceItem)
            activePayload.sourceItem.internalDragActive = false
        restoreDragCursor()
        reorderController.cancelDrag()
    }

    function pointerOutsideWindow() {
        const window = Window.window
        if (!window)
            return false
        const point = mapToItem(null, reorderController.currentPointerX,
                                reorderController.currentPointerY)
        return point.x < 0 || point.y < 0 || point.x > window.width || point.y > window.height
    }

    function updateDragCursor() {
        if (!dragging || typeof anykeepCursor === "undefined" || !anykeepCursor)
            return
        if (pointerOutsideWindow())
            anykeepCursor.setTrashCursor()
        else
            anykeepCursor.setOverrideCursor(Qt.ClosedHandCursor)
        dragCursorOverridden = true
    }

    function restoreDragCursor() {
        if (!dragCursorOverridden)
            return
        if (typeof anykeepCursor !== "undefined" && anykeepCursor)
            anykeepCursor.restoreOverrideCursor()
        dragCursorOverridden = false
    }

    Component.onDestruction: restoreDragCursor()

    function sourceContains(item) {
        return reorderLayout.containsSource(item)
    }

    function directTargetContains(item) {
        return Boolean(item && directDropTarget
                       && rowKey(item) === rowKey(directDropTarget))
    }

    function boundaryTargets(item, after) {
        return Boolean(item && dropBoundary
                       && dropBoundary.ownerKey === rowKey(item)
                       && Boolean(dropBoundary.afterOwner) === Boolean(after)
                       && !directTargetContains(item))
    }

    function rowTranslation(item) {
        return reorderLayout.translationByOrder(item, dropBoundary, draggedExtent)
    }

    function boundaryByOrder(item, after, payload) {
        return reorderLayout.boundaryByOrder(item, after, payload || {}, 0, true)
    }

    function boundaries(items, payloadProvider) {
        return reorderLayout.boundaries(items || visibleItems(), payloadProvider)
    }

    function trailingBoundaries(items, payloadProvider, includeLeading) {
        return reorderLayout.trailingBoundaries(
                    items || visibleItems(), payloadProvider, includeLeading)
    }

    function remainingItems(items) {
        return reorderLayout.remainingItems(items || visibleItems())
    }

    function sourceExtentBefore(item) {
        return reorderLayout.sourceExtentBefore(item)
    }
}
