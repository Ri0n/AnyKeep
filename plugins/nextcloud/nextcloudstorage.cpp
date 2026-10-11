#include "nextcloudstorage.h"

#include "foldercatalogmanager.h"
#include "localmediastore.h"
#include "nextcloudcategory.h"
#include "nextcloudmedialinks.h"
#include "nextcloudworker.h"
#include "notedata.h"
#include "settingscontroller.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QRegularExpression>
#include <QHostAddress>
#include <QMetaObject>
#include <QMimeDatabase>
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
        server.description = tr("Use HTTPS for Nextcloud. Plain HTTP is permitted only for loopback tests.");
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


    // The provider currently publishes bounded, native Notes attachments only.
    // Large objects need a streaming LocalMediaStore source and chunked WebDAV.
    constexpr qint64 MaxNativeAttachmentBytes = 16 * 1024 * 1024;

    struct PreparedAttachment {
        MediaReference reference;
        QByteArray bytes;
    };

    struct MediaSaveResult {
        NextcloudNoteResult note;
        QList<MediaReference> media;
    };

    struct NativeRestoredFile {
        QString path;
        QByteArray bytes;
    };

    struct NativeRestoreResult {
        NextcloudNoteResult note;
        QList<NativeRestoredFile> files;
    };

    // The Notes 5.0.2 API uploads to the category directory and its files
    // become inaccessible after a category move. The web Notes editor instead
    // stores per-note attachments under .attachments.<noteId>; its directory
    // follows the note when moved. Preserve that native on-disk convention.
    QString attachmentDirectory(const QString &notesPath, const QString &category, const QString &id)
    {
        return notesPath + QLatin1Char('/')
            + (category.isEmpty() ? QString() : category + QLatin1Char('/'))
            + QStringLiteral(".attachments.") + id;
    }

    QString stableAttachmentFileName(const PreparedAttachment &attachment)
    {
        QString suffix = QFileInfo(attachment.reference.portableName).suffix().toLower();
        static const QRegularExpression safeSuffix(QStringLiteral("^[a-z0-9]{1,12}$"));
        if (!safeSuffix.match(suffix).hasMatch())
            suffix = QStringLiteral("bin");
        const auto digest = QCryptographicHash::hash(attachment.bytes, QCryptographicHash::Sha256)
                                .toHex().left(24);
        return attachment.reference.id.toString(QUuid::WithoutBraces)
            + QLatin1Char('-') + QString::fromLatin1(digest) + QLatin1Char('.') + suffix;
    }

    // Bounded independent Range GETs: verification never downloads an
    // unbounded HTTP response, nor publishes a URL pointing at incomplete
    // or corrupted bytes. Empty objects require a separate metadata probe.
    QString verifyNativeBytes(NextcloudWorker *worker, const QString &davPath,
                              const QByteArray &expected, bool *missing, int *httpStatus)
    {
        if (missing)
            *missing = false;
        if (expected.isEmpty())
            return QStringLiteral("Zero-length media requires an explicit WebDAV stat");
        constexpr qint64 readSize = 1024 * 1024;
        for (qint64 offset = 0; offset < expected.size();) {
            const qint64 count = qMin<qint64>(readSize, expected.size() - offset);
            const auto response = worker->readWebDavRange(davPath, offset, count);
            if (httpStatus)
                *httpStatus = response.httpStatus;
            if (!response.ok) {
                if (missing && offset == 0 && response.httpStatus == 404)
                    *missing = true;
                return response.error;
            }
            if (response.totalSize != expected.size() || response.data.size() != count
                || response.data != expected.mid(offset, count))
                return QStringLiteral("The native Nextcloud attachment changed or failed read-back");
            offset += count;
        }
        return {};
    }

    bool prepareAttachments(const Note &note, LocalMediaStore *store, QList<PreparedAttachment> *prepared,
                            QString *error)
    {
        if (note.media().isEmpty())
            return true;
        if (note.id().isEmpty()) {
            *error = QStringLiteral("Creating notes with media requires a recoverable remote note identity");
            return false;
        }

        if (!store->initialize(error))
            return false;

        QHash<QString, QString> probePaths;
        for (const auto &reference : note.media()) {
            if (!reference.isValid() || reference.size < 0 || reference.size > MaxNativeAttachmentBytes) {
                *error = QStringLiteral("Invalid or oversized media reference; streaming upload is not available yet");
                return false;
            }
            // Every persisted manifest entry must be reachable from the body;
            // otherwise another native client could never reconstruct it.
            if (!note.text().contains(reference.uri())) {
                *error = QStringLiteral("Media manifest contains an attachment not referenced by Markdown");
                return false;
            }
            const auto data = store->data(reference);
            if (!data) {
                *error = QStringLiteral("Could not read local media: ") + data.error;
                return false;
            }
            if (data.value.size() > MaxNativeAttachmentBytes
                || (reference.size != 0 && data.value.size() != reference.size)
                || (reference.checksum.size() == 32
                    && QCryptographicHash::hash(data.value, QCryptographicHash::Sha256) != reference.checksum)) {
                *error = QStringLiteral("Local media size or SHA-256 checksum changed");
                return false;
            }
            probePaths.insert(reference.uri(), QStringLiteral("pending/") + reference.portableName);
            prepared->append({ reference, data.value });
        }

        QString linkError;
        (void)NextcloudMediaLinks::toNative(note.text(), probePaths, &linkError);
        if (!linkError.isEmpty()) {
            *error = linkError;
            return false;
        }
        return true;
    }

} // namespace

