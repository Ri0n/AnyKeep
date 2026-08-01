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

namespace QtNote {

Q_LOGGING_CATEGORY(logWorkspaceFolders, "qtnote.workspace.folders")

NotesWorkspaceController::NotesWorkspaceController(QObject *parent) :
    NotesWorkspaceController(FolderCatalogManager::instance(), parent)
{
}

NotesWorkspaceController::NotesWorkspaceController(FolderCatalogManager *folderCatalogManager, QObject *parent) :
    QObject(parent)
{
    folderCatalogManager_ = folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance();
    notesModel_           = new NotesModel(folderCatalogManager_, this);
    searchModel_          = new NotesSearchModel(this);
    searchModel_->setSourceModel(notesModel_);
    recentNotesModel_     = new RecentNotesModel(searchModel_, this);
    storagePriorityModel_ = new StoragePriorityModel(this);
    folderNotesModel_     = new FolderNotesModel(folderCatalogManager_, this);
    folderNotesModel_->setSearchModel(searchModel_);
    folderOperations_ = new FolderOperationsController(folderCatalogManager_, NoteManager::instance(), this);

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

    auto *drafts = DraftManager::instance();
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

bool NotesWorkspaceController::folderCatalogAvailable() const
{
    return folderCatalogManager_ && folderCatalogManager_->isAvailable();
}

QString NotesWorkspaceController::lastTrashedFolderName() const
{
    if (deletedFolderBranches_.isEmpty())
        return {};
    const auto &branch = deletedFolderBranches_.constLast();
    for (const auto &folder : branch.folders) {
        if (folder.id == branch.rootId)
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

bool NotesWorkspaceController::openNote(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return false;
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId)
        return true;
    if (loadJob_)
        loadJob_->cancel();

    setError({});
    setLoading(true);
    auto *job = NoteManager::instance()->loadNoteAsync(storageId, noteId, this);
    loadJob_  = job;
    connect(job, &StorageJob::finished, this, [this, job]() {
        if (loadJob_ != job) {
            job->deleteLater();
            return;
        }
        loadJob_.clear();
        setLoading(false);
        if (job->state() != StorageJob::Succeeded) {
            if (job->state() != StorageJob::Cancelled)
                setError(job->error().message.isEmpty() ? tr("Failed to load note") : job->error().message);
            job->deleteLater();
            return;
        }
        const Note loaded = job->result();
        job->deleteLater();
        if (!openNote(loaded))
            setError(errorString_.isEmpty() ? tr("Could not switch to the selected note") : errorString_);
    });
    return true;
}

bool NotesWorkspaceController::openNote(const Note &note, const QUuid &draftId)
{
    if (note.isNull())
        return false;
    if (currentEditor_ && !closeCurrentNote())
        return false;
    auto editorNote = note;
    editorNote.setFolderId(effectiveFolderId(note));
    setCurrentEditor(new NoteEditor(editorNote, draftId, this));
    setError({});
    return true;
}

bool NotesWorkspaceController::createNote(const QString &storageId) { return createNoteInFolder({}, storageId); }

bool NotesWorkspaceController::createNoteInFolder(const QString &folderIdText, const QString &storageId)
{
    QUuid folderId;
    if (!parseFolderId(folderIdText, &folderId))
        return false;
    if (!folderId.isNull() && (!ensureFolderCatalogAvailable() || !folderCatalogManager_->catalog().folder(folderId))) {
        if (folderCatalogManager_ && folderCatalogManager_->isAvailable())
            setError(tr("The selected folder no longer exists"));
        return false;
    }

    auto storage
        = storageId.isEmpty() ? NoteManager::instance()->defaultStorage() : NoteManager::instance()->storage(storageId);
    if (!storage || !storage->canAcceptWrites()) {
        setError(tr("No writable note storage is available"));
        return false;
    }
    auto note = storage->createNote();
    if (note.isNull()) {
        setError(tr("Could not create a note"));
        return false;
    }
    note.setFolderId(folderId);
    if (!openNote(note))
        return false;
    if (!folderId.isNull()) {
        currentEditor_->setFolderUserOverride();
        rememberPendingFolderAssignment(currentEditor_->draftId(), folderId);
        folderOperations_->prepareNativeFolderTree(storage->systemName());
    }
    return true;
}

bool NotesWorkspaceController::saveCurrentNote()
{
    if (!currentEditor_)
        return true;
    if (!currentEditor_->save()) {
        setError(currentEditor_->errorString());
        return false;
    }
    return true;
}

bool NotesWorkspaceController::closeCurrentNote()
{
    if (!currentEditor_)
        return true;
    if (!currentEditor_->close()) {
        setError(currentEditor_->errorString());
        return false;
    }
    clearCurrentEditor();
    DraftManager::instance()->publishPending();
    return true;
}

bool NotesWorkspaceController::reloadCurrentNote()
{
    if (!currentEditor_ || currentEditor_->isDirty())
        return false;
    if (!currentEditor_->reloadNewerDraft())
        return false;
    emit currentTitleChanged();
    return true;
}

bool NotesWorkspaceController::deleteNote(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return false;
    setError({});
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId) {
        if (!DraftManager::instance()->isLastEditingSession(currentEditor_->draftId())) {
            setError(tr("The note is open in another editor and cannot be deleted yet"));
            return false;
        }
        if (!currentEditor_->discardAndClose()) {
            setError(currentEditor_->errorString());
            return false;
        }
        clearCurrentEditor();
    }
    const auto error = DraftManager::instance()->queueRemoval(storageId, noteId);
    if (error) {
        setError(error.message);
        return false;
    }
    DraftManager::instance()->publishPending();
    return true;
}

bool NotesWorkspaceController::trashNote(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty())
        return false;
    setError({});

    if (noteId.isEmpty()) {
        if (!currentEditor_ || currentEditor_->storageId() != storageId || !currentEditor_->noteId().isEmpty())
            return false;
        if (!currentEditor_->discardAndClose()) {
            setError(currentEditor_->errorString());
            return false;
        }
        clearCurrentEditor();
        return true;
    }

    if (!ensureFolderCatalogAvailable())
        return false;
    const QUuid previousFolderId(folderIdForNote(storageId, noteId));
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId) {
        if (!DraftManager::instance()->isLastEditingSession(currentEditor_->draftId())) {
            setError(tr("The note is open in another editor and cannot be moved to the recycle bin yet"));
            return false;
        }
        if (!currentEditor_->discardAndClose()) {
            setError(currentEditor_->errorString());
            return false;
        }
        clearCurrentEditor();
    }

