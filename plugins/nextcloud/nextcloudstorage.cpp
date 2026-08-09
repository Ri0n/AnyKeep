#include "nextcloudstorage.h"

#include "foldercatalogmanager.h"
#include "nextcloudcategory.h"
#include "nextcloudworker.h"
#include "notedata.h"
#include "settingscontroller.h"

#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace AnyKeep {
class NextcloudSettingsController final : public SettingsController {
public:
    explicit NextcloudSettingsController(NextcloudStorage *storage, QObject *parent = nullptr) :
        SettingsController(parent), storage_(storage)
    {
        const auto   config = storage_->readConfig();
        QList<Field> fields;
        Field        server;
        server.key         = QStringLiteral("serverUrl");
        server.label       = tr("Server URL");
        server.description = tr("The base HTTP or HTTPS URL of the Nextcloud server.");
        server.value       = config.serverUrl.toString();
        server.placeholder = QStringLiteral("https://cloud.example.org");
        fields.append(server);

        Field user;
        user.key   = QStringLiteral("userName");
        user.label = tr("User name");
        user.value = config.userName;
        fields.append(user);

        Field password;
        password.key         = QStringLiteral("appPassword");
        password.label       = tr("App password");
        password.type        = Password;
        password.value       = config.appPassword;
        password.description = tr("Use a dedicated Nextcloud app password rather than the account password.");
        fields.append(password);

        Field timeout;
        timeout.key     = QStringLiteral("timeoutSeconds");
        timeout.label   = tr("Operation timeout");
        timeout.type    = Integer;
        timeout.value   = qBound(2, config.timeoutMs / 1000, 120);
        timeout.minimum = 2;
        timeout.maximum = 120;
        fields.append(timeout);
        setFields(std::move(fields));
    }

protected:
    bool applyValues(const QVariantMap &values, QString *error) override
    {
        NextcloudConfig config;
        config.serverUrl   = QUrl::fromUserInput(values.value(QStringLiteral("serverUrl")).toString().trimmed());
        config.userName    = values.value(QStringLiteral("userName")).toString().trimmed();
        config.appPassword = values.value(QStringLiteral("appPassword")).toString();
        config.timeoutMs   = values.value(QStringLiteral("timeoutSeconds"), 15).toInt() * 1000;
        if (!storage_->configIsValid(config, error))
            return false;
        storage_->applyConfig(config);
        return true;
    }

private:
    NextcloudStorage *storage_;
};

namespace {

    template <typename Result, typename Function> Result invokeWorker(NextcloudWorker *worker, Function &&function)
    {
        Result result;

        if (QThread::currentThread() == worker->thread()) {
            return function();
        }

        const bool invoked
            = QMetaObject::invokeMethod(worker, [&]() { result = function(); }, Qt::BlockingQueuedConnection);

        if (!invoked) {
            result.ok    = false;
            result.error = QStringLiteral("Failed to invoke the Nextcloud network worker");
        }
        return result;
    }

} // namespace

NextcloudStorage::NextcloudStorage(QObject *parent, FolderCatalogManager *folderCatalogManager) :
    NoteStorage(parent),
    folderCatalogManager_(folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance())
{
    workerThread_.setObjectName(QStringLiteral("AnyKeepNextcloudWorker"));
    worker_ = new NextcloudWorker;
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    workerThread_.start();
}

NextcloudStorage::~NextcloudStorage() { shutdown(); }

void NextcloudStorage::shutdown()
{
    if (!workerThread_.isRunning())
        return;
    workerThread_.quit();
    workerThread_.wait();
}

NextcloudConfig NextcloudStorage::readConfig() const
{
    QSettings       settings;
    NextcloudConfig config;

    const auto urlText = settings.value(QStringLiteral("storage.nextcloud.url")).toString();
    if (!urlText.trimmed().isEmpty()) {
        config.serverUrl = QUrl::fromUserInput(urlText.trimmed());
    }

    config.userName    = settings.value(QStringLiteral("storage.nextcloud.username")).toString();
    config.appPassword = settings.value(QStringLiteral("storage.nextcloud.appPassword")).toString();
    config.timeoutMs   = settings.value(QStringLiteral("storage.nextcloud.timeoutMs"), 15000).toInt();

    return config;
}

