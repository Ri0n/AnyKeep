#include "notesworkspacecontroller.h"

#include "draftmanager.h"
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

#include <QSet>
#include <QTimer>
#include <algorithm>

namespace QtNote {

NotesWorkspaceController::NotesWorkspaceController(QObject *parent) : QObject(parent)
{
    notesModel_  = new NotesModel(this);
    searchModel_ = new NotesSearchModel(this);
    searchModel_->setSourceModel(notesModel_);
    recentNotesModel_     = new RecentNotesModel(searchModel_, this);
    storagePriorityModel_ = new StoragePriorityModel(this);

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

    auto *drafts = DraftManager::instance();
    connect(drafts, &DraftManager::draftPublished, this, [this, drafts](const QUuid &draftId, const Note &note) {
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
int     NotesWorkspaceController::noteCount() const { return notesModel_->noteCount(); }
QString NotesWorkspaceController::searchText() const { return searchModel_->searchText(); }
bool    NotesWorkspaceController::searchInBody() const { return searchModel_->searchInBody(); }
bool    NotesWorkspaceController::searching() const { return searchModel_->searching(); }

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
    setCurrentEditor(new NoteEditor(note, draftId, this));
    setError({});
    return true;
}

bool NotesWorkspaceController::createNote(const QString &storageId)
{
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
    return openNote(note);
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
        if (!stageMove(source, destinationStorageId, &destinationDraftId))
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

    const auto stageAndPublish = [this, destinationStorageId](const Note &source) {
        QUuid draftId;
        if (!stageMove(source, destinationStorageId, &draftId))
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
        return stageAndPublish(source);
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
}

void NotesWorkspaceController::clearCurrentEditor()
{
    if (!currentEditor_)
        return;
    auto *old = currentEditor_.data();
    currentEditor_.clear();
    old->deleteLater();
    emit currentEditorChanged();
    emit currentTitleChanged();
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

bool NotesWorkspaceController::stageMove(const Note &source, const QString &destinationStorageId, QUuid *draftId)
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
    destination.setMedia(source.media());

    auto       *drafts    = DraftManager::instance();
    const QUuid stagedId  = drafts->acquireEditingSession(destination);
    const auto  saveError = drafts->saveEditing(stagedId, destination, title, body, destinationFormat);
    if (saveError) {
        drafts->releaseEditingSession(stagedId);
        setError(saveError.message);
        return false;
    }
    const auto readyError = drafts->markReady(stagedId);
    drafts->releaseEditingSession(stagedId);
    if (readyError) {
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
    connect(editor, &NoteEditor::errorStringChanged, this, [this, editor]() { setError(editor->errorString()); });
}

} // namespace QtNote