    if (const auto error = folderCatalogManager_->recycleNote(storageId, noteId, previousFolderId)) {
        setError(error.message);
        return false;
    }
    return folderOperations_->assignNoteFolder(storageId, noteId, FolderCatalog::recycleBinId(), true);
}

bool NotesWorkspaceController::restoreRecycledNote(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty() || noteId.isEmpty() || !ensureFolderCatalogAvailable())
        return false;
    setError({});
    const auto restored = folderCatalogManager_->restoreRecycledNote(storageId, noteId);
    if (!restored) {
        setError(restored.error.message);
        return false;
    }
    return folderOperations_->assignNoteFolder(storageId, noteId, restored.value, true);
}

bool NotesWorkspaceController::emptyRecycleBin()
{
    if (!ensureFolderCatalogAvailable())
        return false;
    setError({});

    QList<QPair<QString, QString>> recycledNotes;
    for (const auto &assignment : folderCatalogManager_->catalog().snapshot().assignments) {
        if (!assignment.tombstone && FolderCatalog::isRecycleBinId(assignment.folderId))
            recycledNotes.append({ assignment.storageId, assignment.noteId });
    }

    for (const auto &[storageId, noteId] : recycledNotes) {
        if (!deleteNote(storageId, noteId))
            return false;
    }
    return true;
}

bool NotesWorkspaceController::isRecycledNote(const QString &storageId, const QString &noteId) const
{
    return folderCatalogManager_ && folderCatalogManager_->isAvailable()
        && folderCatalogManager_->catalog().isRecycled(storageId, noteId);
}

bool NotesWorkspaceController::askBeforePermanentDelete() const
{
    return QSettings().value(QStringLiteral("ui.ask-on-delete"), true).toBool();
}

void NotesWorkspaceController::setAskBeforePermanentDelete(bool enabled)
{
    QSettings().setValue(QStringLiteral("ui.ask-on-delete"), enabled);
}

bool NotesWorkspaceController::moveNote(const QString &sourceStorageId, const QString &noteId,
                                        const QString &destinationStorageId)
{
    return moveNoteAt(sourceStorageId, noteId, destinationStorageId);
}

bool NotesWorkspaceController::moveNoteAt(const QString &sourceStorageId, const QString &noteId,
                                          const QString &destinationStorageId, const QUuid &reorderBatchId,
                                          int reorderIndex)
{
    if (sourceStorageId.isEmpty() || noteId.isEmpty() || destinationStorageId.isEmpty()
        || sourceStorageId == destinationStorageId) {
        return false;
    }
    setError({});

    if (currentEditor_ && currentEditor_->storageId() == sourceStorageId && currentEditor_->noteId() == noteId) {
        if (!DraftManager::instance()->isLastEditingSession(currentEditor_->draftId())) {
            setError(tr("The note is open in another editor and cannot be moved yet"));
            return false;
        }
        if (!saveCurrentNote())
            return false;
        Note       source = currentEditor_->note();
        const auto split  = Utils::splitTitle(currentEditor_->text());
        source.setTitle(split.first);
        source.setText(split.second, currentEditor_->format());
        source.setMedia(currentEditor_->media());

        QUuid destinationDraftId;
        if (!stageMove(source, destinationStorageId, &destinationDraftId, currentEditor_->folderUserOverride()))
            return false;

        // Moving is not a normal close of the source editing session: publishing
        // that source draft and the destination draft concurrently could recreate
        // the source after its queued removal. The destination is staged first so
        // failure cannot lose local edits; only then discard the source checkpoint.
        if (!currentEditor_->discardAndClose()) {
            DraftManager::instance()->discard(destinationDraftId);
            setError(currentEditor_->errorString());
            return false;
        }
        clearCurrentEditor();
        startStagedMove(destinationDraftId, source, reorderBatchId, reorderIndex);
        return true;
    }

    beginOperation();
    auto      *job    = NoteManager::instance()->loadNoteAsync(sourceStorageId, noteId, this);
    const auto loaded = [this, job, destinationStorageId, reorderBatchId, reorderIndex]() {
        if (job->state() != StorageJob::Succeeded) {
            if (job->state() != StorageJob::Cancelled)
                setError(job->error().message.isEmpty() ? tr("Failed to load the note for moving")
                                                        : job->error().message);
            job->deleteLater();
            endOperation();
            completePendingReorderMove(reorderBatchId, reorderIndex, {});
            return;
        }
        const Note source = job->result();
        job->deleteLater();
        endOperation();
        if (!beginMove(source, destinationStorageId, reorderBatchId, reorderIndex))
            completePendingReorderMove(reorderBatchId, reorderIndex, {});
    };
    connect(job, &StorageJob::finished, this, loaded);
    if (job->isFinished())
        QTimer::singleShot(0, this, loaded);
    return true;
}