bool NextcloudStorage::configIsValid(const NextcloudConfig &config, QString *error) const
{
    if (!config.serverUrl.isValid() || config.serverUrl.host().isEmpty()
        || (config.serverUrl.scheme() != QStringLiteral("https")
            && config.serverUrl.scheme() != QStringLiteral("http"))) {
        if (error) {
            *error = tr("Enter a valid HTTP or HTTPS Nextcloud server URL.");
        }
        return false;
    }

    if (config.userName.isEmpty()) {
        if (error) {
            *error = tr("Enter the Nextcloud user name.");
        }
        return false;
    }

    if (config.appPassword.isEmpty()) {
        if (error) {
            *error = tr("Enter a Nextcloud app password.");
        }
        return false;
    }

    return true;
}

bool NextcloudStorage::init()
{
    config_ = readConfig();

    QString validationError;
    if (!configIsValid(config_, &validationError)) {
        accessible_ = false;
        cacheValid_ = false;
        return false;
    }

    const auto result = invokeWorker<NextcloudStatusResult>(worker_, [&]() {
        worker_->setConfig(config_);
        return worker_->probe();
    });

    accessible_ = result.ok;
    cacheValid_ = false;

    if (!result.ok) {
        reportError(result.error);
    }
    return result.ok;
}

StorageInitJob *NextcloudStorage::initAsync(QObject *owner)
{
    auto *job = new StorageInitJob(owner ? owner : this);
    job->start();
    config_ = readConfig();
    QString validationError;
    if (!configIsValid(config_, &validationError)) {
        accessible_ = false;
        StorageError error { StorageError::NotConfigured, validationError, false };
        job->fail(error);
        return job;
    }
    QPointer<StorageInitJob> guard(job);
    const auto               config = config_;
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config]() {
            worker_->setConfig(config);
            const auto result = worker_->probe();
            QMetaObject::invokeMethod(
                this,
                [this, guard, result]() {
                    if (!guard || guard->isFinished())
                        return;
                    accessible_ = result.ok;
                    cacheValid_ = false;
                    if (result.ok)
                        guard->complete();
                    else {
                        reportError(result.error);
                        guard->fail({ StorageError::Network, result.error, true });
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return job;
}

const QString NextcloudStorage::systemName() const { return storageId; }

const QString NextcloudStorage::name() const { return tr("Nextcloud Notes"); }

QIcon NextcloudStorage::storageIcon() const { return QIcon(QStringLiteral(":/nextcloud/nextcloud-notes.svg")); }

QIcon NextcloudStorage::noteIcon() const { return QIcon(QStringLiteral(":/nextcloud/nextcloud-notes.svg")); }

bool NextcloudStorage::isAccessible() const { return accessible_; }

QList<Note::Format> NextcloudStorage::availableFormats() const { return { Note::Markdown }; }

Note NextcloudStorage::fromRemote(const NextcloudRemoteNote &remote)
{
    Note note(new NoteData(this));
    applyRemote(note, remote);
    return note;
}

void NextcloudStorage::applyRemote(Note &note, const NextcloudRemoteNote &remote)
{
    note.setId(remote.id);
    note.setTitle(remote.title);
    note.setFormat(Note::Markdown);
    note.setBackendValue(QStringLiteral("etag"), remote.etag);
    note.setBackendValue(QStringLiteral("category"), remote.category);
    note.setBackendValue(QStringLiteral("favorite"), remote.favorite);
    note.setBackendValue(QStringLiteral("readOnly"), remote.readOnly);
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
        note.setFolderId(folderCatalogManager_->catalog().folderForNote(systemName(), remote.id));
    } else {
        note.setFolderId({});
    }
    if (remote.modified > 0) {
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        note.setLastChangeUTC(QDateTime::fromSecsSinceEpoch(remote.modified, Qt::UTC));
#else
        note.setLastChangeUTC(QDateTime::fromSecsSinceEpoch(remote.modified, QTimeZone::utc()));
#endif
    } else {
        note.setLastChangeUTC({});
    }
    if (remote.contentPresent)
        note.setText(remote.content, Note::Markdown);
    else
        note.unload();
}

bool NextcloudStorage::categoryForFolder(const QUuid &folderId, QString *category, QString *error) const
{
    if (category)
        category->clear();
    if (error)
        error->clear();
    if (folderId.isNull())
        return true;
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable()) {
        if (error)
            *error = tr("The folder catalog is unavailable");
        return false;
    }

    const auto path = folderCatalogManager_->catalog().pathForFolder(folderId);
    if (path.isEmpty()) {
        if (error)
            *error = tr("The selected folder no longer exists");
        return false;
    }
    return NextcloudCategory::encode(path, category, error);
}

bool NextcloudStorage::categoryForNote(const Note &note, QString *category, QString *error) const
{
    if (!note.folderId().isNull())
        return categoryForFolder(note.folderId(), category, error);

    if (category)
        category->clear();
    if (error)
        error->clear();

    if (note.id().isEmpty())
        return true;
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
        const auto *assignment = folderCatalogManager_->catalog().assignment(systemName(), note.id());
        if (assignment && assignment->tombstone)
            return true;
    }

    if (category)
        *category = note.backendValue(QStringLiteral("category")).toString();
    return true;
}

