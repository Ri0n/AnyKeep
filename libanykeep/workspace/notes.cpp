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
    setCurrentEditor(new NoteEditor(editorNote, *draftManager_, draftId, this));
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
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId) {
        if (!draftManager_->isLastEditingSession(currentEditor_->draftId())) {
            setError(tr("The note is open in another editor and cannot be deleted yet"));
            return false;
        }
        if (!currentEditor_->discardAndClose()) {
            setError(currentEditor_->errorString());
            return false;
        }
        clearCurrentEditor();
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
    QString     title;
    for (const auto &note : NoteManager::instance()->notesIndex()->notes(storageId)) {
        if (note.id() == noteId) {
            title = note.title();
            break;
        }
    }
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId) {
        if (!draftManager_->isLastEditingSession(currentEditor_->draftId())) {
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
    const bool accepted = folderOperations_->assignNoteFolder(storageId, noteId, FolderCatalog::recycleBinId(), true);
    trashUndoEntries_.append({ TrashUndoEntry::NoteTrash, storageId, noteId, title, {} });
    emit trashUndoChanged();
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
