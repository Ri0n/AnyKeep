#include "backuprestorepreviewjob.h"

#include "draftmanager.h"
#include "notedata.h"
#include "notetransfercontroller.h"

#include <QHash>
#include <QMetaObject>
#include <QSet>

#include <utility>

namespace AnyKeep {
namespace {
    Note draftNote(const DraftRecord &record)
    {
        Note note(new NoteData(nullptr));
        note.setId(record.remoteNoteId);
        note.setTitle(record.title);
        note.setText(record.body, record.format);
        note.setTags(record.tags);
        note.setFolderId(record.folderId);
        note.setFavorite(record.backendData.value(QString::fromLatin1(FavoriteBackendKey)).toBool());
        note.setMedia(record.media);
        note.setLastChangeUTC(record.updatedAt);
        return note;
    }

    Note detachedNote(const Note &source)
    {
        Note note(new NoteData(nullptr));
        note.setId(source.id());
        note.setTitle(source.title());
        note.setText(source.text(), source.format());
        note.setTags(source.tags());
        note.setFolderId(source.folderId());
        note.setFavorite(source.isFavorite());
        note.setMedia(source.media());
        note.setLastChangeUTC(source.lastChangeUTC());
        return note;
    }

    bool identical(const Note &backup, const Note &current)
    {
        if (backup.isNull() || current.isNull() || !current.isLoaded()
            || backup.folderId() != current.folderId() || backup.isFavorite() != current.isFavorite())
            return false;
        auto backupTags = backup.tags();
        auto currentTags = current.tags();
        backupTags.sort();
        currentTags.sort();
        if (backupTags != currentTags)
            return false;

        auto title = NoteTransferController::convertTextFormat(current.title(), current.format(), backup.format());
        auto body = NoteTransferController::convertTextFormat(current.text(), current.format(), backup.format());
        const auto backupMedia = backup.media();
        const auto currentMedia = current.media();
        if (backupMedia.size() != currentMedia.size())
            return false;
        QHash<QUuid, MediaReference> byId;
        for (const auto &item : currentMedia) {
            if (!item.isValid() || byId.contains(item.id))
                return false;
            byId.insert(item.id, item);
        }
        QSet<QUuid> seen;
        for (const auto &item : backupMedia) {
            const auto found = byId.constFind(item.id);
            // Do not infer equality from names, size or timestamps alone.
            if (!item.isValid() || seen.contains(item.id) || found == byId.cend()
                || item.checksum.size() != 32 || found->checksum != item.checksum
                || found->size != item.size || found->originalName != item.originalName
                || found->mediaType != item.mediaType)
                return false;
            seen.insert(item.id);
            title.replace(found->uri(), item.uri());
            body.replace(found->uri(), item.uri());
        }
        return backup.title() == title && backup.text() == body;
    }
} // namespace

BackupRestorePreviewJob::BackupRestorePreviewJob(QObject *parent) : StorageJob(parent) {}

BackupRestorePreviewJob::~BackupRestorePreviewJob() { stopRead(); }

BackupRestorePreviewJob *BackupRestorePreviewJob::compare(
    const QList<PortableBackupStorage> &storages, const QMap<QString, NoteStorage::Ptr> &providers,
    DraftManager &drafts, const FolderCatalogSnapshot &folders, QObject *owner)
{
    auto *job = new BackupRestorePreviewJob(owner);
    job->storages_ = storages;
    job->providers_ = providers;
    job->drafts_ = &drafts;
    job->start();
    if (const auto error = job->folders_.replaceSnapshot(folders)) {
        job->fail({ StorageError::Other, error.message, false });
        return job;
    }
    // Capture the current local editing state once and index both sides of
    // pending transfers. Avoid decrypting all drafts for each backup note.
    const auto records = drafts.backupRecords();
    if (!records) {
        job->fail({ StorageError::Io, records.error.message, false });
        return job;
    }
    job->currentDrafts_ = records.value;
    for (qsizetype i = 0; i < records.value.size(); ++i) {
        const auto &record = records.value.at(i);
        job->draftById_.insert(record.id, i);
        if (!record.storageId.isEmpty() && !record.remoteNoteId.isEmpty())
            job->draftsByOrigin_.insert(qMakePair(record.storageId, record.remoteNoteId), i);
        if (!record.removeSourceStorageId.isEmpty() && !record.removeSourceNoteId.isEmpty())
            job->draftsByOrigin_.insert(qMakePair(record.removeSourceStorageId, record.removeSourceNoteId), i);
    }
    for (qsizetype i = 0; i < storages.size(); ++i) {
        const auto &storage = storages.at(i);
        QHash<QUuid, PortableBackupDraftInfo> draftOrigins;
        for (const auto &draft : storage.drafts)
            draftOrigins.insert(draft.id, draft);
        for (qsizetype n = 0; n < storage.notes.size(); ++n) {
            const auto &entry = storage.notes.at(n);
            BackupRestoreDecision decision;
            decision.storageIndex = i;
            decision.noteIndex = n;
            if (entry.draftId.isNull()) {
                decision.originStorageId = storage.storageId;
                decision.originNoteId = entry.sourceNoteId;
            } else {
                const auto origin = draftOrigins.value(entry.draftId);
                decision.originStorageId = origin.storageId;
                decision.originNoteId = origin.noteId;
            }
            if (providers.value(decision.originStorageId))
                decision.destinationStorageId = decision.originStorageId;
            job->decisions_.append(std::move(decision));
        }
    }
    connect(&drafts, &QObject::destroyed, job, [job] {
        if (!job->isFinished()) {
            job->stopRead();
            job->fail({ StorageError::Unavailable, tr("The draft manager became unavailable"), false });
        }
    });
    QMetaObject::invokeMethod(job, &BackupRestorePreviewJob::next, Qt::QueuedConnection);
    return job;
}

void BackupRestorePreviewJob::stopRead()
{
    disconnect(providerDestroyed_);
    providerDestroyed_ = {};
    if (auto *job = active_.data()) {
        active_.clear();
        disconnect(job, nullptr, this, nullptr);
        if (!job->isFinished())
            job->cancel();
        job->deleteLater();
    }
}

void BackupRestorePreviewJob::cancel()
{
    if (!isFinished()) {
        stopRead();
        StorageJob::cancel();
    }
}

void BackupRestorePreviewJob::finishEntry(BackupRestoreDecision::Status status, const QString &message)
{
    if (isFinished())
        return;
    auto &decision = decisions_[index_];
    decision.status = status;
    decision.message = message;
    ++index_;
    QMetaObject::invokeMethod(this, &BackupRestorePreviewJob::next, Qt::QueuedConnection);
}

void BackupRestorePreviewJob::conflict(const Note &current, const QString &message)
{
    if (!drafts_) {
        fail({ StorageError::Unavailable, tr("The draft manager is unavailable"), false });
        return;
    }
    const auto &decision = decisions_.at(index_);
    const auto &entry = storages_.at(decision.storageIndex).notes.at(decision.noteIndex);
    const auto &note = entry.snapshot;
    DraftRecord record;
    record.id = entry.draftId;
    record.storageId = decision.originStorageId;
    record.remoteNoteId = decision.originNoteId;
    record.title = note.title();
    record.body = note.text();
    record.format = note.format();
    record.tags = note.tags();
    record.folderId = note.folderId();
    record.media = note.media();
    record.updatedAt = note.lastChangeUTC();
    record.backendData.insert(QString::fromLatin1(FavoriteBackendKey), note.isFavorite());
    // The normal resolver supplies a recommendation only. In particular
    // CreateCopy must not mark a draft Ready or schedule publication here.
    const QPointer<BackupRestorePreviewJob> guard(this);
    drafts_->previewRecoveryConflict({ record, current, message },
        [guard, expected = index_, message](ConflictResolution resolution) {
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard, [guard, expected, message, resolution = std::move(resolution)] {
                if (!guard || guard->isFinished() || guard->index_ != expected)
                    return;
                guard->decisions_[expected].resolution = resolution;
                guard->finishEntry(BackupRestoreDecision::Conflict, message);
            }, Qt::QueuedConnection);
        });
}