bool NextcloudStorage::toRemote(const Note &note, NextcloudRemoteNote *remote, QString *error) const
{
    if (!remote)
        return false;
    NextcloudRemoteNote result;
    result.id             = note.id();
    result.etag           = note.backendValue(QStringLiteral("etag")).toString();
    result.readOnly       = note.backendValue(QStringLiteral("readOnly")).toBool();
    result.content        = note.text();
    result.contentPresent = note.isLoaded();
    result.title          = note.title();
    result.favorite       = note.backendValue(QStringLiteral("favorite")).toBool();
    if (!categoryForNote(note, &result.category, error))
        return false;
    const auto requestedModified
        = note.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).toDateTime();
    result.modified
        = (requestedModified.isValid() ? requestedModified : QDateTime::currentDateTimeUtc()).toSecsSinceEpoch();
    *remote = std::move(result);
    return true;
}

void NextcloudStorage::reconcileRemoteFolders(const QList<NextcloudRemoteNote> &notes)
{
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return;

    QList<ProviderFolderPathAssignment> assignments;
    assignments.reserve(notes.size());
    for (const auto &remote : notes) {
        QStringList path;
        QString     error;
        if (!NextcloudCategory::decode(remote.category, &path, &error)) {
            reportError(tr("Could not import a Nextcloud category as a folder path: %1").arg(error));
            continue;
        }
        ProviderFolderPathAssignment assignment;
        assignment.noteId = remote.id;
        assignment.path   = std::move(path);
        if (remote.modified > 0) {
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
            assignment.modifiedAt = QDateTime::fromSecsSinceEpoch(remote.modified, Qt::UTC);
#else
            assignment.modifiedAt = QDateTime::fromSecsSinceEpoch(remote.modified, QTimeZone::utc());
#endif
        }
        assignments.append(std::move(assignment));
    }
    if (assignments.isEmpty())
        return;

    if (const auto result = folderCatalogManager_->reconcileProviderFolderPaths(systemName(), assignments))
        reportError(tr("Could not merge Nextcloud folders: %1").arg(result.message));
}