bool NotesWorkspaceController::moveCurrentNote(const QString &destinationStorageId)
{
    if (!currentEditor_)
        return false;
    return moveNote(currentEditor_->storageId(), currentEditor_->noteId(), destinationStorageId);
}

bool NotesWorkspaceController::copyNote(const QString &sourceStorageId, const QString &noteId,
                                        const QString &destinationStorageId)
{
    if (sourceStorageId.isEmpty() || noteId.isEmpty() || destinationStorageId.isEmpty()
        || sourceStorageId == destinationStorageId) {
        return false;
    }
    setError({});

    const auto stageAndPublish = [this, destinationStorageId](const Note &source, bool folderUserOverride = false) {
        QUuid draftId;
        if (!stageMove(source, destinationStorageId, &draftId, folderUserOverride))
            return false;
        DraftManager::instance()->publishPending();
        return true;
    };

    if (currentEditor_ && currentEditor_->storageId() == sourceStorageId && currentEditor_->noteId() == noteId) {
        if (!saveCurrentNote())
            return false;
        Note       source = currentEditor_->note();
        const auto split  = Utils::splitTitle(currentEditor_->text());
        source.setTitle(split.first);
        source.setText(split.second, currentEditor_->format());
        source.setMedia(currentEditor_->media());
        return stageAndPublish(source, currentEditor_->folderUserOverride());
    }

    beginOperation();
    auto *job = NoteManager::instance()->loadNoteAsync(sourceStorageId, noteId, this);
    connect(job, &StorageJob::finished, this, [this, job, destinationStorageId]() {
        if (job->state() != StorageJob::Succeeded) {
            if (job->state() != StorageJob::Cancelled)
                setError(job->error().message.isEmpty() ? tr("Failed to load the note for copying")
                                                        : job->error().message);
            job->deleteLater();
            endOperation();
            return;
        }
        const Note source = job->result();
        job->deleteLater();
        QUuid draftId;
        if (stageMove(source, destinationStorageId, &draftId))
            DraftManager::instance()->publishPending();
        endOperation();
    });
    return true;
}

