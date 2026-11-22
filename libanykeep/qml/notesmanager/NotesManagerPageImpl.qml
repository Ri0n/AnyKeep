pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../notelist" as NoteList
import ".." as Shared
import "." as Manager

Item {
    id: root

    readonly property int recentMode: 0
    readonly property int groupedByStorageMode: 1
    readonly property int foldersMode: 2

    required property var workspace
    property var platformBackend: null
    property var desktopActions: null
    property var speechController: null
    property var updateController: null
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
    readonly property int selectedNoteCount: Object.keys(selectedNotes).length
    readonly property bool selectionMode: touchActions && selectedNoteCount > 0
    readonly property bool selectionActionsAvailable: !workspace.metadataBatchBusy && !workspace.movePreparationBusy
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
        objectName: "notesManagerSelectionController"
    }

    component CompactContextMenuItem: MenuItem {
        implicitHeight: visible ? (root.touchActions ? 40 : 32) : 0
    }

    component CompactContextSeparator: MenuSeparator {
        implicitHeight: visible ? (root.touchActions ? 8 : 6) : 0
    }

    component SelectionIconButton: ToolButton {
        id: button
        required property string iconFile

        Layout.preferredWidth: 52
        Layout.preferredHeight: 52
        focusPolicy: Qt.NoFocus
        enabled: root.selectionActionsAvailable
        contentItem: Shared.ThemedIcon {
            themeName: "__bundled__"
            fallbackName: button.iconFile
            recolorFallback: true
            pixelSize: 28
        }
        ToolTip.visible: hovered || pressed
        ToolTip.text: Accessible.name
    }

    component MobileSelectionAction: Action {
        required property string iconFile
        property string buttonName
        icon.source: "qrc:/svg/" + iconFile
        enabled: root.selectionActionsAvailable
    }

    MobileSelectionAction {
        id: clearSelectionAction
        iconFile: "selection-clear-symbolic.svg"
        text: qsTr("Clear selection")
        enabled: true
        onTriggered: root.clearMobileSelection()
    }

    MobileSelectionAction {
        id: tagSelectionAction
        buttonName: "mobileSelectionTag"
        iconFile: "tag-symbolic.svg"
        text: qsTr("Add tag…")
        onTriggered: {
            if (root.prepareMobileSelectedNotes()) {
                mobileTagName.text = ""
                mobileTagError.text = ""
                mobileTagDialog.open()
            }
        }
    }

    MobileSelectionAction {
        id: favoriteSelectionAction
        buttonName: "mobileSelectionFavorite"
        iconFile: "emblem-favorite-symbolic.svg"
        text: qsTr("Toggle favorites")
        onTriggered: {
            if (root.prepareMobileSelectedNotes())
                root.setContextNotesFavorite(!root.contextNotesAllFavorite())
        }
    }

    MobileSelectionAction {
        id: folderSelectionAction
        buttonName: "mobileSelectionFolderButton"
        iconFile: "folder-symbolic.svg"
        text: qsTr("Move to folder…")
        enabled: root.workspace.folderCatalogAvailable && root.selectionActionsAvailable
        onTriggered: {
            if (root.prepareMobileSelectedNotes())
                mobileDestinationDialog.openPicker("folder")
        }
    }

    MobileSelectionAction {
        id: storageSelectionAction
        buttonName: "mobileSelectionStorageButton"
        iconFile: "drive-harddisk-symbolic.svg"
        text: qsTr("Move to storage…")
        onTriggered: {
            if (root.prepareMobileSelectedNotes())
                mobileDestinationDialog.openPicker("storage")
        }
    }

    MobileSelectionAction {
        id: archiveSelectionAction
        buttonName: "mobileSelectionArchiveButton"
        iconFile: "archive-symbolic.svg"
        text: qsTr("Archive…")
        enabled: root.workspace.folderCatalogAvailable && root.selectionActionsAvailable
        onTriggered: {
            if (root.prepareMobileSelectedNotes())
                mobileDestinationDialog.openPicker("archive")
        }
    }

    component SelectionMenuItem: MenuItem {
        implicitHeight: visible ? Math.max(48, implicitContentHeight + topPadding + bottomPadding) : 0
    }

    Manager.NotesManagerActionController {
        id: actionController
        page: root
        workspace: root.workspace
        noteSelection: noteSelection
        editorPanel: editorPanel
        editorPane: managerEditorPane
        foldersPage: foldersPage
        noteContextMenu: noteContextMenu
        storageContextMenu: storageContextMenu
        deleteDialog: deleteDialog
    }

    Manager.NotesManagerDragController {
        id: dragController
        page: root
        workspace: root.workspace
        groupedNotes: groupedNotes
    }

    function flushEditorChanges() { return actionController.flushEditorChanges() }
    function checkpointEditor() { return actionController.checkpointEditor() }
    function reloadEditor() { return actionController.reloadEditor() }
    function closeWorkspace() { return actionController.closeWorkspace() }
    function insertionRowAtPoint(x, y) { return actionController.insertionRowAtPoint(x, y) }
    function clearPendingBodyFind() { return actionController.clearPendingBodyFind() }
    function openPendingBodyFindIfReady() { return actionController.openPendingBodyFindIfReady() }
    function selectNote(storageId, noteId, title) { return actionController.selectNote(storageId, noteId, title) }
    function createNote() { return actionController.createNote() }
    function openStandalone(storageId, noteId) { return actionController.openStandalone(storageId, noteId) }
    function requestDelete(storageId, noteId, title) { return actionController.requestDelete(storageId, noteId, title) }
    function shouldConfirmPermanentDeletion() { return actionController.shouldConfirmPermanentDeletion() }
    function requestPermanentDeletion(notes) { return actionController.requestPermanentDeletion(notes) }
    function commitPermanentDeletion() { return actionController.commitPermanentDeletion() }
    function handleNotesDroppedOutside(notes) { return actionController.handleNotesDroppedOutside(notes) }
    function handleOutsideDrop(payload) { return actionController.handleOutsideDrop(payload) }
    function selectedNoteDescriptors() { return actionController.selectedNoteDescriptors() }
    function prepareMobileSelectedNotes() {
        const notes = selectedNoteDescriptors()
        if (notes.length === 0)
            return false
        contextMenuNotes = notes
        return true
    }
    function clearMobileSelection() { noteSelection.clear() }
    function contextNotesAllFavorite() { return actionController.contextNotesAllFavorite() }
    function setContextNotesFavorite(favorite) { return actionController.setContextNotesFavorite(favorite) }
    function addTagToContextNotes(tag) { return actionController.addTagToContextNotes(tag) }
    function archiveContextNotes(folderId) { return actionController.archiveContextNotes(folderId) }
    function contextNoteCount() { return actionController.contextNoteCount() }
    function contextNotesAllRecycled() { return actionController.contextNotesAllRecycled() }
    function contextNotesAnyRecycled() { return actionController.contextNotesAnyRecycled() }
    function contextNotesCommonFolderId() { return actionController.contextNotesCommonFolderId() }
    function contextNotesHaveMixedFolders() { return actionController.contextNotesHaveMixedFolders() }
    function requestContextNotesDeletion() { return actionController.requestContextNotesDeletion() }
    function restoreContextNotes() { return actionController.restoreContextNotes() }
    function assignContextNotesFolder(folderId) { return actionController.assignContextNotesFolder(folderId) }
    function moveContextNotesToStorage(destinationStorageId) { return actionController.moveContextNotesToStorage(destinationStorageId) }
    function copyContextNotesToStorage(destinationStorageId) { return actionController.copyContextNotesToStorage(destinationStorageId) }
    function showNoteMenu(storageId, noteId, title, position) { return actionController.showNoteMenu(storageId, noteId, title, position) }
    function selectedNoteFolderId() { return actionController.selectedNoteFolderId() }
    function assignSelectedNoteFolder(folderId) { return actionController.assignSelectedNoteFolder(folderId) }
    function showStorageMenu(storageId, title, position) { return actionController.showStorageMenu(storageId, title, position) }
    function groupedItemAtRow(row) { return dragController.groupedItemAtRow(row) }
    function cancelGroupedDrag() { return dragController.cancelGroupedDrag() }
    function sharedGroupedNoteBoundaries(view, payload, delegates) { return dragController.sharedGroupedNoteBoundaries(view, payload, delegates) }
    function sharedGroupedStorageBoundaries(view, payload, delegates) { return dragController.sharedGroupedStorageBoundaries(view, payload, delegates) }
    function sharedGroupedBoundaries(view, payload, delegates) { return dragController.sharedGroupedBoundaries(view, payload, delegates) }
    function sharedGroupedDirectTarget(view, payload, pointerX, pointerY) { return dragController.sharedGroupedDirectTarget(view, payload, pointerX, pointerY) }
    function commitSharedGroupedDrop(payload, boundary, directTarget) { return dragController.commitSharedGroupedDrop(payload, boundary, directTarget) }
    function recentNoteBoundaries(view, payload, delegates) { return dragController.recentNoteBoundaries(view, payload, delegates) }
    function canReorderRecentNote(item) { return !item.pendingDraft && dragController.canReorderRecentNote(item) }
    function commitRecentDrop(payload, boundary) { return dragController.commitRecentDrop(payload, boundary) }

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

    Rectangle {
        id: updateBanner
        objectName: "updateReadyBanner"
        visible: Boolean(root.updateController && root.updateController.updateReady)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: visible ? Math.max(54, updateBannerLayout.implicitHeight + 16) : 0
        color: "#1f7a4d"
        border.color: "#46b97a"
        border.width: 1
        z: 20

        RowLayout {
            id: updateBannerLayout
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 10
            anchors.topMargin: 8
            anchors.bottomMargin: 8
            spacing: 12

            Label {
                Layout.fillWidth: true
                color: "white"
                elide: Text.ElideRight
                text: root.updateController
                      ? (root.updateController.managedByStore && !root.updateController.storePackageDownloaded
                         ? qsTr("AnyKeep %1 is available in Microsoft Store.")
                               .arg(root.updateController.availableVersion)
                         : qsTr("AnyKeep %1 is ready. The update is already downloaded and prepared.")
                               .arg(root.updateController.availableVersion))
                      : ""
            }

            Button {
                objectName: "applyPreparedUpdateButton"
                text: root.updateController
                      && root.updateController.managedByStore
                      && !root.updateController.storePackageDownloaded
                      ? qsTr("Download")
                      : qsTr("Update and restart")
                focusPolicy: Qt.NoFocus
                onClicked: root.updateController.applyUpdate()
            }
        }
    }

    SplitView {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: updateBanner.visible ? updateBanner.top : parent.bottom
        orientation: Qt.Horizontal
        handle: Rectangle {
            // The default Fluent handle is a broad Base-coloured gap. Keep a
            // narrow divider which visually continues the navigation and
            // toolbar surface instead.
            implicitWidth: 2
            color: root.palette.window
        }

        Pane {
            id: navigationPane
            SplitView.preferredWidth: root.embeddedEditor ? root.navigationWidth : root.width
            SplitView.minimumWidth: root.embeddedEditor ? 230 : 0
            SplitView.maximumWidth: root.embeddedEditor ? Math.max(520, root.width * 0.65) : root.width
            leftPadding: 8
            rightPadding: 8
            topPadding: 4
            bottomPadding: 8
            onWidthChanged: {
                if (root.embeddedEditor && width >= SplitView.minimumWidth)
                    root.navigationWidth = width
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

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
                                objectName: "notesSearchField"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Search notes")
                                text: root.workspace.searchText
                                onTextEdited: root.workspace.searchText = text
                                Keys.onEscapePressed: root.closeSearch()
                            }

                            ToolButton {
                                objectName: "newNoteButton"
                                visible: root.showCreateButton
                                // Keep a conventional desktop hit target. The
                                // Fluent style otherwise compresses this icon
                                // beside the search field to a tiny button.
                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 36
                                padding: 6
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

                            ToolButton {
                                objectName: "undoTrashButton"
                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 36
                                padding: 6
                                enabled: Boolean(root.workspace["canUndoTrash"]
                                                 || root.workspace["canUndoFolderTrash"])
                                display: AbstractButton.IconOnly
                                contentItem: Shared.ThemedIcon {
                                    themeName: "__bundled__"
                                    fallbackName: "edit-undo-symbolic.svg"
                                    recolorFallback: true
                                    fallbackTintMode: "auto"
                                    pixelSize: 24
                                }
                                Accessible.name: String(root.workspace["lastTrashedItemName"]
                                                        || root.workspace["lastTrashedFolderName"] || "").length > 0
                                                 ? qsTr("Undo deleting “%1”")
                                                       .arg(String(root.workspace["lastTrashedItemName"]
                                                                   || root.workspace["lastTrashedFolderName"]))
                                                 : qsTr("Undo delete")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                onClicked: {
                                    if (typeof root.workspace.undoTrash === "function")
                                        root.workspace.undoTrash()
                                    else if (typeof root.workspace.undoFolderTrash === "function")
                                        root.workspace.undoFolderTrash()
                                }
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

                        TabButton {
                            text: ""
                            display: AbstractButton.IconOnly
                            Accessible.name: qsTr("Recent")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            contentItem: Shared.ThemedIcon {
                                themeName: "__bundled__"
                                fallbackName: "document-open-recent-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "auto"
                                pixelSize: 24
                            }
                        }

                        TabButton {
                            text: ""
                            display: AbstractButton.IconOnly
                            Accessible.name: qsTr("By storage")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            contentItem: Shared.ThemedIcon {
                                themeName: "__bundled__"
                                fallbackName: "drive-harddisk-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "auto"
                                pixelSize: 24
                            }
                        }

                        TabButton {
                            text: ""
                            display: AbstractButton.IconOnly
                            Accessible.name: qsTr("Folders")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            contentItem: Shared.ThemedIcon {
                                themeName: "__bundled__"
                                fallbackName: "folder-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "auto"
                                pixelSize: 24
                            }
                        }
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
                            contentItem: Shared.ThemedIcon {
                                themeName: "__bundled__"
                                fallbackName: "edit-find-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "light"
                                pixelSize: 24
                            }
                            Accessible.name: root.searchExpanded ? qsTr("Close search") : qsTr("Search notes")
                            onClicked: root.searchExpanded ? root.closeSearch() : root.openSearch()
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
                        openingStorageId: root.workspace.loading ? root.selectedStorageId : ""
                        openingNoteId: root.workspace.loading ? root.selectedNoteId : ""
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
                        openingStorageId: root.workspace.loading ? root.selectedStorageId : ""
                        openingNoteId: root.workspace.loading ? root.selectedNoteId : ""
                        selectedGroupId: root.selectedNoteId.length === 0
                                         ? root.selectedStorageId : ""
                        viewObjectName: "notesTree"
                        previewObjectName: "managerDragPreview"
                        previewObjectNamePrefix: "managerDragPreviewItem-"
                        rowObjectNameProvider: function(item) {
                            return "groupedDelegate-" + item.storageId + "-" + item.noteId
                        }
                        incrementalFetchHandler: function(storageId, lastVisibleNoteId) {
                            if (typeof root.workspace.fetchMoreGroupedNotes !== "function")
                                return false
                            return root.workspace.fetchMoreGroupedNotes(storageId, lastVisibleNoteId)
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
                            storageContextMenu.canSelectNotes = item.noteCount > 0
                            root.showStorageMenu(item.storageId, item.title,
                                                 position === undefined ? undefined
                                                                        : groupedNotes.mapToItem(root, position))
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
                    Shared.FoldersPage {
                        id: foldersPage
                        objectName: "foldersPage"

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

                ToolBar {
                    id: selectionActionBar
                    objectName: "mobileNoteSelectionActionBar"
                    visible: root.selectionMode
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    background: Rectangle { color: selectionActionBar.palette.window }

                    RowLayout {
                        anchors.fill: parent
                        spacing: 4
                        layoutDirection: Qt.application.layoutDirection

                        Label {
                            Layout.minimumWidth: 24
                            text: String(root.selectedNoteCount)
                            horizontalAlignment: Text.AlignHCenter
                            Accessible.name: qsTr("%1 notes selected").arg(root.selectedNoteCount)
                        }
                        SelectionIconButton {
                            objectName: "mobileSelectionClear"
                            iconFile: clearSelectionAction.iconFile
                            action: clearSelectionAction
                            enabled: true
                            Accessible.name: clearSelectionAction.text
                        }
                        ListView {
                            objectName: "mobileSelectionActionStrip"
                            Layout.fillWidth: true
                            Layout.preferredWidth: contentWidth
                            Layout.preferredHeight: 52
                            clip: true
                            orientation: ListView.Horizontal
                            layoutDirection: Qt.application.layoutDirection
                            boundsBehavior: Flickable.StopAtBounds
                            interactive: contentWidth > width
                            spacing: 4
                            model: [tagSelectionAction, favoriteSelectionAction, folderSelectionAction,
                                    storageSelectionAction, archiveSelectionAction]

                            delegate: SelectionIconButton {
                                required property MobileSelectionAction modelData

                                objectName: modelData.buttonName
                                width: 52
                                height: 52
                                action: modelData
                                enabled: modelData.enabled
                                iconFile: modelData.iconFile
                                Accessible.name: modelData.text
                            }

                            ScrollBar.horizontal: ScrollBar { interactive: false }
                        }
                        SelectionIconButton {
                            id: selectionMoreButton
                            objectName: "mobileSelectionMore"
                            iconFile: "overflow-menu-symbolic.svg"
                            Accessible.name: qsTr("More actions for selected notes")
                            onClicked: {
                                if (root.prepareMobileSelectedNotes())
                                    selectionMoreMenu.open()
                            }

                            Menu {
                                id: selectionMoreMenu
                                objectName: "mobileSelectionMoreMenu"
                                parent: selectionMoreButton
                                modal: true
                                dim: false
                                focus: true
                                margins: 8
                                width: Math.min(280, root.width - 2 * margins)
                                height: Math.min(implicitHeight, root.height - 2 * margins)
                                x: Qt.application.layoutDirection === Qt.RightToLeft
                                   ? 0 : selectionMoreButton.width - width
                                y: -height - 4

                                SelectionMenuItem {
                                    objectName: "mobileSelectionRestore"
                                    icon.source: "qrc:/svg/edit-undo-symbolic.svg"
                                    visible: root.contextNotesAllRecycled()
                                    enabled: root.workspace.folderCatalogAvailable && root.selectionActionsAvailable
                                    text: root.contextNoteCount() > 1 ? qsTr("Restore notes from Recycle Bin")
                                                                    : qsTr("Restore from Recycle Bin")
                                    onTriggered: {
                                        if (root.prepareMobileSelectedNotes() && root.contextNotesAllRecycled())
                                            root.restoreContextNotes()
                                    }
                                }
                                SelectionMenuItem {
                                    objectName: "mobileSelectionMenuTag"
                                    action: tagSelectionAction
                                }
                                SelectionMenuItem {
                                    objectName: "mobileSelectionMenuFavorite"
                                    action: favoriteSelectionAction
                                }
                                SelectionMenuItem {
                                    objectName: "mobileSelectionMoveFolder"
                                    action: folderSelectionAction
                                }
                                SelectionMenuItem {
                                    objectName: "mobileSelectionMoveStorage"
                                    action: storageSelectionAction
                                }
                                SelectionMenuItem {
                                    objectName: "mobileSelectionArchive"
                                    action: archiveSelectionAction
                                }
                                SelectionMenuItem {
                                    objectName: "mobileSelectionMenuClear"
                                    action: clearSelectionAction
                                }
                                MenuSeparator {}
                                SelectionMenuItem {
                                    objectName: "mobileSelectionDelete"
                                    icon.source: "qrc:/svg/user-trash-full-symbolic.svg"
                                    text: root.contextNotesAllRecycled() ? qsTr("Delete permanently")
                                          : root.contextNotesAnyRecycled() ? qsTr("Delete selected notes")
                                          : qsTr("Move to Recycle Bin")
                                    onTriggered: {
                                        if (root.prepareMobileSelectedNotes())
                                            root.requestContextNotesDeletion()
                                    }
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: actionController.preparingSelectionMove || root.workspace.errorString.length > 0
                    text: actionController.preparingSelectionMove
                          ? qsTr("Preparing selected notes for moving…") : root.workspace.errorString
                    color: actionController.preparingSelectionMove ? palette.windowText : palette.brightText
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

            Shared.NoteEditorPane {
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
            enabled: root.contextNoteCount() > 0 && !root.workspace.movePreparationBusy
            onTriggered: {
                if (root.touchActions)
                    mobileDestinationDialog.openPicker("storage")
                else
                    moveDialog.open()
            }
        }
        Shared.FolderPickerMenu {
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

        property bool canSelectNotes: false
        readonly property int storageRow: actionController.storagePriorityRow(root.selectedStorageId)
        readonly property var storage: storageRow >= 0 ? root.workspace.storages[storageRow] : null

        CompactContextMenuItem {
            objectName: "storageContextNewNote"
            icon.source: "qrc:/icons/new"
            text: qsTr("New note in this storage")
            enabled: Boolean(storageContextMenu.storage && storageContextMenu.storage.writable)
            onTriggered: actionController.createNoteInStorage(root.selectedStorageId)
        }
        CompactContextMenuItem {
            objectName: "storageContextSelectAll"
            icon.source: "qrc:/svg/selection-all-symbolic.svg"
            text: root.workspace.searchText.trim().length > 0 ? qsTr("Select all matching notes")
                                                             : qsTr("Select all notes in this storage")
            enabled: storageContextMenu.canSelectNotes
            onTriggered: actionController.selectStorageNotes(root.selectedStorageId)
        }
        CompactContextMenuItem {
            objectName: "storageContextRaisePriority"
            icon.source: "qrc:/svg/go-up-symbolic.svg"
            text: qsTr("Raise storage priority")
            enabled: storageContextMenu.storageRow > 0
            onTriggered: actionController.raiseStoragePriority(root.selectedStorageId)
        }
        CompactContextMenuItem {
            objectName: "storageContextSettings"
            icon.source: "qrc:/svg/preferences-system-symbolic.svg"
            text: qsTr("Storage settings…")
            enabled: Boolean(storageContextMenu.storage && storageContextMenu.storage.configurable)
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
        id: mobileTagDialog
        function applyTag() {
            if (root.addTagToContextNotes(mobileTagName.text.trim()))
                mobileTagDialog.close()
            else
                mobileTagError.text = root.workspace.errorString || qsTr("Could not add tag")
        }
        objectName: "mobileBulkTagDialog"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        title: qsTr("Add tag to selected notes")
        standardButtons: Dialog.Cancel

        ColumnLayout {
            width: parent.width
            spacing: 8

            TextField {
                id: mobileTagName
                objectName: "mobileBulkTagName"
                Layout.fillWidth: true
                placeholderText: qsTr("Tag name (without #)")
                selectByMouse: true
                onAccepted: mobileTagDialog.applyTag()
            }
            Label {
                id: mobileTagError
                Layout.fillWidth: true
                visible: text.length > 0
                color: palette.brightText
                wrapMode: Text.WordWrap
            }
            Button {
                id: mobileTagApply
                objectName: "mobileBulkTagApply"
                Layout.alignment: Qt.AlignRight
                text: qsTr("Add tag")
                enabled: mobileTagName.text.trim().length > 0
                onClicked: mobileTagDialog.applyTag()
            }
        }
    }

    Dialog {
        id: mobileArchiveCreateDialog
        function createArchive() {
            const folderId = root.workspace.createFolderWithFlags(archiveFolderName.text.trim(),
                                                                   "", false, true)
            if (folderId.length === 0) {
                archiveCreateError.text = root.workspace.errorString
                        || qsTr("Could not create folder")
                return
            }
            mobileArchiveCreateDialog.close()
            root.archiveContextNotes(folderId)
        }
        objectName: "mobileBulkArchiveCreateDialog"
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        title: qsTr("New archived folder")
        standardButtons: Dialog.Cancel

        ColumnLayout {
            width: parent.width
            spacing: 8

            TextField {
                id: archiveFolderName
                objectName: "mobileBulkArchiveFolderName"
                Layout.fillWidth: true
                placeholderText: qsTr("Folder name")
                selectByMouse: true
                onAccepted: mobileArchiveCreateDialog.createArchive()
            }
            Label {
                id: archiveCreateError
                Layout.fillWidth: true
                visible: text.length > 0
                color: palette.brightText
                wrapMode: Text.WordWrap
            }
            Button {
                id: archiveCreateButton
                objectName: "mobileBulkArchiveCreate"
                Layout.alignment: Qt.AlignRight
                text: qsTr("Create and archive")
                enabled: archiveFolderName.text.trim().length > 0
                onClicked: mobileArchiveCreateDialog.createArchive()
            }
        }
    }

    Dialog {
        id: mobileDestinationDialog
        objectName: "mobileMoveDestinationDialog"

        property string mode: ""
        property var destinations: []
        property var currentFolderId: null
        readonly property int rowHeight: 52

        function openPicker(destinationMode) {
            mode = destinationMode
            currentFolderId = mode !== "storage" && !root.contextNotesHaveMixedFolders()
                              ? root.selectedNoteFolderId() : null
            if (mode === "storage") {
                destinations = root.workspace.storages.map(function(storage) {
                    const hasNotesToMove = root.contextMenuNotes.some(function(note) {
                        return note.storageId !== storage.storageId
                    })
                    return {
                        id: storage.storageId,
                        title: storage.name,
                        iconSource: storage.iconSource || "qrc:/svg/drive-harddisk-symbolic.svg",
                        current: !hasNotesToMove,
                        available: storage.writable && hasNotesToMove
                    }
                })
            } else {
                const archived = mode === "archive"
                const folders = root.workspace.folderNotesModel?.folderPickerItems(archived) || []
                const entries = archived ? [] : [{ id: "", title: qsTr("Unsorted"), available: true }]
                for (const folder of folders) {
                    if (archived && !folder.inArchivedBranch)
                        continue
                    const prefix = "    ".repeat(folder.depth) + (folder.favorite ? "★ " : "")
                    entries.push({ id: folder.folderId, title: prefix + folder.title, available: true })
                }
                if (archived)
                    entries.push({ id: "", title: qsTr("Create archived folder…"), available: true })
                destinations = entries
            }
            destinationList.positionViewAtBeginning()
            open()
        }

        function choose(destinationId) {
            close()
            if (mode === "storage") {
                root.moveContextNotesToStorage(destinationId)
            } else if (mode === "folder") {
                root.assignContextNotesFolder(destinationId)
            } else if (destinationId.length > 0) {
                root.archiveContextNotes(destinationId)
            } else {
                archiveFolderName.text = ""
                archiveCreateError.text = ""
                mobileArchiveCreateDialog.open()
            }
        }

        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(420, root.width - 32)
        height: Math.min(implicitHeight, Math.max(0, root.height - 32))
        modal: true
        focus: true
        title: mode === "storage" ? qsTr("Move to storage")
                                 : mode === "folder" ? qsTr("Move to folder") : qsTr("Archive in folder")
        standardButtons: Dialog.Cancel

        contentItem: ListView {
            id: destinationList
            objectName: "mobileMoveDestinationList"
            implicitHeight: Math.max(1, count) * mobileDestinationDialog.rowHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: mobileDestinationDialog.destinations
            currentIndex: -1
            ScrollBar.vertical: ScrollBar {}

            Label {
                anchors.centerIn: parent
                visible: destinationList.count === 0
                text: qsTr("No destinations available")
            }

            delegate: ItemDelegate {
                id: destinationRow
                required property var modelData
                readonly property bool currentStorage: Boolean(modelData.current)
                objectName: "mobileMoveDestination-" + modelData.id
                width: destinationList.width
                height: mobileDestinationDialog.rowHeight
                text: modelData.title
                enabled: Boolean(modelData.available)
                highlighted: mobileDestinationDialog.mode !== "storage"
                             && (mobileDestinationDialog.mode === "folder" || modelData.id !== "")
                             && modelData.id === mobileDestinationDialog.currentFolderId
                Accessible.description: currentStorage ? qsTr("Current storage") : ""
                font.pixelSize: 18

                contentItem: RowLayout {
                    spacing: 10
                    layoutDirection: destinationRow.mirrored ? Qt.RightToLeft : Qt.LeftToRight

                    Image {
                        objectName: "mobileDestinationStorageIcon"
                        visible: mobileDestinationDialog.mode === "storage"
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        sourceSize.width: 26
                        sourceSize.height: 26
                        source: destinationRow.modelData.iconSource || ""
                        fillMode: Image.PreserveAspectFit
                        opacity: destinationRow.enabled || destinationRow.currentStorage ? 1 : 0.55
                    }
                    Label {
                        objectName: "mobileDestinationName"
                        Layout.fillWidth: true
                        text: destinationRow.text
                        font: destinationRow.font
                        elide: Text.ElideRight
                        color: destinationRow.enabled || destinationRow.currentStorage
                               ? mobileDestinationDialog.palette.windowText : mobileDestinationDialog.palette.mid
                    }
                    Label {
                        objectName: "mobileDestinationCurrentMark"
                        visible: destinationRow.currentStorage
                        text: "✓"
                        font: destinationRow.font
                        color: mobileDestinationDialog.palette.windowText
                        Accessible.ignored: true
                    }
                }

                onClicked: mobileDestinationDialog.choose(modelData.id)
            }
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
        ignoreUnknownSignals: true
        function onMetadataBatchFinished(failedNotes, errorSummary) {
            actionController.finishMetadataBatch(failedNotes)
        }
        function onNoteMovesPrepared(preparedNotes) {
            actionController.finishSelectionMove(preparedNotes)
        }
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