QList<Note> NextcloudStorage::noteList(int limit)
{
    if (!accessible_ && !init()) {
        return {};
    }

    const auto result = invokeWorker<NextcloudListResult>(worker_, [&]() { return worker_->listNotes(); });

    if (!result.ok) {
        reportError(result.error);
        auto cached = cache_.values();
        std::sort(cached.begin(), cached.end(), noteListItemModifyComparer);
        return limit > 0 ? cached.mid(0, limit) : cached;
    }

    accessible_ = true;
    reconcileRemoteFolders(result.notes);
    QHash<QString, Note> refreshed;
    refreshed.reserve(result.notes.size());

    for (const auto &remote : result.notes) {
        const auto old = cache_.constFind(remote.id);
        if (old != cache_.cend()) {
            if (old.value().backendValue(QStringLiteral("etag")).toString() == remote.etag) {
                auto cached = old.value();
                if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
                    cached.setFolderId(folderCatalogManager_->catalog().folderForNote(systemName(), remote.id));
                }
                refreshed.insert(remote.id, cached);
                continue;
            }
        }
        refreshed.insert(remote.id, fromRemote(remote));
    }

    for (auto it = locallySaved_.begin(); it != locallySaved_.end();) {
        const auto remote = refreshed.constFind(it.key());
        if (remote != refreshed.cend()
            && remote.value().backendValue(QStringLiteral("etag")) == it.value().backendValue(QStringLiteral("etag"))) {
            it = locallySaved_.erase(it);
        } else {
            refreshed.insert(it.key(), it.value());
            ++it;
        }
    }
    for (auto it = locallyRemoved_.begin(); it != locallyRemoved_.end();) {
        if (!refreshed.contains(*it))
            it = locallyRemoved_.erase(it);
        else {
            refreshed.remove(*it);
            ++it;
        }
    }

    cache_      = std::move(refreshed);
    cacheValid_ = true;

    auto notes = cache_.values();
    std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
    return limit > 0 ? notes.mid(0, limit) : notes;
}

