#include "xmppstorage.h"

#include "private.h"

#include "fileremotecachestore.h"
#include "foldercatalogmanager.h"
#include "localdatakeystore.h"
#include "notedata.h"
#include "remotecachestore.h"
#include "utils.h"
#include "xmppbackend.h"

#include <QCryptographicHash>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace AnyKeep {

using namespace XmppStoragePrivate;

Note XmppStorage::fromRemote(const XmppRemoteNote &remote)
{
    Note note(new NoteData(this));
    applyRemote(note, remote);
    return note;
}

void XmppStorage::applyRemote(Note &note, const XmppRemoteNote &remote)
{
    note.setId(remote.id);
    note.setTitle(remote.title);
    note.setFormat(Note::Markdown);
    note.setLastChangeUTC(remote.modified);
    note.setBackendValue(QStringLiteral("revision"), remote.revision);
    note.setBackendValue(ContentRevisionBackendKey, remote.contentRevision);
    note.setBackendValue(QStringLiteral("parentRevision"), remote.parentRevision);
    note.setBackendValue(QStringLiteral("originId"), remote.originId);
    note.setBackendValue(IndexRecordTemplateKey, remote.indexRecordTemplate);
    note.setBackendValue(ContentRecordTemplateKey, remote.contentRecordTemplate);
    note.setBackendValue(FolderPathBackendKey, remote.folderPath);
    note.setFavorite(remote.favorite);
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
        note.setFolderId(folderCatalogManager_->catalog().folderForNote(systemName(), remote.id));
    } else {
        note.setFolderId({});
    }
    if (remote.contentPresent)
        note.setText(remote.content, Note::Markdown);
    else
        note.unload();
    note.setTags(remote.tags);
    QList<MediaReference> media;
    media.reserve(remote.media.size());
    for (const auto &remoteMedia : remote.media) {
        auto reference = remoteMedia.reference;
        if (!remoteMedia.fileSharingXml.isEmpty())
            reference.remoteData.insert(QStringLiteral("xmpp.sfs"), remoteMedia.fileSharingXml);
        media.append(std::move(reference));
    }
    note.setMedia(media);
}

bool XmppStorage::folderPathForFolder(const QUuid &folderId, QStringList *path, QString *error) const
{
    if (path)
        path->clear();
    if (error)
        error->clear();
    if (folderId.isNull())
        return true;
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable()) {
        if (error)
            *error = tr("The folder catalog is unavailable");
        return false;
    }

    const auto folderPath = folderCatalogManager_->catalog().pathForFolder(folderId);
    if (folderPath.isEmpty()) {
        if (error)
            *error = tr("The selected folder no longer exists");
        return false;
    }
    if (path)
        *path = folderPath;
    return true;
}

bool XmppStorage::folderPathForNote(const Note &note, QStringList *path, QString *error) const
{
    if (!note.folderId().isNull())
        return folderPathForFolder(note.folderId(), path, error);

    if (path)
        path->clear();
    if (error)
        error->clear();
    if (note.id().isEmpty())
        return true;
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
        const auto assignedFolder = folderCatalogManager_->catalog().folderForNote(systemName(), note.id());
        if (!assignedFolder.isNull())
            return folderPathForFolder(assignedFolder, path, error);
        const auto *assignment = folderCatalogManager_->catalog().assignment(systemName(), note.id());
        if (assignment && assignment->tombstone)
            return true;
    }

    if (path)
        *path = note.backendValue(FolderPathBackendKey).toStringList();
    return true;
}

