#include "notesworkspacecontroller.h"

#include "draftmanager.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notestorage.h"
#include "notetagline.h"
#include "storagejob.h"

#include <QSet>
#include <memory>

namespace AnyKeep {

bool NotesWorkspaceController::addTagToNotes(const QVariantList &notes, const QString &tag)
{
    const QString trimmed = tag.trimmed();
    const QString name = trimmed.startsWith(QLatin1Char('#')) ? trimmed.mid(1) : trimmed;
    if (!NoteTagLine::isValidTagName(name)) {
        setError(tr("Invalid tag name"));
        return false;
    }
    return updateNotesMetadata(notes, name, -1);
}

bool NotesWorkspaceController::setNotesFavorite(const QVariantList &notes, bool favorite)
{
    return updateNotesMetadata(notes, {}, favorite ? 1 : 0);
}

bool NotesWorkspaceController::updateNotesMetadata(const QVariantList &notes, const QString &tag, int favoriteChange)
{
    if (metadataBatchBusy_) {
        setError(tr("Another bulk note operation is still running"));
        return false;
    }
    if (notes.isEmpty()) {
        setError(tr("Select notes first"));
        return false;
    }

    struct BatchState {
        int remaining { 0 };
        QVariantList failed;
        QStringList errors;
    };
    auto batch = std::make_shared<BatchState>();
    batch->remaining = notes.size();
    metadataBatchBusy_ = true;
    emit metadataBatchBusyChanged();
    beginOperation();

    const auto complete = [this, batch](const QVariantMap &descriptor, const QString &error) {
        if (!error.isEmpty()) {
            batch->failed.append(descriptor);
            const QString title = descriptor.value(QStringLiteral("title")).toString();
            batch->errors.append(title.isEmpty() ? error : title + QStringLiteral(": ") + error);
        }
        if (--batch->remaining > 0)
            return;
        metadataBatchBusy_ = false;
        emit metadataBatchBusyChanged();
        const QString summary = batch->errors.isEmpty() ? QString()
            : tr("%n selected note(s) could not be updated", nullptr, int(batch->failed.size()))
                  + QStringLiteral(": ") + batch->errors.join(QStringLiteral("; "));
        setError(summary);
        emit metadataBatchFinished(batch->failed, summary);
        endOperation();
    };

    const auto applyToEditor = [tag, favoriteChange](NoteEditor *editor) -> QString {
        if (!editor)
            return QObject::tr("The note is unavailable");
        if (favoriteChange >= 0) {
            if (!editor->supportsFavorite())
                return QObject::tr("Favorites are not supported by this storage");
            editor->setFavorite(bool(favoriteChange));
        }
        if (!tag.isEmpty() && !editor->addTag(tag))
            return editor->errorString().isEmpty() ? QObject::tr("Could not add tag") : editor->errorString();
        if (!editor->save())
            return editor->errorString().isEmpty() ? QObject::tr("Could not save note draft") : editor->errorString();
        return {};
    };

    const auto applyLoaded = [this, tag, favoriteChange, applyToEditor](const QString &storageId,
                                                                      const QString &noteId,
                                                                      const Note &loaded) -> QString {
        // A new editor or durable draft may have appeared during the load.
        // Always update that canonical source instead of overwriting it with
        // the older remote body returned by the storage job.
        if (auto *editor = draftManager_->liveEditorForNote(storageId, noteId))
            return applyToEditor(editor);
        if (const auto pending = draftManager_->pendingDraftForNote(storageId, noteId)) {
            if (const auto error = draftManager_->updatePendingMetadata(pending.value.id, tag, favoriteChange))
                return error.message;
            return {};
        }
        if (loaded.isNull())
            return QObject::tr("Could not load note");
        NoteEditor editor(loaded, *draftManager_);
        if (const auto error = applyToEditor(&editor); !error.isEmpty())
            return error;
        return editor.close() ? QString()
                              : (editor.errorString().isEmpty() ? QObject::tr("Could not queue note publication")
                                                                : editor.errorString());
    };

    QSet<QString> seen;
    for (const QVariant &value : notes) {
        const QVariantMap descriptor = value.toMap();
        const QString storageId = descriptor.value(QStringLiteral("storageId")).toString();
        const QString noteId = descriptor.value(QStringLiteral("noteId")).toString();
        const QString key = storageId + QChar(0x1f) + noteId;
        if (storageId.isEmpty() || noteId.isEmpty() || seen.contains(key)) {
            complete(descriptor, tr("Invalid or duplicate note selection"));
            continue;
        }
        seen.insert(key);
        if (isRecycledNote(storageId, noteId)) {
            complete(descriptor, tr("Restore this note before changing its metadata"));
            continue;
        }

        const bool isDraft = storageId == DraftManager::draftsStorageId();
        auto storage = isDraft ? NoteStorage::Ptr {} : NoteManager::instance()->storage(storageId);
        if (!isDraft && !storage) {
            complete(descriptor, tr("The note storage is unavailable"));
            continue;
        }
        if (!isDraft && !storage->canAcceptWrites()) {
            complete(descriptor, tr("The note storage is read-only"));
            continue;
        }
        if (favoriteChange >= 0 && (isDraft || !storage->supportsFavorite())) {
            complete(descriptor, tr("Favorites are not supported by this storage"));
            continue;
        }

        auto *live = isDraft ? draftManager_->liveEditorForDraft(QUuid(noteId))
                             : draftManager_->liveEditorForNote(storageId, noteId);
        if (live) {
            complete(descriptor, applyToEditor(live));
            continue;
        }

        const auto pending = isDraft ? draftManager_->pendingDraft(QUuid(noteId))
                                     : draftManager_->pendingDraftForNote(storageId, noteId);
        if (pending) {
            const auto error = draftManager_->updatePendingMetadata(pending.value.id, tag, favoriteChange);
            complete(descriptor, error ? error.message : QString());
            continue;
        }
        if (isDraft) {
            complete(descriptor, tr("The selected draft no longer exists"));
            continue;
        }

        auto *job = storage->loadNoteAsync(noteId, this);
        if (!job) {
            complete(descriptor, tr("Could not start loading the selected note"));
            continue;
        }
        const auto finished = [this, job, descriptor, storageId, noteId, applyLoaded, complete]() {
            QString error;
            if (job->state() == StorageJob::Succeeded)
                error = applyLoaded(storageId, noteId, job->result());
            else
                error = job->error().message.isEmpty() ? tr("Could not load note") : job->error().message;
            complete(descriptor, error);
            job->deleteLater();
        };
        connect(job, &StorageJob::finished, this, finished);
        // Some storage adapters finish synchronously before returning a job.
        if (job->isFinished())
            finished();
    }
    return true;
}

} // namespace AnyKeep
