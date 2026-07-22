#include "notesworkspacecontroller.h"

#include "draftmanager.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notesmodel.h"
#include "notessearchmodel.h"
#include "notestorage.h"
#include "recentnotesmodel.h"
#include "storagejob.h"
#include "utils.h"

namespace QtNote {

NotesWorkspaceController::NotesWorkspaceController(QObject *parent) : QObject(parent)
{
    notesModel_  = new NotesModel(this);
    searchModel_ = new NotesSearchModel(this);
    searchModel_->setSourceModel(notesModel_);
    recentNotesModel_ = new RecentNotesModel(searchModel_, this);

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
    if (!storage || !storage->isAccessible()) {
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
    if (sourceStorageId.isEmpty() || noteId.isEmpty() || destinationStorageId.isEmpty()
        || sourceStorageId == destinationStorageId) {
        return false;
    }

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
        startStagedMove(destinationDraftId, source);
        return true;
    }

    beginOperation();
    auto *job = NoteManager::instance()->loadNoteAsync(sourceStorageId, noteId, this);
    connect(job, &StorageJob::finished, this, [this, job, destinationStorageId]() {
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
        beginMove(source, destinationStorageId);
    });
    return true;
}

bool NotesWorkspaceController::moveCurrentNote(const QString &destinationStorageId)
{
    if (!currentEditor_)
        return false;
    return moveNote(currentEditor_->storageId(), currentEditor_->noteId(), destinationStorageId);
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
    if (source.isNull() || !destinationStorage || !destinationStorage->isAccessible()) {
        setError(tr("The destination storage is unavailable"));
        return false;
    }
    if (!destinationStorage->availableFormats().contains(source.format())) {
        setError(tr("The destination storage does not support this note format"));
        return false;
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
    destination.setTitle(source.title());
    destination.setText(source.text(), source.format());
    destination.setTags(source.tags());
    destination.setMedia(source.media());

    auto       *drafts    = DraftManager::instance();
    const QUuid stagedId  = drafts->acquireEditingSession(destination);
    const auto  saveError = drafts->saveEditing(stagedId, destination, source.title(), source.text(), source.format());
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

bool NotesWorkspaceController::beginMove(const Note &source, const QString &destinationStorageId)
{
    QUuid draftId;
    if (!stageMove(source, destinationStorageId, &draftId))
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
