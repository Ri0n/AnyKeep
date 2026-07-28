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
#include <QTimeZone>
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
    connect(drafts, &DraftManager::draftPublished, this, [this, drafts](const QUuid &draftId, const Note &) {
        if (!pendingMoves_.contains(draftId))
            return;
        const auto move  = pendingMoves_.take(draftId);
        const auto error = drafts->queueRemoval(move.sourceStorageId, move.sourceNoteId);
        if (error)
            setError(error.message);
        drafts->publishPending();
        endOperation();
    });
    connect(drafts, &DraftManager::draftPublishFailed, this, [this](const QUuid &draftId, const QString &message) {
        if (!pendingMoves_.contains(draftId))
            return;
        pendingMoves_.remove(draftId);
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
    return moveNoteAt(sourceStorageId, noteId, destinationStorageId, {});
}

bool NotesWorkspaceController::moveNoteAt(const QString &sourceStorageId, const QString &noteId,
                                          const QString &destinationStorageId, const QDateTime &requestedModified)
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
        if (!stageMove(source, destinationStorageId, &destinationDraftId, requestedModified))
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
        startStagedMove(destinationDraftId, source);
        return true;
    }

    beginOperation();
    auto *job = NoteManager::instance()->loadNoteAsync(sourceStorageId, noteId, this);
    connect(job, &StorageJob::finished, this, [this, job, destinationStorageId, requestedModified]() {
        if (job->state() != StorageJob::Succeeded) {
            if (job->state() != StorageJob::Cancelled)
                setError(job->error().message.isEmpty() ? tr("Failed to load the note for moving")
                                                        : job->error().message);
            job->deleteLater();
            endOperation();
            return;
        }
        const Note source = job->result();
        job->deleteLater();
        endOperation();
        beginMove(source, destinationStorageId, requestedModified);
    });
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
    const qint64 timeStep = destinationStorage->requestedModificationTimeResolutionMs();
    if (timeStep <= 0) {
        setError(tr("The destination storage does not support manual note ordering"));
        return false;
    }

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

    const auto  indexed = NoteManager::instance()->notesIndex()->notes(destinationStorageId);
    QList<Note> remaining;
    remaining.reserve(indexed.size());
    for (const auto &note : indexed) {
        const QString key = destinationStorageId + QLatin1Char('\n') + note.id();
        if (!seen.contains(key))
            remaining.append(note);
    }

    int insertionIndex = 0;
    if (!anchorNoteId.isEmpty()) {
        const auto anchor = std::find_if(remaining.cbegin(), remaining.cend(),
                                         [&anchorNoteId](const Note &note) { return note.id() == anchorNoteId; });
        if (anchor == remaining.cend()) {
            setError(tr("The note used as the drop boundary is no longer available"));
            return false;
        }
        insertionIndex = int(std::distance(remaining.cbegin(), anchor)) + (insertAfter ? 1 : 0);
    }

    const auto nowMs  = QDateTime::currentMSecsSinceEpoch();
    const auto now    = QDateTime::fromMSecsSinceEpoch(nowMs - nowMs % timeStep, QTimeZone::UTC);
    QDateTime  cursor = insertionIndex > 0 && remaining.at(insertionIndex - 1).lastChangeUTC().isValid()
         ? remaining.at(insertionIndex - 1).lastChangeUTC()
         : now.addMSecs(timeStep);
    if (cursor > now.addMSecs(timeStep))
        cursor = now.addMSecs(timeStep);

    QList<QDateTime> requestedTimes;
    requestedTimes.reserve(sources.size());
    for (qsizetype i = 0; i < sources.size(); ++i) {
        cursor = cursor.addMSecs(-timeStep);
        requestedTimes.append(cursor);
    }

    // Normally the neighboring timestamps have a wide gap. If several notes
    // share one storage-resolution tick, shift only the colliding tail
    // backwards; this keeps every timestamp out of the future.
    QHash<QString, QDateTime> adjustedExisting;
    for (qsizetype i = insertionIndex; i < remaining.size(); ++i) {
        const auto existingTime = remaining.at(i).lastChangeUTC();
        if (!existingTime.isValid() || existingTime >= cursor) {
            cursor = cursor.addMSecs(-timeStep);
            adjustedExisting.insert(remaining.at(i).id(), cursor);
        } else {
            cursor = existingTime;
        }
    }

    bool started = false;
    for (qsizetype i = 0; i < sources.size(); ++i) {
        const auto &source = sources.at(i);
        if (source.storageId == destinationStorageId)
            started = resaveNoteAt(source.storageId, source.noteId, requestedTimes.at(i)) || started;
        else
            started
                = moveNoteAt(source.storageId, source.noteId, destinationStorageId, requestedTimes.at(i)) || started;
    }
    for (auto it = adjustedExisting.cbegin(); it != adjustedExisting.cend(); ++it)
        started = resaveNoteAt(destinationStorageId, it.key(), it.value()) || started;
    return started;
}

