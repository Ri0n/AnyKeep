#include "xmppstorage.h"

#include "private.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "notedata.h"
#include "xmppbackend.h"

#include <QPointer>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <utility>

namespace AnyKeep {

using namespace XmppStoragePrivate;

class XmppStorage::ReorderOperation final : public QObject {
public:
    ReorderOperation(XmppStorage *storage, NoteReorderJob *job, QList<NoteReorderChange> changes) :
        QObject(job), storage_(storage), job_(job), changes_(std::move(changes)),
        config_(storage ? storage->config_ : XmppConfig {}), epoch_(storage ? storage->configEpoch_ : 0)
    {
    }

    void startNext()
    {
        if (!job_ || job_->isFinished())
            return;
        if (!storage_ || storage_->shuttingDown_ || epoch_ != storage_->configEpoch_) {
            job_->cancel();
            return;
        }
        if (next_ >= changes_.size()) {
            job_->complete();
            return;
        }

        const auto &change  = changes_.at(next_);
        Note        desired = change.note;
        desired.setLastChangeUTC(change.modified);
        desired.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), change.modified);

        XmppRemoteNote local;
        QString        conversionError;
        if (!storage_->toRemote(desired, &local, &conversionError)) {
            job_->fail({ StorageError::Other,
                         conversionError.isEmpty() ? tr("Could not prepare the reordered note index") : conversionError,
                         false });
            return;
        }
        if (local.id.isEmpty() || local.revision.isEmpty()) {
            job_->fail(
                { StorageError::NotFound, tr("The note selected for reordering is no longer available"), false });
            return;
        }

        auto                      *backend = storage_->backend_;
        QPointer<ReorderOperation> guard(this);
        QMetaObject::invokeMethod(
            backend,
            [backend, guard, config = config_, local = std::move(local)]() mutable {
                if (!guard)
                    return;
                backend->setConfig(config);
                backend->updateNoteIndexAsync(std::move(local), [guard](XmppNoteResult result) mutable {
                    if (!guard || !guard->storage_)
                        return;
                    QMetaObject::invokeMethod(
                        guard->storage_,
                        [guard, result = std::move(result)]() mutable {
                            if (guard)
                                guard->handleResult(std::move(result));
                        },
                        Qt::QueuedConnection);
                });
            },
            Qt::QueuedConnection);
    }

private:
    void handleResult(XmppNoteResult result)
    {
        if (!job_ || job_->isFinished())
            return;
        if (!storage_ || storage_->shuttingDown_ || epoch_ != storage_->configEpoch_) {
            job_->cancel();
            return;
        }

        const auto noteId = changes_.at(next_).note.id();
        if (!result.ok) {
            if (result.remoteOnConflict) {
                storage_->reconcileRemoteFolders({ *result.remoteOnConflict });
                auto       conflicting = storage_->fromRemote(*result.remoteOnConflict);
                const auto previous    = storage_->cache_.value(noteId);
                if (!previous.isNull() && previous.isLoaded() && !conflicting.isLoaded()) {
                    conflicting.setText(previous.text(), previous.format());
                    conflicting.setMedia(previous.media());
                }
                storage_->cache_.insert(result.remoteOnConflict->id, conflicting);
                storage_->persistCache();
            }
            job_->fail(storageError(result, result.conflict ? StorageError::Conflict : StorageError::Network));
            return;
        }
        if (result.note.id != noteId) {
            job_->fail(
                { StorageError::Other, tr("The XMPP server returned a different note while reordering"), false });
            return;
        }

        storage_->reconcileRemoteFolders({ result.note });
        auto       changed  = storage_->fromRemote(result.note);
        const auto previous = storage_->cache_.value(noteId);
        if (!previous.isNull() && previous.isLoaded()) {
            changed.setText(previous.text(), previous.format());
            changed.setMedia(previous.media());
        }
        storage_->cache_.insert(noteId, changed);
        storage_->cacheValid_ = storage_->accessible_ = true;
        storage_->persistCache();
        emit storage_->noteModified(changed);

        ++next_;
        QTimer::singleShot(0, this, [guard = QPointer<ReorderOperation>(this)]() {
            if (guard)
                guard->startNext();
        });
    }

    QPointer<XmppStorage>    storage_;
    QPointer<NoteReorderJob> job_;
    QList<NoteReorderChange> changes_;
    XmppConfig               config_;
    quint64                  epoch_ { 0 };
    qsizetype                next_ { 0 };
};

QList<Note> XmppStorage::noteList(int limit)
{
    auto notes = cache_.values();
    std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
    return limit > 0 ? notes.mid(0, limit) : notes;
}

