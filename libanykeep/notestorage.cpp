#include "notestorage.h"

#include <QPointer>
#include <QSet>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <utility>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto TimeZoneUTC = QTimeZone::Initialization::UTC;
#else
constexpr auto TimeZoneUTC = Qt::UTC;
#endif

namespace AnyKeep {

namespace {
    StorageError unavailableError(const QString &message)
    {
        StorageError error;
        error.code      = StorageError::Unavailable;
        error.message   = message;
        error.retryable = true;
        return error;
    }

    struct GenericReorderStep {
        QString   noteId;
        QDateTime modified;
    };

    class GenericReorderOperation final : public QObject {
    public:
        GenericReorderOperation(NoteStorage *storage, NoteReorderJob *result, QList<GenericReorderStep> steps) :
            QObject(result), storage_(storage), result_(result), steps_(std::move(steps))
        {
        }

        void startNext()
        {
            if (!result_ || result_->isFinished())
                return;
            if (!storage_) {
                result_->fail({ StorageError::Unavailable, tr("The note storage is no longer available"), false });
                return;
            }
            if (nextStep_ >= steps_.size()) {
                result_->complete();
                return;
            }

            const auto step   = steps_.at(nextStep_);
            auto      *load   = storage_->loadNoteAsync(step.noteId, this);
            const auto loaded = [this, load, step]() {
                if (!result_ || result_->isFinished())
                    return;
                if (load->state() != StorageJob::Succeeded) {
                    const auto error = load->error();
                    result_->fail(error ? error
                                        : StorageError { StorageError::Other,
                                                         tr("Failed to load a note for reordering"), false });
                    load->deleteLater();
                    return;
                }
                Note note = load->result();
                load->deleteLater();
                note.setLastChangeUTC(step.modified);
                note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), step.modified);

                auto      *save  = storage_->saveNoteAsync(note, this);
                const auto saved = [this, save]() {
                    if (!result_ || result_->isFinished())
                        return;
                    if (save->state() != StorageJob::Succeeded) {
                        const auto error = save->error();
                        result_->fail(
                            error ? error
                                  : StorageError { StorageError::Other, tr("Failed to save a reordered note"), false });
                        save->deleteLater();
                        return;
                    }
                    save->deleteLater();
                    ++nextStep_;
                    startNext();
                };
                connect(save, &StorageJob::finished, this, saved);
                if (save->isFinished())
                    QTimer::singleShot(0, this, saved);
            };
            connect(load, &StorageJob::finished, this, loaded);
            if (load->isFinished())
                QTimer::singleShot(0, this, loaded);
        }

    private:
        QPointer<NoteStorage>     storage_;
        QPointer<NoteReorderJob>  result_;
        QList<GenericReorderStep> steps_;
        qsizetype                 nextStep_ { 0 };
    };
}

bool NoteStorage::loadNote(Note &target)
{
    if (target.isNull() || target.id().isEmpty())
        return !target.isNull();
    auto loaded = note(target.id());
    if (loaded.isNull())
        return false;
    target = loaded;
    return target.isLoaded();
}

StorageInitJob *NoteStorage::initAsync(QObject *owner)
{
    auto *job = new StorageInitJob(owner ? owner : this);
    job->start();
    QPointer<StorageInitJob> guard(job);
    QTimer::singleShot(0, this, [this, guard]() {
        if (!guard || guard->isFinished())
            return;
        if (init())
            guard->complete();
        else
            guard->fail(unavailableError(tr("Failed to initialize storage %1").arg(name())));
    });
    return job;
}

NoteListJob *NoteStorage::refreshNotesAsync(int limit, QObject *owner)
{
    auto *job = new NoteListJob(owner ? owner : this);
    job->start();
    QPointer<NoteListJob> guard(job);
    QTimer::singleShot(0, this, [this, guard, limit]() {
        if (!guard || guard->isFinished())
            return;
        const auto notes = noteList(limit);
        if (isAccessible())
            guard->complete(notes);
        else
            guard->fail(unavailableError(tr("Storage %1 is unavailable").arg(name())));
    });
    return job;
}

NoteLoadJob *NoteStorage::loadNoteAsync(const QString &id, QObject *owner)
{
    auto *job = new NoteLoadJob(owner ? owner : this);
    job->start();
    QPointer<NoteLoadJob> guard(job);
    QTimer::singleShot(0, this, [this, guard, id]() {
        if (!guard || guard->isFinished())
            return;
        auto loadedNote = note(id);
        if (loadedNote.isNull()) {
            StorageError error;
            error.code    = StorageError::NotFound;
            error.message = tr("Note was not found");
            guard->fail(error);
            return;
        }
        if (!loadedNote.isLoaded() && !loadedNote.load()) {
            guard->fail(unavailableError(tr("Failed to load note from %1").arg(name())));
            return;
        }
        guard->complete(loadedNote);
    });
    return job;
}