bool XmppStorage::toRemote(const Note &note, XmppRemoteNote *remote, QString *error) const
{
    if (!remote)
        return false;

    XmppRemoteNote result;
    result.id              = note.id();
    result.revision        = note.backendValue(QStringLiteral("revision")).toString();
    result.contentRevision = note.backendValue(ContentRevisionBackendKey).toString();
    result.parentRevision  = note.backendValue(QStringLiteral("parentRevision")).toString();
    result.originId        = note.backendValue(QStringLiteral("originId")).toString();
    result.title           = note.title();
    result.content         = note.text();
    result.modified        = note.lastChangeUTC();
    const auto requestedModified
        = note.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).toDateTime();
    result.preserveModified = requestedModified.isValid();
    if (result.preserveModified)
        result.modified = requestedModified;
    result.format   = QStringLiteral("markdown");
    result.tags     = note.tags();
    result.favorite = note.isFavorite();
    for (const auto &reference : note.media()) {
        XmppRemoteMedia media;
        media.reference      = reference;
        media.fileSharingXml = reference.remoteData.value(QStringLiteral("xmpp.sfs")).toByteArray();
        result.media.append(std::move(media));
    }
    result.contentPresent        = note.isLoaded();
    result.indexRecordTemplate   = note.backendValue(IndexRecordTemplateKey).toByteArray();
    result.contentRecordTemplate = note.backendValue(ContentRecordTemplateKey).toByteArray();
    if (!folderPathForNote(note, &result.folderPath, error))
        return false;
    *remote = std::move(result);
    return true;
}

void XmppStorage::reconcileRemoteFolders(const QList<XmppRemoteNote> &notes)
{
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return;

    QList<ProviderFolderPathAssignment> assignments;
    assignments.reserve(notes.size());
    for (const auto &remote : notes) {
        if (remote.id.isEmpty())
            continue;
        ProviderFolderPathAssignment assignment;
        assignment.noteId     = remote.id;
        assignment.path       = remote.folderPath;
        assignment.modifiedAt = remote.modified;
        assignments.append(std::move(assignment));
    }
    if (assignments.isEmpty())
        return;
    if (const auto result = folderCatalogManager_->reconcileProviderFolderPaths(systemName(), assignments))
        reportError(tr("Could not merge XMPP folders: %1").arg(result.message));
}

void XmppStorage::reconcileCachedFolders()
{
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return;

    QList<XmppRemoteNote> notes;
    notes.reserve(cache_.size());
    for (const auto &cached : std::as_const(cache_)) {
        if (!cached.backendData().contains(FolderPathBackendKey))
            continue;
        XmppRemoteNote remote;
        remote.id         = cached.id();
        remote.modified   = cached.lastChangeUTC();
        remote.folderPath = cached.backendValue(FolderPathBackendKey).toStringList();
        notes.append(std::move(remote));
    }
    reconcileRemoteFolders(notes);

    bool changed = false;
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        const auto folderId = folderCatalogManager_->catalog().folderForNote(systemName(), it.key());
        if (it.value().folderId() == folderId)
            continue;
        it.value().setFolderId(folderId);
        changed = true;
    }
    if (changed)
        persistCache();
}

void XmppStorage::scheduleFolderPathSynchronization()
{
    if (folderPathUpdateScheduled_ || shuttingDown_ || errorState_ || !accessible_ || !folderCatalogManager_
        || !folderCatalogManager_->isAvailable()) {
        return;
    }
    folderPathUpdateScheduled_ = true;
    QTimer::singleShot(0, this, [this]() {
        folderPathUpdateScheduled_ = false;
        enqueueFolderPathUpdates();
    });
}

void XmppStorage::enqueueFolderPathUpdates()
{
    if (shuttingDown_ || errorState_ || !accessible_ || !folderCatalogManager_
        || !folderCatalogManager_->isAvailable()) {
        return;
    }

    const auto &catalog = folderCatalogManager_->catalog();
    for (const auto &cached : std::as_const(cache_)) {
        if (cached.isNull() || cached.id().isEmpty() || folderPathUpdateInFlight_.contains(cached.id()))
            continue;

        Note desired = cached;
        if (const auto *assignment = catalog.assignment(systemName(), cached.id()))
            desired.setFolderId(assignment->tombstone ? QUuid {} : assignment->folderId);
        else
            desired.setFolderId({});

        QStringList path;
        QString     error;
        if (!folderPathForNote(desired, &path, &error)) {
            reportError(tr("Could not resolve the XMPP folder path for a note: %1").arg(error));
            continue;
        }
        if (path == cached.backendValue(FolderPathBackendKey).toStringList())
            continue;
        if (!folderPathUpdateQueued_.contains(cached.id())) {
            folderPathUpdateQueued_.insert(cached.id());
            folderPathUpdateQueue_.append(cached.id());
        }
    }
    publishNextFolderPathUpdate();
}

