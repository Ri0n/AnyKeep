pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "notelist" as NoteList

Item {
    id: root

    readonly property int recentMode: 0
    readonly property int groupedByStorageMode: 1
    readonly property int foldersMode: 2

    required property var workspace
    property var platformBackend: null
    property var desktopActions: null
    property var speechController: null
    property bool embeddedEditor: true
    property bool showCreateButton: true
    property bool showViewModeSelector: true
    property bool touchActions: false
    // An embedding may disable confirmation (the mobile app binds this to
    // its settings); the desktop reads the stored setting at deletion time.
    property bool confirmDelete: true
    property bool compact: width < 760
    property int viewMode: embeddedEditor ? groupedByStorageMode : recentMode
    property real navigationWidth: 340
    property string selectedStorageId: ""
    property string selectedNoteId: ""
    property string selectedTitle: ""
    property string pendingBodyFindStorageId: ""
    property string pendingBodyFindNoteId: ""
    property string pendingBodyFindQuery: ""
    property bool editorFocusOwned: false
    property bool mobileSearchExpanded: false
    property var pendingPermanentDeletionNotes: []
    property var pendingRecycleNotes: []
    property var contextMenuNotes: []
    property bool dontAskAgainForPermanentDeletion: false
    readonly property var selectedNotes: noteSelection.selectedNotes
    readonly property var activeDragDelegate: groupedNotes.activePayload
    readonly property var dropBoundary: groupedNotes.dropBoundary
    readonly property var dropTargetDelegate: dropBoundary
                                              && dropBoundary.ownerKey !== undefined
                                              ? groupedNotes.itemForKey(dropBoundary.ownerKey)
                                              : groupedNotes.directDropTarget
    readonly property bool dropOnStorageHeader: Boolean(groupedNotes.directDropTarget
                                                        && groupedNotes.directDropTarget.groupRow)
    readonly property bool dropTargetAfter: dropBoundary
                                            ? Boolean(dropBoundary.afterOwner) : false
    readonly property real dragTranslationX: groupedNotes.dragTranslationX
    readonly property real dragTranslationY: groupedNotes.dragTranslationY
    readonly property real draggedExtent: groupedNotes.draggedExtent
    readonly property bool committingDrop: groupedNotes.committingDrop
    readonly property bool dragSelectionSuppressed: groupedNotes.dragging
    readonly property int draggedItemType: activeDragDelegate
                                            ? (activeDragDelegate.kind === "group" ? 0 : 1) : -1
    readonly property string activeDraggedNoteId: activeDragDelegate
                                                   && draggedItemType === 1
                                                   && activeDragDelegate.notes.length > 0
                                                   ? activeDragDelegate.notes[0].noteId : ""
    readonly property bool searchExpanded: !touchActions || mobileSearchExpanded
                                           || workspace.searchText.length > 0 || workspace.searchInBody
    readonly property bool searchOptionsVisible: searchField.activeFocus || searchInTextCheckBox.pressed

    NoteList.NoteSelectionController {
        id: noteSelection
    }

    component CompactContextMenuItem: MenuItem {
        implicitHeight: visible ? (root.touchActions ? 40 : 32) : 0
    }

    component CompactContextSeparator: MenuSeparator {
        implicitHeight: visible ? (root.touchActions ? 8 : 6) : 0
    }

    function flushEditorChanges() {
        Qt.inputMethod.commit()
        if (editorPanel.visible && workspace.currentEditor)
            managerEditorPane.blockEditor.flushPendingEditorChanges()
    }

    function checkpointEditor() {
        flushEditorChanges()
        return workspace.saveCurrentNote()
    }

    function reloadEditor() {
        if (!workspace.currentEditor || workspace.currentEditor.dirty)
            return false
        return workspace.reloadCurrentNote()
    }

    function closeWorkspace() {
        flushEditorChanges()
        return workspace.closeCurrentNote()
    }

    function insertionRowAtPoint(x, y) {
        if (!embeddedEditor || !workspace.currentEditor)
            return -1
        const editor = managerEditorPane.blockEditor
        const point = editor.mapFromItem(root, x, y)
        if (point.x < 0 || point.y < 0 || point.x >= editor.width || point.y >= editor.height)
            return -1
        return editor.insertionRowAtPoint(point.x, point.y)
    }

    function clearPendingBodyFind() {
        pendingBodyFindStorageId = ""
        pendingBodyFindNoteId = ""
        pendingBodyFindQuery = ""
    }

    function openPendingBodyFindIfReady() {
        if (!embeddedEditor || pendingBodyFindQuery.length === 0 || !workspace.currentEditor
                || workspace.currentStorageId !== pendingBodyFindStorageId
                || workspace.currentNoteId !== pendingBodyFindNoteId)
            return false
        const query = pendingBodyFindQuery
        clearPendingBodyFind()
        Qt.callLater(function() { managerEditorPane.openFind(query, true) })
        return true
    }

    function selectNote(storageId, noteId, title) {
        if (workspace.currentEditor && !checkpointEditor())
            return false
        selectedStorageId = storageId
        selectedNoteId = noteId
        selectedTitle = title
        if (embeddedEditor && workspace.noteMatchesBodySearch(storageId, noteId)) {
            pendingBodyFindStorageId = storageId
            pendingBodyFindNoteId = noteId
            pendingBodyFindQuery = workspace.searchText.trim()
        } else {
            clearPendingBodyFind()
        }
        const opened = workspace.openNote(storageId, noteId)
        if (!opened) {
            clearPendingBodyFind()
            return false
        }
        openPendingBodyFindIfReady()
        return true
    }

    function createNote() {
        if (workspace.currentEditor && !checkpointEditor())
            return false
        return workspace.createNote(selectedStorageId)
    }

    function openStandalone(storageId, noteId) {
        if (workspace.currentEditor && !checkpointEditor())
            return false
        return workspace.openStandalone(storageId, noteId)
    }

    function requestDelete(storageId, noteId, title) {
        selectedStorageId = storageId
        selectedNoteId = noteId
        selectedTitle = title
        if (workspace.isRecycledNote(storageId, noteId))
            return requestPermanentDeletion([{ storageId: storageId, noteId: noteId, title: title }])
        if (workspace.currentEditor && !checkpointEditor())
            return false
        return workspace.trashNote(storageId, noteId)
    }

    function shouldConfirmPermanentDeletion() {
        if (!confirmDelete)
            return false
        return !workspace || typeof workspace.askBeforePermanentDelete !== "function"
                || workspace.askBeforePermanentDelete()
    }

    function requestPermanentDeletion(notes) {
        if (!notes || notes.length === 0)
            return false
        pendingPermanentDeletionNotes = notes.slice()
        pendingRecycleNotes = []
        if (shouldConfirmPermanentDeletion()) {
            dontAskAgainForPermanentDeletion = false
            deleteDialog.open()
            return true
        }
        return commitPermanentDeletion()
    }

    function commitPermanentDeletion() {
        if ((!pendingPermanentDeletionNotes || pendingPermanentDeletionNotes.length === 0)
                && (!pendingRecycleNotes || pendingRecycleNotes.length === 0))
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false

        let changed = false
        for (const note of pendingRecycleNotes) {
            if (workspace.trashNote(note.storageId, note.noteId))
                changed = true
        }
        for (const note of pendingPermanentDeletionNotes) {
            if (workspace.deleteNote(note.storageId, note.noteId))
                changed = true
        }
        pendingPermanentDeletionNotes = []
        pendingRecycleNotes = []
        if (changed)
            noteSelection.clear()
        return changed
    }

    function handleNotesDroppedOutside(notes) {
        if (!notes || notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false

        const recycled = []
        const recycle = []
        for (const note of notes) {
            if (workspace.isRecycledNote(note.storageId, note.noteId))
                recycled.push(note)
            else
                recycle.push(note)
        }
        if (recycled.length > 0) {
            pendingPermanentDeletionNotes = recycled
            pendingRecycleNotes = recycle
            if (shouldConfirmPermanentDeletion()) {
                dontAskAgainForPermanentDeletion = false
                deleteDialog.open()
                return true
            }
            return commitPermanentDeletion()
        }
        let changed = false
        for (const note of recycle) {
            if (workspace.trashNote(note.storageId, note.noteId))
                changed = true
        }
        return changed
    }

    function handleOutsideDrop(payload) {
        return payload && payload.kind === "notes"
                ? handleNotesDroppedOutside(payload.notes) : false
    }

    function selectedNoteDescriptors() {
        const result = []
        const selection = root.selectedNotes || ({})
        for (const selectedKey of Object.keys(selection))
            result.push(Object.assign({}, selection[selectedKey]))
        result.sort(function(left, right) {
            const leftOrder = Number(left.order === undefined ? -1 : left.order)
            const rightOrder = Number(right.order === undefined ? -1 : right.order)
            if (leftOrder !== rightOrder)
                return leftOrder - rightOrder
            const leftKey = String(left.storageId || "") + "\n" + String(left.noteId || "")
            const rightKey = String(right.storageId || "") + "\n" + String(right.noteId || "")
            return leftKey < rightKey ? -1 : (leftKey > rightKey ? 1 : 0)
        })
        return result
    }

    function contextNoteCount() {
        return contextMenuNotes ? contextMenuNotes.length : 0
    }

    function contextNotesAllRecycled() {
        if (contextNoteCount() === 0)
            return false
        for (const note of contextMenuNotes) {
            if (!workspace.isRecycledNote(note.storageId, note.noteId))
                return false
        }
        return true
    }

    function contextNotesAnyRecycled() {
        for (const note of contextMenuNotes || []) {
            if (workspace.isRecycledNote(note.storageId, note.noteId))
                return true
        }
        return false
    }

    function contextNotesCommonFolderId() {
        if (contextNoteCount() === 0)
            return ""
        let folderId = workspace.folderIdForNote(contextMenuNotes[0].storageId,
                                                  contextMenuNotes[0].noteId)
        for (let index = 1; index < contextMenuNotes.length; ++index) {
            const note = contextMenuNotes[index]
            if (workspace.folderIdForNote(note.storageId, note.noteId) !== folderId)
                return ""
        }
        return folderId
    }

    function contextNotesHaveMixedFolders() {
        if (contextNoteCount() < 2)
            return false
        const firstFolder = workspace.folderIdForNote(contextMenuNotes[0].storageId,
                                                       contextMenuNotes[0].noteId)
        for (let index = 1; index < contextMenuNotes.length; ++index) {
            const note = contextMenuNotes[index]
            if (workspace.folderIdForNote(note.storageId, note.noteId) !== firstFolder)
                return true
        }
        return false
    }

    function requestContextNotesDeletion() {
        const notes = (contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        const changed = handleNotesDroppedOutside(notes)
        // Keep the selection when a permanent-delete confirmation is still
        // open. Cancelling that dialog must not silently discard the user's
        // multi-selection. commitPermanentDeletion() clears it on success.
        if (changed && pendingPermanentDeletionNotes.length === 0
                && pendingRecycleNotes.length === 0) {
            noteSelection.clear()
        }
        return changed
    }

    function restoreContextNotes() {
        const notes = (contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        let changed = false
        for (const note of notes) {
            if (workspace.isRecycledNote(note.storageId, note.noteId)
                    && workspace.restoreRecycledNote(note.storageId, note.noteId)) {
                changed = true
            }
        }
        if (changed)
            noteSelection.clear()
        return changed
    }

    function assignContextNotesFolder(folderId) {
        const notes = (contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        let changed = false
        for (const note of notes) {
            if (workspace.assignNoteFolder(note.storageId, note.noteId, folderId))
                changed = true
        }
        if (changed)
            noteSelection.clear()
        return changed
    }

    function moveContextNotesToStorage(destinationStorageId) {
        if (!destinationStorageId)
            return false
        const notes = []
        for (const note of contextMenuNotes || []) {
            if (note.storageId !== destinationStorageId) {
                notes.push({
                    storageId: note.storageId,
                    noteId: note.noteId,
                    title: note.title,
                    order: note.order
                })
            }
        }
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        const changed = workspace.moveNotes(notes, destinationStorageId, "", false)
        if (changed)
            noteSelection.clear()
        return changed
    }

    function copyContextNotesToStorage(destinationStorageId) {
        if (!destinationStorageId)
            return false
        const notes = (contextMenuNotes || []).slice()
        if (notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        let changed = false
        for (const note of notes) {
            if (note.storageId !== destinationStorageId
                    && workspace.copyNote(note.storageId, note.noteId, destinationStorageId)) {
                changed = true
            }
        }
        return changed
    }

    function showNoteMenu(storageId, noteId, title, position) {
        selectedStorageId = storageId
        selectedNoteId = noteId
        selectedTitle = title

        const selected = noteSelection.isSelected(storageId, noteId)
        const descriptors = selected ? selectedNoteDescriptors() : []
        contextMenuNotes = descriptors.length > 0 ? descriptors : [{
            storageId: String(storageId || ""),
            noteId: String(noteId || ""),
            title: String(title || ""),
            order: -1
        }]

        if (position !== undefined)
            noteContextMenu.popup(root, position)
        else
            noteContextMenu.popup()
    }

    function selectedNoteFolderId() {
        return contextNotesCommonFolderId()
    }

    function assignSelectedNoteFolder(folderId) {
        return assignContextNotesFolder(folderId)
    }

    function showStorageMenu(storageId, title, position) {
        selectedStorageId = storageId
        selectedNoteId = ""
        selectedTitle = title
        if (position !== undefined)
            storageContextMenu.popup(root, position)
        else
            storageContextMenu.popup()
    }

    function groupedItemAtRow(row) {
        return groupedNotes.itemAtRow(row)
    }

    function cancelGroupedDrag() {
        groupedNotes.cancelDrag()
    }

    function sharedGroupedNoteBoundaries(view, payload, delegates) {
        const remaining = view.remainingItems(delegates)
        return view.trailingBoundaries(delegates, function(item) {
            const owner = item || (remaining.length > 0 ? remaining[0] : null)
            return {
                storageId: owner ? owner.storageId : "",
                anchorNoteId: owner && owner.noteRow ? owner.noteId : "",
                insertAfter: Boolean(item && item.noteRow)
            }
        }, true)
    }

    function sharedGroupedStorageBoundaries(view, payload, delegates) {
        const remainingItems = view.remainingItems(delegates)
        const groups = []
        for (const item of remainingItems) {
            let group = groups.length > 0 ? groups[groups.length - 1] : null
            if (!group || group.storageId !== item.storageId) {
                group = {
                    storageId: item.storageId,
                    first: item,
                    last: item
                }
                groups.push(group)
            } else {
                group.last = item
            }
        }
        const boundaries = []
        if (groups.length > 0) {
            const leading = view.boundaryByOrder(groups[0].first, false, {
                storageDestinationRow: 0,
                rootGroupDrop: true
            })
            if (leading)
                boundaries.push(leading)
        }
        for (let index = 0; index < groups.length; ++index) {
            const after = view.boundaryByOrder(groups[index].last, true, {
                storageDestinationRow: index + 1,
                rootGroupDrop: true
            })
            if (after)
                boundaries.push(after)
        }
        return boundaries
    }

    function sharedGroupedBoundaries(view, payload, delegates) {
        return payload && payload.kind === "group"
                ? sharedGroupedStorageBoundaries(view, payload, delegates)
                : sharedGroupedNoteBoundaries(view, payload, delegates)
    }

    function sharedGroupedDirectTarget(view, payload, pointerX, pointerY) {
        return null
    }

    function commitSharedGroupedDrop(payload, boundary, directTarget) {
        if (!payload)
            return false
        if (payload.kind === "group") {
            return boundary
                    && workspace.moveStorageToRow(
                        payload.groupId,
                        Number(boundary.storageDestinationRow))
        }
        if (payload.kind !== "notes" || !payload.notes || payload.notes.length === 0)
            return false
        if (workspace.currentEditor && !checkpointEditor())
            return false
        const storageId = directTarget
                ? String(directTarget.storageId)
                : String(boundary ? boundary.storageId || "" : "")
        const anchorNoteId = directTarget
                ? "" : String(boundary ? boundary.anchorNoteId || "" : "")
        const insertAfter = directTarget
                ? false : Boolean(boundary && boundary.insertAfter)
        return workspace.moveNotes(payload.notes, storageId, anchorNoteId, insertAfter)
    }

    function recentNoteBoundaries(view, payload, delegates) {
        return view.boundaries(delegates, function(item, after) {
            return {
                storageId: item ? String(item.storageId) : "",
                anchorNoteId: item ? String(item.noteId) : "",
                insertAfter: Boolean(after)
            }
        })
    }

    function canReorderRecentNote(item) {
        if (!item || !item.noteRow)
            return false
        const storageId = String(item.storageId)
        for (const storage of workspace.storages || []) {
            if (String(storage.storageId) === storageId)
                return Boolean(storage.supportsNoteReordering)
        }
        return false
    }

    function commitRecentDrop(payload, boundary) {
        if (!payload || payload.kind !== "notes" || !payload.notes || payload.notes.length === 0
                || !boundary || !boundary.storageId || !boundary.anchorNoteId) {
            return false
        }
        if (workspace.currentEditor && !checkpointEditor())
            return false
        return workspace.reorderRecentNotes(payload.notes,
                                            String(boundary.storageId),
                                            String(boundary.anchorNoteId),
                                            Boolean(boundary.insertAfter))
    }

    function openSearch() {
        mobileSearchExpanded = true
        Qt.callLater(function() {
            searchField.forceActiveFocus()
            searchField.selectAll()
        })
    }

    function closeSearch() {
        workspace.searchText = ""
        workspace.searchInBody = false
        mobileSearchExpanded = false
        searchField.focus = false
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        Pane {
            id: navigationPane
            SplitView.preferredWidth: root.embeddedEditor ? root.navigationWidth : root.width
            SplitView.minimumWidth: root.embeddedEditor ? 230 : 0
            SplitView.maximumWidth: root.embeddedEditor ? Math.max(520, root.width * 0.65) : root.width
            padding: 8
            onWidthChanged: {
                if (root.embeddedEditor && width >= SplitView.minimumWidth)
                    root.navigationWidth = width
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                GridLayout {
                    id: navigationHeader
                    Layout.fillWidth: true
                    columns: root.touchActions && root.showViewModeSelector && width >= 380 ? 2 : 1
                    columnSpacing: 4
                    rowSpacing: 2

                    TabBar {
                        id: modeTabs
                        visible: root.showViewModeSelector
                        Layout.fillWidth: true
                        currentIndex: root.viewMode
                        Accessible.name: qsTr("Notes view")
                        onCurrentIndexChanged: {
                            if (currentIndex >= 0 && root.viewMode !== currentIndex)
                                root.viewMode = currentIndex
                        }

                        TabButton { text: qsTr("Recent") }
                        TabButton { text: qsTr("By storage") }
                        TabButton { text: qsTr("Folders") }
                    }

                    RowLayout {
                        visible: root.touchActions
                        Layout.fillWidth: !root.showViewModeSelector
                        Layout.alignment: Qt.AlignRight
                        spacing: 4

                        Item { Layout.fillWidth: !root.showViewModeSelector }

                        ToolButton {
                            id: searchButton
                            display: AbstractButton.IconOnly
                            contentItem: ThemedIcon {
                                themeName: "edit-find-symbolic"
                                fallbackName: "edit-find-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "light"
                            }
                            Accessible.name: root.searchExpanded ? qsTr("Close search") : qsTr("Search notes")
                            onClicked: root.searchExpanded ? root.closeSearch() : root.openSearch()
                        }
                    }
                }

                Pane {
                    id: searchPane
                    visible: true
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible && root.searchExpanded
                                            ? searchLayout.implicitHeight + topPadding + bottomPadding : 0
                    enabled: visible && root.searchExpanded
                    padding: 6
                    clip: true
                    opacity: root.searchExpanded ? 1 : 0

                    Behavior on Layout.preferredHeight { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 110 } }

                    ColumnLayout {
                        id: searchLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            TextField {
                                id: searchField
                                Layout.fillWidth: true
                                placeholderText: qsTr("Search notes")
                                text: root.workspace.searchText
                                onTextEdited: root.workspace.searchText = text
                                Keys.onEscapePressed: root.closeSearch()
                            }

                            ToolButton {
                                visible: root.showCreateButton && root.viewMode !== root.foldersMode
                                Layout.preferredWidth: 27
                                Layout.preferredHeight: 27
                                padding: 3
                                display: AbstractButton.IconOnly
                                contentItem: Image {
                                    source: "qrc:/icons/new"
                                    sourceSize.width: 24
                                    sourceSize.height: 24
                                    fillMode: Image.PreserveAspectFit
                                }
                                Accessible.name: qsTr("New note")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                onClicked: root.createNote()
                            }
                        }

                        CheckBox {
                            id: searchInTextCheckBox
                            visible: root.searchOptionsVisible
                            enabled: visible
                            focusPolicy: Qt.NoFocus
                            Layout.preferredHeight: visible ? implicitHeight : 0
                            text: qsTr("Search in text")
                            checked: root.workspace.searchInBody
                            onToggled: root.workspace.searchInBody = checked
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    NoteList.NoteCollectionView {
                        id: recentNotes

                        anchors.fill: parent
                        anchors.rightMargin: -navigationPane.rightPadding
                        visible: root.viewMode === root.recentMode
                        enabled: visible
                        model: root.workspace.recentNotesModel
                        selectionController: noteSelection
                        nativeModelHierarchy: false
                        flatNoteRows: true
                        touchActions: root.touchActions
                        embeddedEditor: root.embeddedEditor
                        currentStorageId: root.selectedStorageId
                        currentNoteId: root.selectedNoteId
                        viewObjectName: "recentNotes"
                        rowObjectNamePrefix: "recentDelegate-"
                        allowNoteDrag: true
                        allowGroupDrag: false
                        dragEnabledProvider: root.canReorderRecentNote
                        boundaryProvider: root.recentNoteBoundaries
                        directTargetProvider: function() { return null }
                        commitHandler: root.commitRecentDrop
                        outsideDropHandler: root.handleOutsideDrop
                        swipeDeleteEnabled: true
                        noteActivateHandler: function(item) {
                            root.selectNote(item.storageId, item.noteId, item.title)
                        }
                        noteStandaloneHandler: function(item) {
                            root.openStandalone(item.storageId, item.noteId)
                        }
                        noteContextHandler: function(item, position) {
                            root.showNoteMenu(item.storageId, item.noteId, item.title,
                                              recentNotes.mapToItem(root, position))
                        }
                        noteDeleteHandler: function(item) {
                            root.requestDelete(item.storageId, item.noteId, item.title)
                        }
                    }

                    NoteList.NoteCollectionView {
                        id: groupedNotes

                        anchors.fill: parent
                        anchors.rightMargin: -navigationPane.rightPadding
                        visible: root.viewMode === root.groupedByStorageMode
                        enabled: visible
                        model: root.workspace.groupedNotesModel
                        selectionController: noteSelection
                        nativeModelHierarchy: true
                        touchActions: root.touchActions
                        embeddedEditor: root.embeddedEditor
                        defaultGroupKind: "storage"
                        currentStorageId: root.selectedStorageId
                        currentNoteId: root.selectedNoteId
                        selectedGroupId: root.selectedNoteId.length === 0
                                         ? root.selectedStorageId : ""
                        viewObjectName: "notesTree"
                        previewObjectName: "managerDragPreview"
                        previewObjectNamePrefix: "managerDragPreviewItem-"
                        rowObjectNameProvider: function(item) {
                            return "groupedDelegate-" + item.storageId + "-" + item.noteId
                        }
                        groupActivateHandler: function(item) {
                            root.selectedStorageId = item.storageId
                            root.selectedNoteId = ""
                            root.selectedTitle = item.title
                            groupedNotes.toggleGroup(item)
                        }
                        noteActivateHandler: function(item) {
                            root.selectNote(item.storageId, item.noteId, item.title)
                        }
                        noteStandaloneHandler: function(item) {
                            root.openStandalone(item.storageId, item.noteId)
                        }
                        noteContextHandler: function(item, position) {
                            root.showNoteMenu(item.storageId, item.noteId, item.title,
                                              groupedNotes.mapToItem(root, position))
                        }
                        groupContextHandler: function(item, position) {
                            root.showStorageMenu(item.storageId, item.title,
                                                 groupedNotes.mapToItem(root, position))
                        }
                        groupSourceProvider: function(item, items) {
                            return items.filter(function(candidate) {
                                return candidate.storageId === item.storageId
                            })
                        }
                        boundaryProvider: root.sharedGroupedBoundaries
                        directTargetProvider: root.sharedGroupedDirectTarget
                        commitHandler: root.commitSharedGroupedDrop
                        outsideDropHandler: root.handleOutsideDrop
                    }

                    // A Component declared here is bound to NotesManagerPage
                    // because this file opts in to ComponentBehavior: Bound.
                    // Passing that component through Loader.sourceComponent
                    // makes Qt instantiate it outside its lexical creation
                    // context, which fails when the Folders tab is first
                    // selected. Keep the page in the same context and only
                    // toggle its visibility instead.
                    FoldersPage {
                        id: foldersPage

                        anchors.fill: parent
                        anchors.rightMargin: -navigationPane.rightPadding
                        visible: root.viewMode === root.foldersMode
                        enabled: visible
                        workspace: root.workspace
                        selectionController: noteSelection
                        touchActions: root.touchActions
                        embeddedEditor: root.embeddedEditor
                        toolbarRightMargin: navigationPane.rightPadding
                        currentStorageId: root.selectedStorageId
                        currentNoteId: root.selectedNoteId
                        checkpointHandler: function() { return root.checkpointEditor() }
                        outsideNotesDropHandler: root.handleNotesDroppedOutside
                        onNoteActivated: function(storageId, noteId, title) {
                            root.selectNote(storageId, noteId, title)
                        }
                        onNoteStandaloneRequested: function(storageId, noteId) {
                            root.openStandalone(storageId, noteId)
                        }
                        onNoteMenuRequested: function(storageId, noteId, title, position) {
                            root.showNoteMenu(storageId, noteId, title, position)
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: root.viewMode !== root.foldersMode
                                 && root.workspace.noteCount === 0 && !root.workspace.busy
                        width: Math.min(parent.width - 32, 360)
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: palette.mid
                        text: qsTr("No notes yet. Create the first note with the + button.")
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        running: root.viewMode !== root.foldersMode
                                 && root.workspace.busy && root.workspace.noteCount === 0
                        visible: running
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.workspace.errorString.length > 0
                    text: root.workspace.errorString
                    color: palette.brightText
                    wrapMode: Text.WordWrap
                }
            }
        }

        Pane {
            id: editorPanel
            visible: root.embeddedEditor
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
            padding: 0
            background: Rectangle { color: palette.base }

            NoteEditorPane {
                id: managerEditorPane
                anchors.fill: parent
                visible: root.workspace.currentEditor !== null
                editor: root.workspace.currentEditor
                platformBackend: root.platformBackend
                audioTranscriptionController: root.speechController
                folderWorkspace: root.workspace
                showDeleteButton: true
                showDesktopActions: root.desktopActions !== null
                microphoneVisible: root.speechController && root.speechController.available
                microphoneBusy: root.speechController && root.speechController.busy
                microphoneRecording: root.speechController && root.speechController.recording
                microphoneHoldToRecord: true
                microphoneModeSwitchVisible: root.speechController && root.speechController.modeSwitchVisible
                microphoneMode: root.speechController ? root.speechController.mode : 0
                saveHandler: function() { return root.workspace.saveCurrentNote() }
                onDeleteRequested: root.requestDelete(editor.storageId, editor.noteId,
                                                      root.workspace.currentTitle)
                onPrintRequested: root.desktopActions.printNote()
                onExportRequested: root.desktopActions.exportNote()
                onMicrophoneRequested: root.speechController.start(managerEditorPane.blockEditor.insertionBlockIndex())
                onMicrophoneReleased: root.speechController.finish()
                onMicrophoneModeRequested: mode => root.speechController.setMode(mode)
                Connections {
                    target: root.speechController
                    function onRecognizedText(text) { managerEditorPane.insertTextAtCursor(text) }
                }
            }

            BusyIndicator {
                anchors.centerIn: parent
                z: 10
                running: root.workspace.loading && root.workspace.currentEditor !== null
                visible: running
            }

            ColumnLayout {
                anchors.fill: parent
                visible: root.workspace.currentEditor === null
                spacing: 8

                Item { Layout.fillHeight: true }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.workspace.loading ? qsTr("Loading note…") : qsTr("Select a note to edit")
                    color: palette.text
                }
                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: root.workspace.loading
                    visible: running
                }
                Item { Layout.fillHeight: true }
            }
        }
    }

    Menu {
        id: noteContextMenu
        objectName: "noteContextMenu"
        modal: true
        dim: false
        focus: true
        width: root.touchActions ? Math.min(280, root.width - 32) : implicitWidth

        CompactContextMenuItem {
            objectName: "noteContextOpen"
            text: qsTr("Open")
            enabled: root.contextNoteCount() === 1
            onTriggered: root.selectNote(root.selectedStorageId, root.selectedNoteId, root.selectedTitle)
        }
        CompactContextMenuItem {
            objectName: "noteContextOpenStandalone"
            visible: root.embeddedEditor
            text: qsTr("Open in separate window")
            enabled: root.contextNoteCount() === 1
            onTriggered: root.openStandalone(root.selectedStorageId, root.selectedNoteId)
        }
        CompactContextMenuItem {
            objectName: "noteContextSend"
            text: root.contextNoteCount() > 1 ? qsTr("Send notes to storage…")
                                              : qsTr("Send to storage…")
            enabled: root.contextNoteCount() > 0
            onTriggered: sendDialog.open()
        }
        CompactContextMenuItem {
            objectName: "noteContextMove"
            text: root.contextNoteCount() > 1 ? qsTr("Move notes…") : qsTr("Move…")
            enabled: root.contextNoteCount() > 0
            onTriggered: moveDialog.open()
        }
        FolderPickerMenu {
            id: noteFolderPicker
            objectName: "noteFolderPicker"
            // Menu.visible opens the popup. Availability belongs to enabled,
            // which FolderPickerMenu already derives from the workspace.
            workspace: root.workspace
            currentFolderId: root.selectedNoteFolderId()
            selectionMixed: root.contextNotesHaveMixedFolders()
            onFolderSelected: function(folderId) { root.assignSelectedNoteFolder(folderId) }
        }
        CompactContextSeparator { }
        CompactContextMenuItem {
            objectName: "noteContextRestore"
            visible: root.contextNotesAllRecycled()
            text: root.contextNoteCount() > 1 ? qsTr("Restore notes from Recycle Bin")
                                              : qsTr("Restore from Recycle Bin")
            onTriggered: root.restoreContextNotes()
        }
        CompactContextMenuItem {
            objectName: "noteContextDelete"
            text: root.contextNotesAllRecycled()
                  ? (root.contextNoteCount() > 1 ? qsTr("Delete notes permanently")
                                                 : qsTr("Delete permanently"))
                  : (root.contextNotesAnyRecycled() ? qsTr("Delete selected notes")
                                                    : (root.contextNoteCount() > 1
                                                       ? qsTr("Move notes to Recycle Bin")
                                                       : qsTr("Move to Recycle Bin")))
            onTriggered: root.requestContextNotesDeletion()
        }
    }

    Menu {
        id: storageContextMenu
        objectName: "storageContextMenu"
        modal: true
        dim: false
        focus: true
        width: root.touchActions ? Math.min(280, root.width - 32) : implicitWidth

        CompactContextMenuItem {
            text: qsTr("New note in this storage")
            onTriggered: {
                if (!root.workspace.currentEditor || root.checkpointEditor())
                    root.workspace.createNote(root.selectedStorageId)
            }
        }
        CompactContextMenuItem {
            text: qsTr("Storage settings…")
            onTriggered: root.workspace.openStorageSettings(root.selectedStorageId)
        }
    }

    Dialog {
        id: deleteDialog
        objectName: "permanentDeleteDialog"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        height: Math.min(190, root.height - 32)
        title: root.pendingPermanentDeletionNotes.length > 1
               ? qsTr("Delete notes permanently") : qsTr("Delete note permanently")
        standardButtons: Dialog.Yes | Dialog.No

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                id: permanentDeleteMessage
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.pendingRecycleNotes.length > 0
                      ? qsTr("Permanently delete the selected recycled notes? The other selected notes will be moved to the Recycle Bin.")
                      : (root.pendingPermanentDeletionNotes.length === 1
                         ? qsTr("Permanently delete “%1”? This cannot be undone.")
                               .arg(String(root.pendingPermanentDeletionNotes[0].title || root.selectedTitle))
                         : qsTr("Permanently delete %1 notes? This cannot be undone.")
                               .arg(root.pendingPermanentDeletionNotes.length))
            }

            CheckBox {
                id: dontAskAgain
                text: qsTr("Don't ask again")
                checked: root.dontAskAgainForPermanentDeletion
                onToggled: root.dontAskAgainForPermanentDeletion = checked
            }
        }

        onAccepted: {
            if (root.dontAskAgainForPermanentDeletion
                    && root.workspace
                    && typeof root.workspace.setAskBeforePermanentDelete === "function") {
                root.workspace.setAskBeforePermanentDelete(false)
            }
            root.commitPermanentDeletion()
        }
        onRejected: {
            root.pendingPermanentDeletionNotes = []
            root.pendingRecycleNotes = []
            root.dontAskAgainForPermanentDeletion = false
        }
    }

    Dialog {
        id: moveDialog
        objectName: "moveNotesDialog"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        title: root.contextNoteCount() > 1 ? qsTr("Move notes") : qsTr("Move note")
        standardButtons: Dialog.Ok | Dialog.Cancel

        ColumnLayout {
            width: parent.width
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.contextNoteCount() > 1
                      ? qsTr("Move %1 selected notes to:").arg(root.contextNoteCount())
                      : qsTr("Move “%1” to:").arg(root.selectedTitle)
            }
            ComboBox {
                id: destinationStorage
                objectName: "moveNotesDestination"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: root.workspace.storages
                textRole: "name"
                valueRole: "storageId"
            }
        }

        onAccepted: root.moveContextNotesToStorage(destinationStorage.currentValue)
    }

    Dialog {
        id: sendDialog
        objectName: "sendNotesDialog"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        title: root.contextNoteCount() > 1 ? qsTr("Send notes to storage")
                                           : qsTr("Send note to storage")
        standardButtons: Dialog.Ok | Dialog.Cancel

        ColumnLayout {
            width: parent.width
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.contextNoteCount() > 1
                      ? qsTr("Copy %1 selected notes to:").arg(root.contextNoteCount())
                      : qsTr("Copy “%1” to:").arg(root.selectedTitle)
            }
            ComboBox {
                id: sendDestinationStorage
                objectName: "sendNotesDestination"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: root.workspace.storages
                textRole: "name"
                valueRole: "storageId"
            }
        }

        onAccepted: root.copyContextNotesToStorage(sendDestinationStorage.currentValue)
    }

    Connections {
        target: root.workspace
        function onCurrentEditorChanged() {
            if (root.workspace.currentEditor) {
                root.selectedStorageId = root.workspace.currentStorageId
                root.selectedNoteId = root.workspace.currentNoteId
                root.selectedTitle = root.workspace.currentTitle
            }
            if (root.workspace.currentEditor && root.embeddedEditor) {
                Qt.callLater(function() {
                    if (!root.openPendingBodyFindIfReady())
                        managerEditorPane.blockEditor.focusInitialEditor()
                })
            }
        }
        function onSearchTextChanged() {
            if (root.pendingBodyFindQuery.length > 0
                    && root.workspace.searchText.trim() !== root.pendingBodyFindQuery)
                root.clearPendingBodyFind()
        }
        function onSearchInBodyChanged() {
            if (!root.workspace.searchInBody)
                root.clearPendingBodyFind()
        }
        function onLoadingChanged() {
            if (!root.workspace.loading && root.workspace.currentEditor) {
                root.selectedStorageId = root.workspace.currentStorageId
                root.selectedNoteId = root.workspace.currentNoteId
                root.selectedTitle = root.workspace.currentTitle
            }
            if (!root.workspace.loading && root.pendingBodyFindQuery.length > 0) {
                Qt.callLater(function() {
                    if (!root.openPendingBodyFindIfReady() && !root.workspace.loading)
                        root.clearPendingBodyFind()
                })
            }
        }
    }

    Connections {
        target: root.workspace.groupedNotesModel
        function onRowsInserted() {
            Qt.callLater(function() { groupedNotes.expandGroups(1) })
        }
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            Qt.callLater(function() {
                const ownsFocus = root.workspace.currentEditor
                    && managerEditorPane.blockEditor.documentHistoryOwnsFocus()
                if (root.editorFocusOwned && !ownsFocus)
                    root.checkpointEditor()
                root.editorFocusOwned = ownsFocus
            })
        }
    }

}