NoteSaveJob *NoteStorage::saveNoteAsync(const Note &note, QObject *owner)
{
    auto *job = new NoteSaveJob(owner ? owner : this);
    job->start();
    QPointer<NoteSaveJob> guard(job);
    QTimer::singleShot(0, this, [this, guard, note]() {
        if (!guard || guard->isFinished())
            return;

        // Synchronous storages may assign or change the persistent identifier
        // inside saveNote(). Their notification carries the canonical saved
        // Note, while the input object still has the old (or empty) id.
        Note       saved;
        const auto captureSaved = [&saved](const Note &persisted) { saved = persisted; };
        const auto added        = connect(this, &NoteStorage::noteAdded, this, captureSaved, Qt::DirectConnection);
        const auto modified     = connect(this, &NoteStorage::noteModified, this, captureSaved, Qt::DirectConnection);
        const auto idChanged    = connect(
            this, &NoteStorage::noteIdChanged, this,
            [&saved](const Note &persisted, const QString &) { saved = persisted; }, Qt::DirectConnection);
        const bool succeeded = saveNote(note);
        disconnect(added);
        disconnect(modified);
        disconnect(idChanged);

        if (!guard)
            return;
        if (succeeded)
            guard->complete(saved.isNull() ? note : saved);
        else
            guard->fail(unavailableError(tr("Failed to save note to %1").arg(name())));
    });
    return job;
}

NoteFolderChangeJob *NoteStorage::changeNoteFolderAsync(const Note &note, QObject *owner)
{
    Q_UNUSED(note)
    auto *job = new NoteFolderChangeJob(owner ? owner : this);
    job->start();
    job->fail({ StorageError::Other, tr("This storage does not support folders"), false });
    return job;
}

FolderCatalogJob *NoteStorage::replaceNativeFolderCatalogAsync(const FolderCatalogSnapshot &snapshot, QObject *owner)
{
    Q_UNUSED(snapshot)
    auto *job = new FolderCatalogJob(owner ? owner : this);
    job->start();
    job->fail({ StorageError::Other, tr("This storage does not support a native folder catalog"), false });
    return job;
}

NoteRemoveJob *NoteStorage::removeNoteAsync(const QString &id, QObject *owner)
{
    auto *job = new NoteRemoveJob(owner ? owner : this);
    job->start();
    QPointer<NoteRemoveJob> guard(job);
    QTimer::singleShot(0, this, [this, guard, id]() {
        if (!guard || guard->isFinished())
            return;
        removeNote(id);
        guard->complete();
    });
    return job;
}

