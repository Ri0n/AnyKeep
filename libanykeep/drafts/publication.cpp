#include "draftmanager.h"

#include "private.h"

#include "conflictresolver.h"
#include "notemanager.h"
#include "notestorage.h"
#include "storagejob.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>

#include <utility>

// Uncomment for detailed draft publication/conflict diagnostics.
// #define ANYKEEP_ENABLE_CONFLICT_TRACE

#ifdef ANYKEEP_ENABLE_CONFLICT_TRACE
#define CONFLICT_TRACE qInfo().noquote()
#else
#define CONFLICT_TRACE QNoDebug()
#endif

namespace AnyKeep {

using DraftManagerPrivate::concurrencySummary;
using DraftManagerPrivate::draftOperationName;
using DraftManagerPrivate::draftStateName;

namespace {

    bool sameMediaReference(const MediaReference &left, const MediaReference &right)
    {
        return left.id == right.id && left.blobId == right.blobId && left.originalName == right.originalName
            && left.portableName == right.portableName && left.mediaType == right.mediaType && left.size == right.size
            && left.checksum == right.checksum && left.remoteData == right.remoteData;
    }

    bool hasSamePublishedContents(const DraftRecord &draft, const Note &note)
    {
        if (draft.title != note.title() || draft.body != note.text() || draft.format != note.format()
            || draft.folderId != note.folderId()
            || draft.backendData.value(QString::fromLatin1(FavoriteBackendKey)).toBool() != note.isFavorite()
            || draft.media.size() != note.media().size())
            return false;
        const auto media = note.media();
        for (qsizetype index = 0; index < draft.media.size(); ++index) {
            if (!sameMediaReference(draft.media.at(index), media.at(index)))
                return false;
        }
        return true;
    }

