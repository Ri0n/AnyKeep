pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "notelist" as NoteList
import "reorder" as Reorder

Item {
    id: root

    required property var workspace
    property bool touchActions: false
    property bool embeddedEditor: true
    property real toolbarRightMargin: 0
    property string currentStorageId: ""
    property string currentNoteId: ""
    property var checkpointHandler: null
    property var outsideNotesDropHandler: null
    property var selectionController: localSelection
    property string selectedFolderId: ""
    property bool unsortedSelected: false
    property string editingFolderId: ""
    property string contextFolderId: ""
    property string contextFolderTitle: ""
    property bool contextFolderCollapsed: false
    property bool contextFolderFavorite: false
    property bool contextFolderArchived: false
    property bool contextFolderSystem: false
    property int contextFolderNoteCount: 0

    readonly property var selectedNotes: folderList.selectedNotes
    readonly property bool dragging: folderList.dragging
    readonly property int previewCount: folderList.previewCount
    readonly property bool dragSelectionSuppressed: folderList.dragSelectionSuppressed
    readonly property real draggedExtent: folderList.draggedExtent
    readonly property bool committingDrop: folderList.committingDrop

    signal noteActivated(string storageId, string noteId, string title)
    signal noteStandaloneRequested(string storageId, string noteId)
    signal noteMenuRequested(string storageId, string noteId, string title, point position)

    NoteList.NoteSelectionController {
        id: localSelection
    }

    Reorder.HierarchyDropPolicy {
        id: hierarchyDropPolicy
    }

    function normalizedFolderId(value) {
        const folderId = String(value || "")
        // QModel roles normally expose Unsorted as an empty string.  Treat a
        // serialized null QUuid equivalently: some delegates can retain it
        // briefly while their model row changes during an animated drop.
        return folderId === "00000000-0000-0000-0000-000000000000" ? "" : folderId
    }

    function assignmentFolderFor(item) {
        if (!item || item.groupKind === "unsorted")
            return ""
        return normalizedFolderId(item.folderId)
    }

    function noteDropBoundaries(view, payload, items) {
        return view.trailingBoundaries(items, function(item) {
            return {
                assignmentFolderId: item
                        ? root.assignmentFolderFor(item) : ""
            }
        }, true)
    }

    function remainingFolderGroups(view, items) {
        return view.remainingItems(items || view.visibleItems()).filter(
                    function(item) {
                        return item.groupRow && item.groupKind === "folder"
                    })
    }

    function folderDropBoundaries(view, payload, items) {
        const groups = remainingFolderGroups(view, items)
        return view.trailingBoundaries(items, function(item) {
            let insertion = 0
            if (item) {
                while (insertion < groups.length
                       && Number(groups[insertion].rowIndex)
                          <= Number(item.rowIndex)) {
                    ++insertion
                }
            }
            return {
                groupInsertionIndex: insertion,
                parentFolderId: "",
                beforeFolderId: groups.length > 0
                        ? String(groups[0].folderId) : "",
                targetDepth: 0
            }
        }, true)
    }

    function dropBoundaries(view, payload, items) {
        return payload && payload.kind === "group"
                ? folderDropBoundaries(view, payload, items)
                : noteDropBoundaries(view, payload, items)
    }

    function updateDropTarget(view, payload, boundary, pointerX, pointerY) {
        view.groupDropTargetDepth = -1
        if (!payload || payload.kind !== "group" || !boundary)
            return
        const groups = remainingFolderGroups(view, view.visibleItems())
        const insertion = Math.max(
                    0, Math.min(groups.length,
                                Number(boundary.groupInsertionIndex || 0)))
        const previous = insertion > 0 ? groups[insertion - 1] : null
        const maximumDepth = previous ? Number(previous.itemDepth) + 1 : 0
        const targetDepth = hierarchyDropPolicy.depthFromDrag(
                    pointerX, view.dragStartPointerX,
                    Number(payload.sourceDepth || 0), 18,
                    maximumDepth, false)
        const target = hierarchyDropPolicy.treeTarget(
                    groups, insertion, targetDepth,
                    function(item) { return item.folderId },
                    function(item) { return item.parentFolderId },
                    function(item) { return item.itemDepth })
        boundary.parentFolderId = target.parentId
        boundary.beforeFolderId = target.beforeId
        boundary.targetDepth = target.depth
        view.groupDropTargetDepth = target.depth
    }

    function canMoveFolderTo(payload, parentFolderId, beforeFolderId) {
        if (!payload || String(payload.groupId).length === 0)
            return false
        return payload.descendantGroupIds.indexOf(String(parentFolderId)) < 0
                && payload.descendantGroupIds.indexOf(String(beforeFolderId)) < 0
    }

    function commitDrop(payload, boundary, directTarget) {
        if (!payload) {
            console.warn("[folder-dnd] commit rejected: missing payload")
            return false
        }
        if (payload.kind === "group") {
            const parentFolderId = directTarget
                    ? String(directTarget.folderId) : String(boundary ? boundary.parentFolderId || "" : "")
            const beforeFolderId = directTarget
                    ? "" : String(boundary ? boundary.beforeFolderId || "" : "")
            if (!canMoveFolderTo(payload, parentFolderId, beforeFolderId)) {
                console.warn("[folder-dnd] folder move rejected",
                             "folder=", payload.groupId,
                             "parent=", parentFolderId,
                             "before=", beforeFolderId)
                return false
            }
            const moved = workspace.moveFolderBefore(payload.groupId, parentFolderId, beforeFolderId)
            console.info("[folder-dnd] folder move requested",
                         "folder=", payload.groupId,
                         "parent=", parentFolderId,
                         "before=", beforeFolderId,
                         "accepted=", moved)
            return moved
        }

        if (payload.kind !== "notes" || !payload.notes || payload.notes.length === 0) {
            console.warn("[folder-dnd] note assignment rejected: empty note payload",
                         "kind=", payload.kind)
            return false
        }
        const folderId = directTarget
                ? assignmentFolderFor(directTarget)
                : root.normalizedFolderId(boundary ? boundary.assignmentFolderId : "")
        console.info("[folder-dnd] committing note assignment",
                     "notes=", payload.notes.length,
                     "folder=", folderId,
                     "directTarget=", directTarget ? folderList.rowKey(directTarget) : "",
                     "boundary=", boundary ? String(boundary.ownerKey || "") : "")
        if (workspace.currentEditor && typeof checkpointHandler === "function"
                && !checkpointHandler()) {
            console.warn("[folder-dnd] note assignment rejected: editor checkpoint failed")
            return false
        }

        let changed = false
        for (const note of payload.notes) {
            const accepted = workspace.assignNoteFolder(note.storageId, note.noteId, folderId)
            console.info("[folder-dnd] assignment request",
                         "storage=", note.storageId,
                         "note=", note.noteId,
                         "folder=", folderId,
                         "accepted=", accepted)
            if (accepted)
                changed = true
        }
        return changed
    }

    function trashDroppedItems(payload) {
        if (!payload)
            return false
        if (payload.kind === "group") {
            const deleted = workspace.trashFolder(String(payload.groupId || ""))
            if (deleted) {
                selectionController.clear()
                selectedFolderId = ""
                unsortedSelected = false
                editingFolderId = ""
            }
            return deleted
        }
        if (payload.kind !== "notes" || !payload.notes || payload.notes.length === 0)
            return false
        if (typeof outsideNotesDropHandler === "function")
            return Boolean(outsideNotesDropHandler(payload.notes))
        if (workspace.currentEditor && typeof checkpointHandler === "function"
                && !checkpointHandler()) {
            return false
        }

        let changed = false
        for (const note of payload.notes) {
            if (workspace.trashNote(note.storageId, note.noteId))
                changed = true
        }
        return changed
    }

    function createFolder(parentFolderId) {
        const parentId = String(parentFolderId || "")
        const created = workspace.createFolder("", parentId)
        if (created.length === 0)
            return
        if (parentId.length > 0)
            workspace.setFolderCollapsed(parentId, false)
        selectedFolderId = created
        unsortedSelected = false
        beginFolderRename(created)
    }

    function focusFolderRename(folderId, attemptsRemaining) {
        if (editingFolderId !== String(folderId))
            return
        const row = workspace.folderNotesModel.rowForFolder(editingFolderId)
        if (row < 0)
            return
        folderList.revealRow(row)
        Qt.callLater(function() {
            if (editingFolderId !== String(folderId))
                return
            const item = folderList.itemAtRow(row)
            if (item) {
                item.focusRenameField()
            } else if (attemptsRemaining > 0) {
                root.focusFolderRename(folderId, attemptsRemaining - 1)
            }
        })
    }

    function beginFolderRename(folderId) {
        editingFolderId = String(folderId)
        focusFolderRename(editingFolderId, 2)
    }

    function cancelFolderRename(folderId) {
        if (editingFolderId === String(folderId))
            editingFolderId = ""
    }

    function createNoteInSelectedFolder() {
        if (workspace.currentEditor && typeof checkpointHandler === "function"
                && !checkpointHandler()) {
            return false
        }
        return workspace.createNoteInFolder(selectedFolderId)
    }

    function emptyRecycleBin() {
        if (workspace.currentEditor && typeof checkpointHandler === "function"
                && !checkpointHandler()) {
            return false
        }
        return workspace.emptyRecycleBin()
    }

    function trashContextFolder() {
        const folderId = contextFolderId
        if (!workspace.trashFolder(folderId))
            return false
        selectionController.clear()
        selectedFolderId = ""
        editingFolderId = ""
        unsortedSelected = false
        return true
    }

    function mapMenuPosition(position) {
        return position === undefined ? undefined : folderList.mapToItem(root, position)
    }

    function showFolderMenu(item, position) {
        contextFolderId = String(item.folderId)
        contextFolderTitle = String(item.title)
        contextFolderCollapsed = Boolean(item.groupCollapsed)
        contextFolderFavorite = Boolean(item.favorite)
        contextFolderArchived = Boolean(item.archived)
        contextFolderSystem = Boolean(item.systemFolder)
        contextFolderNoteCount = Number(item.noteCount || 0)
        const mapped = mapMenuPosition(position)
        const menu = contextFolderSystem ? recycleBinContextMenu : folderContextMenu
        if (mapped === undefined)
            menu.popup()
        else
            menu.popup(root, mapped)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        Pane {
            Layout.fillWidth: true
            Layout.rightMargin: root.toolbarRightMargin
            Layout.preferredHeight: toolbarRow.implicitHeight + topPadding + bottomPadding
            padding: 3

            RowLayout {
                id: toolbarRow

                anchors.fill: parent
                spacing: 3

                ToolButton {
                    display: AbstractButton.IconOnly
                    enabled: root.workspace.folderCatalogAvailable
                    contentItem: Item {
                        implicitWidth: 22
                        implicitHeight: 22

                        Image {
                            anchors.centerIn: parent
                            width: 22
                            height: 22
                            source: "qrc:/icons/new"
                            sourceSize.width: 22
                            sourceSize.height: 22
                            fillMode: Image.PreserveAspectFit
                        }
                    }
                    Accessible.name: qsTr("New note in selected folder")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.createNoteInSelectedFolder()
                }

                ToolButton {
                    display: AbstractButton.IconOnly
                    enabled: root.workspace.folderCatalogAvailable
                    contentItem: ThemedIcon {
                        themeName: "folder-new-symbolic"
                        fallbackName: "folder-symbolic"
                        recolorFallback: true
                        fallbackTintMode: "auto"
                        pixelSize: 21
                    }
                    Accessible.name: qsTr("New folder")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.createFolder("")
                }

                ToolButton {
                    display: AbstractButton.IconOnly
                    enabled: root.workspace.folderCatalogAvailable
                    contentItem: ThemedIcon {
                        themeName: "view-collapse-symbolic"
                        fallbackName: "go-next-symbolic"
                        recolorFallback: true
                        fallbackTintMode: "auto"
                        pixelSize: 18
                        rotation: -90
                    }
                    Accessible.name: qsTr("Collapse all folders")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.workspace.collapseAllFolders()
                }

                ToolButton {
                    display: AbstractButton.IconOnly
                    enabled: Boolean(root.workspace["canUndoFolderTrash"] || false)
                    contentItem: ThemedIcon {
                        themeName: "edit-undo-symbolic"
                        fallbackName: "edit-undo-symbolic.svg"
                        recolorFallback: true
                        fallbackTintMode: "auto"
                        pixelSize: 18
                    }
                    Accessible.name: String(root.workspace["lastTrashedFolderName"] || "").length > 0
                                     ? qsTr("Undo deleting folder “%1”")
                                           .arg(String(root.workspace["lastTrashedFolderName"]))
                                     : qsTr("Undo deleting folder")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.workspace.undoFolderTrash()
                }

                Item { Layout.fillWidth: true }

                Label {
                    visible: root.selectedFolderId.length > 0
                    text: qsTr("Selected folder")
                    color: palette.placeholderText
                    elide: Text.ElideRight
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            NoteList.NoteCollectionView {
                id: folderList

                anchors.fill: parent
                model: root.workspace.folderNotesModel
                selectionController: root.selectionController
                nativeModelHierarchy: false
                touchActions: root.touchActions
                embeddedEditor: root.embeddedEditor
                defaultGroupKind: "folder"
                currentStorageId: root.currentStorageId
                currentNoteId: root.currentNoteId
                selectedGroupId: root.selectedFolderId.length > 0
                                 ? root.selectedFolderId
                                 : (root.unsortedSelected ? "unsorted" : "")
                editingGroupId: root.editingFolderId
                viewObjectName: "foldersList"
                previewObjectName: "folderDragPreview"
                previewObjectNamePrefix: "folderDragPreviewItem-"
                renameObjectNamePrefix: "folderRenameField-"
                rowObjectNameProvider: function(item) {
                    return item.groupRow
                            ? "foldersRow-folder-" + item.groupId
                            : "foldersRow-note-" + item.storageId + "-" + item.noteId
                }
                displayTitleProvider: function(item) {
                    return item.groupRow
                            ? qsTr("%1 (%2)").arg(item.title).arg(item.noteCount)
                            : item.title
                }
                groupCanCollapseProvider: function(item) {
                    return item.groupKind === "folder" || item.groupKind === "unsorted"
                }
                groupToggleHandler: function(item) {
                    if (item.groupKind === "folder")
                        root.workspace.setFolderCollapsed(item.folderId, !item.groupCollapsed)
                    else if (item.groupKind === "unsorted")
                        root.workspace.setUnsortedCollapsed(!item.groupCollapsed)
                }
                groupActivateHandler: function(item) {
                    root.selectedFolderId = item.groupKind === "unsorted" ? "" : item.folderId
                    root.unsortedSelected = item.groupKind === "unsorted"
                    if (item.groupKind === "folder")
                        root.workspace.setFolderCollapsed(item.folderId, !item.groupCollapsed)
                    else if (item.groupKind === "unsorted")
                        root.workspace.setUnsortedCollapsed(!item.groupCollapsed)
                }
                noteSelectionHandler: function(item) {
                    // A note selection is independent from a group selection.
                    // Selecting the containing folder here highlights both rows
                    // and makes the toolbar action look as if the folder, not
                    // the note, was selected.
                    root.selectedFolderId = ""
                    root.unsortedSelected = false
                }
                noteActivateHandler: function(item) {
                    root.noteActivated(item.storageId, item.noteId, item.title)
                }
                noteStandaloneHandler: function(item) {
                    root.noteStandaloneRequested(item.storageId, item.noteId)
                }
                noteContextHandler: function(item, position) {
                    const mapped = root.mapMenuPosition(position)
                    root.noteMenuRequested(item.storageId, item.noteId, item.title,
                                           mapped === undefined ? Qt.point(item.width / 2, item.height / 2)
                                                                : mapped)
                }
                groupContextHandler: function(item, position) {
                    if (item.groupKind === "folder")
                        root.showFolderMenu(item, position)
                }
                dragEnabledProvider: function(item) {
                    return root.workspace.folderCatalogAvailable
                            && (item.noteRow || (item.groupKind === "folder" && !item.systemFolder))
                }
                dragPayloadProvider: function(item, payload) {
                    if (payload.kind === "group") {
                        payload.groupId = item.folderId
                        payload.sourceDepth = item.itemDepth
                        payload.descendantGroupIds = payload.descendantGroupIds.filter(
                                    function(id) { return String(id).length > 0 && id !== "unsorted" })
                    }
                    return payload
                }
                boundaryProvider: root.dropBoundaries
                directTargetProvider: function() { return null }
                targetUpdateHandler: root.updateDropTarget
                commitHandler: root.commitDrop
                outsideDropHandler: root.trashDroppedItems
                groupRenameHandler: function(item, name) {
                    if (!root.workspace.renameFolder(item.folderId, name))
                        return false
                    root.editingFolderId = ""
                    return true
                }
                groupRenameCancelHandler: function(item) {
                    root.cancelFolderRename(item.folderId)
                }
                diagnosticHandler: function(event, details) {
                    console.info("[folder-dnd]", event, JSON.stringify(details))
                }
            }

            Label {
                anchors.centerIn: parent
                visible: folderList.rowCount() <= 1
                         && root.workspace.noteCount === 0
                         && root.workspace.folderCatalogAvailable
                width: Math.min(parent.width - 32, 360)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: palette.mid
                text: qsTr("No notes or folders yet. Create a folder or a note to get started.")
            }

            Label {
                anchors.centerIn: parent
                visible: !root.workspace.folderCatalogAvailable
                width: Math.min(parent.width - 32, 420)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: palette.brightText
                text: qsTr("Folders are unavailable until the encrypted folder catalog is recovered.")
            }
        }
    }

    Menu {
        id: folderContextMenu
        objectName: "folderContextMenu"

        modal: true
        dim: false
        focus: true
        width: root.touchActions ? Math.min(280, root.width - 32) : implicitWidth

        MenuItem {
            text: qsTr("New subfolder")
            onTriggered: root.createFolder(root.contextFolderId)
        }
        MenuItem {
            text: qsTr("Rename")
            onTriggered: root.beginFolderRename(root.contextFolderId)
        }
        MenuItem {
            text: root.contextFolderCollapsed ? qsTr("Expand") : qsTr("Collapse")
            onTriggered: root.workspace.setFolderCollapsed(root.contextFolderId,
                                                            !root.contextFolderCollapsed)
        }
        MenuSeparator { }
        MenuItem {
            text: root.contextFolderFavorite ? qsTr("Remove from favorites") : qsTr("Add to favorites")
            onTriggered: root.workspace.setFolderFlags(root.contextFolderId,
                                                        !root.contextFolderFavorite,
                                                        root.contextFolderArchived)
        }
        MenuItem {
            text: root.contextFolderArchived ? qsTr("Show in menu") : qsTr("Hide from menu")
            onTriggered: root.workspace.setFolderFlags(root.contextFolderId,
                                                        root.contextFolderFavorite,
                                                        !root.contextFolderArchived)
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Move to Recycle Bin")
            onTriggered: root.trashContextFolder()
        }
    }

    Menu {
        id: recycleBinContextMenu
        objectName: "recycleBinContextMenu"

        modal: true
        dim: false
        focus: true
        width: root.touchActions ? Math.min(280, root.width - 32) : implicitWidth

        MenuItem {
            objectName: "emptyRecycleBinAction"
            enabled: root.contextFolderNoteCount > 0
            text: qsTr("Empty Recycle Bin")
            onTriggered: emptyRecycleBinDialog.open()
        }
    }

    Dialog {
        id: emptyRecycleBinDialog

        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        height: 150
        title: qsTr("Empty Recycle Bin")
        standardButtons: Dialog.Yes | Dialog.No

        contentItem: Label {
            wrapMode: Text.WordWrap
            text: qsTr("Permanently delete all notes in the Recycle Bin? This cannot be undone.")
        }

        onAccepted: root.emptyRecycleBin()
    }
}