void XmppStorage::publishNextFolderPathUpdate()
{
    if (folderPathUpdateRunning_ || shuttingDown_ || errorState_ || !accessible_)
        return;

    while (!folderPathUpdateQueue_.isEmpty()) {
        const auto noteId = folderPathUpdateQueue_.takeFirst();
        folderPathUpdateQueued_.remove(noteId);
        if (folderPathUpdateInFlight_.contains(noteId))
            continue;

        const auto cached = cache_.value(noteId);
        if (cached.isNull())
            continue;

        Note desired = cached;
        if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
            if (const auto *assignment = folderCatalogManager_->catalog().assignment(systemName(), noteId))
                desired.setFolderId(assignment->tombstone ? QUuid {} : assignment->folderId);
            else
                desired.setFolderId({});
        }

        QStringList path;
        QString     error;
        if (!folderPathForNote(desired, &path, &error)) {
            reportError(tr("Could not resolve the XMPP folder path for a note: %1").arg(error));
            continue;
        }
        if (path == cached.backendValue(FolderPathBackendKey).toStringList())
            continue;

        XmppRemoteNote local;
        if (!toRemote(desired, &local, &error)) {
            reportError(tr("Could not prepare an XMPP folder update: %1").arg(error));
            continue;
        }
        if (local.revision.isEmpty())
            continue;
        local.folderPath = std::move(path);

        folderPathUpdateRunning_ = true;
        folderPathUpdateInFlight_.insert(noteId);
        const auto config = config_;
        const auto epoch  = configEpoch_;
        QMetaObject::invokeMethod(
            backend_,
            [this, config, local = std::move(local), noteId, epoch]() {
                if (shuttingDown_ || epoch != configEpoch_)
                    return;
                backend_->setConfig(config);
                backend_->updateNoteIndexAsync(local, [this, noteId, epoch](XmppNoteResult result) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, noteId, epoch, result = std::move(result)]() {
                            folderPathUpdateInFlight_.remove(noteId);
                            folderPathUpdateRunning_ = false;
                            if (shuttingDown_ || epoch != configEpoch_)
                                return;

                            if (!result.ok) {
                                if (result.remoteOnConflict) {
                                    reconcileRemoteFolders({ *result.remoteOnConflict });
                                    cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                                    persistCache();
                                }
                                if (result.retryable())
                                    handleTransientFailure(result.error, false);
                                else
                                    reportError(
                                        tr("Could not synchronize an XMPP folder change: %1").arg(result.error));
                            } else {
                                reconcileRemoteFolders({ result.note });
                                auto       changed  = fromRemote(result.note);
                                const auto previous = cache_.value(noteId);
                                if (!previous.isNull() && previous.isLoaded()) {
                                    changed.setText(previous.text(), previous.format());
                                    changed.setMedia(previous.media());
                                }
                                cache_.insert(noteId, changed);
                                cacheValid_ = accessible_ = true;
                                persistCache();
                                emit noteModified(changed);
                            }

                            QTimer::singleShot(0, this, &XmppStorage::publishNextFolderPathUpdate);
                        },
                        Qt::QueuedConnection);
                });
            },
            Qt::QueuedConnection);
        return;
    }
}

