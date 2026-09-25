#include "notesworkspacecontroller.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "foldernotesmodel.h"
#include "folderoperationscontroller.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notedata.h"
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
#include <utility>

namespace AnyKeep {
namespace {

Note noteFromEditingDraft(const DraftRecord &draft)
{
    Note note;
    if (!draft.storageId.isEmpty()) {
        if (auto storage = NoteManager::instance()->storage(draft.storageId))
            note = storage->createNote();
    }
    if (note.isNull())
        note = Note(new NoteData(nullptr));

    if (!draft.remoteNoteId.isEmpty())
        note.setId(draft.remoteNoteId);
    note.setTitle(draft.title);
    note.setText(draft.body, draft.format);
    note.setTags(draft.tags);
    note.setFolderId(draft.folderId);
    note.setBackendData(draft.backendData);
    note.setMedia(draft.media);
    return note;
}

} // namespace

bool NotesWorkspaceController::openNote(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return false;

    QUuid   draftId;
    QString effectiveStorageId = storageId;
    QString effectiveNoteId    = noteId;

    if (storageId == DraftManager::draftsStorageId()) {
        draftId = QUuid(noteId);
        if (draftId.isNull()) {
            setError(tr("The draft identifier is invalid"));
            return false;
        }
        if (currentEditor_ && currentEditor_->draftId() == draftId)
            return true;
        const auto resumed = draftManager_->resumeEditingDraft(draftId);
        if (!resumed) {
            setError(resumed.error.message.isEmpty() ? tr("The draft is no longer available") : resumed.error.message);
            return false;
        }
        // The encrypted draft is the authoritative working copy. Opening it
        // must not depend on the target plugin being present/readable.
        auto note = noteFromEditingDraft(resumed.value);
        if (note.isNull() || !openNote(note, draftId)) {
            setError(tr("The draft could not be opened"));
            return false;
        }
        return true;
    } else {
        if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId)
            return true;
        auto pending = draftManager_->pendingDraftForNote(storageId, noteId);
        if (!pending) {
            const QUuid presentedDraftId(noteId);
            auto        presentedDraft = draftManager_->pendingDraft(presentedDraftId);
            if (presentedDraft && presentedDraft.value.storageId == storageId)
                pending = std::move(presentedDraft);
        }
        if (pending) {
            const auto resumed = draftManager_->resumeEditingDraft(pending.value.id);
            if (!resumed) {
                setError(resumed.error.message.isEmpty() ? tr("The pending draft could not be opened")
                                                         : resumed.error.message);
                return false;
            }
            draftId = pending.value.id;
            // Never reload an origin/target body over a durable local draft.
            // A detached NoteData keeps recovery editable even while the
            // target storage plugin is absent.
            auto note = noteFromEditingDraft(resumed.value);
            if (note.isNull() || !openNote(note, draftId)) {
                setError(tr("The pending draft could not be opened"));
                return false;
            }
            return true;
        }
    }

    if (loadJob_)
        loadJob_->cancel();

