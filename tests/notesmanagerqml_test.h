#pragma once

#include <QObject>

class NotesManagerQmlTest : public QObject {
    Q_OBJECT

private slots:
    void loadsNotesManagerQmlShell();
    void notesManagerFoldersTabInstantiatesInOwnerContext();
    void notesManagerContextMenusAndSelectionWork();
    void notesManagerInternalDragsWork();
    void notesManagerVirtualizedDragsWork();
    void recentNoteSwipeClosesEveryDeleteAction();
    void notesManagerOutsideDropRecyclesOrPermanentlyDeletes();
    void flatNoteCollectionUsesSharedTreeDragAnimation();
    void genericReorderUsesOutsideDropHandler();
    void foldersPageUsesInlineRenameAndSharedDragLifecycle();
    void foldersPageOffersEmptyRecycleBinAction();
    void folderPickerMenuBuildsTheCompleteFolderTree();
    void editorToolbarFolderPickerAssignsTheActiveNote();
};
