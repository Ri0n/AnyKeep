pragma ComponentBehavior: Bound

import QtQuick

NoteListRow {
    id: treeRow

    required property int row
    required property int column
    required property int depth
    required property bool expanded
    required property bool hasChildren
    required property bool isTreeNode
    required property var model

    rowIndex: row
    itemDepth: collection.nativeModelHierarchy
               ? depth : collection.numberRole(model, "depth", depth)
    itemType: collection.numberRole(
                  model, "itemType",
                  collection.numberRole(model, "rowKind", collection.noteItemType))
    rowKind: itemType
    storageId: collection.stringRole(model, "storageId")
    noteId: collection.stringRole(model, "noteId")
    title: collection.stringRole(model, "title")
    preview: collection.stringRole(model, "preview")
    storageName: collection.stringRole(model, "storageName")
    iconSource: collection.stringRole(model, "iconSource")
    groupKind: collection.stringRole(
                   model, "groupKind",
                   itemType === collection.noteItemType ? "" : collection.defaultGroupKind)
    groupId: collection.stringRole(
                 model, "groupId",
                 collection.stringRole(model, "folderId", storageId))
    parentGroupId: collection.stringRole(
                       model, "parentGroupId",
                       collection.stringRole(model, "parentFolderId"))
    folderId: collection.stringRole(model, "folderId")
    parentFolderId: collection.stringRole(model, "parentFolderId")
    groupExpanded: collection.nativeModelHierarchy
                   ? expanded : !collection.boolRole(model, "collapsed", false)
    groupCollapsed: !groupExpanded
    groupHasChildren: collection.nativeModelHierarchy
                      ? isTreeNode && hasChildren
                      : collection.numberRole(model, "childFolderCount", 0) > 0
                        || collection.numberRole(model, "noteCount", 0) > 0
    favorite: collection.boolRole(model, "favorite", false)
    archived: collection.boolRole(model, "archived", false)
    systemFolder: collection.boolRole(model, "systemFolder", false)
    pendingDraft: collection.boolRole(model, "pendingDraft", false)
    draftState: collection.stringRole(model, "draftState")
    draftError: collection.stringRole(model, "draftError")
    loading: collection.boolRole(model, "loading", false)
    errorString: collection.stringRole(model, "errorString")
    noteCount: collection.numberRole(model, "noteCount", 0)
}
