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

Q_LOGGING_CATEGORY(logWorkspaceFolders, "anykeep.workspace.folders")

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

bool NotesWorkspaceController::setDraftsCollapsed(bool collapsed)
{
    return folderNotesModel_ && folderNotesModel_->setDraftsCollapsed(collapsed);
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
        if (!draftManager_->isLastEditingSession(currentEditor_->draftId())) {
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

    TrashUndoEntry undoEntry;
    undoEntry.kind         = TrashUndoEntry::FolderTrash;
    undoEntry.folderBranch = deleted.value;
    trashUndoEntries_.append(std::move(undoEntry));
    emit trashUndoChanged();

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
    for (qsizetype index = trashUndoEntries_.size() - 1; index >= 0; --index) {
        if (trashUndoEntries_.at(index).kind == TrashUndoEntry::FolderTrash)
            return restoreFolderTrashAt(index);
    }
    return false;
}

bool NotesWorkspaceController::undoTrash()
{
    if (trashUndoEntries_.isEmpty())
        return false;
    const auto entry = trashUndoEntries_.constLast();
    if (entry.kind == TrashUndoEntry::FolderTrash)
        return restoreFolderTrashAt(trashUndoEntries_.size() - 1);
    return restoreRecycledNote(entry.storageId, entry.noteId);
}

bool NotesWorkspaceController::restoreFolderTrashAt(qsizetype index)
{
    if (!ensureFolderCatalogAvailable() || index < 0 || index >= trashUndoEntries_.size()
        || trashUndoEntries_.at(index).kind != TrashUndoEntry::FolderTrash) {
        return false;
    }
    const auto branch = trashUndoEntries_.at(index).folderBranch;
    if (const auto restoreError = folderCatalogManager_->restoreFolderBranch(branch)) {
        setError(restoreError.message);
        return false;
    }
    trashUndoEntries_.removeAt(index);
    emit trashUndoChanged();
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
    const auto previousSize = trashUndoEntries_.size();
    trashUndoEntries_.removeIf([](const TrashUndoEntry &entry) { return entry.kind == TrashUndoEntry::FolderTrash; });
    if (trashUndoEntries_.size() == previousSize)
        return;
    emit trashUndoChanged();
}

void NotesWorkspaceController::clearTrashUndo()
{
    if (trashUndoEntries_.isEmpty())
        return;
    trashUndoEntries_.clear();
    emit trashUndoChanged();
}

void NotesWorkspaceController::removeNoteTrashUndo(const QString &storageId, const QString &noteId)
{
    const auto previousSize = trashUndoEntries_.size();
    trashUndoEntries_.removeIf([&](const TrashUndoEntry &entry) {
        return entry.kind == TrashUndoEntry::NoteTrash && entry.storageId == storageId && entry.noteId == noteId;
    });
    if (trashUndoEntries_.size() != previousSize)
        emit trashUndoChanged();
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
    if (storageId == DraftManager::draftsStorageId()) {
        const QUuid draftId(noteId);
        if (draftId.isNull())
            return {};
        if (currentEditor_ && currentEditor_->draftId() == draftId)
            return currentEditor_->folderIdString();
        const auto draft = draftManager_->pendingDraft(draftId);
        return draft ? draft.value.folderId.toString(QUuid::WithoutBraces) : QString();
    }
    if (currentEditor_ && currentEditor_->storageId() == storageId && currentEditor_->noteId() == noteId)
        return currentEditor_->folderIdString();
    const auto pending = draftManager_->pendingDraftForNote(storageId, noteId);
    if (pending)
        return pending.value.folderId.toString(QUuid::WithoutBraces);
    const QUuid presentedDraftId(noteId);
    const auto  presentedDraft = draftManager_->pendingDraft(presentedDraftId);
    if (presentedDraft && presentedDraft.value.storageId == storageId)
        return presentedDraft.value.folderId.toString(QUuid::WithoutBraces);

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
    QUuid draftId;
    if (storageId == DraftManager::draftsStorageId())
        draftId = QUuid(noteId);
    else {
        const auto pending = draftManager_->pendingDraftForNote(storageId, noteId);
        if (pending) {
            draftId = pending.value.id;
        } else {
            const QUuid presentedDraftId(noteId);
            const auto  presentedDraft = draftManager_->pendingDraft(presentedDraftId);
            if (presentedDraft && presentedDraft.value.storageId == storageId)
                draftId = presentedDraftId;
        }
    }
    if (!draftId.isNull()) {
        if (currentEditor_ && currentEditor_->draftId() == draftId) {
            qCInfo(logWorkspaceFolders) << "Routing draft folder assignment through current editor";
            return assignCurrentNoteFolder(folderIdText);
        }
        if (const auto error = draftManager_->setDraftFolder(draftId, folderId, true)) {
            setError(error.message);
            return false;
        }
        const auto draft = draftManager_->pendingDraft(draftId);
        if (draft && draft.value.state != DraftRecord::Editing) {
            if (const auto error = draftManager_->retryDraftNow(draftId)) {
                setError(error.message);
                return false;
            }
        }
        return true;
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

} // namespace AnyKeep