bool NotesWorkspaceController::moveNotes(const QVariantList &notes, const QString &destinationStorageId,
                                         const QString &anchorNoteId, bool insertAfter)
{
    if (notes.isEmpty() || destinationStorageId.isEmpty())
        return false;
    const auto destinationStorage = NoteManager::instance()->storage(destinationStorageId);
    if (!destinationStorage || !destinationStorage->canAcceptWrites()) {
        setError(tr("The destination storage is unavailable"));
        return false;
    }
    setError({});

    struct Source {
        QString storageId;
        QString noteId;
    };
    QList<Source> sources;
    QSet<QString> seen;
    for (const QVariant &value : notes) {
        const auto    note      = value.toMap();
        const QString storageId = note.value(QStringLiteral("storageId")).toString();
        const QString noteId    = note.value(QStringLiteral("noteId")).toString();
        const QString key       = storageId + QLatin1Char('\n') + noteId;
        if (storageId.isEmpty() || noteId.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        sources.append({ storageId, noteId });
    }
    if (sources.isEmpty())
        return false;

    const bool  reorderSupported = destinationStorage->supportsNoteReordering();
    const auto  indexed          = NoteManager::instance()->notesIndex()->notes(destinationStorageId);
    QList<Note> remaining;
    remaining.reserve(indexed.size());
    for (const auto &note : indexed) {
        const QString key = destinationStorageId + QLatin1Char('\n') + note.id();
        if (!seen.contains(key))
            remaining.append(note);
    }

    QString afterNoteId;
    if (reorderSupported) {
        qsizetype insertionIndex = 0;
        if (!anchorNoteId.isEmpty()) {
            const auto anchor = std::find_if(remaining.cbegin(), remaining.cend(),
                                             [&anchorNoteId](const Note &note) { return note.id() == anchorNoteId; });
            if (anchor == remaining.cend()) {
                setError(tr("The note used as the drop boundary is no longer available"));
                return false;
            }
            insertionIndex = std::distance(remaining.cbegin(), anchor) + (insertAfter ? 1 : 0);
        }
        if (insertionIndex > 0)
            afterNoteId = remaining.at(insertionIndex - 1).id();
    }

    int sameStorageCount = 0;
    int pendingMoveCount = 0;
    for (const auto &source : std::as_const(sources)) {
        if (source.storageId == destinationStorageId)
            ++sameStorageCount;
        else
            ++pendingMoveCount;
    }

    if (pendingMoveCount == 0) {
        if (!reorderSupported)
            return true;
        QStringList noteIds;
        noteIds.reserve(sources.size());
        for (const auto &source : std::as_const(sources))
            noteIds.append(source.noteId);
        return startStorageReorder(destinationStorage, noteIds, afterNoteId);
    }

    QUuid reorderBatchId;
    if (reorderSupported) {
        reorderBatchId = QUuid::createUuid();
        PendingReorder batch;
        batch.storage      = destinationStorage;
        batch.afterNoteId  = afterNoteId;
        batch.pendingMoves = pendingMoveCount;
        batch.orderedNoteIds.reserve(sources.size());
        for (const auto &source : std::as_const(sources)) {
            batch.orderedNoteIds.append(source.storageId == destinationStorageId ? source.noteId : QString());
        }
        pendingReorders_.insert(reorderBatchId, std::move(batch));
    }

    bool started = false;
    for (qsizetype i = 0; i < sources.size(); ++i) {
        const auto &source = sources.at(i);
        if (source.storageId == destinationStorageId)
            continue;
        const bool moveStarted
            = moveNoteAt(source.storageId, source.noteId, destinationStorageId, reorderBatchId, int(i));
        started = moveStarted || started;
        if (!moveStarted)
            completePendingReorderMove(reorderBatchId, int(i), {});
    }
    return started || sameStorageCount > 0;
}

bool NotesWorkspaceController::reorderRecentNotes(const QVariantList &notes, const QString &anchorStorageId,
                                                  const QString &anchorNoteId, bool insertAfter)
{
    if (notes.isEmpty() || anchorStorageId.isEmpty() || anchorNoteId.isEmpty()) {
        setError(tr("The note used as the drop boundary is no longer available"));
        return false;
    }

    for (const QVariant &value : notes) {
        const auto note = value.toMap();
        if (note.value(QStringLiteral("storageId")).toString() != anchorStorageId) {
            setError(tr("Recent notes can only be reordered within the same storage"));
            return false;
        }
    }

    const auto storage = NoteManager::instance()->storage(anchorStorageId);
    if (!storage || !storage->supportsNoteReordering()) {
        setError(tr("This storage does not support manual note ordering"));
        return false;
    }
    return moveNotes(notes, anchorStorageId, anchorNoteId, insertAfter);
}

bool NotesWorkspaceController::startStorageReorder(NoteStorage *storage, const QStringList &noteIds,
                                                   const QString &afterNoteId)
{
    if (!storage || noteIds.isEmpty())
        return false;
    if (!storage->supportsNoteReordering())
        return true;
    beginOperation();
    auto      *job      = storage->reorderNotesAsync(noteIds, afterNoteId, this);
    const auto finished = [this, job]() {
        if (job->state() != StorageJob::Succeeded && job->state() != StorageJob::Cancelled) {
            setError(job->error().message.isEmpty() ? tr("Failed to reorder the note") : job->error().message);
        }
        job->deleteLater();
        endOperation();
    };
    connect(job, &StorageJob::finished, this, finished);
    if (job->isFinished())
        QTimer::singleShot(0, this, finished);
    return true;
}

void NotesWorkspaceController::completePendingReorderMove(const QUuid &batchId, int index,
                                                          const QString &destinationNoteId)
{
    if (batchId.isNull())
        return;
    auto it = pendingReorders_.find(batchId);
    if (it == pendingReorders_.end())
        return;
    if (index >= 0 && index < it->orderedNoteIds.size())
        it->orderedNoteIds[index] = destinationNoteId;
    it->pendingMoves = qMax(0, it->pendingMoves - 1);
    if (it->pendingMoves > 0)
        return;

    const auto  batch = pendingReorders_.take(batchId);
    QStringList reorderedIds;
    reorderedIds.reserve(batch.orderedNoteIds.size());
    for (const auto &noteId : batch.orderedNoteIds) {
        if (!noteId.isEmpty())
            reorderedIds.append(noteId);
    }
    if (!reorderedIds.isEmpty() && batch.storage)
        startStorageReorder(batch.storage, reorderedIds, batch.afterNoteId);
}

bool NotesWorkspaceController::moveStorage(const QString &sourceStorageId, const QString &destinationStorageId)
{
    return storagePriorityModel_ && storagePriorityModel_->moveStorageById(sourceStorageId, destinationStorageId);
}

bool NotesWorkspaceController::moveStorageToRow(const QString &sourceStorageId, int destinationRow)
{
    return storagePriorityModel_ && storagePriorityModel_->moveStorageToRow(sourceStorageId, destinationRow);
}

void NotesWorkspaceController::openStorageSettings(const QString &storageId)
{
    const auto storage = NoteManager::instance()->storage(storageId);
    if (storage && storage->isConfigurable())
        emit storageSettingsRequested(storageId);
}

void NotesWorkspaceController::refresh() { notesModel_->refresh(); }

bool NotesWorkspaceController::openStandalone(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return false;
    emit openStandaloneRequested(storageId, noteId);
    return true;
}

bool NotesWorkspaceController::openCurrentStandalone()
{
    if (!currentEditor_ || currentEditor_->storageId().isEmpty() || currentEditor_->noteId().isEmpty())
        return false;
    if (!saveCurrentNote())
        return false;
    return openStandalone(currentEditor_->storageId(), currentEditor_->noteId());
}

QString NotesWorkspaceController::createFolder(const QString &name, const QString &parentFolderIdText)
{
    if (!ensureFolderCatalogAvailable())
        return {};

    QUuid parentFolderId;
    if (!parseFolderId(parentFolderIdText, &parentFolderId))
        return {};
    if (!parentFolderId.isNull() && !folderCatalogManager_->catalog().folder(parentFolderId)) {
        setError(tr("The parent folder no longer exists"));
        return {};
    }

    FolderRecord folder;
    folder.name = name.trimmed();
    if (folder.name.isEmpty())
        folder.name = defaultFolderName(parentFolderId);
    folder.parentId   = parentFolderId;
    folder.sortOrder  = nextFolderSortOrder(parentFolderId);
    const auto result = folderCatalogManager_->addFolder(std::move(folder));
    if (!result) {
        setError(result.error.message);
        return {};
    }
    folderOperations_->prepareNativeFolderTrees();
    return result.value.toString(QUuid::WithoutBraces);
}

bool NotesWorkspaceController::renameFolder(const QString &folderIdText, const QString &name)
{
    if (!ensureFolderCatalogAvailable())
        return false;
    QUuid folderId;
    if (!parseFolderId(folderIdText, &folderId) || folderId.isNull()) {
        if (folderId.isNull())
            setError(tr("A folder is required"));
        return false;
    }
    return applyFolderMutation(folderCatalogManager_->renameFolder(folderId, name));
}

bool NotesWorkspaceController::moveFolder(const QString &folderIdText, const QString &parentFolderIdText,
                                          qint64 sortOrder)
{
    if (!ensureFolderCatalogAvailable())
        return false;
    QUuid folderId;
    QUuid parentFolderId;
    if (!parseFolderId(folderIdText, &folderId) || !parseFolderId(parentFolderIdText, &parentFolderId)
        || folderId.isNull()) {
        if (folderId.isNull())
            setError(tr("A folder is required"));
        return false;
    }
    return applyFolderMutation(folderCatalogManager_->moveFolder(folderId, parentFolderId, sortOrder));
}

bool NotesWorkspaceController::moveFolderBefore(const QString &folderIdText, const QString &parentFolderIdText,
                                                const QString &beforeFolderIdText)
{
    if (!ensureFolderCatalogAvailable())
        return false;
    QUuid folderId;
    QUuid parentFolderId;
    QUuid beforeFolderId;
    if (!parseFolderId(folderIdText, &folderId) || !parseFolderId(parentFolderIdText, &parentFolderId)
        || !parseFolderId(beforeFolderIdText, &beforeFolderId) || folderId.isNull()) {
        if (folderId.isNull())
            setError(tr("A folder is required"));
        return false;
    }
    return applyFolderMutation(folderCatalogManager_->moveFolderRelative(folderId, parentFolderId, beforeFolderId));
}

bool NotesWorkspaceController::setFolderCollapsed(const QString &folderIdText, bool collapsed)
{
    if (!ensureFolderCatalogAvailable())
        return false;
    QUuid folderId;
    if (!parseFolderId(folderIdText, &folderId) || folderId.isNull()) {
        if (folderId.isNull())
            setError(tr("A folder is required"));
        return false;
    }
    return applyFolderMutation(folderCatalogManager_->setFolderCollapsed(folderId, collapsed));
}

bool NotesWorkspaceController::setUnsortedCollapsed(bool collapsed)
{
    if (!ensureFolderCatalogAvailable())
        return false;
    return folderNotesModel_->setUnsortedCollapsed(collapsed);
}

bool NotesWorkspaceController::setFolderFlags(const QString &folderIdText, bool favorite, bool archived)
{
    if (!ensureFolderCatalogAvailable())
        return false;
    QUuid folderId;
    if (!parseFolderId(folderIdText, &folderId) || folderId.isNull()) {
        if (folderId.isNull())
            setError(tr("A folder is required"));
        return false;
    }
    return applyFolderMutation(folderCatalogManager_->setFolderFlags(folderId, favorite, archived));
}

bool NotesWorkspaceController::trashFolder(const QString &folderIdText)
{
    if (!ensureFolderCatalogAvailable())
        return false;

    QUuid folderId;
    if (!parseFolderId(folderIdText, &folderId) || folderId.isNull() || FolderCatalog::isRecycleBinId(folderId)) {
        setError(tr("A deletable folder is required"));
        return false;
    }

    const auto &catalog = folderCatalogManager_->catalog();
    if (!catalog.folder(folderId)) {
        setError(tr("The folder no longer exists"));
        return false;
    }

    const auto isInsideBranch = [&catalog, &folderId](QUuid candidate) {
        QSet<QUuid> visited;
        while (!candidate.isNull()) {
            if (candidate == folderId)
                return true;
            if (visited.contains(candidate))
                return false;
            visited.insert(candidate);
            const auto *record = catalog.folder(candidate);
            if (!record)
                return false;
            candidate = record->parentId;
        }
        return false;
    };

    if (currentEditor_ && isInsideBranch(currentEditor_->folderId())) {
        if (!DraftManager::instance()->isLastEditingSession(currentEditor_->draftId())) {
            setError(tr("A note in this folder is open in another editor and the folder cannot be moved to the "
                        "Recycle Bin yet"));
            return false;
        }
        if (!currentEditor_->discardAndClose()) {
            setError(currentEditor_->errorString());
            return false;
        }
        clearCurrentEditor();
    }

    // Native providers can expose a folder directly on Note before the global
    // overlay has an assignment. Materialize those assignments first so every
    // note in the deleted branch participates in the atomic catalog mutation.
    for (const auto &storage : NoteManager::instance()->storages(true)) {
        if (!storage)
            continue;
        for (const auto &note : NoteManager::instance()->notesIndex()->notes(storage->systemName())) {
            const QUuid noteFolderId = effectiveFolderId(note);
            if (!isInsideBranch(noteFolderId))
                continue;
            const auto *assignment = folderCatalogManager_->catalog().assignment(note.storageId(), note.id());
            if (assignment && !assignment->tombstone && assignment->folderId == noteFolderId)
                continue;
            if (const auto assignmentError
                = folderCatalogManager_->assignNote(note.storageId(), note.id(), noteFolderId)) {
                setError(assignmentError.message);
                return false;
            }
        }
    }

    setError({});
    const auto deleted = folderCatalogManager_->trashFolderBranch(folderId);
    if (!deleted) {
        setError(deleted.error.message);
        return false;
    }

    deletedFolderBranches_.append(deleted.value);
    emit folderTrashUndoChanged();

    for (const auto &assignment : deleted.value.assignments) {
        folderOperations_->assignNoteFolder(assignment.storageId, assignment.noteId, FolderCatalog::recycleBinId(),
                                            true);
    }
    // A native catalog may contain this global tree even when it has no note
    // in the deleted branch, so propagate the folder tombstones explicitly.
    folderOperations_->prepareNativeFolderTrees();
    return true;
}

bool NotesWorkspaceController::undoFolderTrash()
{
    if (!ensureFolderCatalogAvailable() || deletedFolderBranches_.isEmpty())
        return false;

    const auto branch = deletedFolderBranches_.constLast();
    if (const auto restoreError = folderCatalogManager_->restoreFolderBranch(branch)) {
        setError(restoreError.message);
        return false;
    }

    deletedFolderBranches_.removeLast();
    emit folderTrashUndoChanged();
    setError({});

    const auto &catalog = folderCatalogManager_->catalog();
    for (const auto &assignment : branch.assignments) {
        if (catalog.folderForNote(assignment.storageId, assignment.noteId) != assignment.folderId)
            continue;
        folderOperations_->assignNoteFolder(assignment.storageId, assignment.noteId, assignment.folderId, true);
    }
    folderOperations_->prepareNativeFolderTrees();
    return true;
}

void NotesWorkspaceController::clearFolderTrashUndo()
{
    if (deletedFolderBranches_.isEmpty())
        return;
    deletedFolderBranches_.clear();
    emit folderTrashUndoChanged();
}

bool NotesWorkspaceController::collapseAllFolders()
{
    if (!ensureFolderCatalogAvailable())
        return false;
    const bool changed = applyFolderMutation(folderCatalogManager_->setAllFoldersCollapsed(true));
    if (changed)
        folderNotesModel_->setUnsortedCollapsed(true);
    return changed;
}

QString NotesWorkspaceController::folderIdForNote(const QString &storageId, const QString &noteId) const
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return {};
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId)
        return currentEditor_->folderIdString();

    Note note;
    for (const auto &candidate : NoteManager::instance()->notesIndex()->notes(storageId)) {
        if (candidate.id() == noteId) {
            note = candidate;
            break;
        }
    }
    if (note.isNull()) {
        const auto storage = NoteManager::instance()->storage(storageId);
        if (storage)
            note = storage->note(noteId);
    }
    if (note.isNull())
        return {};
    return effectiveFolderId(note).toString(QUuid::WithoutBraces);
}