    setError({});
    setLoading(true);
    auto *job = NoteManager::instance()->loadNoteAsync(effectiveStorageId, effectiveNoteId, this);
    loadJob_  = job;
    connect(job, &StorageJob::finished, this, [this, job, draftId]() {
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
        if (!openNote(loaded, draftId))
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
    auto *editor = draftManager_->acquireEditor(editorNote, draftId);
    if (!editor) {
        setError(tr("Could not acquire the shared note model"));
        return false;
    }
    setCurrentEditor(editor);
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
    draftManager_->publishPending();
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

    QUuid       pendingDraftId;
    DraftRecord pendingRecord;
    bool        hasPendingRecord = false;
    if (storageId == DraftManager::draftsStorageId()) {
        pendingDraftId = QUuid(noteId);
        if (pendingDraftId.isNull()) {
            setError(tr("The draft identifier is invalid"));
            return false;
        }
        const auto pending = draftManager_->pendingDraft(pendingDraftId);
        if (pending) {
            pendingRecord    = pending.value;
            hasPendingRecord = true;
        }
    } else {
        auto pending = draftManager_->pendingDraftForNote(storageId, noteId);
        if (!pending) {
            const QUuid presentedDraftId(noteId);
            const auto  presentedDraft = draftManager_->pendingDraft(presentedDraftId);
            if (presentedDraft && presentedDraft.value.storageId == storageId)
                pending = presentedDraft;
        }
        if (pending) {
            pendingDraftId   = pending.value.id;
            pendingRecord    = pending.value;
            hasPendingRecord = true;
        }
    }

    auto closeError = storageId == DraftManager::draftsStorageId()
        ? draftManager_->discardEditingSessionsForDraft(pendingDraftId)
        : draftManager_->discardEditingSessionsForNote(storageId, noteId);
    if (!closeError && storageId != DraftManager::draftsStorageId() && !pendingDraftId.isNull()
        && draftManager_->editingSessionCount(pendingDraftId) > 0) {
        closeError = draftManager_->discardEditingSessionsForDraft(pendingDraftId);
    }
    if (closeError) {
        setError(closeError.message);
        return false;
    }

    if (!pendingDraftId.isNull()) {
        // DraftManager owns the persistent transfer state machine. A
        // post-ACK transfer can temporarily have both a destination object and
        // an undeleted source; queueDraftDeletion() turns every such identity
        // into a durable Delete record before dropping the transfer draft.
        const auto error = draftManager_->queueDraftDeletion(pendingDraftId);
        if (error) {
            setError(error.message);
            return false;
        }

        if (hasPendingRecord) {
            if (!pendingRecord.storageId.isEmpty() && !pendingRecord.remoteNoteId.isEmpty())
                removeNoteTrashUndo(pendingRecord.storageId, pendingRecord.remoteNoteId);
            if (!pendingRecord.removeSourceStorageId.isEmpty() && !pendingRecord.removeSourceNoteId.isEmpty())
                removeNoteTrashUndo(pendingRecord.removeSourceStorageId, pendingRecord.removeSourceNoteId);
        }
        draftManager_->publishPending();
        return true;
    }

    const auto error = draftManager_->queueRemoval(storageId, noteId);
    if (error) {
        setError(error.message);
        return false;
    }
    removeNoteTrashUndo(storageId, noteId);
    draftManager_->publishPending();
    return true;
}

bool NotesWorkspaceController::trashNote(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty())
        return false;
    setError({});

    // Drafts-list deletion is intentionally permanent rather than a folder
    // recycle operation; keep its existing semantics.
    if (storageId == DraftManager::draftsStorageId()) {
        if (noteId.isEmpty())
            return false;
        return deleteNote(storageId, noteId);
    }

    if (!ensureFolderCatalogAvailable())
        return false;

    QUuid knownDraftId;
    if (currentEditor_) {
        const bool currentIdentity = currentEditor_->storageId() == storageId
            && currentEditor_->noteId() == noteId;
        const bool unpublishedCurrent
            = noteId.isEmpty() && currentEditor_->storageId() == storageId && currentEditor_->noteId().isEmpty();
        const bool presentedCurrent = QUuid(noteId) == currentEditor_->draftId();
        if (currentIdentity || unpublishedCurrent || presentedCurrent)
            knownDraftId = currentEditor_->draftId();
    }

    const auto prepared = draftManager_->prepareForRecycle(storageId, noteId, knownDraftId);
    if (!prepared) {
        setError(prepared.error.message);
        return false;
    }

    const auto recycleStorageId = prepared.value.first;
    const auto recycleNoteId    = prepared.value.second;
    if (recycleStorageId.isEmpty() || recycleNoteId.isEmpty()) {
        draftManager_->publishPending();
        return true; // An unpublished local draft was simply discarded.
    }

    const QUuid previousFolderId(folderIdForNote(recycleStorageId, recycleNoteId));
    QString     title;
    for (const auto &note : NoteManager::instance()->notesIndex()->notes(recycleStorageId)) {
        if (note.id() == recycleNoteId) {
            title = note.title();
            break;
        }
    }

    if (const auto error
        = folderCatalogManager_->recycleNote(recycleStorageId, recycleNoteId, previousFolderId)) {
        setError(error.message);
        return false;
    }

    const bool accepted
        = folderOperations_->assignNoteFolder(recycleStorageId, recycleNoteId, FolderCatalog::recycleBinId(), true);
    trashUndoEntries_.append({ TrashUndoEntry::NoteTrash, recycleStorageId, recycleNoteId, title, {} });
    emit trashUndoChanged();
    draftManager_->publishPending();
    return accepted;
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
    const bool accepted = folderOperations_->assignNoteFolder(storageId, noteId, restored.value, true);
    removeNoteTrashUndo(storageId, noteId);
    return accepted;
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
    if (storageId.isEmpty() || noteId.isEmpty())
        return false;
    if (storageId == DraftManager::draftsStorageId()) {
        const auto draft = draftManager_->pendingDraft(QUuid(noteId));
        return draft && FolderCatalog::isRecycleBinId(draft.value.folderId);
    }
    const auto pending = draftManager_->pendingDraftForNote(storageId, noteId);
    if (pending)
        return FolderCatalog::isRecycleBinId(pending.value.folderId);
    const QUuid presentedDraftId(noteId);
    const auto  presentedDraft = draftManager_->pendingDraft(presentedDraftId);
    if (presentedDraft && presentedDraft.value.storageId == storageId)
        return FolderCatalog::isRecycleBinId(presentedDraft.value.folderId);
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

} // namespace AnyKeep