NoteListJob *XmppStorage::refreshNotesAsync(int limit, QObject *owner)
{
    auto *job = new NoteListJob(owner ? owner : this);
    job->start();
    if (cacheValid_ || (cacheAvailable_ && !accessible_)) {
        auto notes = cache_.values();
        std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
        QPointer<NoteListJob> guard(job);
        QTimer::singleShot(0, this, [guard, notes = limit > 0 ? notes.mid(0, limit) : notes]() {
            if (guard && !guard->isFinished())
                guard->complete(notes);
        });
        return job;
    }
    if (errorState_) {
        job->fail({ StorageError::Unavailable, errorStateMessage_, false });
        return job;
    }

    if (refreshAttempt_ && refreshAttempt_->epoch == configEpoch_) {
        refreshAttempt_->waiters.append(RefreshWaiter { job, limit });
        return job;
    }

    const auto attempt = std::make_shared<RefreshAttempt>();
    attempt->epoch     = configEpoch_;
    attempt->waiters.append(RefreshWaiter { job, limit });
    refreshAttempt_ = attempt;

    const auto config = config_;
    const auto epoch  = attempt->epoch;
    QMetaObject::invokeMethod(
        backend_,
        [this, attempt, config, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                for (const auto &waiter : attempt->waiters) {
                    if (waiter.job && !waiter.job->isFinished())
                        waiter.job->cancel();
                }
                if (refreshAttempt_ == attempt)
                    refreshAttempt_.reset();
                return;
            }
            backend_->setConfig(config);
            backend_->listNotesAsync([this, attempt, epoch](XmppListResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, attempt, result = std::move(result), epoch]() {
                        if (refreshAttempt_ != attempt)
                            return;
                        refreshAttempt_.reset();
                        if (shuttingDown_ || epoch != configEpoch_) {
                            for (const auto &waiter : attempt->waiters) {
                                if (waiter.job && !waiter.job->isFinished())
                                    waiter.job->cancel();
                            }
                            return;
                        }
                        if (!result.ok) {
                            qWarning().noquote() << "XMPP index refresh failed:" << result.error;
                            if (result.retryable())
                                handleTransientFailure(result.error);
                            else
                                enterErrorState(result.error, true);
                            const auto error = storageError(result, StorageError::Network);
                            for (const auto &waiter : attempt->waiters) {
                                if (waiter.job && !waiter.job->isFinished())
                                    waiter.job->fail(error);
                            }
                            return;
                        }
                        reconcileRemoteFolders(result.notes);
                        QHash<QString, Note> refreshed = result.partial ? cache_ : QHash<QString, Note> {};
                        QStringList          missingBodies;
                        for (const auto &remote : result.notes) {
                            const auto old = cache_.constFind(remote.id);
                            if (old != cache_.cend()
                                && old.value().backendValue(QStringLiteral("revision")).toString() == remote.revision) {
                                auto cached = old.value();
                                if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
                                    cached.setFolderId(
                                        folderCatalogManager_->catalog().folderForNote(systemName(), remote.id));
                                }
                                refreshed.insert(remote.id, cached);
                                if (!old.value().isLoaded())
                                    missingBodies.append(remote.id);
                            } else {
                                refreshed.insert(remote.id, fromRemote(remote));
                                missingBodies.append(remote.id);
                            }
                        }
                        cache_      = std::move(refreshed);
                        cacheValid_ = accessible_ = true;
                        resetRetryBackoff();
                        persistCache();
                        scheduleFolderPathSynchronization();
                        auto notes = cache_.values();
                        std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
                        qInfo() << "XMPP index refresh loaded" << notes.size() << "note(s) for"
                                << attempt->waiters.size() << "caller(s)"
                                << (result.partial ? "from a partial remote result" : "from a complete remote result");
                        for (const auto &waiter : attempt->waiters) {
                            if (!waiter.job || waiter.job->isFinished())
                                continue;
                            waiter.job->complete(waiter.limit > 0 ? notes.mid(0, waiter.limit) : notes);
                        }
                        startBodyPrefetch(missingBodies);
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

void XmppStorage::cancelRefreshAttempt()
{
    const auto attempt = std::exchange(refreshAttempt_, std::shared_ptr<RefreshAttempt> {});
    if (!attempt)
        return;
    for (const auto &waiter : attempt->waiters) {
        if (waiter.job && !waiter.job->isFinished())
            waiter.job->cancel();
    }
}

Note XmppStorage::note(const QString &id) { return cache_.value(id); }

NoteLoadJob *XmppStorage::loadNoteAsync(const QString &id, QObject *owner)
{
    auto *job = new NoteLoadJob(owner ? owner : this);
    job->start();
    if (id.isEmpty()) {
        job->fail({ StorageError::NotFound, tr("Note was not found"), false });
        return job;
    }
    const auto cached = cache_.value(id);
    if (!cached.isNull() && cached.isLoaded()) {
        QPointer<NoteLoadJob> guard(job);
        QTimer::singleShot(0, this, [guard, cached]() {
            if (guard && !guard->isFinished())
                guard->complete(cached);
        });
        return job;
    }
    if (errorState_ || !accessible_) {
        job->fail({ StorageError::Unavailable,
                    errorState_ ? errorStateMessage_ : tr("The remote note body is not available offline."), false });
        return job;
    }
    const auto            config = config_;
    const auto            epoch  = configEpoch_;
    QPointer<NoteLoadJob> guard(job);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, id, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->getNoteAsync(id, [this, guard, id, epoch](XmppNoteResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), id, epoch]() {
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok) {
                            if (result.notFound) {
                                cache_.remove(id);
                                persistCache();
                            }
                            guard->fail(
                                storageError(result, result.notFound ? StorageError::NotFound : StorageError::Network));
                            return;
                        }
                        reconcileRemoteFolders({ result.note });
                        auto loaded = fromRemote(result.note);
                        cache_.insert(id, loaded);
                        accessible_ = true;
                        persistCache();
                        guard->complete(loaded);
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

Note XmppStorage::createNote()
{
    Note note(new NoteData(this));
    note.setText(QString(), Note::Markdown);
    note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
    return note;
}

bool XmppStorage::loadNote(Note &note)
{
    const QString id = note.id();
    if (id.isEmpty())
        return true;
    const auto cached = cache_.value(id);
    if (cached.isNull() || !cached.isLoaded())
        return false;
    note = cached;
    return true;
}

bool XmppStorage::saveNote(const Note &note)
{
    if (note.isNull() || note.storage() != this || !note.isLoaded())
        return false;
    const auto draftId = QUuid::createUuid();
    auto       error   = DraftManager::instance()->saveEditing(draftId, note, note.title(), note.text(), note.format());
    if (!error)
        error = DraftManager::instance()->markReady(draftId);
    return !error;
}

NoteSaveJob *XmppStorage::saveNoteAsync(const Note &note, QObject *owner)
{
    auto *job = new NoteSaveJob(owner ? owner : this);
    job->start();
    if (note.isNull() || note.storage() != this || !note.isLoaded() || errorState_) {
        job->fail({ StorageError::Other,
                    errorState_ ? errorStateMessage_ : tr("The note cannot be saved in its current state."), false });
        return job;
    }
    XmppRemoteNote local;
    QString        folderError;
    if (!toRemote(note, &local, &folderError)) {
        job->fail({ StorageError::Other, folderError, false });
        return job;
    }
    const auto            config = config_;
    const auto            epoch  = configEpoch_;
    const auto            oldId  = note.id();
    QPointer<NoteSaveJob> guard(job);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, local, oldId, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->saveNoteAsync(local, [this, guard, oldId, epoch](XmppNoteResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), oldId, epoch]() {
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok) {
                            auto error = storageError(result,
                                                      result.conflict ? StorageError::Conflict : StorageError::Network);
                            if (result.remoteOnConflict) {
                                reconcileRemoteFolders({ *result.remoteOnConflict });
                                cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                            }
                            guard->fail(error);
                            return;
                        }
                        reconcileRemoteFolders({ result.note });
                        auto       saved   = fromRemote(result.note);
                        const bool existed = !oldId.isEmpty() && cache_.contains(oldId);
                        if (!oldId.isEmpty() && oldId != saved.id())
                            cache_.remove(oldId);
                        cache_.insert(saved.id(), saved);
                        cacheValid_ = accessible_ = true;
                        persistCache();
                        guard->complete(saved);
                        if (!oldId.isEmpty() && oldId != saved.id())
                            emit noteIdChanged(saved, oldId);
                        if (existed || !oldId.isEmpty())
                            emit noteModified(saved);
                        else
                            emit noteAdded(saved);
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

NoteReorderJob *XmppStorage::reorderNotesAsync(const QStringList &noteIds, const QString &afterNoteId, QObject *owner)
{
    auto *job = new NoteReorderJob(owner ? owner : this);
    job->start();
    if (shuttingDown_ || errorState_ || !backend_) {
        job->fail({ StorageError::Unavailable, errorState_ ? errorStateMessage_ : tr("The XMPP storage is unavailable"),
                    false });
        return job;
    }

    StorageError error;
    auto         changes = noteReorderChanges(noteIds, afterNoteId, &error);
    if (error) {
        job->fail(error);
        return job;
    }
    if (changes.isEmpty()) {
        job->complete();
        return job;
    }

    auto *operation = new ReorderOperation(this, job, std::move(changes));
    QTimer::singleShot(0, operation, [operation]() { operation->startNext(); });
    return job;
}

NoteFolderChangeJob *XmppStorage::changeNoteFolderAsync(const Note &note, QObject *owner)
{
    auto *job = new NoteFolderChangeJob(owner ? owner : this);
    job->start();
    if (note.isNull() || note.storage() != this) {
        job->fail({ StorageError::Other, tr("Attempted to move a note owned by another storage."), false });
        return job;
    }
    if (note.id().isEmpty()) {
        job->fail({ StorageError::NotFound, tr("The note must be saved before it can be moved."), false });
        return job;
    }
    if (errorState_) {
        job->fail({ StorageError::Unavailable, errorStateMessage_, false });
        return job;
    }
    if (folderPathUpdateInFlight_.contains(note.id())) {
        job->fail({ StorageError::Network, tr("Another folder update for this note is already in progress."), true });
        return job;
    }

    QStringList folderPath;
    QString     folderError;
    if (!folderPathForFolder(note.folderId(), &folderPath, &folderError)) {
        job->fail({ StorageError::Other, folderError, false });
        return job;
    }

    XmppRemoteNote local;
    QString        remoteError;
    if (!toRemote(note, &local, &remoteError)) {
        job->fail({ StorageError::Other, remoteError, false });
        return job;
    }
    local.folderPath = std::move(folderPath);

    const auto                    config   = config_;
    const auto                    epoch    = configEpoch_;
    const auto                    noteId   = note.id();
    const auto                    folderId = note.folderId();
    QPointer<NoteFolderChangeJob> guard(job);
    folderPathUpdateInFlight_.insert(noteId);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, local, noteId, folderId, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->updateNoteIndexAsync(local, [this, guard, noteId, folderId, epoch](XmppNoteResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), noteId, folderId, epoch]() {
                        folderPathUpdateInFlight_.remove(noteId);
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok) {
                            if (result.remoteOnConflict) {
                                reconcileRemoteFolders({ *result.remoteOnConflict });
                                cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                                persistCache();
                            }
                            guard->fail(
                                storageError(result, result.conflict ? StorageError::Conflict : StorageError::Network));
                            return;
                        }

                        reconcileRemoteFolders({ result.note });
                        auto       changed = fromRemote(result.note);
                        const auto cached  = cache_.value(noteId);
                        if (!cached.isNull() && cached.isLoaded()) {
                            changed.setText(cached.text(), cached.format());
                            changed.setMedia(cached.media());
                        }
                        // The folder controller already recorded the local
                        // assignment before this request. Keep the summary
                        // aligned with that intent if a catalog merge is
                        // temporarily delayed by a timestamp tie.
                        if (changed.folderId() != folderId)
                            changed.setFolderId(folderId);
                        cache_.insert(noteId, changed);
                        cacheValid_ = accessible_ = true;
                        persistCache();
                        emit noteModified(changed);
                        guard->complete(changed);
                        scheduleFolderPathSynchronization();
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

void XmppStorage::removeNote(const QString &noteId)
{
    if (!noteId.isEmpty())
        DraftManager::instance()->queueRemoval(systemName(), noteId);
}

NoteRemoveJob *XmppStorage::removeNoteAsync(const QString &noteId, QObject *owner)
{
    auto *job = new NoteRemoveJob(owner ? owner : this);
    job->start();
    if (noteId.isEmpty() || errorState_) {
        job->fail({ noteId.isEmpty() ? StorageError::NotFound : StorageError::Unavailable,
                    noteId.isEmpty() ? tr("Note was not found") : errorStateMessage_, false });
        return job;
    }
    const auto              config  = config_;
    const auto              epoch   = configEpoch_;
    const auto              removed = cache_.value(noteId);
    QPointer<NoteRemoveJob> guard(job);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, noteId, removed, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->deleteNoteAsync(noteId, [this, guard, noteId, removed, epoch](XmppStatusResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), noteId, removed, epoch]() {
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok && !result.notFound) {
                            guard->fail(storageError(result, StorageError::Network));
                            return;
                        }
                        cache_.remove(noteId);
                        persistCache();
                        if (!removed.isNull())
                            emit noteRemoved(removed);
                        guard->complete();
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

} // namespace AnyKeep