bool NotesWorkspaceController::assignNoteFolder(const QString &storageId, const QString &noteId,
                                                const QString &folderIdText)
{
    qCInfo(logWorkspaceFolders) << "Workspace folder assignment received: storage=" << storageId
                                << "note=" << noteId.left(16) << "folder=" << folderIdText << "currentEditor="
                                << bool(currentEditor_ && currentEditor_->storageId() == storageId
                                        && currentEditor_->noteId() == noteId);
    QUuid folderId;
    if (!parseFolderId(folderIdText, &folderId)) {
        qCWarning(logWorkspaceFolders) << "Workspace folder assignment rejected: invalid folder id" << folderIdText;
        return false;
    }
    if (!folderId.isNull() && (!ensureFolderCatalogAvailable() || !folderCatalogManager_->catalog().folder(folderId))) {
        if (folderCatalogManager_ && folderCatalogManager_->isAvailable())
            setError(tr("The selected folder no longer exists"));
        return false;
    }
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId) {
        qCInfo(logWorkspaceFolders) << "Routing folder assignment through current editor";
        return assignCurrentNoteFolder(folderIdText);
    }
    qCInfo(logWorkspaceFolders) << "Routing folder assignment directly to folder operations";
    return folderOperations_->assignNoteFolder(storageId, noteId, folderId);
}