bool NotesWorkspaceController::resaveNoteAt(const QString &storageId, const QString &noteId,
                                            const QDateTime &requestedModified)
{
    if (storageId.isEmpty() || noteId.isEmpty() || !requestedModified.isValid())
        return false;

    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId) {
        if (!DraftManager::instance()->isLastEditingSession(currentEditor_->draftId())) {
            setError(tr("The note is open in another editor and cannot be reordered yet"));
            return false;
        }
        if (!saveCurrentNote())
            return false;
        Note       source = currentEditor_->note();
        const auto split  = Utils::splitTitle(currentEditor_->text());
        source.setTitle(split.first);
        source.setText(split.second, currentEditor_->format());
        source.setMedia(currentEditor_->media());
        if (!currentEditor_->discardAndClose()) {
            setError(currentEditor_->errorString());
            return false;
        }
        clearCurrentEditor();
        return saveLoadedNoteAt(source, requestedModified);
    }

    beginOperation();
    auto *job = NoteManager::instance()->loadNoteAsync(storageId, noteId, this);
    connect(job, &StorageJob::finished, this, [this, job, requestedModified]() {
        if (job->state() != StorageJob::Succeeded) {
            if (job->state() != StorageJob::Cancelled)
                setError(job->error().message.isEmpty() ? tr("Failed to load the note for reordering")
                                                        : job->error().message);
            job->deleteLater();
            endOperation();
            return;
        }
        Note note = job->result();
        job->deleteLater();
        endOperation();
        saveLoadedNoteAt(std::move(note), requestedModified);
    });
    return true;
}

bool NotesWorkspaceController::saveLoadedNoteAt(Note note, const QDateTime &requestedModified)
{
    auto *storage = note.storage();
    if (note.isNull() || !note.isLoaded() || !storage || !requestedModified.isValid())
        return false;
    note.setLastChangeUTC(requestedModified);
    note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), requestedModified);

    beginOperation();
    auto *job = storage->saveNoteAsync(note, this);
    connect(job, &StorageJob::finished, this, [this, job]() {
        if (job->state() != StorageJob::Succeeded && job->state() != StorageJob::Cancelled)
            setError(job->error().message.isEmpty() ? tr("Failed to reorder the note") : job->error().message);
        job->deleteLater();
        endOperation();
    });
    return true;
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

bool NotesWorkspaceController::stageMove(const Note &source, const QString &destinationStorageId, QUuid *draftId,
                                         const QDateTime &requestedModified)
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
    if (requestedModified.isValid()) {
        destination.setLastChangeUTC(requestedModified);
        destination.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), requestedModified);
    }

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

void NotesWorkspaceController::startStagedMove(const QUuid &draftId, const Note &source)
{
    pendingMoves_.insert(draftId, { source.storageId(), source.id() });
    beginOperation();
    DraftManager::instance()->publishPending();
}

bool NotesWorkspaceController::beginMove(const Note &source, const QString &destinationStorageId,
                                         const QDateTime &requestedModified)
{
    QUuid draftId;
    if (!stageMove(source, destinationStorageId, &draftId, requestedModified))
        return false;
    startStagedMove(draftId, source);
    return true;
}

void NotesWorkspaceController::connectEditorSignals(NoteEditor *editor)
{
    connect(editor, &NoteEditor::textChanged, this, &NotesWorkspaceController::currentTitleChanged);
    connect(editor, &NoteEditor::errorStringChanged, this, [this, editor]() { setError(editor->errorString()); });
}

} // namespace QtNote
