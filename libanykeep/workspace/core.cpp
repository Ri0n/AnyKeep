#include "notesworkspacecontroller.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "foldernotesmodel.h"
#include "folderoperationscontroller.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notesmodel.h"
#include "notessearchmodel.h"
#include "notestorage.h"
#include "notetransfercontroller.h"
#include "recentnotesmodel.h"
#include "storagejob.h"
#include "storageprioritymodel.h"
#include "utils.h"

#include <QLoggingCategory>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <limits>

namespace AnyKeep {

NotesWorkspaceController::NotesWorkspaceController(QObject *parent) :
    NotesWorkspaceController(FolderCatalogManager::instance(), DraftManager::instance(), parent)
{
}

NotesWorkspaceController::NotesWorkspaceController(FolderCatalogManager *folderCatalogManager, QObject *parent) :
    NotesWorkspaceController(folderCatalogManager, DraftManager::instance(), parent)
{
}

NotesWorkspaceController::NotesWorkspaceController(FolderCatalogManager *folderCatalogManager,
                                                   DraftManager *draftManager, QObject *parent) : QObject(parent)
{
    folderCatalogManager_ = folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance();
    draftManager_         = draftManager ? draftManager : DraftManager::instance();
    notesModel_           = new NotesModel(folderCatalogManager_, this);
    searchModel_          = new NotesSearchModel(this);
    searchModel_->setSourceModel(notesModel_);
    recentNotesModel_     = new RecentNotesModel(searchModel_, this);
    storagePriorityModel_ = new StoragePriorityModel(this);
    folderNotesModel_     = new FolderNotesModel(folderCatalogManager_, this);
    folderNotesModel_->setSearchModel(searchModel_);
    folderOperations_ = new FolderOperationsController(folderCatalogManager_, NoteManager::instance(), this);
    connect(this, &NotesWorkspaceController::trashUndoChanged, this, &NotesWorkspaceController::folderTrashUndoChanged);

    connect(notesModel_, &NotesModel::statsChanged, this, &NotesWorkspaceController::noteCountChanged);
    connect(notesModel_, &NotesModel::notesDropRequested, this,
            [this](const QStringList &storageIds, const QStringList &noteIds, const QString &destinationStorageId) {
                const int count = qMin(storageIds.size(), noteIds.size());
                for (int i = 0; i < count; ++i)
                    moveNote(storageIds.at(i), noteIds.at(i), destinationStorageId);
            });
    connect(searchModel_, &NotesSearchModel::searchTextChanged, this, &NotesWorkspaceController::searchTextChanged);
    connect(searchModel_, &NotesSearchModel::searchInBodyChanged, this, &NotesWorkspaceController::searchInBodyChanged);
    connect(searchModel_, &NotesSearchModel::searchingChanged, this, &NotesWorkspaceController::searchingChanged);

    auto *manager = NoteManager::instance();
    connect(manager, &NoteManager::storageAdded, this, &NotesWorkspaceController::storagesChanged);
    connect(manager, &NoteManager::storageRemoved, this, &NotesWorkspaceController::storagesChanged);
    connect(manager, &NoteManager::storageChanged, this, &NotesWorkspaceController::storagesChanged);
    connect(manager, &NoteManager::storageReady, this, &NotesWorkspaceController::storagesChanged);
    connect(manager, &NoteManager::storageOrderChanged, this, &NotesWorkspaceController::storagesChanged);

    connect(folderCatalogManager_, &FolderCatalogManager::availabilityChanged, this,
            [this](bool) { emit folderCatalogAvailabilityChanged(); });
    connect(folderOperations_, &FolderOperationsController::busyChanged, this, &NotesWorkspaceController::busyChanged);
    connect(folderOperations_, &FolderOperationsController::errorStringChanged, this, [this] {
        if (!folderOperations_->errorString().isEmpty())
            setError(folderOperations_->errorString());
    });
    connect(folderOperations_, &FolderOperationsController::assignmentFinished, this,
            [this](const QString &storageId, const QString &noteId, const QUuid &folderId, bool nativeStored) {
                if (!nativeStored)
                    return;
                if (!currentEditor_ || currentEditor_->storageId() != storageId || currentEditor_->noteId() != noteId
                    || currentEditor_->folderId() != folderId) {
                    return;
                }
                currentEditor_->markFolderPersisted(folderId);
                pendingFolderAssignments_.remove(currentEditor_->draftId());
            });

    auto *drafts = draftManager_;
    connect(drafts, &DraftManager::draftPublished, this, [this, drafts](const QUuid &draftId, const Note &note) {
        const auto assignment = pendingFolderAssignments_.find(draftId);
        if (assignment != pendingFolderAssignments_.end() && !note.storageId().isEmpty() && !note.id().isEmpty()) {
            if (folderOperations_->storeOverlayAssignment(note.storageId(), note.id(), assignment->folderId))
                pendingFolderAssignments_.erase(assignment);
        }

        if (!pendingMoves_.contains(draftId))
            return;
        const auto move  = pendingMoves_.take(draftId);
        const auto error = drafts->queueRemoval(move.sourceStorageId, move.sourceNoteId);
        if (error)
            setError(error.message);
        completePendingReorderMove(move.reorderBatchId, move.reorderIndex, note.id());
        drafts->publishPending();
        endOperation();
    });
    connect(drafts, &DraftManager::draftPublishFailed, this, [this](const QUuid &draftId, const QString &message) {
        if (pendingFolderAssignments_.contains(draftId))
            setError(message);
        if (!pendingMoves_.contains(draftId))
            return;
        const auto move = pendingMoves_.take(draftId);
        completePendingReorderMove(move.reorderBatchId, move.reorderIndex, {});
        setError(message);
        endOperation();
    });
}

NotesWorkspaceController::~NotesWorkspaceController() { closeCurrentNote(); }

QAbstractItemModel *NotesWorkspaceController::notesModel() const { return searchModel_; }
QAbstractItemModel *NotesWorkspaceController::groupedNotesModel() const { return searchModel_; }
QAbstractItemModel *NotesWorkspaceController::recentNotesModel() const { return recentNotesModel_; }
QAbstractItemModel *NotesWorkspaceController::folderNotesModel() const { return folderNotesModel_; }
QAbstractItemModel *NotesWorkspaceController::storagePriorityModel() const { return storagePriorityModel_; }
QObject            *NotesWorkspaceController::currentEditor() const { return currentEditor_; }
NoteEditor         *NotesWorkspaceController::editor() const { return currentEditor_.data(); }
QString             NotesWorkspaceController::currentStorageId() const
{
    return currentEditor_ ? currentEditor_->storageId() : QString();
}
QString NotesWorkspaceController::currentNoteId() const
{
    return currentEditor_ ? currentEditor_->noteId() : QString();
}
QString NotesWorkspaceController::currentTitle() const
{
    if (!currentEditor_)
        return {};
    return Utils::splitTitle(currentEditor_->text()).first.trimmed();
}
QString NotesWorkspaceController::currentFolderId() const
{
    return currentEditor_ ? currentEditor_->folderIdString() : QString();
}
bool NotesWorkspaceController::busy() const
{
    return loading_ || pendingOperations_ > 0 || (folderOperations_ && folderOperations_->busy());
}
int     NotesWorkspaceController::noteCount() const { return notesModel_->noteCount(); }
QString NotesWorkspaceController::searchText() const { return searchModel_->searchText(); }
bool    NotesWorkspaceController::searchInBody() const { return searchModel_->searchInBody(); }
bool    NotesWorkspaceController::searching() const { return searchModel_->searching(); }

bool NotesWorkspaceController::noteMatchesBodySearch(const QString &storageId, const QString &noteId) const
{
    return searchModel_ && searchModel_->searchInBody() && !searchModel_->searchText().trimmed().isEmpty()
        && searchModel_->hasBodyMatch(storageId, noteId);
}

bool NotesWorkspaceController::fetchMoreGroupedNotes(const QString &storageId, const QString &lastVisibleNoteId)
{
    return notesModel_ && notesModel_->fetchMoreNear(storageId, lastVisibleNoteId);
}

bool NotesWorkspaceController::folderCatalogAvailable() const
{
    return folderCatalogManager_ && folderCatalogManager_->isAvailable();
}

QString NotesWorkspaceController::lastTrashedFolderName() const
{
    for (auto entry = trashUndoEntries_.crbegin(); entry != trashUndoEntries_.crend(); ++entry) {
        if (entry->kind != TrashUndoEntry::FolderTrash)
            continue;
        for (const auto &folder : entry->folderBranch.folders) {
            if (folder.id == entry->folderBranch.rootId)
                return folder.name;
        }
    }
    return {};
}

bool NotesWorkspaceController::canUndoFolderTrash() const
{
    return std::any_of(trashUndoEntries_.cbegin(), trashUndoEntries_.cend(),
                       [](const TrashUndoEntry &entry) { return entry.kind == TrashUndoEntry::FolderTrash; });
}

QString NotesWorkspaceController::lastTrashedItemName() const
{
    if (trashUndoEntries_.isEmpty())
        return {};
    const auto &entry = trashUndoEntries_.constLast();
    if (entry.kind == TrashUndoEntry::NoteTrash)
        return entry.title;
    for (const auto &folder : entry.folderBranch.folders) {
        if (folder.id == entry.folderBranch.rootId)
            return folder.name;
    }
    return {};
}

QVariantList NotesWorkspaceController::storages() const
{
    QVariantList result;
    for (const auto &storage : NoteManager::instance()->prioritizedStorages(true)) {
        if (!storage)
            continue;
        QVariantMap item;
        item.insert(QStringLiteral("storageId"), storage->systemName());
        item.insert(QStringLiteral("name"), storage->name());
        item.insert(QStringLiteral("accessible"), storage->isAccessible());
        item.insert(QStringLiteral("supportsMedia"), storage->supportsMedia());
        item.insert(QStringLiteral("supportsNoteReordering"), storage->supportsNoteReordering());
        result.append(item);
    }
    return result;
}

void NotesWorkspaceController::setSearchText(const QString &text) { searchModel_->setSearchText(text); }
void NotesWorkspaceController::setSearchInBody(bool enabled) { searchModel_->setSearchInBody(enabled); }

void NotesWorkspaceController::setCurrentEditor(NoteEditor *editor)
{
    if (currentEditor_ == editor)
        return;
    currentEditor_ = editor;
    if (editor)
        connectEditorSignals(editor);
    emit currentEditorChanged();
    emit currentTitleChanged();
    emit currentFolderIdChanged();
}

void NotesWorkspaceController::clearCurrentEditor()
{
    if (!currentEditor_)
        return;
    auto *old = currentEditor_.data();
    currentEditor_.clear();
    if (!old->hasPersistedDraft())
        pendingFolderAssignments_.remove(old->draftId());
    old->deleteLater();
    emit currentEditorChanged();
    emit currentTitleChanged();
    emit currentFolderIdChanged();
}

void NotesWorkspaceController::setLoading(bool loading)
{
    if (loading_ == loading)
        return;
    const bool oldBusy = busy();
    loading_           = loading;
    emit loadingChanged();
    if (oldBusy != busy())
        emit busyChanged();
}

void NotesWorkspaceController::setError(const QString &error)
{
    if (errorString_ == error)
        return;
    errorString_ = error;
    emit errorStringChanged();
}

void NotesWorkspaceController::beginOperation()
{
    const bool oldBusy = busy();
    ++pendingOperations_;
    if (oldBusy != busy())
        emit busyChanged();
}

void NotesWorkspaceController::endOperation()
{
    const bool oldBusy = busy();
    pendingOperations_ = qMax(0, pendingOperations_ - 1);
    if (oldBusy != busy())
        emit busyChanged();
}

void NotesWorkspaceController::connectEditorSignals(NoteEditor *editor)
{
    connect(editor, &NoteEditor::textChanged, this, &NotesWorkspaceController::currentTitleChanged);
    connect(editor, &NoteEditor::folderIdChanged, this, &NotesWorkspaceController::currentFolderIdChanged);
    connect(editor, &NoteEditor::errorStringChanged, this, [this, editor]() { setError(editor->errorString()); });
}

} // namespace AnyKeep