bool NotesWorkspaceController::assignCurrentNoteFolder(const QString &folderIdText)
{
    if (!currentEditor_) {
        setError(tr("No note is open"));
        return false;
    }

    QUuid folderId;
    if (!parseFolderId(folderIdText, &folderId))
        return false;
    if (!folderId.isNull() && (!ensureFolderCatalogAvailable() || !folderCatalogManager_->catalog().folder(folderId))) {
        if (folderCatalogManager_ && folderCatalogManager_->isAvailable())
            setError(tr("The selected folder no longer exists"));
        return false;
    }
    if (currentEditor_->folderId() == folderId)
        return true;

    const QUuid previousFolderId           = currentEditor_->folderId();
    const bool  previousFolderUserOverride = currentEditor_->folderUserOverride();
    const bool  publishWithContent
        = currentEditor_->isDirty() || currentEditor_->hasPersistedDraft() || currentEditor_->noteId().isEmpty();
    qCInfo(logWorkspaceFolders) << "Assigning current editor folder: storage=" << currentEditor_->storageId()
                                << "note=" << currentEditor_->noteId().left(16)
                                << "folder=" << folderId.toString(QUuid::WithoutBraces)
                                << "publishWithContent=" << publishWithContent << "dirty=" << currentEditor_->isDirty()
                                << "persistedDraft=" << currentEditor_->hasPersistedDraft();
    currentEditor_->setFolderId(folderId);
    currentEditor_->setFolderUserOverride();
    rememberPendingFolderAssignment(currentEditor_->draftId(), folderId);

    if (publishWithContent) {
        if (!currentEditor_->save()) {
            setError(currentEditor_->errorString());
            return false;
        }
        if (!currentEditor_->noteId().isEmpty()
            && !folderOperations_->storeOverlayAssignment(currentEditor_->storageId(), currentEditor_->noteId(),
                                                          folderId)) {
            return false;
        }
        if (!folderId.isNull())
            folderOperations_->prepareNativeFolderTree(currentEditor_->storageId());
        return true;
    }

    if (folderOperations_->assignNoteFolder(currentEditor_->storageId(), currentEditor_->noteId(), folderId))
        return true;

    pendingFolderAssignments_.remove(currentEditor_->draftId());
    currentEditor_->setFolderId(previousFolderId);
    currentEditor_->setFolderUserOverride(previousFolderUserOverride);
    return false;
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

bool NotesWorkspaceController::stageMove(const Note &source, const QString &destinationStorageId, QUuid *draftId,
                                         bool folderUserOverride)
{
    auto destinationStorage = NoteManager::instance()->storage(destinationStorageId);
    if (source.isNull() || !destinationStorage || !destinationStorage->canAcceptWrites()) {
        setError(tr("The destination storage is unavailable"));
        return false;
    }
    const auto   formats           = destinationStorage->availableFormats();
    Note::Format destinationFormat = source.format();
    if (!formats.contains(destinationFormat)) {
        const QList<Note::Format> conversionPreference {
            Note::Markdown,
            Note::PlainText,
            Note::Html,
        };
        const auto supported = std::ranges::find_if(
            conversionPreference, [&formats](Note::Format format) { return formats.contains(format); });
        if (supported == conversionPreference.cend()) {
            setError(tr("The destination storage does not support this note format"));
            return false;
        }
        destinationFormat = *supported;
    }
    if (!source.media().isEmpty() && !destinationStorage->supportsMedia()) {
        setError(tr("The destination storage does not support note attachments"));
        return false;
    }

    Note destination = destinationStorage->createNote();
    if (destination.isNull()) {
        setError(tr("Could not create the destination note"));
        return false;
    }
    const QString title = NoteTransferController::convertTextFormat(source.title(), source.format(), destinationFormat);
    const QString body  = NoteTransferController::convertTextFormat(source.text(), source.format(), destinationFormat);
    destination.setTitle(title);
    destination.setText(body, destinationFormat);
    destination.setTags(source.tags());
    destination.setFolderId(effectiveFolderId(source));
    destination.setMedia(source.media());

    auto       *drafts   = DraftManager::instance();
    const QUuid stagedId = drafts->acquireEditingSession(destination);
    const auto  saveError
        = drafts->saveEditing(stagedId, destination, title, body, destinationFormat, folderUserOverride);
    if (saveError) {
        drafts->releaseEditingSession(stagedId);
        setError(saveError.message);
        return false;
    }
    if (!destination.folderId().isNull()) {
        rememberPendingFolderAssignment(stagedId, destination.folderId());
        folderOperations_->prepareNativeFolderTree(destinationStorage->systemName());
    }
    const auto readyError = drafts->markReady(stagedId);
    drafts->releaseEditingSession(stagedId);
    if (readyError) {
        pendingFolderAssignments_.remove(stagedId);
        drafts->discard(stagedId);
        setError(readyError.message);
        return false;
    }

    if (draftId)
        *draftId = stagedId;
    return true;
}

void NotesWorkspaceController::startStagedMove(const QUuid &draftId, const Note &source, const QUuid &reorderBatchId,
                                               int reorderIndex)
{
    pendingMoves_.insert(draftId, { source.storageId(), source.id(), reorderBatchId, reorderIndex });
    beginOperation();
    DraftManager::instance()->publishPending();
}

bool NotesWorkspaceController::beginMove(const Note &source, const QString &destinationStorageId,
                                         const QUuid &reorderBatchId, int reorderIndex)
{
    QUuid draftId;
    if (!stageMove(source, destinationStorageId, &draftId))
        return false;
    startStagedMove(draftId, source, reorderBatchId, reorderIndex);
    return true;
}

void NotesWorkspaceController::connectEditorSignals(NoteEditor *editor)
{
    connect(editor, &NoteEditor::textChanged, this, &NotesWorkspaceController::currentTitleChanged);
    connect(editor, &NoteEditor::folderIdChanged, this, &NotesWorkspaceController::currentFolderIdChanged);
    connect(editor, &NoteEditor::errorStringChanged, this, [this, editor]() { setError(editor->errorString()); });
}

bool NotesWorkspaceController::ensureFolderCatalogAvailable()
{
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable())
        return true;
    setError(folderCatalogManager_ && !folderCatalogManager_->lastError().isEmpty()
                 ? folderCatalogManager_->lastError()
                 : tr("The encrypted folder catalog is unavailable"));
    return false;
}

