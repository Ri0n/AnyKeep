#include "backupsnapshotjob.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "notedata.h"
#include "notemanager.h"

#include <QMetaObject>
#include <QSet>

#include <utility>

namespace AnyKeep {
namespace {
    Note detachedSnapshot(const Note &source)
    {
        Note note(new NoteData(nullptr));
        note.setId(source.id());
        note.setTitle(source.title());
        note.setText(source.text(), source.format());
        note.setTags(source.tags());
        note.setFolderId(source.folderId());
        note.setFavorite(source.isFavorite());
        note.setLastChangeUTC(source.lastChangeUTC());
        note.setMedia(source.media());
        return note;
    }

    Note draftSnapshot(const DraftRecord &record)
    {
        Note note(new NoteData(nullptr));
        note.setId(record.remoteNoteId);
        note.setTitle(record.title);
        note.setText(record.body, record.format);
        note.setTags(record.tags);
        note.setFolderId(record.folderId);
        note.setFavorite(record.backendData.value(QString::fromLatin1(FavoriteBackendKey)).toBool());
        note.setLastChangeUTC(record.updatedAt);
        note.setMedia(record.media);
        return note;
    }
} // namespace

BackupSnapshotJob::BackupSnapshotJob(QObject *parent) : StorageJob(parent) {}

BackupSnapshotJob *BackupSnapshotJob::collect(NoteManager &notes, DraftManager &drafts,
                                               FolderCatalogManager &folders, QObject *owner)
{
    auto *job = new BackupSnapshotJob(owner);
    job->useLiveFolders_ = true;
    job->folderManager_ = &folders;
    job->start();
    if (!folders.isAvailable()) {
        job->fail({ StorageError::Unavailable, tr("The folder catalog is unavailable"), false });
        return job;
    }
    job->result_.folders = folders.snapshot();
    job->begin(notes.storages(true), drafts);
    return job;
}

BackupSnapshotJob *BackupSnapshotJob::collect(const QMap<QString, NoteStorage::Ptr> &providers, DraftManager &drafts,
                                               const FolderCatalogSnapshot &folders, QObject *owner)
{
    auto *job = new BackupSnapshotJob(owner);
    job->start();
    job->result_.folders = folders;
    job->begin(providers, drafts);
    return job;
}

void BackupSnapshotJob::begin(const QMap<QString, NoteStorage::Ptr> &providers, DraftManager &drafts)
{
    const auto records = drafts.backupRecords();
    if (!records) {
        fail({ StorageError::Io, tr("Could not read the local draft snapshots: %1").arg(records.error.message), false });
        return;
    }

    for (auto it = providers.cbegin(); it != providers.cend(); ++it) {
        PortableBackupStorage source;
        source.storageId = it.key();
        if (it.value()) {
            source.displayName = it.value()->name();
            source.remote = it.value()->connectivityState() != NoteStorage::ConnectivityState::NotApplicable;
        }
        result_.storages.append(std::move(source));
        providers_.append(it.value());
    }

    PortableBackupStorage draftGroup;
    draftGroup.storageId = DraftManager::draftsStorageId();
    draftGroup.displayName = tr("Local drafts");
    draftGroup.complete = true;
    for (const auto &record : records.value) {
        draftGroup.drafts.append({ record.id, record.storageId, record.remoteNoteId, record.operation, record.state,
                                   record.removeSourceStorageId, record.removeSourceNoteId, record.revision,
                                   record.updatedAt });
        if (record.operation == DraftRecord::Publish)
            draftGroup.notes.append({ draftSnapshot(record), record.remoteNoteId, record.id });
    }
    result_.storages.append(std::move(draftGroup));

    // Collection is a sequence of asynchronous storage jobs, never a nested
    // event loop or a blocking remote call.
    QMetaObject::invokeMethod(this, [this] { nextProvider(); }, Qt::QueuedConnection);
}

void BackupSnapshotJob::cancel()
{
    if (isFinished())
        return;
    if (auto *job = activeJob_.data()) {
        activeJob_.clear();
        disconnect(job, nullptr, this, nullptr);
        job->cancel();
        job->deleteLater();
    }
    StorageJob::cancel();
}

void BackupSnapshotJob::observe(StorageJob *job, std::function<void()> completion)
{
    activeJob_ = job;
    const QPointer<StorageJob> guard(job);
    const auto dispatch = [this, guard, completion = std::move(completion)] {
        QMetaObject::invokeMethod(this, [this, guard, completion] {
            if (isFinished() || !guard || activeJob_ != guard)
                return;
            activeJob_.clear();
            completion();
        }, Qt::QueuedConnection);
    };
    connect(job, &StorageJob::finished, this, dispatch);
    if (job->isFinished())
        dispatch();
}

void BackupSnapshotJob::nextProvider()
{
    if (isFinished())
        return;
    ++providerIndex_;
    emit progressChanged(int(providerIndex_), int(providers_.size()));
    if (isFinished())
        return;
    if (providerIndex_ >= providers_.size()) {
        finishCollection();
        return;
    }

    const auto storage = providers_.at(providerIndex_);
    if (!storage) {
        QMetaObject::invokeMethod(this, [this] { nextProvider(); }, Qt::QueuedConnection);
        return;
    }
    const auto index = providerIndex_;
    // A provider can be unregistered while a list or load is outstanding.
    // Cancel only our own read job, never a provider's other clients.
    connect(storage.data(), &QObject::destroyed, this, [this, index] {
        if (isFinished())
            return;
        result_.storages[index].complete = false;
        if (providerIndex_ == index && activeJob_ && !activeJob_->isFinished())
            activeJob_->cancel();
    });

    auto *job = storage->refreshNotesAsync(0, this);
    observe(job, [this, job, storage, index] {
        auto &source = result_.storages[index];
        summaries_.clear();
        noteIndex_ = 0;
        if (job->state() == StorageJob::Succeeded && storage) {
            summaries_ = job->result();
            source.complete = storage->isAccessible()
                && (!source.remote || storage->connectivityState() == NoteStorage::ConnectivityState::Online);
        }
        // A failed asynchronous list does not authorize a synchronous
        // network fallback. Providers exposing an offline cache return it
        // through their asynchronous list API.
        job->deleteLater();

        if (storage) {
            const auto changed = [this, index] {
                if (!isFinished())
                    result_.storages[index].complete = false;
            };
            connect(storage.data(), &NoteStorage::noteAdded, this, changed);
            connect(storage.data(), &NoteStorage::noteModified, this, changed);
            connect(storage.data(), &NoteStorage::noteRemoved, this, changed);
            connect(storage.data(), &NoteStorage::noteIdChanged, this, changed);
            connect(storage.data(), &NoteStorage::invalidated, this, changed);
            connect(storage.data(), &NoteStorage::storageErorr, this, changed);
        }

        QSet<QString> seen;
        summaries_.removeIf([&](const Note &note) {
            if (note.isNull() || note.id().isEmpty() || seen.contains(note.id())) {
                source.complete = false;
                return true;
            }
            seen.insert(note.id());
            return false;
        });
        nextNote();
    });
}

void BackupSnapshotJob::captureNote(const Note &note, const QString &id)
{
    auto &source = result_.storages[providerIndex_];
    if (note.isNull() || !note.isLoaded() || note.id() != id) {
        source.omittedNoteIds.append(id);
        source.complete = false;
        return;
    }
    source.notes.append({ detachedSnapshot(note), id, {} });
}

void BackupSnapshotJob::nextNote()
{
    if (isFinished())
        return;
    if (noteIndex_ >= summaries_.size()) {
        summaries_.clear();
        nextProvider();
        return;
    }

    const Note summary = summaries_.at(noteIndex_++);
    const auto storage = providers_.at(providerIndex_);
    // Even a list entry with a body may not have resolved its media
    // references (e.g. Nextcloud native attachments). Use the provider's
    // load API, which owns that normalization.
    if (!storage) {
        captureNote({}, summary.id());
        QMetaObject::invokeMethod(this, [this] { nextNote(); }, Qt::QueuedConnection);
        return;
    }
    auto *job = storage->loadNoteAsync(summary.id(), this);
    observe(job, [this, job, summary] {
        if (job->state() == StorageJob::Succeeded) {
            if (summary.lastChangeUTC().isValid() && job->result().lastChangeUTC().isValid()
                && summary.lastChangeUTC() != job->result().lastChangeUTC())
                result_.storages[providerIndex_].complete = false;
            captureNote(job->result(), summary.id());
        } else {
            auto &source = result_.storages[providerIndex_];
            source.complete = false;
            // Keep readable cached content, but report the failed full load.
            source.omittedNoteIds.append(summary.id());
            if (summary.isLoaded())
                source.notes.append({ detachedSnapshot(summary), summary.id(), {} });
        }
        job->deleteLater();
        nextNote();
    });
}

void BackupSnapshotJob::finishCollection()
{
    if (useLiveFolders_) {
        if (!folderManager_ || !folderManager_->isAvailable()) {
            fail({ StorageError::Unavailable, tr("The folder catalog became unavailable"), false });
            return;
        }
        result_.folders = folderManager_->snapshot();
    }
    FolderCatalog catalog;
    if (const auto error = catalog.replaceSnapshot(result_.folders)) {
        fail({ StorageError::Other, error.message, false });
        return;
    }
    // Local overlays are authoritative for provider notes. Drafts retain
    // their own folder choice, including explicit unsorted state.
    for (auto &storage : result_.storages) {
        for (auto &entry : storage.notes) {
            if (!entry.draftId.isNull())
                continue;
            if (const auto *assignment = catalog.assignment(storage.storageId, entry.sourceNoteId))
                entry.snapshot.setFolderId(assignment->tombstone ? QUuid{} : assignment->folderId);
        }
    }
    complete();
}

} // namespace AnyKeep