void BackupRestorePreviewJob::next()
{
    if (isFinished())
        return;
    emit progressChanged(int(index_), int(decisions_.size()));
    if (isFinished())
        return;
    if (index_ >= decisions_.size()) {
        complete();
        return;
    }
    const auto &decision = decisions_.at(index_);
    const auto &entry = storages_.at(decision.storageIndex).notes.at(decision.noteIndex);
    if (!drafts_) {
        fail({ StorageError::Unavailable, tr("The draft manager is unavailable"), false });
        return;
    }
    // A preview observes current state; selection/commit must revalidate it.
    // Never treat a pending local change or deletion as an absent remote object.
    qsizetype localIndex = entry.draftId.isNull() ? -1 : draftById_.value(entry.draftId, -1);
    if (localIndex < 0 && !decision.originStorageId.isEmpty() && !decision.originNoteId.isEmpty())
        localIndex = draftsByOrigin_.value(qMakePair(decision.originStorageId, decision.originNoteId), -1);
    if (localIndex >= 0) {
        const auto &record = currentDrafts_.at(localIndex);
        conflict(record.operation == DraftRecord::Publish ? draftNote(record) : Note {},
                 tr("A current local editing or deletion intent refers to this note"));
        return;
    }
    if (decision.originStorageId.isEmpty()) {
        finishEntry(BackupRestoreDecision::Unrouted, tr("No original storage was recorded"));
        return;
    }
    const auto storage = providers_.value(decision.originStorageId);
    if (!storage) {
        finishEntry(BackupRestoreDecision::ProviderMissing, tr("The original storage is not configured"));
        return;
    }
    if (decision.originNoteId.isEmpty()) {
        finishEntry(BackupRestoreDecision::Unrouted, tr("The original draft was not published"));
        return;
    }
    const auto online = [](const NoteStorage *provider) {
        return provider->isAccessible()
            && (provider->connectivityState() == NoteStorage::ConnectivityState::Online
                || provider->connectivityState() == NoteStorage::ConnectivityState::NotApplicable);
    };
    if (!online(storage.data())) {
        finishEntry(BackupRestoreDecision::Unavailable, tr("The original storage is unavailable"));
        return;
    }

    const auto expected = index_;
    auto *job = storage->loadNoteAsync(decision.originNoteId, this);
    active_ = job;
    providerDestroyed_ = connect(storage.data(), &QObject::destroyed, this, [this, expected] {
        if (isFinished() || expected != index_)
            return;
        stopRead();
        finishEntry(BackupRestoreDecision::Unavailable, tr("The original storage was removed"));
    });
    const QPointer<NoteLoadJob> guard(job);
    const auto dispatch = [this, guard, storage, expected, online] {
        QMetaObject::invokeMethod(this, [this, guard, storage, expected, online] {
            if (isFinished() || expected != index_ || !guard || active_ != guard)
                return;
            const auto state = guard->state();
            const auto error = guard->error();
            const Note loaded = guard->result();
            const auto &decision = decisions_.at(index_);
            const auto &entry = storages_.at(decision.storageIndex).notes.at(decision.noteIndex);
            const bool valid = state == Succeeded && !loaded.isNull()
                && loaded.isLoaded() && loaded.id() == decision.originNoteId;
            Note current;
            if (valid)
                current = detachedNote(loaded);
            stopRead();
            if (!storage || !online(storage.data())) {
                finishEntry(BackupRestoreDecision::Unavailable, tr("The original storage became unavailable"));
                return;
            }
            if (state == Failed && error.code == StorageError::NotFound) {
                finishEntry(BackupRestoreDecision::NoteMissing, tr("The original note was not found"));
                return;
            }
            if (!valid) {
                finishEntry(BackupRestoreDecision::Unavailable,
                            error.message.isEmpty() ? tr("Could not compare the original note") : error.message);
                return;
            }
            if (const auto *assignment = folders_.assignment(decision.originStorageId, decision.originNoteId))
                current.setFolderId(assignment->tombstone ? QUuid {} : assignment->folderId);
            if (identical(entry.snapshot, current)) {
                finishEntry(BackupRestoreDecision::Identical);
                return;
            }
            conflict(current, tr("The backup and the current note contain different versions"));
        }, Qt::QueuedConnection);
    };
    connect(job, &StorageJob::finished, this, dispatch);
    if (job->isFinished())
        dispatch();
}

} // namespace AnyKeep
