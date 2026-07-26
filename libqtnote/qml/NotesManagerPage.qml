pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models

Item {
    id: root

    readonly property int recentMode: 0
    readonly property int groupedByStorageMode: 1

    required property var workspace
    property var platformBackend: null
    property var desktopActions: null
    property var speechController: null
    property bool embeddedEditor: true
    property bool showCreateButton: true
    property bool showViewModeSelector: true
    property bool touchActions: false
    property bool confirmDelete: true
    property bool compact: width < 760
    property int viewMode: embeddedEditor ? groupedByStorageMode : recentMode
    property real navigationWidth: 340
    property string selectedStorageId: ""
    property string selectedNoteId: ""
    property string selectedTitle: ""
    property bool editorFocusOwned: false
    property bool mobileSearchExpanded: false
    readonly property bool searchExpanded: !touchActions || mobileSearchExpanded
                                           || workspace.searchText.length > 0 || workspace.searchInBody
    readonly property bool searchOptionsVisible: searchField.activeFocus || searchInTextCheckBox.pressed

    component CompactContextMenuItem: MenuItem {
        implicitHeight: visible ? (root.touchActions ? 40 : 32) : 0
    }

    component CompactContextSeparator: MenuSeparator {
        implicitHeight: visible ? (root.touchActions ? 8 : 6) : 0
    }

    function flushEditorChanges() {
        Qt.inputMethod.commit()
        if (editorPanel.visible && editorLoader.item)
            editorLoader.item.blockEditor.flushPendingEditorChanges()
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
        if (!embeddedEditor || !editorLoader.item)
            return -1
        const editor = editorLoader.item.blockEditor
        const point = editor.mapFromItem(root, x, y)
        if (point.x < 0 || point.y < 0 || point.x >= editor.width || point.y >= editor.height)
            return -1
        return editor.insertionRowAtPoint(point.x, point.y)
    }

    function selectNote(storageId, noteId, title) {
        if (workspace.currentEditor && !checkpointEditor())
            return false
        selectedStorageId = storageId
        selectedNoteId = noteId
        selectedTitle = title
        return workspace.openNote(storageId, noteId)
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
        if (confirmDelete) {
            deleteDialog.open()
            return true
        }
        if (!workspace.currentEditor || checkpointEditor())
            return workspace.deleteNote(storageId, noteId)
        return false
    }

    function showNoteMenu(storageId, noteId, title) {
        selectedStorageId = storageId
        selectedNoteId = noteId
        selectedTitle = title
        noteContextMenu.popup()
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
                            contentItem: Image {
                                width: 20
                                height: 20
                                source: "image://qtnoteicons/edit-find-symbolic/edit-find-symbolic.svg/light"
                                sourceSize.width: 20
                                sourceSize.height: 20
                                fillMode: Image.PreserveAspectFit
                            }
                            Accessible.name: root.searchExpanded ? qsTr("Close search") : qsTr("Search notes")
                            onClicked: root.searchExpanded ? root.closeSearch() : root.openSearch()
                        }
                    }
                }

                Pane {
                    id: searchPane
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.searchExpanded
                                            ? searchLayout.implicitHeight + topPadding + bottomPadding : 0
                    enabled: root.searchExpanded
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
                                visible: root.showCreateButton
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

                    ListView {
                        id: recentNotes
                        anchors.fill: parent
                        visible: root.viewMode === root.recentMode
                        clip: true
                        spacing: 1
                        model: root.workspace.recentNotesModel
                        bottomMargin: root.touchActions ? 88 : 0

                        delegate: SwipeDelegate {
                            id: recentDelegate

                            required property string storageId
                            required property string noteId
                            required property string title
                            required property string preview
                            required property string storageName
                            required property string iconSource
                            property double suppressClickUntil: 0

                            width: recentNotes.width
                            implicitHeight: root.touchActions ? 44 : 34
                            hoverEnabled: true
                            highlighted: root.selectedStorageId === storageId && root.selectedNoteId === noteId
                            leftPadding: 8
                            rightPadding: 8
                            topPadding: 3
                            bottomPadding: 3

                            background: Rectangle {
                                radius: 4
                                color: recentDelegate.highlighted
                                       ? recentDelegate.palette.highlight
                                       : (recentDelegate.hovered ? Qt.rgba(recentDelegate.palette.button.r, recentDelegate.palette.button.g, recentDelegate.palette.button.b, 0.45) : "transparent")
                            }

                            ToolTip.visible: hovered
                            ToolTip.text: recentDelegate.storageName

                            contentItem: RowLayout {
                                id: contentRow
                                spacing: 8

                                Item {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    Layout.alignment: Qt.AlignVCenter

                                    Image {
                                        id: recentIcon
                                        anchors.fill: parent
                                        source: recentDelegate.iconSource
                                        sourceSize.width: 22
                                        sourceSize.height: 22
                                        fillMode: Image.PreserveAspectFit
                                    }

                                    Label {
                                        anchors.centerIn: parent
                                        visible: recentIcon.status !== Image.Ready
                                        text: "◆"
                                        font.pixelSize: 15
                                        color: recentDelegate.highlighted
                                               ? recentDelegate.palette.highlightedText
                                               : recentDelegate.palette.text
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.alignment: Qt.AlignVCenter
                                    text: recentDelegate.title
                                    color: recentDelegate.highlighted
                                           ? recentDelegate.palette.highlightedText
                                           : recentDelegate.palette.text
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            swipe.left: Button {
                                visible: root.touchActions
                                width: visible ? Math.max(92, implicitWidth) : 0
                                height: recentDelegate.height
                                text: qsTr("Delete")
                                onClicked: {
                                    recentDelegate.swipe.close()
                                    root.requestDelete(recentDelegate.storageId,
                                                       recentDelegate.noteId,
                                                       recentDelegate.title)
                                }
                            }

                            onClicked: {
                                if (Date.now() < suppressClickUntil) {
                                    suppressClickUntil = 0
                                    return
                                }
                                root.selectNote(storageId, noteId, title)
                            }

                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                acceptedDevices: PointerDevice.Mouse
                                enabled: root.embeddedEditor
                                onDoubleTapped: root.openStandalone(recentDelegate.storageId,
                                                                     recentDelegate.noteId)
                            }

                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                acceptedDevices: PointerDevice.Mouse
                                onTapped: root.showNoteMenu(recentDelegate.storageId,
                                                            recentDelegate.noteId,
                                                            recentDelegate.title)
                            }

                            TapHandler {
                                enabled: root.touchActions
                                acceptedButtons: Qt.LeftButton
                                acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                                gesturePolicy: TapHandler.DragThreshold
                                onLongPressed: {
                                    recentDelegate.suppressClickUntil = Date.now() + 1000
                                    root.showNoteMenu(recentDelegate.storageId,
                                                      recentDelegate.noteId,
                                                      recentDelegate.title)
                                }
                            }
                        }
                    }

                    TreeView {
                        id: notesTree
                        anchors.fill: parent
                        visible: root.viewMode === root.groupedByStorageMode
                        clip: true
                        model: root.workspace.groupedNotesModel
                        bottomMargin: root.touchActions ? 88 : 0
                        Component.onCompleted: Qt.callLater(function() { expandRecursively(-1, 1) })
                        selectionModel: ItemSelectionModel { model: notesTree.model }

                        delegate: ItemDelegate {
                            id: groupedDelegate

                            required property int row
                            required property int column
                            required property int depth
                            required property bool expanded
                            required property bool hasChildren
                            required property bool isTreeNode
                            required property string storageId
                            required property string noteId
                            required property int itemType
                            required property string title
                            required property string preview
                            required property bool loading
                            required property string errorString
                            required property bool hasMore
                            required property int noteCount
                            required property string iconSource
                            property double suppressClickUntil: 0

                            width: notesTree.width
                            implicitHeight: root.touchActions ? 44 : 34
                            hoverEnabled: true
                            highlighted: itemType === 0
                                         ? root.selectedStorageId === storageId && root.selectedNoteId.length === 0
                                         : root.selectedStorageId === storageId && root.selectedNoteId === noteId
                            padding: 0
                            ToolTip.visible: hovered && (errorString.length > 0 || preview.length > 0)
                            ToolTip.text: errorString.length > 0 ? errorString : preview

                            background: Rectangle {
                                radius: 4
                                color: groupedDelegate.highlighted
                                       ? groupedDelegate.palette.highlight
                                       : (groupedDelegate.hovered ? Qt.rgba(groupedDelegate.palette.button.r, groupedDelegate.palette.button.g, groupedDelegate.palette.button.b, 0.45) : "transparent")
                            }

                            contentItem: RowLayout {
                                spacing: 8

                                Item { Layout.preferredWidth: 8 + groupedDelegate.depth * 18 }

                                Label {
                                    Layout.preferredWidth: 12
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: groupedDelegate.isTreeNode && groupedDelegate.hasChildren
                                    text: groupedDelegate.expanded ? "▾" : "▸"
                                    color: groupedDelegate.highlighted
                                           ? groupedDelegate.palette.highlightedText
                                           : groupedDelegate.palette.text
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Item {
                                    visible: !(groupedDelegate.isTreeNode && groupedDelegate.hasChildren)
                                    Layout.preferredWidth: visible ? 12 : 0
                                }

                                Item {
                                    Layout.preferredWidth: 20
                                    Layout.preferredHeight: 20
                                    Layout.alignment: Qt.AlignVCenter

                                    Image {
                                        id: groupedIcon
                                        anchors.fill: parent
                                        source: groupedDelegate.iconSource
                                        sourceSize.width: 20
                                        sourceSize.height: 20
                                        fillMode: Image.PreserveAspectFit
                                    }

                                    Label {
                                        anchors.centerIn: parent
                                        visible: groupedIcon.status !== Image.Ready
                                        text: groupedDelegate.itemType === 0 ? "▣" : "◆"
                                        font.pixelSize: 14
                                        color: groupedDelegate.highlighted
                                               ? groupedDelegate.palette.highlightedText
                                               : groupedDelegate.palette.text
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    text: groupedDelegate.itemType === 0
                                          ? (groupedDelegate.loading
                                             ? qsTr("%1 — loading…").arg(groupedDelegate.title)
                                             : qsTr("%1 (%2)").arg(groupedDelegate.title).arg(groupedDelegate.noteCount))
                                          : groupedDelegate.title
                                    font.bold: groupedDelegate.itemType === 0
                                    color: groupedDelegate.highlighted
                                           ? groupedDelegate.palette.highlightedText
                                           : groupedDelegate.palette.text
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            Drag.active: noteDrag.active
                            Drag.source: groupedDelegate
                            Drag.keys: ["qtnote-note"]
                            Drag.hotSpot.x: width / 2
                            Drag.hotSpot.y: height / 2

                            DragHandler {
                                id: noteDrag
                                target: null
                                enabled: groupedDelegate.itemType === 1
                            }

                            DropArea {
                                anchors.fill: parent
                                enabled: groupedDelegate.itemType === 0
                                keys: ["qtnote-note"]
                                onEntered: function(drag) {
                                    if (drag.source && drag.source.storageId !== groupedDelegate.storageId)
                                        drag.accepted = true
                                }
                                onDropped: function(drop) {
                                    if (!drop.source || drop.source.storageId === groupedDelegate.storageId)
                                        return
                                    if (root.workspace.currentEditor && !root.checkpointEditor())
                                        return
                                    if (root.workspace.moveNote(drop.source.storageId,
                                                                drop.source.noteId,
                                                                groupedDelegate.storageId)) {
                                        drop.acceptProposedAction()
                                    }
                                }
                            }

                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                acceptedDevices: PointerDevice.Mouse
                                enabled: groupedDelegate.itemType === 1 && root.embeddedEditor
                                onDoubleTapped: root.openStandalone(groupedDelegate.storageId,
                                                                     groupedDelegate.noteId)
                            }

                            onClicked: {
                                if (Date.now() < suppressClickUntil) {
                                    suppressClickUntil = 0
                                    return
                                }
                                notesTree.selectionModel.setCurrentIndex(notesTree.index(row, column),
                                                                         ItemSelectionModel.ClearAndSelect)
                                if (itemType === 0) {
                                    root.selectedStorageId = storageId
                                    root.selectedNoteId = ""
                                    root.selectedTitle = title
                                    notesTree.toggleExpanded(row)
                                } else {
                                    root.selectNote(storageId, noteId, title)
                                }
                            }

                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                acceptedDevices: PointerDevice.Mouse
                                onTapped: {
                                    if (groupedDelegate.itemType === 1) {
                                        root.showNoteMenu(groupedDelegate.storageId,
                                                          groupedDelegate.noteId,
                                                          groupedDelegate.title)
                                    }
                                }
                            }

                            TapHandler {
                                enabled: root.touchActions && groupedDelegate.itemType === 1
                                acceptedButtons: Qt.LeftButton
                                acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                                gesturePolicy: TapHandler.DragThreshold
                                onLongPressed: {
                                    groupedDelegate.suppressClickUntil = Date.now() + 1000
                                    root.showNoteMenu(groupedDelegate.storageId,
                                                      groupedDelegate.noteId,
                                                      groupedDelegate.title)
                                }
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: root.workspace.noteCount === 0 && !root.workspace.busy
                        width: Math.min(parent.width - 32, 360)
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: palette.mid
                        text: qsTr("No notes yet. Create the first note with the + button.")
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        running: root.workspace.busy && root.workspace.noteCount === 0
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

            Loader {
                id: editorLoader
                anchors.fill: parent
                active: root.workspace.currentEditor !== null

                sourceComponent: Component {
                    NoteEditorPane {
                        id: managerEditorPane
                        editor: root.workspace.currentEditor
                        platformBackend: root.platformBackend
                        showDeleteButton: true
                        showDesktopActions: root.desktopActions !== null
                        microphoneVisible: root.speechController && root.speechController.available
                        microphoneBusy: root.speechController && root.speechController.busy
                        microphoneHoldToRecord: true
                        saveHandler: function() { return root.workspace.saveCurrentNote() }
                        onDeleteRequested: root.requestDelete(editor.storageId, editor.noteId,
                                                              root.workspace.currentTitle)
                        onPrintRequested: root.desktopActions.printNote()
                        onExportRequested: root.desktopActions.exportNote()
                        onMicrophoneRequested: root.speechController.start()
                        onMicrophoneReleased: root.speechController.finish()
                        Connections {
                            target: root.speechController
                            function onRecognizedText(text) { managerEditorPane.insertTextAtCursor(text) }
                        }
                    }
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
                    color: palette.mid
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
        width: root.touchActions ? Math.min(280, root.width - 32) : implicitWidth

        CompactContextMenuItem {
            text: qsTr("Open")
            onTriggered: root.selectNote(root.selectedStorageId, root.selectedNoteId, root.selectedTitle)
        }
        CompactContextMenuItem {
            visible: root.embeddedEditor
            text: qsTr("Open in separate window")
            onTriggered: root.openStandalone(root.selectedStorageId, root.selectedNoteId)
        }
        CompactContextMenuItem {
            text: qsTr("Move…")
            onTriggered: moveDialog.open()
        }
        CompactContextSeparator { }
        CompactContextMenuItem {
            text: qsTr("Delete")
            onTriggered: root.requestDelete(root.selectedStorageId, root.selectedNoteId, root.selectedTitle)
        }
    }

    Dialog {
        id: deleteDialog
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        title: qsTr("Delete note")
        standardButtons: Dialog.Yes | Dialog.No

        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Delete “%1”?").arg(root.selectedTitle)
        }

        onAccepted: {
            if (!root.workspace.currentEditor || root.checkpointEditor())
                root.workspace.deleteNote(root.selectedStorageId, root.selectedNoteId)
        }
    }

    Dialog {
        id: moveDialog
        parent: root
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        modal: true
        width: Math.min(420, root.width - 32)
        title: qsTr("Move note")
        standardButtons: Dialog.Ok | Dialog.Cancel

        ColumnLayout {
            width: parent.width
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Move “%1” to:").arg(root.selectedTitle)
            }
            ComboBox {
                id: destinationStorage
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: root.workspace.storages
                textRole: "name"
                valueRole: "storageId"
            }
        }

        onAccepted: {
            if (destinationStorage.currentValue
                    && destinationStorage.currentValue !== root.selectedStorageId
                    && (!root.workspace.currentEditor || root.checkpointEditor())) {
                root.workspace.moveNote(root.selectedStorageId,
                                        root.selectedNoteId,
                                        destinationStorage.currentValue)
            }
        }
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
                    if (editorLoader.item)
                        editorLoader.item.blockEditor.focusInitialEditor()
                })
            }
        }
        function onLoadingChanged() {
            if (!root.workspace.loading && root.workspace.currentEditor) {
                root.selectedStorageId = root.workspace.currentStorageId
                root.selectedNoteId = root.workspace.currentNoteId
                root.selectedTitle = root.workspace.currentTitle
            }
        }
    }

    Connections {
        target: root.workspace.groupedNotesModel
        function onRowsInserted() {
            Qt.callLater(function() { notesTree.expandRecursively(-1, 1) })
        }
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            Qt.callLater(function() {
                const ownsFocus = editorLoader.item
                    && editorLoader.item.blockEditor.documentHistoryOwnsFocus()
                if (root.editorFocusOwned && !ownsFocus)
                    root.checkpointEditor()
                root.editorFocusOwned = ownsFocus
            })
        }
    }
}
