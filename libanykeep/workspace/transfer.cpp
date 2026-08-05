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
        if (!draftManager_->isLastEditingSession(currentEditor_->draftId())) {
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
            draftManager_->discard(destinationDraftId);
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
        draftManager_->publishPending();
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
            draftManager_->publishPending();
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

    auto       *drafts   = draftManager_;
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
    draftManager_->publishPending();
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

} // namespace AnyKeep