NoteListJob *NextcloudStorage::refreshNotesAsync(int limit, QObject *owner)
{
    auto *job = new NoteListJob(owner ? owner : this);
    job->start();
    config_ = readConfig();
    QString validationError;
    if (!configIsValid(config_, &validationError)) {
        job->fail({ StorageError::NotConfigured, validationError, false });
        return job;
    }
    QPointer<NoteListJob> guard(job);
    const auto            config = config_;
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, limit]() {
            worker_->setConfig(config);
            auto                status = worker_->probe();
            NextcloudListResult result;
            if (status.ok)
                result = worker_->listNotes();
            else {
                result.ok    = false;
                result.error = status.error;
            }
            QMetaObject::invokeMethod(
                this,
                [this, guard, result, limit]() {
                    if (!guard || guard->isFinished())
                        return;
                    if (!result.ok) {
                        accessible_ = false;
                        reportError(result.error);
                        guard->fail({ StorageError::Network, result.error, true });
                        return;
                    }
                    reconcileRemoteFolders(result.notes);
                    QHash<QString, Note> refreshed;
                    for (const auto &remote : result.notes) {
                        const auto old = cache_.constFind(remote.id);
                        if (old != cache_.cend()
                            && old.value().backendValue(QStringLiteral("etag")).toString() == remote.etag) {
                            auto cached = old.value();
                            if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
                                cached.setFolderId(
                                    folderCatalogManager_->catalog().folderForNote(systemName(), remote.id));
                            }
                            refreshed.insert(remote.id, cached);
                        } else {
                            refreshed.insert(remote.id, fromRemote(remote));
                        }
                    }
                    for (auto it = locallySaved_.begin(); it != locallySaved_.end();) {
                        const auto remote = refreshed.constFind(it.key());
                        if (remote != refreshed.cend()
                            && remote.value().backendValue(QStringLiteral("etag"))
                                == it.value().backendValue(QStringLiteral("etag"))) {
                            it = locallySaved_.erase(it);
                        } else {
                            refreshed.insert(it.key(), it.value());
                            ++it;
                        }
                    }
                    for (auto it = locallyRemoved_.begin(); it != locallyRemoved_.end();) {
                        if (!refreshed.contains(*it))
                            it = locallyRemoved_.erase(it);
                        else {
                            refreshed.remove(*it);
                            ++it;
                        }
                    }
                    cache_      = std::move(refreshed);
                    cacheValid_ = accessible_ = true;
                    auto notes                = cache_.values();
                    std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
                    guard->complete(limit > 0 ? notes.mid(0, limit) : notes);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return job;
}

Note NextcloudStorage::note(const QString &id)
{
    if (id.isEmpty()) {
        return {};
    }
    if (!accessible_ && !init()) {
        return {};
    }

    const auto result = invokeWorker<NextcloudNoteResult>(worker_, [&]() { return worker_->getNote(id); });

    if (!result.ok) {
        if (result.httpStatus == 404) {
            cache_.remove(id);
            return {};
        }
        reportError(result.error);
        return {};
    }

    reconcileRemoteFolders({ result.note });
    accessible_     = true;
    auto remoteNote = fromRemote(result.note);
    cache_.insert(result.note.id, remoteNote);
    return remoteNote;
}

NoteLoadJob *NextcloudStorage::loadNoteAsync(const QString &id, QObject *owner)
{
    auto *job = new NoteLoadJob(owner ? owner : this);
    job->start();
    if (id.isEmpty()) {
        job->fail({ StorageError::NotFound, tr("Note was not found"), false });
        return job;
    }
    const auto            config = readConfig();
    QPointer<NoteLoadJob> guard(job);
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, id]() {
            worker_->setConfig(config);
            const auto result = worker_->getNote(id);
            QMetaObject::invokeMethod(
                this,
                [this, guard, result, id]() {
                    if (!guard || guard->isFinished())
                        return;
                    if (!result.ok) {
                        if (result.httpStatus == 404)
                            cache_.remove(id);
                        guard->fail({ result.httpStatus == 404 ? StorageError::NotFound : StorageError::Network,
                                      result.error, result.httpStatus != 404 });
                        return;
                    }
                    reconcileRemoteFolders({ result.note });
                    auto loaded = fromRemote(result.note);
                    cache_.insert(result.note.id, loaded);
                    accessible_ = true;
                    guard->complete(loaded);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return job;
}

Note NextcloudStorage::createNote()
{
    Note note(new NoteData(this));
    note.setText(QString(), Note::Markdown);
    note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
    return note;
}

bool NextcloudStorage::loadNote(Note &note)
{
    if (note.id().isEmpty()) {
        return true;
    }
    if (!accessible_ && !init()) {
        return false;
    }

    const QString id     = note.id();
    const auto    result = invokeWorker<NextcloudNoteResult>(worker_, [&]() { return worker_->getNote(id); });

    if (!result.ok) {
        reportError(result.error);
        return false;
    }

    reconcileRemoteFolders({ result.note });
    applyRemote(note, result.note);
    accessible_ = true;
    return true;
}

bool NextcloudStorage::saveNote(const Note &note)
{
    if (note.isNull()) {
        return false;
    }

    if (note.storage() != this) {
        reportError(tr("Attempted to save a note owned by another storage."));
        return false;
    }

    Note saved = note;
    if (!saved.id().isEmpty() && !saved.isLoaded() && !loadNote(saved)) {
        return false;
    }

    if (saved.backendValue(QStringLiteral("readOnly")).toBool()) {
        reportError(tr("The note is read-only and cannot be saved."));
        return false;
    }

    if (!accessible_ && !init()) {
        return false;
    }

    const QString       oldId = note.id();
    NextcloudRemoteNote local;
    QString             folderError;
    if (!toRemote(saved, &local, &folderError)) {
        reportError(folderError);
        return false;
    }

    const auto result = invokeWorker<NextcloudNoteResult>(
        worker_, [&]() { return oldId.isEmpty() ? worker_->createNote(local) : worker_->updateNote(local); });

    if (!result.ok) {
        if (result.conflict) {
            if (result.remoteOnConflict) {
                reconcileRemoteFolders({ *result.remoteOnConflict });
                cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
            }
            reportError(tr("Save conflict: this note was changed on the server. "
                           "Your local text was not overwritten. Reload the note or save the local text "
                           "as a new note."),
                        true);
        } else {
            reportError(result.error);
        }
        return false;
    }

    reconcileRemoteFolders({ result.note });
    applyRemote(saved, result.note);
    accessible_ = true;

    const QString newId   = saved.id();
    const bool    existed = !oldId.isEmpty() && cache_.contains(oldId);

    if (!oldId.isEmpty() && oldId != newId) {
        cache_.remove(oldId);
    }
    cache_.insert(newId, saved);
    locallySaved_.insert(newId, saved);
    locallyRemoved_.remove(newId);
    cacheValid_ = true;

    if (!oldId.isEmpty() && oldId != newId) {
        emit noteIdChanged(saved, oldId);
    }

    if (existed || !oldId.isEmpty()) {
        emit noteModified(saved);
    } else {
        emit noteAdded(saved);
    }
    return true;
}

NoteSaveJob *NextcloudStorage::saveNoteAsync(const Note &note, QObject *owner)
{
    auto *job = new NoteSaveJob(owner ? owner : this);
    job->start();
    if (note.isNull() || note.storage() != this) {
        job->fail({ StorageError::Other, tr("Attempted to save a note owned by another storage."), false });
        return job;
    }
    if (!note.isLoaded() || note.backendValue(QStringLiteral("readOnly")).toBool()) {
        job->fail({ StorageError::Other,
                    note.isLoaded() ? tr("The note is read-only and cannot be saved.")
                                    : tr("Load the note before saving it."),
                    false });
        return job;
    }

    const auto          config = readConfig();
    NextcloudRemoteNote local;
    QString             folderError;
    if (!toRemote(note, &local, &folderError)) {
        job->fail({ StorageError::Other, folderError, false });
        return job;
    }
    const auto            oldId = note.id();
    QPointer<NoteSaveJob> guard(job);
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, local, oldId]() {
            worker_->setConfig(config);
            const auto result = oldId.isEmpty() ? worker_->createNote(local) : worker_->updateNote(local);
            QMetaObject::invokeMethod(
                this,
                [this, guard, result, oldId]() {
                    if (!guard || guard->isFinished())
                        return;
                    if (!result.ok) {
                        StorageError error { result.conflict ? StorageError::Conflict : StorageError::Network,
                                             result.error, !result.conflict };
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
                    locallySaved_.insert(saved.id(), saved);
                    locallyRemoved_.remove(saved.id());
                    cacheValid_ = accessible_ = true;
                    guard->complete(saved);
                    if (!oldId.isEmpty() && oldId != saved.id())
                        emit noteIdChanged(saved, oldId);
                    if (existed || !oldId.isEmpty())
                        emit noteModified(saved);
                    else
                        emit noteAdded(saved);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return job;
}

NoteFolderChangeJob *NextcloudStorage::changeNoteFolderAsync(const Note &note, QObject *owner)
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
    if (note.backendValue(QStringLiteral("readOnly")).toBool()) {
        job->fail({ StorageError::Other, tr("The note is read-only and cannot be moved."), false });
        return job;
    }

    QString category;
    QString folderError;
    if (!categoryForFolder(note.folderId(), &category, &folderError)) {
        job->fail({ StorageError::Other, folderError, false });
        return job;
    }
    const auto config = readConfig();
    QString    configError;
    if (!configIsValid(config, &configError)) {
        job->fail({ StorageError::NotConfigured, configError, false });
        return job;
    }

    const QString                 noteId   = note.id();
    const QString                 etag     = note.backendValue(QStringLiteral("etag")).toString();
    const QUuid                   folderId = note.folderId();
    QPointer<NoteFolderChangeJob> guard(job);
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, noteId, category, etag, folderId]() {
            worker_->setConfig(config);
            const auto result = worker_->updateNoteCategory(noteId, category, etag);
            QMetaObject::invokeMethod(
                this,
                [this, guard, result, noteId, folderId]() {
                    if (!guard || guard->isFinished())
                        return;
                    if (!result.ok) {
                        if (result.remoteOnConflict) {
                            reconcileRemoteFolders({ *result.remoteOnConflict });
                            cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                        }
                        guard->fail({ result.conflict ? StorageError::Conflict : StorageError::Network, result.error,
                                      !result.conflict });
                        return;
                    }

                    reconcileRemoteFolders({ result.note });
                    auto changed = fromRemote(result.note);
                    if (!result.note.contentPresent) {
                        const auto cached = cache_.value(noteId);
                        if (!cached.isNull() && cached.isLoaded()) {
                            changed.setText(cached.text(), cached.format());
                            changed.setMedia(cached.media());
                        }
                    }
                    // The catalog stores the optimistic intent before this
                    // request starts. Keep the canonical summary aligned with
                    // it even when a server response omits a category field.
                    if (changed.folderId() != folderId)
                        changed.setFolderId(folderId);

                    cache_.insert(noteId, changed);
                    locallySaved_.insert(noteId, changed);
                    locallyRemoved_.remove(noteId);
                    cacheValid_ = accessible_ = true;
                    emit noteModified(changed);
                    guard->complete(changed);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return job;
}

void NextcloudStorage::removeNote(const QString &noteId)
{
    if (noteId.isEmpty()) {
        return;
    }
    if (!accessible_ && !init()) {
        return;
    }

    Note removed = cache_.value(noteId);
    if (removed.isNull()) {
        removed = note(noteId);
    }

    const auto result = invokeWorker<NextcloudStatusResult>(worker_, [&]() { return worker_->deleteNote(noteId); });

    if (!result.ok && result.httpStatus != 404) {
        reportError(result.error);
        return;
    }

    cache_.remove(noteId);
    locallySaved_.remove(noteId);
    locallyRemoved_.insert(noteId);
    if (!removed.isNull()) {
        emit noteRemoved(removed);
    }
}

NoteRemoveJob *NextcloudStorage::removeNoteAsync(const QString &noteId, QObject *owner)
{
    auto *job = new NoteRemoveJob(owner ? owner : this);
    job->start();
    if (noteId.isEmpty()) {
        job->fail({ StorageError::NotFound, tr("Note was not found"), false });
        return job;
    }
    const auto              removed = cache_.value(noteId);
    const auto              config  = readConfig();
    QPointer<NoteRemoveJob> guard(job);
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, noteId, removed]() {
            worker_->setConfig(config);
            const auto result = worker_->deleteNote(noteId);
            QMetaObject::invokeMethod(
                this,
                [this, guard, result, noteId, removed]() {
                    if (!guard || guard->isFinished())
                        return;
                    if (!result.ok && result.httpStatus != 404) {
                        guard->fail({ StorageError::Network, result.error, true });
                        return;
                    }
                    cache_.remove(noteId);
                    locallySaved_.remove(noteId);
                    locallyRemoved_.insert(noteId);
                    if (!removed.isNull())
                        emit noteRemoved(removed);
                    guard->complete();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return job;
}

void NextcloudStorage::reportError(const QString &error, bool invalidate)
{
    if (!error.isEmpty()) {
        emit storageErorr(tr("Nextcloud Notes error: %1").arg(error));
    }
    if (invalidate) {
        cacheValid_ = false;
        emit invalidated();
    }
}

void NextcloudStorage::applyConfig(const NextcloudConfig &config)
{
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"),
                      config.serverUrl.toString(QUrl::RemoveQuery | QUrl::RemoveFragment));
    settings.setValue(QStringLiteral("storage.nextcloud.username"), config.userName);
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), config.appPassword);
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), config.timeoutMs);

    cache_.clear();
    locallySaved_.clear();
    locallyRemoved_.clear();
    cacheValid_ = false;
    accessible_ = false;
    config_     = config;
    init();
    emit invalidated();
}

QUrl NextcloudStorage::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")); }

SettingsController *NextcloudStorage::createSettingsController(QObject *parent)
{
    return new NextcloudSettingsController(this, parent);
}

QString NextcloudStorage::tooltip()
{
    if (config_.serverUrl.isEmpty()) {
        config_ = readConfig();
    }

    if (config_.serverUrl.isEmpty()) {
        return tr("Nextcloud Notes is not configured.");
    }

    return tr("Server: %1\nUser: %2")
        .arg(config_.serverUrl.toDisplayString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment),
             config_.userName);
}

QString NextcloudStorage::storageId = QStringLiteral("nextcloud");

} // namespace AnyKeep