bool XmppStorage::openPersistentCache(const XmppConfig &config)
{
    if (config.instanceId.isEmpty())
        return false;
    const auto nodeHash
        = QCryptographicHash::hash(config.nodeName.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    const auto cacheScope = config.instanceId + QLatin1Char(':') + QString::fromLatin1(nodeHash);
    if (!persistentCache_ || persistentCacheInstanceId_ != cacheScope) {
        QString keyError;
        auto    localKey = LocalDataKeyStore::loadOrCreateMasterKey(&keyError);
        if (localKey.isEmpty()) {
            reportError(tr("The local note cache could not be opened: %1").arg(keyError));
            return false;
        }
        const auto path = Utils::anykeepDataDir() + QStringLiteral("/remote-cache/xmpppubsub/") + config.instanceId
            + QLatin1Char('-') + QString::fromLatin1(nodeHash) + QStringLiteral(".cache");
        persistentCache_           = std::make_unique<FileRemoteCacheStore>(path, cacheScope, std::move(localKey));
        persistentCacheInstanceId_ = cacheScope;
    }

    const auto records = persistentCache_->records();
    if (!records) {
        reportError(tr("The local note cache could not be read: %1").arg(records.error.message));
        return false;
    }
    cache_.clear();
    for (const auto &record : records.value) {
        Note note(new NoteData(this));
        note.setId(record.id);
        note.setTitle(record.title);
        note.setFormat(record.format);
        note.setLastChangeUTC(record.modified);
        note.setFolderId(record.folderId);
        note.setBackendData(record.backendData);
        note.setMedia(record.media);
        if (record.bodyPresent)
            note.setText(record.body, record.format);
        else
            note.unload();
        note.setTags(record.tags);
        cache_.insert(record.id, note);
    }
    cacheAvailable_ = !records.value.isEmpty();
    cacheValid_     = cacheAvailable_;
    reconcileCachedFolders();
    return true;
}

void XmppStorage::persistCache()
{
    if (!persistentCache_)
        return;
    QList<RemoteCacheRecord> records;
    records.reserve(cache_.size());
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &note : std::as_const(cache_)) {
        RemoteCacheRecord record;
        record.id          = note.id();
        record.title       = note.title();
        record.tags        = note.tags();
        record.modified    = note.lastChangeUTC();
        record.format      = note.format();
        record.body        = note.text();
        record.bodyPresent = note.isLoaded();
        record.folderId    = note.folderId();
        record.backendData = note.backendData();
        record.media       = note.media();
        record.syncState   = RemoteCacheRecord::Synced;
        record.cachedAt    = now;
        records.append(std::move(record));
    }
    if (const auto error = persistentCache_->replaceRecords(records)) {
        reportError(tr("The local note cache could not be written: %1").arg(error.message));
        return;
    }
    cacheAvailable_ = !records.isEmpty();
}

void XmppStorage::startBodyPrefetch(const QStringList &ids)
{
    // Iris media hydration may download large attachments. Do not turn the
    // existing body cache warmer into an implicit download-all-media job.
    if (backend_ && backend_->supportsMedia())
        return;
    for (const auto &id : ids) {
        if (!id.isEmpty() && !bodyPrefetchQueue_.contains(id))
            bodyPrefetchQueue_.append(id);
    }
    prefetchNextBody();
}

void XmppStorage::prefetchNextBody()
{
    if (bodyPrefetchRunning_ || bodyPrefetchQueue_.isEmpty() || shuttingDown_ || !accessible_)
        return;
    bodyPrefetchRunning_ = true;
    const auto id        = bodyPrefetchQueue_.takeFirst();
    auto      *job       = loadNoteAsync(id, this);
    const auto finish    = [this, job]() {
        bodyPrefetchRunning_ = false;
        job->deleteLater();
        QTimer::singleShot(0, this, &XmppStorage::prefetchNextBody);
    };
    connect(job, &StorageJob::finished, this, finish);
    if (job->isFinished())
        finish();
}

} // namespace AnyKeep