NextcloudStorage::NextcloudStorage(QObject *parent, FolderCatalogManager *folderCatalogManager,
                                   LocalMediaStore *mediaStore) :
    NoteStorage(parent),
    folderCatalogManager_(folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance()),
    mediaStore_(mediaStore ? mediaStore : LocalMediaStore::instance())
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

    const QString host = config.serverUrl.host();
    const QHostAddress address(host);
    const bool loopback = host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
        || (!address.isNull() && address.isLoopback());
    if (config.serverUrl.scheme() == QStringLiteral("http") && !loopback) {
        if (error)
            *error = tr("Nextcloud requires HTTPS except for loopback test servers.");
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
    note.setFavorite(remote.favorite);
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

bool NextcloudStorage::toRemote(const Note &note, NextcloudRemoteNote *remote, QString *error, bool mediaPrepared) const
{
    if (!remote)
        return false;
    if (!mediaPrepared && !note.media().isEmpty()) {
        if (error)
            *error = tr("Nextcloud media publication is not available yet.");
        return false;
    }
    QString linkError;
    if (!mediaPrepared)
        (void)NextcloudMediaLinks::toNative(note.text(), {}, &linkError);
    if (!linkError.isEmpty()) {
        if (error)
            *error = tr("Nextcloud media links require a completed native upload.");
        return false;
    }
    NextcloudRemoteNote result;
    result.id             = note.id();
    result.etag           = note.backendValue(QStringLiteral("etag")).toString();
    result.readOnly       = note.backendValue(QStringLiteral("readOnly")).toBool();
    result.content        = note.text();
    result.contentPresent = note.isLoaded();
    result.title          = note.title();
    result.favorite       = note.isFavorite();
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
    const auto config = readConfig();
    QString configError;
    if (!configIsValid(config, &configError)) {
        job->fail({ StorageError::NotConfigured, configError, false });
        return job;
    }

    QPointer<NoteLoadJob> guard(job);
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, id]() {
            worker_->setConfig(config);
            NativeRestoreResult restored;
            restored.note = worker_->getNote(id);
            if (restored.note.ok && restored.note.note.contentPresent) {
                // Only Notes' own .attachments.<noteId>/ paths may be read
                // automatically. Arbitrary relative/external links are not
                // considered media owned by this note.
                const QString prefix = QStringLiteral(".attachments.") + id + QLatin1Char('/');
                const auto paths = NextcloudMediaLinks::referencedNativePaths(restored.note.note.content);
                QStringList nativePaths;
                for (const auto &path : paths) {
                    if (path.startsWith(prefix))
                        nativePaths.append(path);
                }
                constexpr qint64 maxRestoredBytes = 32 * 1024 * 1024;
                constexpr int maxRestoredFiles = 12;
                if (!nativePaths.isEmpty()) {
                    const auto settings = worker_->getSettings();
                    if (settings.ok) {
                        qint64 budget = maxRestoredBytes;
                        const QString noteFolder = restored.note.note.category;
                        for (const auto &path : nativePaths.mid(0, maxRestoredFiles)) {
                            const QString davPath = settings.notesPath + QLatin1Char('/')
                                + (noteFolder.isEmpty() ? QString() : noteFolder + QLatin1Char('/')) + path;
                            if (!NextcloudMediaLinks::isSafeNativePath(davPath))
                                continue;
                            const auto first = worker_->readWebDavRange(davPath, 0, 1);
                            if (!first.ok || first.totalSize <= 0
                                || first.totalSize > MaxNativeAttachmentBytes || first.totalSize > budget)
                                continue;
                            QByteArray bytes = first.data;
                            bool complete = true;
                            while (bytes.size() < first.totalSize) {
                                const qint64 amount = qMin<qint64>(1024 * 1024, first.totalSize - bytes.size());
                                const auto segment = worker_->readWebDavRange(davPath, bytes.size(), amount);
                                if (!segment.ok || segment.totalSize != first.totalSize
                                    || segment.data.size() != amount) {
                                    complete = false;
                                    break;
                                }
                                bytes.append(segment.data);
                            }
                            if (!complete)
                                continue;
                            budget -= bytes.size();
                            restored.files.append({ path, std::move(bytes) });
                        }
                    }
                }
            }

            QMetaObject::invokeMethod(
                this,
                [this, guard, restored, id, config]() {
                    if (!guard || guard->isFinished())
                        return;
                    const auto &result = restored.note;
                    if (!result.ok) {
                        if (result.httpStatus == 404)
                            cache_.remove(id);
                        guard->fail({ result.httpStatus == 404 ? StorageError::NotFound : StorageError::Network,
                                      result.error, result.httpStatus != 404 });
                        return;
                    }
                    reconcileRemoteFolders({ result.note });
                    auto loaded = fromRemote(result.note);
                    if (!restored.files.isEmpty()) {
                        QString initError;
                        if (mediaStore_->initialize(&initError)) {
                            QHash<QString, QString> canonicalUris;
                            QList<MediaReference> refs;
                            QMimeDatabase mime;
                            for (const auto &file : restored.files) {
                                const QString fileName = file.path.section(QLatin1Char('/'), -1);
                                const QByteArray identity
                                    = (config.serverUrl.toString() + QLatin1Char('|') + config.userName
                                       + QLatin1Char('|') + id + QLatin1Char('|') + file.path).toUtf8();
                                const QUuid stableId = QUuid::fromRfc4122(
                                    QCryptographicHash::hash(identity, QCryptographicHash::Sha256).left(16));
                                const auto imported = mediaStore_->importData(
                                    file.bytes, fileName,
                                    mime.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension).name(), stableId);
                                if (!imported)
                                    continue;
                                auto reference = imported.value;
                                reference.remoteData.insert(QStringLiteral("nextcloud.noteId"), id);
                                reference.remoteData.insert(QStringLiteral("nextcloud.path"), file.path);
                                canonicalUris.insert(file.path, reference.uri());
                                refs.append(reference);
                            }
                            if (!canonicalUris.isEmpty()) {
                                loaded.setText(NextcloudMediaLinks::toCanonical(result.note.content, canonicalUris),
                                               Note::Markdown);
                                loaded.setMedia(refs);
                            }
                        }
                    }
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

    const bool hasMedia = !note.media().isEmpty();
    QList<PreparedAttachment> attachments;
    QString preparationError;
    if (hasMedia && !prepareAttachments(note, mediaStore_, &attachments, &preparationError)) {
        job->fail({ StorageError::Other, preparationError, false });
        return job;
    }

    NextcloudRemoteNote local;
    QString folderError;
    if (!toRemote(note, &local, &folderError, hasMedia)) {
        job->fail({ StorageError::Other, folderError, false });
        return job;
    }
    const auto config = readConfig();
    QString configError;
    if (!configIsValid(config, &configError)) {
        job->fail({ StorageError::NotConfigured, configError, false });
        return job;
    }

    if (hasMedia && local.etag.isEmpty()) {
        job->fail({ StorageError::Conflict, tr("A known ETag is required before publishing Nextcloud media."),
                    false });
        return job;
    }

    const QString oldId = note.id();
    const QString canonicalText = note.text();
    QPointer<NoteSaveJob> guard(job);
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, local, oldId, attachments, hasMedia, canonicalText]() {
            worker_->setConfig(config);
            MediaSaveResult publication;
            if (!hasMedia) {
                publication.note = oldId.isEmpty() ? worker_->createNote(local) : worker_->updateNote(local);
            } else {
                // Check before staging data. Recheck the ETag again atomically
                // during the final updateNote() to handle concurrent native edits.
                const auto current = worker_->getNote(oldId);
                if (!current.ok) {
                    publication.note = current;
                } else if (current.note.etag != local.etag) {
                    publication.note.conflict = true;
                    publication.note.httpStatus = 412;
                    publication.note.remoteOnConflict = current.note;
                    publication.note.error = QStringLiteral("The Nextcloud note changed before media publication");
                } else {
                    QHash<QString, QString> nativePaths;
                    const auto settings = worker_->getSettings();
                    if (!settings.ok) {
                        publication.note.error = settings.error;
                        publication.note.httpStatus = settings.httpStatus;
                    } else {
                        const QString davFolder = attachmentDirectory(settings.notesPath, local.category, oldId);
                        if (!NextcloudMediaLinks::isSafeNativePath(davFolder)) {
                            publication.note.error = QStringLiteral("Unsafe native Notes attachment directory");
                        } else {
                            const auto folder = worker_->ensureWebDavFolder(davFolder);
                            if (!folder.ok) {
                                publication.note.error = folder.error;
                                publication.note.httpStatus = folder.httpStatus;
                            }
                        }
                        if (publication.note.error.isEmpty()) {
                            for (const auto &attachment : attachments) {
                                const QString receiptPath = attachment.reference.remoteData
                                    .value(QStringLiteral("nextcloud.path")).toString();
                                const QString receiptNote = attachment.reference.remoteData
                                    .value(QStringLiteral("nextcloud.noteId")).toString();
                                const QString ownedPrefix = QStringLiteral(".attachments.") + oldId
                                    + QLatin1Char('/');
                                QString nativePath = ownedPrefix + stableAttachmentFileName(attachment);
                                bool verified = false;
                                if (receiptNote == oldId && receiptPath.startsWith(ownedPrefix)
                                    && NextcloudMediaLinks::isSafeNativePath(receiptPath)) {
                                    const QString previousDav = attachmentDirectory(
                                        settings.notesPath, local.category, oldId)
                                        + QLatin1Char('/') + receiptPath.mid(ownedPrefix.size());
                                    bool missing = false;
                                    QString error = verifyNativeBytes(worker_, previousDav, attachment.bytes,
                                                                      &missing, &publication.note.httpStatus);
                                    if (error.isEmpty()) {
                                        nativePath = receiptPath;
                                        verified = true;
                                    } else if (!missing) {
                                        publication.note.conflict = true;
                                        publication.note.error = error;
                                        break;
                                    }
                                }

                                if (!verified) {
                                    const QString davPath = davFolder + QLatin1Char('/')
                                        + nativePath.mid(ownedPrefix.size());
                                    bool missing = false;
                                    const QString before = verifyNativeBytes(worker_, davPath, attachment.bytes,
                                                                             &missing, &publication.note.httpStatus);
                                    if (before.isEmpty()) {
                                        verified = true; // recover a previously staged upload
                                    } else if (!missing) {
                                        publication.note.conflict = true;
                                        publication.note.error = before;
                                        break;
                                    }

                                    if (!verified) {
                                        QBuffer source;
                                        source.setData(attachment.bytes);
                                        if (!source.open(QIODevice::ReadOnly)) {
                                            publication.note.error = source.errorString();
                                            break;
                                        }
                                        const auto uploaded
                                            = worker_->uploadWebDavChunks(davPath, &source, attachment.bytes.size());
                                        if (!uploaded.ok) {
                                            // A racing identical upload might win the
                                            // atomic no-overwrite MOVE.
                                            const QString check = verifyNativeBytes(
                                                worker_, davPath, attachment.bytes, nullptr,
                                                &publication.note.httpStatus);
                                            if (!check.isEmpty()) {
                                                publication.note.error = uploaded.error;
                                                publication.note.httpStatus = uploaded.httpStatus;
                                                break;
                                            }
                                        }
                                        const QString check = verifyNativeBytes(worker_, davPath, attachment.bytes,
                                                                                 nullptr, &publication.note.httpStatus);
                                        if (!check.isEmpty()) {
                                            publication.note.error = check;
                                            break;
                                        }
                                    }
                                }

                                nativePaths.insert(attachment.reference.uri(), nativePath);
                                auto publishedReference = attachment.reference;
                                publishedReference.remoteData.insert(QStringLiteral("nextcloud.noteId"), oldId);
                                publishedReference.remoteData.insert(QStringLiteral("nextcloud.path"), nativePath);
                                publication.media.append(std::move(publishedReference));
                            }
                        }
                    }

                    if (publication.note.error.isEmpty()) {
                        QString linkError;
                        auto outgoing = local;
                        outgoing.content = NextcloudMediaLinks::toNative(canonicalText, nativePaths, &linkError);
                        if (!linkError.isEmpty()) {
                            publication.note.error = linkError;
                        } else {
                            publication.note = worker_->updateNote(outgoing);
                        }
                    }
                }
            }

            QMetaObject::invokeMethod(
                this,
                [this, guard, publication, oldId, hasMedia, canonicalText]() {
                    if (!guard || guard->isFinished())
                        return;
                    const auto &result = publication.note;
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
                    auto saved = fromRemote(result.note);
                    if (hasMedia) {
                        // Nextcloud stores only native relative Markdown links.
                        // Keep the stable canonical URIs and the verified
                        // publication receipts in the local/cache manifest.
                        saved.setText(canonicalText, Note::Markdown);
                        saved.setMedia(publication.media);
                    }
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
