#ifndef NOTESWORKSPACECONTROLLER_H
#define NOTESWORKSPACECONTROLLER_H

#include "foldercatalog.h"
#include "note.h"
#include "qtnote_export.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

namespace QtNote {

class NoteEditor;
class NoteLoadJob;
class NoteStorage;
class FolderCatalogManager;
class FolderNotesModel;
class FolderOperationsController;
struct FolderCatalogError;
class NotesModel;
class NotesSearchModel;
class RecentNotesModel;
class StoragePriorityModel;

class QTNOTE_EXPORT NotesWorkspaceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *notesModel READ notesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *groupedNotesModel READ groupedNotesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *recentNotesModel READ recentNotesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *folderNotesModel READ folderNotesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *storagePriorityModel READ storagePriorityModel CONSTANT)
    Q_PROPERTY(QObject *currentEditor READ currentEditor NOTIFY currentEditorChanged)
    Q_PROPERTY(QString currentStorageId READ currentStorageId NOTIFY currentEditorChanged)
    Q_PROPERTY(QString currentNoteId READ currentNoteId NOTIFY currentEditorChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY currentTitleChanged)
    Q_PROPERTY(QString currentFolderId READ currentFolderId NOTIFY currentFolderIdChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(int noteCount READ noteCount NOTIFY noteCountChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(bool searchInBody READ searchInBody WRITE setSearchInBody NOTIFY searchInBodyChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(QVariantList storages READ storages NOTIFY storagesChanged)
    Q_PROPERTY(bool folderCatalogAvailable READ folderCatalogAvailable NOTIFY folderCatalogAvailabilityChanged)
    Q_PROPERTY(bool canUndoFolderTrash READ canUndoFolderTrash NOTIFY folderTrashUndoChanged)
    Q_PROPERTY(QString lastTrashedFolderName READ lastTrashedFolderName NOTIFY folderTrashUndoChanged)

public:
    explicit NotesWorkspaceController(QObject *parent = nullptr);
    NotesWorkspaceController(FolderCatalogManager *folderCatalogManager, QObject *parent);
    ~NotesWorkspaceController() override;

    QAbstractItemModel *notesModel() const;
    QAbstractItemModel *groupedNotesModel() const;
    QAbstractItemModel *recentNotesModel() const;
    QAbstractItemModel *folderNotesModel() const;
    QAbstractItemModel *storagePriorityModel() const;
    NotesModel         *sourceModel() const { return notesModel_; }
    NotesSearchModel   *searchModel() const { return searchModel_; }
    QObject            *currentEditor() const;
    NoteEditor         *editor() const;
    QString             currentStorageId() const;
    QString             currentNoteId() const;
    QString             currentTitle() const;
    QString             currentFolderId() const;
    bool                loading() const { return loading_; }
    bool                busy() const;
    QString             errorString() const { return errorString_; }
    int                 noteCount() const;
    QString             searchText() const;
    bool                searchInBody() const;
    bool                searching() const;
    QVariantList        storages() const;
    bool                folderCatalogAvailable() const;
    bool                canUndoFolderTrash() const { return !deletedFolderBranches_.isEmpty(); }
    QString             lastTrashedFolderName() const;

    Q_INVOKABLE bool openNote(const QString &storageId, const QString &noteId);
    bool             openNote(const Note &note, const QUuid &draftId = {});
    Q_INVOKABLE bool createNote(const QString &storageId = {});
    Q_INVOKABLE bool saveCurrentNote();
    Q_INVOKABLE bool closeCurrentNote();
    Q_INVOKABLE bool reloadCurrentNote();
    Q_INVOKABLE bool deleteNote(const QString &storageId, const QString &noteId);
    Q_INVOKABLE bool trashNote(const QString &storageId, const QString &noteId);
    Q_INVOKABLE bool restoreRecycledNote(const QString &storageId, const QString &noteId);
    Q_INVOKABLE bool emptyRecycleBin();
    Q_INVOKABLE bool isRecycledNote(const QString &storageId, const QString &noteId) const;
    Q_INVOKABLE bool askBeforePermanentDelete() const;
    Q_INVOKABLE void setAskBeforePermanentDelete(bool enabled);
    Q_INVOKABLE bool moveNote(const QString &sourceStorageId, const QString &noteId,
                              const QString &destinationStorageId);
    Q_INVOKABLE bool moveCurrentNote(const QString &destinationStorageId);
    Q_INVOKABLE bool copyNote(const QString &sourceStorageId, const QString &noteId,
                              const QString &destinationStorageId);
    Q_INVOKABLE bool moveNotes(const QVariantList &notes, const QString &destinationStorageId,
                               const QString &anchorNoteId = {}, bool insertAfter = false);
    /**
     * Reorder the time-based Recent projection without turning a drop into a
     * cross-storage move.  A native storage owns timestamp ordering, so both
     * the dragged notes and their boundary must belong to it.
     */
    Q_INVOKABLE bool    reorderRecentNotes(const QVariantList &notes, const QString &anchorStorageId,
                                           const QString &anchorNoteId, bool insertAfter = false);
    Q_INVOKABLE bool    moveStorage(const QString &sourceStorageId, const QString &destinationStorageId);
    Q_INVOKABLE bool    moveStorageToRow(const QString &sourceStorageId, int destinationRow);
    Q_INVOKABLE void    openStorageSettings(const QString &storageId);
    Q_INVOKABLE void    refresh();
    Q_INVOKABLE bool    openStandalone(const QString &storageId, const QString &noteId);
    Q_INVOKABLE bool    openCurrentStandalone();
    Q_INVOKABLE QString createFolder(const QString &name, const QString &parentFolderId = {});
    Q_INVOKABLE bool    renameFolder(const QString &folderId, const QString &name);
    Q_INVOKABLE bool    moveFolder(const QString &folderId, const QString &parentFolderId, qint64 sortOrder);
    Q_INVOKABLE bool    moveFolderBefore(const QString &folderId, const QString &parentFolderId,
                                         const QString &beforeFolderId = {});
    Q_INVOKABLE bool    setFolderCollapsed(const QString &folderId, bool collapsed);
    Q_INVOKABLE bool    setUnsortedCollapsed(bool collapsed);
    Q_INVOKABLE bool    setFolderFlags(const QString &folderId, bool favorite, bool archived);
    Q_INVOKABLE bool    trashFolder(const QString &folderId);
    Q_INVOKABLE bool    undoFolderTrash();
    Q_INVOKABLE void    clearFolderTrashUndo();
    Q_INVOKABLE bool    collapseAllFolders();
    Q_INVOKABLE QString folderIdForNote(const QString &storageId, const QString &noteId) const;
    Q_INVOKABLE bool    assignNoteFolder(const QString &storageId, const QString &noteId, const QString &folderId);
    Q_INVOKABLE bool    assignCurrentNoteFolder(const QString &folderId);
    Q_INVOKABLE bool    createNoteInFolder(const QString &folderId, const QString &storageId = {});

public slots:
    void setSearchText(const QString &text);
    void setSearchInBody(bool enabled);

signals:
    void currentEditorChanged();
    void currentTitleChanged();
    void currentFolderIdChanged();
    void loadingChanged();
    void busyChanged();
    void errorStringChanged();
    void noteCountChanged();
    void searchTextChanged();
    void searchInBodyChanged();
    void searchingChanged();
    void storagesChanged();
    void folderCatalogAvailabilityChanged();
    void folderTrashUndoChanged();
    void openStandaloneRequested(const QString &storageId, const QString &noteId);
    void storageSettingsRequested(const QString &storageId);

private:
    struct PendingMove {
        QString sourceStorageId;
        QString sourceNoteId;
        QUuid   reorderBatchId;
        int     reorderIndex { -1 };
    };

    struct PendingReorder {
        QPointer<NoteStorage> storage;
        QString               afterNoteId;
        QStringList           orderedNoteIds;
        int                   pendingMoves { 0 };
    };

    struct PendingFolderAssignment {
        QUuid folderId;
    };

    void    setCurrentEditor(NoteEditor *editor);
    void    clearCurrentEditor();
    void    setLoading(bool loading);
    void    setError(const QString &error);
    void    beginOperation();
    void    endOperation();
    bool    stageMove(const Note &source, const QString &destinationStorageId, QUuid *draftId,
                      bool folderUserOverride = false);
    void    startStagedMove(const QUuid &draftId, const Note &source, const QUuid &reorderBatchId = {},
                            int reorderIndex = -1);
    bool    beginMove(const Note &source, const QString &destinationStorageId, const QUuid &reorderBatchId = {},
                      int reorderIndex = -1);
    bool    moveNoteAt(const QString &sourceStorageId, const QString &noteId, const QString &destinationStorageId,
                       const QUuid &reorderBatchId = {}, int reorderIndex = -1);
    bool    startStorageReorder(NoteStorage *storage, const QStringList &noteIds, const QString &afterNoteId);
    void    completePendingReorderMove(const QUuid &batchId, int index, const QString &destinationNoteId);
    void    connectEditorSignals(NoteEditor *editor);
    bool    ensureFolderCatalogAvailable();
    bool    parseFolderId(const QString &text, QUuid *folderId);
    QUuid   effectiveFolderId(const Note &note) const;
    qint64  nextFolderSortOrder(const QUuid &parentFolderId) const;
    QString defaultFolderName(const QUuid &parentFolderId) const;
    bool    applyFolderMutation(const FolderCatalogError &error);
    void    rememberPendingFolderAssignment(const QUuid &draftId, const QUuid &folderId);

    NotesModel                           *notesModel_ { nullptr };
    NotesSearchModel                     *searchModel_ { nullptr };
    RecentNotesModel                     *recentNotesModel_ { nullptr };
    FolderCatalogManager                 *folderCatalogManager_ { nullptr };
    FolderNotesModel                     *folderNotesModel_ { nullptr };
    FolderOperationsController           *folderOperations_ { nullptr };
    StoragePriorityModel                 *storagePriorityModel_ { nullptr };
    QPointer<NoteEditor>                  currentEditor_;
    QPointer<NoteLoadJob>                 loadJob_;
    QHash<QUuid, PendingMove>             pendingMoves_;
    QHash<QUuid, PendingReorder>          pendingReorders_;
    QHash<QUuid, PendingFolderAssignment> pendingFolderAssignments_;
    QList<DeletedFolderBranch>            deletedFolderBranches_;
    bool                                  loading_ { false };
    int                                   pendingOperations_ { 0 };
    QString                               errorString_;
};

} // namespace QtNote

#endif // NOTESWORKSPACECONTROLLER_H
