pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "reorder" as Reorder

Item {
    id: root

    required property var workspace
    property bool touchActions: false
    property bool embeddedEditor: true
    property string currentStorageId: ""
    property string currentNoteId: ""
    property var checkpointHandler: null
    property string selectedFolderId: ""
    property var selectedNotes: ({})
    property string selectionAnchorKey: ""
    property string editingFolderId: ""
    property string contextFolderId: ""
    property string contextFolderTitle: ""
    property bool contextFolderCollapsed: false
    property bool contextFolderFavorite: false
    property bool contextFolderArchived: false
    property var directDropTarget: null

    readonly property int folderRowKind: 0
    readonly property int noteRowKind: 1
    readonly property int unsortedRowKind: 2
    readonly property real rowHeight: touchActions ? 46 : 36
    readonly property var activePayload: folderReorderController.sourcePayload
    readonly property var dropBoundary: folderReorderController.targetBoundary
    readonly property bool dragging: folderReorderController.dragging
    readonly property int previewCount: folderReorderController.previewCount
    readonly property bool dragSelectionSuppressed: folderReorderController.dragging
    readonly property real draggedExtent: folderReorderController.draggedExtent
    readonly property bool committingDrop: folderReorderController.committingDrop

    signal noteActivated(string storageId, string noteId, string title)
    signal noteStandaloneRequested(string storageId, string noteId)
    signal noteMenuRequested(string storageId, string noteId, string title, point position)

    function modelItem(row) {
        if (!workspace || !workspace.folderNotesModel)
            return ({})
        return workspace.folderNotesModel.itemAt(row)
    }

    function delegateAt(row) {
        return folderList.itemAtIndex(row)
    }

    function rowKey(item) {
        if (!item)
            return ""
        if (Number(item.rowKind) === folderRowKind)
            return "folder:" + String(item.folderId)
        if (Number(item.rowKind) === noteRowKind)
            return "note:" + String(item.storageId) + "\n" + String(item.noteId)
        return "unsorted"
    }

    function noteSelectionKey(storageId, noteId) {
        return String(storageId) + "\n" + String(noteId)
    }

    function noteIsSelected(storageId, noteId) {
        return selectedNotes[noteSelectionKey(storageId, noteId)] !== undefined
    }

    function setNoteSelected(storageId, noteId, title, folderId, selected) {
        const copy = Object.assign({}, selectedNotes)
        const key = noteSelectionKey(storageId, noteId)
        if (selected) {
            copy[key] = {
                storageId: String(storageId),
                noteId: String(noteId),
                title: String(title),
                folderId: String(folderId || "")
            }
        } else {
            delete copy[key]
        }
        selectedNotes = copy
    }

    function selectDesktopNote(delegate, modifiers) {
        if (!delegate || dragSelectionSuppressed)
            return
        const key = noteSelectionKey(delegate.storageId, delegate.noteId)
        const control = Boolean(modifiers & Qt.ControlModifier)
        const shift = Boolean(modifiers & Qt.ShiftModifier)

        if (shift && selectionAnchorKey.length > 0) {
            let anchorRow = -1
            let targetRow = -1
            for (let row = 0; row < folderList.count; ++row) {
                const candidate = modelItem(row)
                if (Number(candidate.rowKind) !== noteRowKind)
                    continue
                const candidateKey = noteSelectionKey(candidate.storageId, candidate.noteId)
                if (candidateKey === selectionAnchorKey)
                    anchorRow = row
                if (candidateKey === key)
                    targetRow = row
            }
            if (anchorRow >= 0 && targetRow >= 0) {
                const copy = control ? Object.assign({}, selectedNotes) : ({})
                const first = Math.min(anchorRow, targetRow)
                const last = Math.max(anchorRow, targetRow)
                for (let row = first; row <= last; ++row) {
                    const candidate = modelItem(row)
                    if (Number(candidate.rowKind) !== noteRowKind)
                        continue
                    const candidateKey = noteSelectionKey(candidate.storageId, candidate.noteId)
                    copy[candidateKey] = {
                        storageId: String(candidate.storageId),
                        noteId: String(candidate.noteId),
                        title: String(candidate.title),
                        folderId: String(candidate.folderId || "")
                    }
                }
                selectedNotes = copy
                selectedFolderId = String(delegate.folderId || "")
                return
            }
        }

        if (control) {
            setNoteSelected(delegate.storageId, delegate.noteId, delegate.title,
                            delegate.folderId, !noteIsSelected(delegate.storageId, delegate.noteId))
            selectionAnchorKey = key
            selectedFolderId = String(delegate.folderId || "")
            return
        }

        const single = ({})
        single[key] = {
            storageId: String(delegate.storageId),
            noteId: String(delegate.noteId),
            title: String(delegate.title),
            folderId: String(delegate.folderId || "")
        }
        selectedNotes = single
        selectionAnchorKey = key
        selectedFolderId = String(delegate.folderId || "")
        noteActivated(delegate.storageId, delegate.noteId, delegate.title)
    }

    function notesForDrag(delegate) {
        if (!delegate)
            return []
        if (!noteIsSelected(delegate.storageId, delegate.noteId)) {
            return [{
                storageId: String(delegate.storageId),
                noteId: String(delegate.noteId),
                title: String(delegate.title),
                folderId: String(delegate.folderId || "")
            }]
        }

        const result = []
        for (let row = 0; row < folderList.count; ++row) {
            const candidate = modelItem(row)
            if (Number(candidate.rowKind) !== noteRowKind
                    || !noteIsSelected(candidate.storageId, candidate.noteId)) {
                continue
            }
            result.push({
                storageId: String(candidate.storageId),
                noteId: String(candidate.noteId),
                title: String(candidate.title),
                folderId: String(candidate.folderId || "")
            })
        }
        return result
    }

    function visibleDelegates() {
        const result = []
        for (let row = 0; row < folderList.count; ++row) {
            const delegate = delegateAt(row)
            if (delegate)
                result.push(delegate)
        }
        return result
    }

    function visibleNoteSources(delegate) {
        const result = []
        for (const candidate of visibleDelegates()) {
            if (Number(candidate.rowKind) === noteRowKind
                    && (candidate === delegate
                        || noteIsSelected(candidate.storageId, candidate.noteId))) {
                result.push(candidate)
            }
        }
        return result
    }

    function folderSubtreeEndRow(startRow, depth) {
        let endRow = startRow
        for (let row = startRow + 1; row < folderList.count; ++row) {
            const candidate = modelItem(row)
            if (Number(candidate.depth) <= Number(depth))
                break
            endRow = row
        }
        return endRow
    }

    function visibleFolderSubtreeSources(delegate) {
        const result = []
        const endRow = folderSubtreeEndRow(delegate.index, delegate.depth)
        for (let row = delegate.index; row <= endRow; ++row) {
            const candidate = delegateAt(row)
            if (candidate)
                result.push(candidate)
        }
        return result
    }

    function folderDescendantIds(delegate) {
        const result = []
        const endRow = folderSubtreeEndRow(delegate.index, delegate.depth)
        for (let row = delegate.index + 1; row <= endRow; ++row) {
            const candidate = modelItem(row)
            if (Number(candidate.rowKind) === folderRowKind)
                result.push(String(candidate.folderId))
        }
        return result
    }

    function activeFolderContains(folderId) {
        if (!activePayload || activePayload.kind !== "folder")
            return false
        return String(activePayload.folderId) === String(folderId)
                || activePayload.descendantFolderIds.indexOf(String(folderId)) >= 0
    }

    function nextSiblingFolderId(delegate) {
        const endRow = folderSubtreeEndRow(delegate.index, delegate.depth)
        for (let row = endRow + 1; row < folderList.count; ++row) {
            const candidate = modelItem(row)
            if (Number(candidate.depth) < Number(delegate.depth))
                return ""
            if (Number(candidate.depth) !== Number(delegate.depth))
                continue
            if (Number(candidate.rowKind) !== folderRowKind)
                return ""
            return String(candidate.parentFolderId) === String(delegate.parentFolderId)
                    ? String(candidate.folderId) : ""
        }
        return ""
    }

    function assignmentFolderFor(item) {
        if (!item || Number(item.rowKind) === unsortedRowKind)
            return ""
        return String(item.folderId || "")
    }

    function rowTranslation(delegate) {
        return reorderLayout.translationByOrder(delegate, dropBoundary, draggedExtent)
    }

    function noteDropBoundaries() {
        const items = visibleDelegates()
        if (reorderLayout.remainingItems(items).length === 0) {
            const fallback = activePayload && activePayload.notes.length > 0
                    ? String(activePayload.notes[0].folderId || "") : ""
            return [{
                position: folderReorderController.startDraggedTopY,
                owner: null,
                finalIndex: 0,
                afterOwner: false,
                assignmentFolderId: fallback
            }]
        }
        return reorderLayout.boundaries(items, function(item) {
            return { assignmentFolderId: root.assignmentFolderFor(item) }
        })
    }

    function folderDropBoundaries() {
        const result = []
        for (const delegate of visibleDelegates()) {
            if (Number(delegate.rowKind) !== folderRowKind
                    || reorderLayout.containsSource(delegate)) {
                continue
            }

            const before = reorderLayout.boundaryByOrder(delegate, false, {
                parentFolderId: String(delegate.parentFolderId || ""),
                beforeFolderId: String(delegate.folderId)
            }, 0, true)
            if (before)
                result.push(before)

            const endDelegate = delegateAt(folderSubtreeEndRow(delegate.index, delegate.depth))
            if (!endDelegate || reorderLayout.containsSource(endDelegate))
                continue
            const after = reorderLayout.boundaryByOrder(endDelegate, true, {
                parentFolderId: String(delegate.parentFolderId || ""),
                beforeFolderId: nextSiblingFolderId(delegate)
            }, 0, true)
            if (after)
                result.push(after)
        }
        if (result.length === 0) {
            result.push({
                position: folderReorderController.startDraggedTopY,
                owner: null,
                finalIndex: 0,
                afterOwner: false,
                parentFolderId: "",
                beforeFolderId: ""
            })
        }
        return result
    }

    function dropBoundaries() {
        if (!activePayload)
            return []
        return activePayload.kind === "folder" ? folderDropBoundaries() : noteDropBoundaries()
    }

    function targetAt(pointerX, pointerY) {
        for (const delegate of visibleDelegates()) {
            const local = delegate.mapFromItem(root, pointerX, pointerY)
            if (local.x < 0 || local.y < 0 || local.x >= delegate.width
                    || local.y >= delegate.baseHeight) {
                continue
            }
            if (activePayload && activePayload.kind === "folder") {
                if (Number(delegate.rowKind) !== folderRowKind
                        || activeFolderContains(delegate.folderId)
                        || local.y < delegate.baseHeight * 0.2
                        || local.y > delegate.baseHeight * 0.8) {
                    continue
                }
                return {
                    key: rowKey(delegate),
                    rowKind: Number(delegate.rowKind),
                    folderId: String(delegate.folderId),
                    assignmentFolderId: String(delegate.folderId)
                }
            }
            if (Number(delegate.rowKind) === folderRowKind
                    || Number(delegate.rowKind) === noteRowKind
                    || Number(delegate.rowKind) === unsortedRowKind) {
                return {
                    key: rowKey(delegate),
                    rowKind: Number(delegate.rowKind),
                    folderId: String(delegate.folderId || ""),
                    assignmentFolderId: assignmentFolderFor(delegate)
                }
            }
        }
        return null
    }

    function canMoveFolderTo(folderId, parentFolderId, beforeFolderId) {
        if (String(folderId).length === 0)
            return false
        if (activeFolderContains(parentFolderId) || activeFolderContains(beforeFolderId))
            return false
        return true
    }

    function beginDrag(delegate) {
        if (!delegate || dragging || !workspace.folderCatalogAvailable)
            return false
        const kind = Number(delegate.rowKind) === folderRowKind ? "folder"
                   : (Number(delegate.rowKind) === noteRowKind ? "notes" : "")
        if (kind.length === 0)
            return false

        const sourceItems = kind === "folder"
                ? visibleFolderSubtreeSources(delegate) : visibleNoteSources(delegate)
        sourceItems.sort(function(left, right) { return Number(left.index) - Number(right.index) })
        const sources = []
        for (const item of sourceItems) {
            sources.push({
                item: item,
                key: rowKey(item),
                order: item.index,
                previewItem: item,
                geometryItem: item,
                naturalExtent: item.baseHeight,
                previewWidth: item.width,
                previewHeight: item.baseHeight
            })
        }

        const payload = kind === "folder" ? {
            kind: "folder",
            sourceDelegate: delegate,
            folderId: String(delegate.folderId),
            descendantFolderIds: folderDescendantIds(delegate)
        } : {
            kind: "notes",
            sourceDelegate: delegate,
            notes: notesForDrag(delegate)
        }
        const started = folderReorderController.beginDrag({
            sources: sources,
            payload: payload,
            pointerItem: delegate,
            pointerLocalX: delegate.width / 2,
            pointerLocalY: delegate.baseHeight / 2,
            targetByDraggedTop: true
        })
        delegate.internalDragActive = started
        if (started) {
            delegate.suppressClickUntil = Date.now() + 500
            directDropTarget = null
            if (kind === "notes") {
                selectedNotes = ({})
                selectionAnchorKey = ""
            }
        }
        return started
    }

    function moveDrag(delegate, dx, dy) {
        if (!activePayload || !delegate || !delegate.internalDragActive)
            return
        folderReorderController.moveDrag(dx, dy)
    }

    function finishDrag(delegate) {
        if (!delegate || !delegate.internalDragActive)
            return
        delegate.internalDragActive = false
        folderReorderController.finishDrag()
    }

    function cancelDrag() {
        if (activePayload && activePayload.sourceDelegate)
            activePayload.sourceDelegate.internalDragActive = false
        folderReorderController.cancelDrag()
    }

    function commitDrop(payload, boundary) {
        if (!payload)
            return false
        if (payload.kind === "folder") {
            const parentFolderId = directDropTarget
                    ? String(directDropTarget.folderId) : String(boundary ? boundary.parentFolderId || "" : "")
            const beforeFolderId = directDropTarget
                    ? "" : String(boundary ? boundary.beforeFolderId || "" : "")
            if (!canMoveFolderTo(payload.folderId, parentFolderId, beforeFolderId))
                return false
            return workspace.moveFolderBefore(payload.folderId, parentFolderId, beforeFolderId)
        }

        if (payload.kind !== "notes" || !payload.notes || payload.notes.length === 0)
            return false
        const folderId = directDropTarget
                ? String(directDropTarget.assignmentFolderId || "")
                : String(boundary ? boundary.assignmentFolderId || "" : "")
        if (workspace.currentEditor && typeof checkpointHandler === "function"
                && !checkpointHandler()) {
            return false
        }

        let changed = false
        for (const note of payload.notes) {
            if (workspace.assignNoteFolder(note.storageId, note.noteId, folderId))
                changed = true
        }
        if (changed)
            selectedNotes = ({})
        return changed
    }

    function createFolder(parentFolderId) {
        const parentId = String(parentFolderId || "")
        const created = workspace.createFolder("", parentId)
        if (created.length === 0)
            return
        if (parentId.length > 0)
            workspace.setFolderCollapsed(parentId, false)
        selectedFolderId = parentId
        beginFolderRename(created)
    }

    function beginFolderRename(folderId) {
        editingFolderId = String(folderId)
        Qt.callLater(function() {
            const row = workspace.folderNotesModel.rowForFolder(editingFolderId)
            const delegate = row >= 0 ? delegateAt(row) : null
            if (delegate)
                delegate.focusRenameField()
        })
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

    function showFolderMenu(delegate, position) {
        contextFolderId = String(delegate.folderId)
        contextFolderTitle = String(delegate.title)
        contextFolderCollapsed = Boolean(delegate.collapsed)
        contextFolderFavorite = Boolean(delegate.favorite)
        contextFolderArchived = Boolean(delegate.archived)
        folderContextMenu.popup(root, position)
    }

    Reorder.LinearReorderLayout {
        id: reorderLayout

        geometryItem: root
        sourceEntries: folderReorderController.sourceEntries
        keyProvider: function(item) { return root.rowKey(item) }
        orderProvider: function(item) { return Number(item.index) }
        extentProvider: function(item) {
            return item && item.baseHeight !== undefined
                    ? Number(item.baseHeight) : root.rowHeight
        }
    }

    Reorder.GenericReorderController {
        id: folderReorderController

        anchors.fill: parent
        geometryItem: root
        scrollItem: folderList
        compensateForScroll: true
        previewObjectName: "folderDragPreview"
        previewObjectNamePrefix: "folderDragPreviewItem-"
        previewHideSources: false
        previewLive: false
        previewCompact: true
        boundaryProvider: function() { return root.dropBoundaries() }
        commitHandler: function(payload, boundary) { return root.commitDrop(payload, boundary) }
        targetChangedHandler: function(boundary, pointerX, pointerY) {
            root.directDropTarget = root.targetAt(pointerX, pointerY)
        }
        resetHandler: function() { root.directDropTarget = null }
    }

    Connections {
        target: root.workspace ? root.workspace.folderNotesModel : null
        enabled: root.dragging

        function onModelAboutToBeReset() {
            if (!folderReorderController.committingDrop)
                root.cancelDrag()
        }
        function onRowsAboutToBeRemoved() {
            if (!folderReorderController.committingDrop)
                root.cancelDrag()
        }
        function onLayoutAboutToBeChanged() {
            if (!folderReorderController.committingDrop)
                root.cancelDrag()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: toolbarRow.implicitHeight + topPadding + bottomPadding
            padding: 3

            RowLayout {
                id: toolbarRow

                anchors.fill: parent
                spacing: 3

                ToolButton {
                    display: AbstractButton.IconOnly
                    enabled: root.workspace.folderCatalogAvailable
                    contentItem: Image {
                        source: "qrc:/icons/new"
                        sourceSize.width: 22
                        sourceSize.height: 22
                        fillMode: Image.PreserveAspectFit
                    }
                    Accessible.name: qsTr("New note in selected folder")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.createNoteInSelectedFolder()
                }

                ToolButton {
                    display: AbstractButton.IconOnly
                    enabled: root.workspace.folderCatalogAvailable
                    contentItem: Item {
                        ThemedIcon {
                            anchors.centerIn: parent
                            themeName: "folder-new-symbolic"
                            fallbackName: "folder-symbolic.svg"
                            recolorFallback: true
                            fallbackTintMode: "auto"
                            pixelSize: 21
                        }
                        Label {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            text: "+"
                            font.bold: true
                            font.pixelSize: 13
                        }
                    }
                    Accessible.name: qsTr("New folder")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.createFolder("")
                }

                ToolButton {
                    display: AbstractButton.TextOnly
                    text: "⌃"
                    font.pixelSize: 20
                    enabled: root.workspace.folderCatalogAvailable
                    Accessible.name: qsTr("Collapse all folders")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.workspace.collapseAllFolders()
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

            ListView {
                id: folderList
                objectName: "foldersList"

                anchors.fill: parent
                clip: true
                model: root.workspace.folderNotesModel
                spacing: 1
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: false
                cacheBuffer: Math.max(1200, height * 2)
                bottomMargin: root.touchActions ? 88 : 0
                ScrollBar.vertical: ScrollBar { }

                delegate: ItemDelegate {
                    id: folderDelegate

                    required property int index
                    required property int rowKind
                    required property string folderId
                    required property string parentFolderId
                    required property string storageId
                    required property string noteId
                    required property string title
                    required property int depth
                    required property bool collapsed
                    required property bool favorite
                    required property bool archived
                    required property int childFolderCount
                    required property int noteCount

                    property bool internalDragActive: false
                    property double suppressClickUntil: 0
                    readonly property real baseHeight: root.rowHeight
                    readonly property bool folderRow: Number(rowKind) === root.folderRowKind
                    readonly property bool noteRow: Number(rowKind) === root.noteRowKind
                    readonly property bool unsortedRow: Number(rowKind) === root.unsortedRowKind
                    readonly property bool editing: folderRow && root.editingFolderId === folderId
                    readonly property bool sourceActive: reorderLayout.containsSource(folderDelegate)
                    readonly property bool targetBefore: root.dropBoundary
                                                       && root.dropBoundary.ownerKey === root.rowKey(folderDelegate)
                                                       && !root.dropBoundary.afterOwner
                    readonly property bool targetAfter: root.dropBoundary
                                                      && root.dropBoundary.ownerKey === root.rowKey(folderDelegate)
                                                      && root.dropBoundary.afterOwner
                    readonly property bool directTarget: root.directDropTarget
                                                        && root.directDropTarget.key === root.rowKey(folderDelegate)
                    readonly property bool noteSelected: !root.dragSelectionSuppressed && noteRow
                                                        && root.noteIsSelected(storageId, noteId)
                    readonly property bool currentNote: noteRow
                                                       && root.currentStorageId === storageId
                                                       && root.currentNoteId === noteId
                    readonly property bool selectedFolder: folderRow
                                                         && root.selectedFolderId === folderId
                    readonly property real reorderOffset: displacement.displacement
                    readonly property string objectKey: folderRow ? "folder-" + folderId
                                                               : (noteRow ? "note-" + storageId + "-" + noteId
                                                                          : "unsorted")

                    objectName: "foldersRow-" + objectKey
                    width: folderList.width
                    implicitHeight: baseHeight
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    hoverEnabled: !root.dragSelectionSuppressed
                    opacity: sourceActive ? 0 : 1
                    transform: Translate { y: folderDelegate.reorderOffset }

                    function focusRenameField() {
                        Qt.callLater(function() {
                            if (folderDelegate.editing) {
                                renameField.forceActiveFocus()
                                renameField.selectAll()
                            }
                        })
                    }

                    function commitRename() {
                        if (!editing)
                            return
                        const name = renameField.text.trim()
                        if (name.length === 0) {
                            renameField.forceActiveFocus()
                            renameField.selectAll()
                            return
                        }
                        if (root.workspace.renameFolder(folderId, name))
                            root.editingFolderId = ""
                        else
                            focusRenameField()
                    }

                    Component.onDestruction: {
                        if (internalDragActive)
                            root.cancelDrag()
                    }

                    Reorder.ReorderDisplacement {
                        id: displacement

                        animationEnabled: root.activePayload !== null && !root.committingDrop
                        sourceActive: folderDelegate.sourceActive
                        targetBefore: folderDelegate.targetBefore
                        targetAfter: folderDelegate.targetAfter
                        naturalExtent: folderDelegate.baseHeight
                        draggedExtent: root.draggedExtent
                        displacement: root.rowTranslation(folderDelegate)
                    }

                    background: Rectangle {
                        radius: 4
                        color: folderDelegate.directTarget
                               ? Qt.rgba(0.30, 0.76, 0.38, 0.30)
                               : (folderDelegate.currentNote || folderDelegate.selectedFolder)
                               ? folderDelegate.palette.highlight
                               : folderDelegate.noteSelected
                               ? Qt.rgba(folderDelegate.palette.highlight.r,
                                         folderDelegate.palette.highlight.g,
                                         folderDelegate.palette.highlight.b, 0.38)
                               : (folderDelegate.hovered
                                  ? Qt.rgba(folderDelegate.palette.button.r,
                                            folderDelegate.palette.button.g,
                                            folderDelegate.palette.button.b, 0.45)
                                  : "transparent")
                        border.width: folderDelegate.directTarget ? 2 : 0
                        border.color: Qt.rgba(0.22, 0.68, 0.30, 0.95)

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: 0
                            height: 3
                            visible: folderDelegate.targetBefore
                            color: folderDelegate.palette.highlight
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: parent.height - height
                            height: 3
                            visible: folderDelegate.targetAfter
                            color: folderDelegate.palette.highlight
                        }
                    }

                    contentItem: RowLayout {
                        spacing: 6

                        Item {
                            Layout.preferredWidth: 8 + folderDelegate.depth * 18
                            Layout.fillHeight: true
                        }

                        Item {
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 22
                            Layout.alignment: Qt.AlignVCenter
                            visible: folderDelegate.folderRow

                            ToolButton {
                                anchors.fill: parent
                                padding: 0
                                text: folderDelegate.collapsed ? "▶" : "▼"
                                font.pixelSize: 11
                                Accessible.name: folderDelegate.collapsed
                                                 ? qsTr("Expand %1").arg(folderDelegate.title)
                                                 : qsTr("Collapse %1").arg(folderDelegate.title)
                                onClicked: root.workspace.setFolderCollapsed(folderDelegate.folderId,
                                                                              !folderDelegate.collapsed)
                            }
                        }

                        Item {
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                            Layout.alignment: Qt.AlignVCenter

                            ThemedIcon {
                                anchors.centerIn: parent
                                visible: folderDelegate.folderRow
                                themeName: "folder-symbolic"
                                fallbackName: "folder-symbolic.svg"
                                recolorFallback: true
                                fallbackTintMode: String(folderDelegate.palette.text)
                                pixelSize: 20
                            }

                            Label {
                                anchors.centerIn: parent
                                visible: folderDelegate.noteRow
                                text: "◆"
                                font.pixelSize: 14
                                color: folderDelegate.currentNote
                                       ? folderDelegate.palette.highlightedText
                                       : folderDelegate.palette.text
                            }

                            Label {
                                anchors.centerIn: parent
                                visible: folderDelegate.unsortedRow
                                text: "•"
                                font.pixelSize: 20
                                color: folderDelegate.palette.placeholderText
                            }
                        }

                        TextField {
                            id: renameField

                            objectName: "folderRenameField-" + folderDelegate.folderId
                            Layout.fillWidth: true
                            visible: folderDelegate.editing
                            enabled: visible
                            text: folderDelegate.title
                            selectByMouse: true
                            verticalAlignment: TextInput.AlignVCenter
                            onVisibleChanged: {
                                if (visible)
                                    folderDelegate.focusRenameField()
                            }
                            onAccepted: folderDelegate.commitRename()
                            onEditingFinished: folderDelegate.commitRename()
                            Keys.onEscapePressed: function(event) {
                                root.cancelFolderRename(folderDelegate.folderId)
                                event.accepted = true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !folderDelegate.editing
                            text: folderDelegate.folderRow
                                  ? qsTr("%1 (%2)").arg(folderDelegate.title)
                                    .arg(folderDelegate.noteCount)
                                  : folderDelegate.title
                            font.bold: folderDelegate.folderRow || folderDelegate.unsortedRow
                            color: (folderDelegate.currentNote || folderDelegate.selectedFolder)
                                   ? folderDelegate.palette.highlightedText
                                   : (folderDelegate.archived
                                      ? folderDelegate.palette.placeholderText
                                      : folderDelegate.palette.text)
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }

                        Label {
                            Layout.preferredWidth: visible ? implicitWidth : 0
                            visible: folderDelegate.folderRow && folderDelegate.favorite
                            text: "★"
                            font.pixelSize: 14
                            color: (folderDelegate.currentNote || folderDelegate.selectedFolder)
                                   ? folderDelegate.palette.highlightedText
                                   : folderDelegate.palette.highlight
                            Accessible.name: qsTr("Favorite folder")
                        }

                        CheckBox {
                            visible: root.touchActions && folderDelegate.noteRow
                            checked: root.noteIsSelected(folderDelegate.storageId, folderDelegate.noteId)
                            Accessible.name: qsTr("Select %1").arg(folderDelegate.title)
                            onClicked: root.setNoteSelected(folderDelegate.storageId,
                                                            folderDelegate.noteId,
                                                            folderDelegate.title,
                                                            folderDelegate.folderId,
                                                            checked)
                        }

                        Item {
                            Layout.preferredWidth: folderDelegate.folderRow || folderDelegate.noteRow ? 24 : 0
                            Layout.preferredHeight: 24
                            visible: folderDelegate.folderRow || folderDelegate.noteRow

                            Label {
                                anchors.centerIn: parent
                                text: "⠿"
                                font.pixelSize: 16
                                color: folderDelegate.palette.placeholderText
                            }

                            Reorder.ReorderDragHandle {
                                anchors.fill: parent
                                dragEnabled: root.workspace.folderCatalogAvailable
                                             && !folderDelegate.editing
                                onDragStarted: root.beginDrag(folderDelegate)
                                onDragMoved: function(dx, dy) {
                                    root.moveDrag(folderDelegate, dx, dy)
                                }
                                onDragFinished: root.finishDrag(folderDelegate)
                            }
                        }
                    }

                    TapHandler {
                        id: desktopSelectionHandler

                        acceptedButtons: Qt.LeftButton
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        enabled: folderDelegate.noteRow && !root.touchActions
                                 && !root.dragSelectionSuppressed && !folderDelegate.editing
                        gesturePolicy: TapHandler.DragThreshold
                        onTapped: function(eventPoint, button) {
                            folderDelegate.suppressClickUntil = Date.now() + 100
                            root.selectDesktopNote(folderDelegate,
                                                   desktopSelectionHandler.point.modifiers)
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        acceptedDevices: PointerDevice.Mouse
                        enabled: folderDelegate.noteRow && root.embeddedEditor
                                 && !root.dragSelectionSuppressed
                        onDoubleTapped: root.noteStandaloneRequested(folderDelegate.storageId,
                                                                       folderDelegate.noteId)
                    }

                    onClicked: {
                        if (root.dragSelectionSuppressed
                                || Date.now() < suppressClickUntil) {
                            suppressClickUntil = 0
                            return
                        }
                        if (folderRow) {
                            root.selectedFolderId = folderId
                            root.workspace.setFolderCollapsed(folderId, !collapsed)
                        } else if (unsortedRow) {
                            root.selectedFolderId = ""
                        } else if (noteRow && root.touchActions) {
                            root.selectedFolderId = folderId
                            root.noteActivated(storageId, noteId, title)
                        }
                    }

                    MouseArea {
                        id: contextArea

                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        preventStealing: true
                        onClicked: function(mouse) {
                            const position = contextArea.mapToItem(root, Qt.point(mouse.x, mouse.y))
                            if (folderDelegate.folderRow)
                                root.showFolderMenu(folderDelegate, position)
                            else if (folderDelegate.noteRow)
                                root.noteMenuRequested(folderDelegate.storageId,
                                                       folderDelegate.noteId,
                                                       folderDelegate.title,
                                                       position)
                        }
                    }

                    TapHandler {
                        enabled: root.touchActions
                        acceptedButtons: Qt.LeftButton
                        acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                        gesturePolicy: TapHandler.DragThreshold
                        onLongPressed: {
                            folderDelegate.suppressClickUntil = Date.now() + 1000
                            if (folderDelegate.folderRow)
                                root.showFolderMenu(folderDelegate, Qt.point(folderDelegate.width / 2,
                                                                              folderDelegate.height / 2))
                            else if (folderDelegate.noteRow)
                                root.noteMenuRequested(folderDelegate.storageId,
                                                       folderDelegate.noteId,
                                                       folderDelegate.title,
                                                       Qt.point(folderDelegate.width / 2,
                                                                folderDelegate.height / 2))
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: folderList.count <= 1
                         && root.workspace.noteCount === 0 && root.workspace.folderCatalogAvailable
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
    }
}