QList<NoteStorage::NoteReorderChange> NoteStorage::noteReorderChanges(const QStringList &noteIds,
                                                                      const QString &afterNoteId, StorageError *error)
{
    if (error)
        *error = {};
    const auto fail = [error](StorageError::Code code, const QString &message) {
        if (error)
            *error = { code, message, false };
        return QList<NoteReorderChange> {};
    };

    const qint64 timeStep = requestedModificationTimeResolutionMs();
    if (!supportsNoteReordering() || timeStep <= 0)
        return fail(StorageError::Other, tr("This storage does not support manual note ordering"));

    QStringList   orderedIds;
    QSet<QString> movedIds;
    for (const auto &id : noteIds) {
        if (!id.isEmpty() && !movedIds.contains(id)) {
            movedIds.insert(id);
            orderedIds.append(id);
        }
    }
    if (orderedIds.isEmpty())
        return {};
    if (movedIds.contains(afterNoteId))
        return fail(StorageError::Other, tr("A note cannot be reordered relative to itself"));

    auto notes = noteList();
    std::stable_sort(notes.begin(), notes.end(), noteListItemModifyComparer);
    QHash<QString, Note> notesById;
    QStringList          originalOrder;
    for (const auto &note : std::as_const(notes)) {
        if (note.isNull() || note.id().isEmpty() || notesById.contains(note.id()))
            continue;
        notesById.insert(note.id(), note);
        originalOrder.append(note.id());
    }
    for (const auto &id : std::as_const(orderedIds)) {
        if (!notesById.contains(id))
            return fail(StorageError::NotFound, tr("A note selected for reordering is no longer available"));
    }

    QList<Note> remaining;
    remaining.reserve(notes.size());
    for (const auto &note : std::as_const(notes)) {
        if (!note.isNull() && !movedIds.contains(note.id()))
            remaining.append(note);
    }

    qsizetype insertionIndex = 0;
    if (!afterNoteId.isEmpty()) {
        const auto anchor = std::find_if(remaining.cbegin(), remaining.cend(),
                                         [&afterNoteId](const Note &note) { return note.id() == afterNoteId; });
        if (anchor == remaining.cend())
            return fail(StorageError::NotFound, tr("The note used as the reorder boundary is no longer available"));
        insertionIndex = std::distance(remaining.cbegin(), anchor) + 1;
    }

    QStringList finalOrder;
    finalOrder.reserve(originalOrder.size());
    for (qsizetype i = 0; i < insertionIndex; ++i)
        finalOrder.append(remaining.at(i).id());
    finalOrder.append(orderedIds);
    for (qsizetype i = insertionIndex; i < remaining.size(); ++i)
        finalOrder.append(remaining.at(i).id());
    if (finalOrder == originalOrder)
        return {};

    const qint64 nowMs      = QDateTime::currentMSecsSinceEpoch();
    const qint64 alignedNow = nowMs - nowMs % timeStep;
    qint64       upperMs    = insertionIndex > 0 && remaining.at(insertionIndex - 1).lastChangeUTC().isValid()
                 ? remaining.at(insertionIndex - 1).lastChangeUTC().toMSecsSinceEpoch()
                 : alignedNow + timeStep;
    upperMs                 = qMin(upperMs, alignedNow + timeStep);

    // Work in storage-resolution ticks. maxTick is the newest representable
    // value strictly below the previous note; minTick is strictly above the
    // following note.
    const qint64 maxTick = (upperMs - 1) / timeStep;
    const auto   nextTime
        = insertionIndex < remaining.size() ? remaining.at(insertionIndex).lastChangeUTC() : QDateTime {};
    const qint64 minTick        = nextTime.isValid() ? nextTime.toMSecsSinceEpoch() / timeStep + 1 : 0;
    const qint64 availableTicks = nextTime.isValid() ? qMax<qint64>(0, maxTick - minTick + 1) : 0;
    const bool   fitsInGap      = nextTime.isValid() && availableTicks >= orderedIds.size();
    const qint64 spacingTicks   = fitsInGap ? qMax<qint64>(1, (availableTicks + 1) / (orderedIds.size() + 1)) : 1;

    QList<NoteReorderChange> changes;
    changes.reserve(orderedIds.size() + remaining.size() - insertionIndex);
    qint64 cursorTick = maxTick + 1;
    for (const auto &id : std::as_const(orderedIds)) {
        cursorTick -= spacingTicks;
        changes.append({ notesById.value(id), QDateTime::fromMSecsSinceEpoch(cursorTick * timeStep, TimeZoneUTC) });
    }

    // If the neighboring timestamps leave no representable gap, move the
    // colliding tail backwards as part of the same storage-owned operation.
    qint64 cursorMs = cursorTick * timeStep;
    for (qsizetype i = insertionIndex; i < remaining.size(); ++i) {
        const auto existingTime = remaining.at(i).lastChangeUTC();
        if (!existingTime.isValid() || existingTime.toMSecsSinceEpoch() >= cursorMs) {
            cursorMs -= timeStep;
            changes.append({ remaining.at(i), QDateTime::fromMSecsSinceEpoch(cursorMs, TimeZoneUTC) });
        } else {
            cursorMs = existingTime.toMSecsSinceEpoch();
        }
    }
    return changes;
}

NoteReorderJob *NoteStorage::reorderNoteAsync(const QString &noteId, const QString &afterNoteId, QObject *owner)
{
    return reorderNotesAsync({ noteId }, afterNoteId, owner);
}

NoteReorderJob *NoteStorage::reorderNotesAsync(const QStringList &noteIds, const QString &afterNoteId, QObject *owner)
{
    auto *job = new NoteReorderJob(owner ? owner : this);
    job->start();
    StorageError error;
    const auto   changes = noteReorderChanges(noteIds, afterNoteId, &error);
    if (error) {
        job->fail(error);
        return job;
    }
    if (changes.isEmpty()) {
        job->complete();
        return job;
    }

    QList<GenericReorderStep> steps;
    steps.reserve(changes.size());
    for (const auto &change : changes)
        steps.append({ change.note.id(), change.modified });
    auto *operation = new GenericReorderOperation(this, job, std::move(steps));
    QTimer::singleShot(0, operation, [operation]() { operation->startNext(); });
    return job;
}

} // namespace AnyKeep
