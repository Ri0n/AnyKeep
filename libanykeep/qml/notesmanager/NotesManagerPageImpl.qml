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
    function canReorderRecentNote(item) { return dragController.canReorderRecentNote(item) }
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
                      ? qsTr("AnyKeep %1 is ready. The update is already downloaded and prepared.")
                            .arg(root.updateController.availableVersion)
                      : ""
            }

            Button {
                objectName: "applyPreparedUpdateButton"
                text: qsTr("Update and restart")
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
                                    sourceSize.width: 22
                                    sourceSize.height: 22
                                    fillMode: Image.PreserveAspectFit
                                }
                                Accessible.name: qsTr("New note")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                onClicked: root.createNote()
                            }

                            ToolButton {
                                objectName: "undoTrashButton"
                                Layout.preferredWidth: 27
                                Layout.preferredHeight: 27
                                padding: 3
                                enabled: Boolean(root.workspace["canUndoTrash"]
                                                 || root.workspace["canUndoFolderTrash"])
                                display: AbstractButton.IconOnly
                                contentItem: Shared.ThemedIcon {
                                    themeName: "edit-undo-symbolic"
                                    fallbackName: "edit-undo-symbolic.svg"
                                    recolorFallback: true
                                    fallbackTintMode: "auto"
                                    pixelSize: 18
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
                                themeName: "document-open-recent-symbolic"
                                fallbackName: "document-open-recent-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "auto"
                                pixelSize: 20
                            }
                        }

                        TabButton {
                            text: ""
                            display: AbstractButton.IconOnly
                            Accessible.name: qsTr("By storage")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            contentItem: Shared.ThemedIcon {
                                themeName: "drive-harddisk-symbolic"
                                fallbackName: "drive-harddisk-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "auto"
                                pixelSize: 20
                            }
                        }

                        TabButton {
                            text: ""
                            display: AbstractButton.IconOnly
                            Accessible.name: qsTr("Folders")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            contentItem: Shared.ThemedIcon {
                                themeName: "folder-symbolic"
                                fallbackName: "folder-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: "auto"
                                pixelSize: 20
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
            enabled: root.contextNoteCount() > 0
            onTriggered: moveDialog.open()
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
