pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Menu {
    id: root

    required property var workspace
    property string currentFolderId: ""
    property bool selectionMixed: false
    // Archived notes belong to an archived folder; there is no per-note archive flag.
    property bool archivedOnly: false
    signal folderSelected(string folderId)
    signal createArchivedFolderRequested()

    title: archivedOnly ? qsTr("Archive in folder") : qsTr("Move to folder")
    enabled: workspace && workspace.folderCatalogAvailable

    function indentation(depth) {
        let result = ""
        for (let level = 0; level < Number(depth); ++level)
            result += "    "
        return result
    }

    function refresh() {
        pickerEntries.clear()
        if (!workspace || !workspace.folderCatalogAvailable || !workspace.folderNotesModel)
            return
        const items = workspace.folderNotesModel.folderPickerItems(root.archivedOnly)
        for (const item of items) {
            if (root.archivedOnly && !Boolean(item.inArchivedBranch))
                continue
            pickerEntries.append({
                folderId: String(item.folderId || ""),
                title: String(item.title || ""),
                depth: Number(item.depth || 0),
                favorite: Boolean(item.favorite)
            })
        }
    }

    function selectFolder(folderId) {
        root.folderSelected(String(folderId || ""))
    }

    onAboutToShow: refresh()

    ListModel {
        id: pickerEntries
    }

    MenuItem {
        objectName: "folderPickerItem-unsorted"
        text: qsTr("Unsorted")
        visible: !root.archivedOnly
        checkable: true
        checked: !root.selectionMixed && root.currentFolderId.length === 0
        onTriggered: root.selectFolder("")
    }

    MenuSeparator {
        visible: !root.archivedOnly && pickerEntries.count > 0
    }

    Instantiator {
        model: pickerEntries

        delegate: MenuItem {
            required property string folderId
            required property string title
            required property int depth
            required property bool favorite

            objectName: "folderPickerItem-" + folderId
            text: (favorite ? "★ " : "") + root.indentation(depth) + title
            checkable: true
            checked: !root.selectionMixed && root.currentFolderId === folderId
            onTriggered: root.selectFolder(folderId)
        }

        onObjectAdded: function(index, object) { root.insertItem(index + 2, object) }
        onObjectRemoved: function(index, object) { root.removeItem(object) }
    }

    MenuSeparator { visible: root.archivedOnly && pickerEntries.count > 0 }
    MenuItem {
        objectName: "folderPickerCreateArchived"
        visible: root.archivedOnly
        text: qsTr("Create archived folder…")
        onTriggered: root.createArchivedFolderRequested()
    }
}