bool NotesWorkspaceController::parseFolderId(const QString &text, QUuid *folderId)
{
    if (!folderId)
        return false;
    *folderId             = {};
    const auto normalized = text.trimmed();
    if (normalized.isEmpty())
        return true;
    *folderId = QUuid(normalized);
    if (!folderId->isNull())
        return true;
    if (normalized == QUuid().toString(QUuid::WithoutBraces) || normalized == QUuid().toString())
        return true;
    setError(tr("The folder identifier is invalid"));
    return false;
}

QUuid NotesWorkspaceController::effectiveFolderId(const Note &note) const
{
    if (note.isNull() || !folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return note.folderId();

    const auto &catalog = folderCatalogManager_->catalog();
    if (const auto *assignment = catalog.assignment(note.storageId(), note.id())) {
        return !assignment->tombstone && catalog.folder(assignment->folderId) ? assignment->folderId : QUuid {};
    }
    return catalog.folder(note.folderId()) ? note.folderId() : QUuid {};
}

qint64 NotesWorkspaceController::nextFolderSortOrder(const QUuid &parentFolderId) const
{
    if (!folderCatalogManager_)
        return 0;
    const auto siblings = folderCatalogManager_->catalog().children(parentFolderId);
    if (siblings.isEmpty())
        return 0;

    qint64 maximum = std::numeric_limits<qint64>::min();
    for (const auto &sibling : siblings)
        maximum = std::max(maximum, sibling.sortOrder);
    return maximum == std::numeric_limits<qint64>::max() ? maximum : maximum + 1;
}

QString NotesWorkspaceController::defaultFolderName(const QUuid &parentFolderId) const
{
    const auto base = tr("New folder");
    if (!folderCatalogManager_)
        return base;

    QSet<QString> siblingNames;
    for (const auto &sibling : folderCatalogManager_->catalog().children(parentFolderId))
        siblingNames.insert(sibling.name.trimmed().toCaseFolded());

    for (int number = 1;; ++number) {
        const auto candidate = number == 1 ? base : tr("%1 %2").arg(base).arg(number);
        if (!siblingNames.contains(candidate.toCaseFolded()))
            return candidate;
    }
}

bool NotesWorkspaceController::applyFolderMutation(const FolderCatalogError &error)
{
    if (error) {
        setError(error.message);
        return false;
    }
    folderOperations_->prepareNativeFolderTrees();
    return true;
}

void NotesWorkspaceController::rememberPendingFolderAssignment(const QUuid &draftId, const QUuid &folderId)
{
    if (!draftId.isNull())
        pendingFolderAssignments_.insert(draftId, { folderId });
}

} // namespace QtNote