    bool sameDraftRecord(const DraftRecord &left, const DraftRecord &right)
    {
        if (left.id != right.id || left.operation != right.operation || left.state != right.state
            || left.storageId != right.storageId || left.remoteNoteId != right.remoteNoteId || left.title != right.title
            || left.body != right.body || left.format != right.format || left.tags != right.tags
            || left.folderId != right.folderId || left.folderUserOverride != right.folderUserOverride
            || left.removeSourceStorageId != right.removeSourceStorageId
            || left.removeSourceNoteId != right.removeSourceNoteId || left.backendData != right.backendData
            || left.revision != right.revision || left.updatedAt != right.updatedAt || left.lastError != right.lastError
            || left.retryAt != right.retryAt || left.media.size() != right.media.size()) {
            return false;
        }
        for (qsizetype index = 0; index < left.media.size(); ++index) {
            if (!sameMediaReference(left.media.at(index), right.media.at(index)))
                return false;
        }
        return true;
    }

} // namespace

void DraftManager::cancelPublication(const QUuid &draftId)
{
    auto job = publishJobs_.take(draftId);
    publishing_.remove(draftId);
    if (job && !job->isFinished()) {
        qCInfo(logDraftPersistence) << "Cancelling active draft publication: id="
                                    << draftId.toString(QUuid::WithoutBraces);
        job->cancel();
    }
}

QList<DraftRecord> DraftManager::pendingDrafts() const
{
    QList<DraftRecord> result;
    if (!store_)
        return result;
    const auto records = store_->records();
    if (!records)
        return result;
    for (const auto &record : records.value) {
        if (record.operation == DraftRecord::Publish)
            result.push_back(record);
    }
    return result;
}

DraftStoreResult<DraftRecord> DraftManager::pendingDraft(const QUuid &draftId) const
{
    if (!store_)
        return { {}, { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ } };
    if (draftId.isNull())
        return { {}, { DraftStoreError::InvalidArgument, tr("Draft identifier is empty") } };
    auto draft = store_->load(draftId);
    if (!draft)
        return draft;
    if (draft.value.operation != DraftRecord::Publish)
        return { {}, { DraftStoreError::NotFound, tr("No note draft was found") } };
    return draft;
}

DraftStoreResult<DraftRecord> DraftManager::pendingDraftForNote(const QString &storageId, const QString &noteId) const
{
    if (!store_)
        return { {}, { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ } };
    if (storageId.isEmpty() || noteId.isEmpty())
        return { {}, { DraftStoreError::InvalidArgument, tr("Storage or note identifier is empty") } };
    const auto records = store_->records();
    if (!records)
        return { {}, records.error };

    const DraftRecord *best = nullptr;
    for (const auto &record : records.value) {
        if (record.operation != DraftRecord::Publish || record.storageId != storageId || record.remoteNoteId != noteId)
            continue;
        if (!best || record.updatedAt > best->updatedAt)
            best = &record;
    }
    if (!best)
        return { {}, { DraftStoreError::NotFound, tr("No pending draft was found for this note") } };
    return { *best, {} };
}

void DraftManager::prepareForShutdown()
{
    if (shuttingDown_)
        return;
    shuttingDown_ = true;
    qCInfo(logDraftPersistence) << "Preparing draft publication for application shutdown; active=" << publishing_.size()
                                << "jobs=" << publishJobs_.size();

    const auto jobs = publishJobs_.values();
    publishJobs_.clear();
    publishing_.clear();
    for (auto job : jobs) {
        if (job && !job->isFinished())
            job->cancel();
    }

    if (!store_)
        return;
    const auto records = store_->records();
    if (!records)
        return;

    bool changed = false;
    for (auto record : records.value) {
        if (record.state != DraftRecord::Publishing && !publishing_.contains(record.id))
            continue;
        record.state = record.operation == DraftRecord::Publish && record.storageId.isEmpty()
            ? DraftRecord::NeedsRouting
            : DraftRecord::Ready;
        record.lastError.clear();
        record.retryAt   = {};
        record.updatedAt = QDateTime::currentDateTimeUtc();
        if (const auto error = store_->write(record)) {
            qCWarning(logDraftPersistence)
                << "Failed to preserve draft during shutdown: id=" << record.id.toString(QUuid::WithoutBraces)
                << error.message;
        } else {
            changed = true;
            qCInfo(logDraftPersistence) << "Requeued draft for next launch: id="
                                        << record.id.toString(QUuid::WithoutBraces)
                                        << "state=" << draftStateName(record.state) << "storage=" << record.storageId
                                        << "remoteNotePresent=" << !record.remoteNoteId.isEmpty();
        }
    }
    publishing_.clear();
    if (changed)
        emit draftsChanged();
}

QList<DraftRecord> DraftManager::recoverableDrafts() const
{
    QList<DraftRecord> result;
    if (!store_)
        return result;
    auto records = store_->records();
    if (!records)
        return result;
    for (const auto &record : records.value) {
        if (record.operation == DraftRecord::Publish && record.state == DraftRecord::Editing)
            result.push_back(record);
    }
    qCInfo(logDraftPersistence) << "Recoverable editing drafts:" << result.size();
    return result;
}

void DraftManager::publishPending()
{
    if (shuttingDown_) {
        qCInfo(logDraftPersistence) << "Skipping draft publication during application shutdown";
        return;
    }
    if (!store_) {
        qCWarning(logDraftPersistence) << "Cannot publish drafts: draft store is unavailable";
        return;
    }
    auto records = store_->records();
    if (!records) {
        qCWarning(logDraftPersistence) << "Cannot enumerate drafts for publication" << int(records.error.code)
                                       << records.error.message;
        return;
    }
    qCInfo(logDraftPersistence) << "Checking" << records.value.size()
                                << "draft records for publication; active=" << publishing_.size();
    for (const auto &record : records.value) {
        qCInfo(logDraftPersistence) << "Draft publication candidate: id=" << record.id.toString(QUuid::WithoutBraces)
                                    << "operation=" << draftOperationName(record.operation)
                                    << "state=" << draftStateName(record.state) << "storage=" << record.storageId
                                    << "remoteNotePresent=" << !record.remoteNoteId.isEmpty()
                                    << "revision=" << record.revision;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &storedRecord : records.value) {
        auto record = storedRecord;
        if (record.state == DraftRecord::Retry) {
            if (!record.retryAt.isValid())
                continue;
            if (record.retryAt > now) {
                QTimer::singleShot(qMax<qint64>(1, now.msecsTo(record.retryAt)), this, &DraftManager::publishPending);
                continue;
            }
        }
        const bool canRoute = record.operation == DraftRecord::Publish
            && (record.state == DraftRecord::NeedsRouting || record.state == DraftRecord::Ready
                || record.state == DraftRecord::Retry)
            && !publishing_.contains(record.id);
        if (canRoute && prePublicationHandler_) {
            auto       routed     = record;
            const auto routeError = prePublicationHandler_(&routed);
            if (routeError) {
                const bool retryable = routeError.code == DraftStoreError::Io
                    || routeError.code == DraftStoreError::Locked
                    || routeError.code == DraftStoreError::CryptoUnavailable;
                retry(record, routeError.message, retryable);
                continue;
            }
            if (routed.id != record.id || routed.operation != record.operation) {
                retry(record, tr("The pre-publication handler changed the draft identity"), false);
                continue;
            }
            if (!sameDraftRecord(record, routed)) {
                routed.updatedAt = now;
                if (const auto writeError = store_->write(routed)) {
                    retry(record, writeError.message, true);
                    continue;
                }
                emit draftsChanged();
                record = std::move(routed);
            }
        }
        if (record.operation == DraftRecord::Publish && record.state == DraftRecord::NeedsRouting) {
            auto target = NoteManager::instance()->defaultStorage();
            if (!target)
                continue;
            auto routed      = record;
            routed.storageId = target->systemName();
            routed.remoteNoteId.clear();
            routed.state = DraftRecord::Ready;
            routed.lastError.clear();
            routed.retryAt = {};
            if (store_->write(routed))
                continue;
            process(routed);
            continue;
        }
        if ((record.state == DraftRecord::Ready || record.state == DraftRecord::Retry
             || record.state == DraftRecord::Publishing)
            && !publishing_.contains(record.id)) {
            process(record);
        }
    }
    if (publishing_.isEmpty())
        emit publishingIdle();
}

void DraftManager::process(const DraftRecord &record)
{
    if (record.operation == DraftRecord::Delete)
        remove(record);
    else
        publish(record);
}

void DraftManager::retry(const DraftRecord &record, const QString &message, bool retryable)
{
    qCWarning(logDraftPersistence) << "Draft publication retry/failure: id=" << record.id.toString(QUuid::WithoutBraces)
                                   << "state=" << draftStateName(record.state) << "retryable=" << retryable
                                   << "message=" << message;
    constexpr qint64 MinimumDelay = 30;
    constexpr qint64 MaximumDelay = 300;
    const auto       now          = QDateTime::currentDateTimeUtc();
    qint64           delay        = MinimumDelay;
    if (record.updatedAt.isValid() && record.retryAt.isValid())
        delay = qBound(MinimumDelay, record.updatedAt.secsTo(record.retryAt) * 2, MaximumDelay);

    auto retry            = record;
    retry.state           = DraftRecord::Retry;
    retry.lastError       = message;
    retry.updatedAt       = now;
    retry.retryAt         = retryable ? now.addSecs(delay) : QDateTime {};
    const auto writeError = store_->write(retry);
    if (writeError)
        qCWarning(logDraftPersistence) << "Failed to persist draft failure state:" << writeError.message;
    else
        emit draftsChanged();
    emit draftPublishFailed(record.id, message);
    if (retryable)
        QTimer::singleShot(delay * 1000, this, &DraftManager::publishPending);
}

void DraftManager::resolveConflict(const DraftRecord &record, const StorageError &error, const Note &remoteNote)
{
    CONFLICT_TRACE << "Conflict trace: invoking resolver draft=" << record.id.toString(QUuid::WithoutBraces)
                   << "note=" << record.remoteNoteId << "base=" << concurrencySummary(record.backendData)
                   << "remote=" << concurrencySummary(remoteNote.backendData()) << "message=" << error.message;
    if (!conflictResolver_) {
        retry(record, error.message, false);
        return;
    }

    // An asynchronous/user-interactive resolver may outlive this turn of the
    // event loop. Persist the draft as recoverable before handing it over.
    auto recoverable      = record;
    recoverable.state     = DraftRecord::Editing;
    recoverable.lastError = error.message;
    recoverable.retryAt   = {};
    if (const auto writeError = store_->write(recoverable)) {
        emit publicationAbandoned(tr("Failed to preserve a conflicting draft: %1").arg(writeError.message));
        return;
    }

    conflictResolver_->resolve(
        { recoverable, remoteNote, error.message },
        [this, id = record.id, fallbackMessage = error.message](ConflictResolution resolution) {
            if (!store_)
                return;
            auto current = store_->load(id);
            if (!current) {
                emit publicationAbandoned(tr("Failed to load a conflicting draft: %1").arg(current.error.message));
                return;
            }

            switch (resolution.action) {
            case ConflictResolution::CreateCopy: {
                CONFLICT_TRACE << "Conflict trace: resolver action=create-copy draft="
                               << id.toString(QUuid::WithoutBraces) << "old-note=" << current.value.remoteNoteId;
                current.value.remoteNoteId.clear();
                current.value.backendData.clear();
                current.value.title = resolution.copyTitle.isEmpty() ? tr("%1 (conflict copy)").arg(current.value.title)
                                                                     : resolution.copyTitle;
                current.value.state
                    = current.value.storageId.isEmpty() ? DraftRecord::NeedsRouting : DraftRecord::Ready;
                current.value.lastError.clear();
                current.value.retryAt = {};
                if (const auto writeError = store_->write(current.value)) {
                    retry(current.value, writeError.message, false);
                    return;
                }
                if (!resolution.notification.isEmpty())
                    emit conflictResolved(resolution.notification);
                QTimer::singleShot(0, this, &DraftManager::publishPending);
                break;
            }
            case ConflictResolution::KeepDraft: {
                CONFLICT_TRACE << "Conflict trace: resolver action=keep-draft draft="
                               << id.toString(QUuid::WithoutBraces);
                current.value.state     = DraftRecord::Editing;
                current.value.lastError = fallbackMessage;
                current.value.retryAt   = {};
                if (const auto writeError = store_->write(current.value))
                    emit publicationAbandoned(tr("Failed to preserve a conflicting draft: %1").arg(writeError.message));
                break;
            }
            case ConflictResolution::Discard:
                CONFLICT_TRACE << "Conflict trace: resolver action=discard draft=" << id.toString(QUuid::WithoutBraces);
                if (const auto removeError = store_->remove(id))
                    emit publicationAbandoned(tr("Failed to discard a conflicting draft: %1").arg(removeError.message));
                break;
            }
        });
}

void DraftManager::publish(const DraftRecord &record)
{
    qCInfo(logDraftPersistence) << "Publishing draft: id=" << record.id.toString(QUuid::WithoutBraces)
                                << "storage=" << record.storageId
                                << "remoteNotePresent=" << !record.remoteNoteId.isEmpty()
                                << "revision=" << record.revision << "titleLength=" << record.title.size()
                                << "bodyLength=" << record.body.size();
    CONFLICT_TRACE << "Conflict trace: publish begin draft=" << record.id.toString(QUuid::WithoutBraces)
                   << "storage=" << record.storageId << "note=" << record.remoteNoteId
                   << "base=" << concurrencySummary(record.backendData);
    auto storage = NoteManager::instance()->storage(record.storageId);
    if (!storage || !storage->canAcceptWrites()) {
        qCWarning(logDraftPersistence) << "Target storage is unavailable for draft"
                                       << record.id.toString(QUuid::WithoutBraces) << record.storageId
                                       << "exists=" << bool(storage)
                                       << "canWrite=" << (storage ? storage->canAcceptWrites() : false);
        retry(record, tr("Target storage is unavailable"));
        return;
    }
    auto publishing  = record;
    publishing.state = DraftRecord::Publishing;
    if (store_->write(publishing))
        return;
    publishing_.insert(record.id);

    const auto save = [this, record, storage](Note note) {
        if (note.isNull()) {
            publishing_.remove(record.id);
            retry(record, tr("Target note could not be created or loaded"));
            return;
        }
        // Restore the captured concurrency token. New-note drafts may also
        // carry one-shot storage hints such as a requested modification time.
        if (!record.backendData.isEmpty())
            note.setBackendData(record.backendData);
        note.setTitle(record.title);
        note.setText(record.body, record.format);
        note.setTags(record.tags);
        note.setFolderId(record.folderId);
        note.setMedia(record.media);
        qCInfo(logDraftPersistence) << "Submitting draft to storage: draft=" << record.id.toString(QUuid::WithoutBraces)
                                    << "storage=" << storage->systemName() << "noteIdPresent=" << !note.id().isEmpty();
        auto *job = storage->saveNoteAsync(note, this);
        publishJobs_.insert(record.id, job);
        connect(job, &StorageJob::finished, this, [this, record, job]() {
            if (publishJobs_.value(record.id) != job) {
                qCInfo(logDraftPersistence)
                    << "Ignoring stale draft save job: draft=" << record.id.toString(QUuid::WithoutBraces);
                job->deleteLater();
                return;
            }
            publishing_.remove(record.id);
            publishJobs_.remove(record.id);
            if (job->state() == StorageJob::Succeeded) {
                qCInfo(logDraftPersistence)
                    << "Draft publication succeeded: draft=" << record.id.toString(QUuid::WithoutBraces)
                    << "storage=" << job->result().storageId() << "noteIdPresent=" << !job->result().id().isEmpty();
                CONFLICT_TRACE << "Conflict trace: publish succeeded draft=" << record.id.toString(QUuid::WithoutBraces)
                               << "note=" << job->result().id()
                               << "result=" << concurrencySummary(job->result().backendData());
                finishPublishedDraft(record, job->result());
            } else {
                qCWarning(logDraftPersistence)
                    << "Draft publication job failed: draft=" << record.id.toString(QUuid::WithoutBraces)
                    << "state=" << int(job->state()) << "code=" << int(job->error().code)
                    << "retryable=" << job->error().retryable << "message=" << job->error().message;
                CONFLICT_TRACE << "Conflict trace: publish failed draft=" << record.id.toString(QUuid::WithoutBraces)
                               << "code=" << int(job->error().code) << "retryable=" << job->error().retryable
                               << "message=" << job->error().message;
                auto pending = store_->load(record.id);
                if (pending) {
                    if (shuttingDown_ && job->state() == StorageJob::Cancelled) {
                        qCInfo(logDraftPersistence)
                            << "Ignoring publication cancellation caused by application shutdown: draft="
                            << record.id.toString(QUuid::WithoutBraces);
                    } else if (job->error().code == StorageError::Conflict)
                        resolveConflict(pending.value, job->error());
                    else
                        retry(pending.value, job->error().message, job->error().retryable);
                }
            }
            job->deleteLater();
            if (publishing_.isEmpty())
                emit publishingIdle();
        });
    };

    if (record.remoteNoteId.isEmpty()) {
        save(storage->createNote());
        return;
    }

    auto *job = storage->loadNoteAsync(record.remoteNoteId, this);
    publishJobs_.insert(record.id, job);
    connect(job, &StorageJob::finished, this, [this, record, job, save]() mutable {
        if (publishJobs_.value(record.id) != job) {
            qCInfo(logDraftPersistence) << "Ignoring stale draft load job: draft="
                                        << record.id.toString(QUuid::WithoutBraces);
            job->deleteLater();
            return;
        }
        publishJobs_.remove(record.id);
        if (job->state() == StorageJob::Succeeded) {
            auto note = job->result();
            // A return to the original contents needs no publication. This is
            // deliberately compared with the storage's current/cached note,
            // not with a full second snapshot in every DraftRecord. It is also
            // safe when the remote changed concurrently but now has identical
            // contents: keeping that remote version is the desired no-op.
            if (hasSamePublishedContents(record, note)) {
                publishing_.remove(record.id);
                finishPublishedDraft(record, note);
                job->deleteLater();
                if (publishing_.isEmpty())
                    emit publishingIdle();
                return;
            }
            CONFLICT_TRACE << "Conflict trace: remote loaded draft=" << record.id.toString(QUuid::WithoutBraces)
                           << "note=" << record.remoteNoteId << "remote=" << concurrencySummary(note.backendData())
                           << "restoring-base=" << concurrencySummary(record.backendData);
            job->deleteLater();
            save(note);
            return;
        }
        publishing_.remove(record.id);
        if (shuttingDown_ && job->state() == StorageJob::Cancelled) {
            qCInfo(logDraftPersistence) << "Ignoring note-load cancellation caused by application shutdown: draft="
                                        << record.id.toString(QUuid::WithoutBraces);
        } else {
            retry(record, job->error().message, job->error().retryable);
        }
        job->deleteLater();
        if (publishing_.isEmpty())
            emit publishingIdle();
    });
}

void DraftManager::finishPublishedDraft(const DraftRecord &record, const Note &note)
{
    if (!store_)
        return;

    if (!record.removeSourceStorageId.isEmpty() && !record.removeSourceNoteId.isEmpty()
        && (record.removeSourceStorageId != note.storageId() || record.removeSourceNoteId != note.id())) {
        // Capture the destination identity before queuing source deletion. If
        // the process stops afterwards, a retry updates this exact destination
        // rather than creating a duplicate copy.
        auto completed         = record;
        completed.storageId    = note.storageId();
        completed.remoteNoteId = note.id();
        completed.backendData  = note.backendData();
        completed.state        = DraftRecord::Ready;
        completed.lastError.clear();
        completed.retryAt = {};
        if (const auto writeError = store_->write(completed)) {
            retry(record, writeError.message, true);
            return;
        }
        if (const auto removeError = queueRemoval(record.removeSourceStorageId, record.removeSourceNoteId)) {
            retry(completed, removeError.message, true);
            return;
        }
    }

    if (const auto removeError = store_->remove(record.id)) {
        retry(record, removeError.message, true);
        return;
    }
    emit draftsChanged();
    emit draftPublished(record.id, note);
}

void DraftManager::remove(const DraftRecord &record)
{
    auto storage = NoteManager::instance()->storage(record.storageId);
    if (!storage || !storage->isAccessible()) {
        retry(record, tr("Target storage is unavailable"));
        return;
    }

    auto removing  = record;
    removing.state = DraftRecord::Publishing;
    if (store_->write(removing))
        return;

    publishing_.insert(record.id);
    auto *job = storage->removeNoteAsync(record.remoteNoteId, this);
    publishJobs_.insert(record.id, job);
    connect(job, &StorageJob::finished, this, [this, id = record.id, job]() {
        if (publishJobs_.value(id) != job) {
            qCInfo(logDraftPersistence) << "Ignoring stale draft removal job: draft="
                                        << id.toString(QUuid::WithoutBraces);
            job->deleteLater();
            return;
        }
        publishing_.remove(id);
        publishJobs_.remove(id);
        if (job->state() == StorageJob::Succeeded) {
            store_->remove(id);
        } else if (!(shuttingDown_ && job->state() == StorageJob::Cancelled)) {
            auto pending = store_->load(id);
            if (pending)
                retry(pending.value, job->error().message, job->error().retryable);
        } else {
            qCInfo(logDraftPersistence) << "Ignoring deletion cancellation caused by application shutdown: draft="
                                        << id.toString(QUuid::WithoutBraces);
        }
        job->deleteLater();
        if (publishing_.isEmpty())
            emit publishingIdle();
    });
}

void DraftManager::storageBecameReady(NoteStorage *storage)
{
    if (!store_ || !storage)
        return;

    // A draft can be moved to Retry during startup before an asynchronous
    // storage has completed init(). Once that exact storage becomes ready,
    // retry immediately instead of waiting for the generic network backoff.
    const auto records = store_->records();
    if (records) {
        for (const auto &record : records.value) {
            if (record.state != DraftRecord::Retry || !record.retryAt.isValid()
                || record.storageId != storage->systemName()) {
                continue;
            }
            auto ready      = record;
            ready.state     = DraftRecord::Ready;
            ready.lastError = {};
            ready.retryAt   = {};
            if (const auto error = store_->write(ready)) {
                qWarning() << "Failed to requeue draft after storage became ready:" << error.message;
            }
        }
    }
    QTimer::singleShot(0, this, &DraftManager::publishPending);
}

void DraftManager::storageAboutToBeRemoved(NoteStorage *storage)
{
    if (!store_ || !storage)
        return;
    auto records = store_->records();
    if (!records)
        return;
    int affected = 0;
    for (auto record : records.value) {
        if (record.storageId != storage->systemName())
            continue;
        cancelPublication(record.id);
        if (shuttingDown_) {
            if (record.state == DraftRecord::Publishing)
                record.state = record.operation == DraftRecord::Publish && record.storageId.isEmpty()
                    ? DraftRecord::NeedsRouting
                    : DraftRecord::Ready;
            record.retryAt = {};
            if (record.lastError == tr("Operation cancelled"))
                record.lastError.clear();
            if (const auto error = store_->write(record))
                qCWarning(logDraftPersistence) << "Failed to preserve draft while storage shuts down:" << error.message;
            else
                ++affected;
            continue;
        }
        if (record.operation == DraftRecord::Delete) {
            record.state     = DraftRecord::Retry;
            record.lastError = tr("The storage plugin was disabled; deletion is still pending");
            record.retryAt   = {};
            if (!store_->write(record))
                ++affected;
            continue;
        }
        if (record.state != DraftRecord::Editing)
            record.state = DraftRecord::NeedsRouting;
        record.storageId.clear();
        record.remoteNoteId.clear();
        record.lastError = tr("The storage plugin was disabled; the remote original was left unchanged");
        record.retryAt   = {};
        if (!store_->write(record))
            ++affected;
    }
    if (affected) {
        emit draftsChanged();
        if (shuttingDown_)
            return;
        emit publicationAbandoned(
            tr("%n note(s) could not be published because storage “%1” was disabled. The remote originals were "
               "left unchanged; local drafts will be routed as new notes.",
               nullptr, affected)
                .arg(storage->name()));
    }
}

} // namespace AnyKeep
