#include "nextcloudstorage.h"

#include "foldercatalogmanager.h"
#include "localmediastore.h"
#include "mediasource.h"
#include "mediastream.h"
#include "nextcloudcategory.h"
#include "nextcloudmedialinks.h"
#include "nextcloudworker.h"
#include "notedata.h"
#include "settingscontroller.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLockFile>
#include <QStandardPaths>
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


    // Individual QIODevice reads and WebDAV chunks are bounded. An upload can
    // represent a large local object without ever materializing the whole
    // plaintext in RAM. The worker's chunked-v2 protocol has a 50 GiB bound.
    constexpr qint64 MaxNativeAttachmentBytes = 5LL * 1024 * 1024 * 10000;
    constexpr qint64 StreamReadSize = 1024 * 1024;

    struct PreparedAttachment {
        MediaReference reference;
        QByteArray sha256;  // SHA-256 of the verified plaintext source.
        qint64 size = 0;
    };

    // A reader is always opened from the local encrypted store. It must not
    // return unauthenticated bytes or depend on a fully decrypted buffer.
    QString hashStream(QIODevice &source, qint64 expectedSize, QByteArray *digest)
    {
        if (expectedSize <= 0 || expectedSize > MaxNativeAttachmentBytes)
            return QStringLiteral("Unsupported media size for streamed Nextcloud publication");
        if (!source.seek(0))
            return QStringLiteral("Could not seek to the beginning of local media");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 consumed = 0;
        while (consumed < expectedSize) {
            const QByteArray chunk = source.read(qMin(StreamReadSize, expectedSize - consumed));
            if (chunk.isEmpty())
                return source.errorString().isEmpty()
                    ? QStringLiteral("Local media ended before the expected length")
                    : source.errorString();
            hash.addData(chunk);
            consumed += chunk.size();
        }
        if (source.read(1).size() != 0)
            return QStringLiteral("Local media exceeds its expected length");
        *digest = hash.result();
        return {};
    }

    struct MediaSaveResult {
        NextcloudNoteResult note;
        QList<MediaReference> media;
        // Errors involving the local source must never be retried as if they
        // were transient HTTP/network failures.
        bool localSourceFailure { false };
    };

    struct NativeRestoredFile {
        QString path;
        QByteArray bytes;
    };

    struct NativeRestoreResult {
        NextcloudNoteResult note;
        QList<NativeRestoredFile> files;
    };

    // A Nextcloud Notes POST assigns the note ID on the server. A timed-out
    // POST is ambiguous: repeating it blindly creates a duplicate note.
    // Store the attempt *before* POST, keyed by the durable encrypted draft
    // identity (or by createNote()'s session identity for direct callers).
    // The journal contains only a token and remote ID, never note plaintext.
    QString creationJournalKey(const NextcloudConfig &config, const QString &token)
    {
        const QByteArray account = (config.serverUrl.toString(QUrl::RemoveQuery | QUrl::RemoveFragment)
                                    + QLatin1Char('\n') + config.userName).toUtf8();
        const QByteArray digest = QCryptographicHash::hash(account, QCryptographicHash::Sha256).toHex();
        return QStringLiteral("storage.nextcloud.creationJournal/")
            + QString::fromLatin1(digest) + QLatin1Char('/') + token;
    }

    QString creationMarker(const QString &token)
    {
        return QStringLiteral("<!-- anykeep-media-create:") + token + QStringLiteral(" -->");
    }

    QString creationTitle(const QString &token)
    {
        return QStringLiteral("AnyKeep pending media ") + token;
    }

    QString creationPlaceholder(const QString &token)
    {
        return QStringLiteral("# ") + creationTitle(token) + QStringLiteral("\n\n")
            + creationMarker(token);
    }

    // This is intentionally conservative. If the POST was sent but the reply
    // was lost, only search for its stable marker; never issue a second POST
    // until a positive identity has been established. The durable DraftRecord
    // remains available for another recovery attempt.
    NextcloudNoteResult ensureCreatedMediaNote(NextcloudWorker *worker, const NextcloudConfig &config,
                                                const NextcloudRemoteNote &requested, const QString &token)
    {
        NextcloudNoteResult result;
        const QString key = creationJournalKey(config, token);
        // Both multiple workers and a second AnyKeep process may try to
        // publish the same durable draft. Serialize the journal claim and
        // ambiguous POST across processes before reading attempted=false.
        const QByteArray lockHash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
        const QString lockDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
            + QStringLiteral("/nextcloud-create-locks");
        if (!QDir().mkpath(lockDir)) {
            result.error = QStringLiteral("Cannot create the Nextcloud publication lock directory");
            return result;
        }
        QLockFile claimLock(lockDir + QLatin1Char('/') + QString::fromLatin1(lockHash)
                            + QStringLiteral(".lock"));
        if (!claimLock.tryLock(5000)) {
            result.error = QStringLiteral("Another process is creating the same Nextcloud note");
            return result;
        }
        QSettings journal;
        const bool attempted = journal.value(key + QStringLiteral("/attempted")).toBool();
        QString remoteId = journal.value(key + QStringLiteral("/remoteId")).toString();

        if (!attempted) {
            journal.setValue(key + QStringLiteral("/attempted"), true);
            journal.sync();
            if (journal.status() != QSettings::NoError) {
                result.error = QStringLiteral("Could not persist the Nextcloud create attempt before POST");
                return result;
            }

            auto placeholder = requested;
            placeholder.title = creationTitle(token);
            placeholder.content = creationPlaceholder(token);
            placeholder.contentPresent = true;
            result = worker->createNote(placeholder);
            if (!result.ok)
                return result; // unknown whether POST committed: retry must search

            if (result.note.id.isEmpty()) {
                result.ok = false;
                result.error = QStringLiteral("Created Nextcloud note has no usable ID");
                return result;
            }
            remoteId = result.note.id;
            journal.setValue(key + QStringLiteral("/remoteId"), remoteId);
            journal.sync();
            if (journal.status() != QSettings::NoError) {
                result.ok = false;
                result.error = QStringLiteral("Could not persist the created Nextcloud note ID");
            }
            return result;
        }

        if (remoteId.isEmpty()) {
            const auto listed = worker->listNotes();
            if (!listed.ok) {
                result.httpStatus = listed.httpStatus;
                result.error = listed.error;
                return result;
            }

            // Notes list is metadata-only; GET candidate notes to confirm the
            // exact body marker instead of trusting a title collision.
            int matches = 0;
            NextcloudNoteResult found;
            for (const auto &candidate : listed.notes) {
                if (!candidate.title.contains(token) || candidate.category != requested.category)
                    continue;
                const auto fetched = worker->getNote(candidate.id);
                if (!fetched.ok) {
                    result.httpStatus = fetched.httpStatus;
                    result.error = fetched.error;
                    return result;
                }
                if (!fetched.note.contentPresent
                    || !fetched.note.content.contains(creationMarker(token)))
                    continue;
                ++matches;
                found = fetched;
            }
            if (matches != 1) {
                result.conflict = matches > 1;
                result.error = matches > 1
                    ? QStringLiteral("Multiple Nextcloud notes match the pending creation identity")
                    : QStringLiteral(
                        "The previous Nextcloud create attempt is unresolved; waiting for its server identity");
                return result;
            }
            remoteId = found.note.id;
            journal.setValue(key + QStringLiteral("/remoteId"), remoteId);
            journal.sync();
            if (journal.status() != QSettings::NoError) {
                result.error = QStringLiteral("Could not persist the recovered Nextcloud note ID");
                return result;
            }
            return found;
        }

        return worker->getNote(remoteId);
    }

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
        const auto digest = attachment.sha256.toHex().left(24);
        return attachment.reference.id.toString(QUuid::WithoutBraces)
            + QLatin1Char('-') + QString::fromLatin1(digest) + QLatin1Char('.') + suffix;
    }

    // Every response is compared to the source through an authenticated local
    // stream. Only bounded chunks of each side are resident in memory.
    QString verifyNativeBytes(NextcloudWorker *worker, const QString &davPath,
                              QIODevice &expected, qint64 size, const QByteArray &digest,
                              bool *missing, int *httpStatus)
    {
        if (missing)
            *missing = false;
        if (size <= 0 || size > MaxNativeAttachmentBytes)
            return QStringLiteral("Zero-length or oversized remote media cannot be verified");
        if (!expected.seek(0))
            return QStringLiteral("Could not seek to local media for remote verification");
        QString baseEtag;
        QCryptographicHash contentHash(QCryptographicHash::Sha256);
        for (qint64 offset = 0; offset < size;) {
            const qint64 count = qMin(StreamReadSize, size - offset);
            const auto response = worker->readWebDavRange(davPath, offset, count, baseEtag);
            if (httpStatus)
                *httpStatus = response.httpStatus;
            if (!response.ok) {
                if (missing && offset == 0 && response.httpStatus == 404)
                    *missing = true;
                return response.error;
            }
            if (baseEtag.isEmpty())
                baseEtag = response.etag;
            const QByteArray localChunk = expected.read(count);
            if ((!baseEtag.isEmpty() && !response.etag.isEmpty() && response.etag != baseEtag)
                || response.totalSize != size || response.data.size() != count
                || localChunk.size() != count || response.data != localChunk)
                return QStringLiteral("The native Nextcloud attachment changed or failed read-back");
            contentHash.addData(localChunk);
            offset += count;
        }
        if (contentHash.result() != digest)
            return QStringLiteral("Local media changed during WebDAV read-back verification");
        return {};
    }

    bool prepareAttachments(const QString &body, const QList<MediaReference> &references,
                            LocalMediaStore *store, QList<PreparedAttachment> *prepared, QString *error)
    {
        if (references.isEmpty())
            return true;

        QHash<QString, QString> probePaths;
        for (const auto &reference : references) {
            if (!reference.isValid() || reference.size <= 0
                || reference.size > MaxNativeAttachmentBytes) {
                *error = QStringLiteral("Invalid, empty or oversized Nextcloud media reference");
                return false;
            }
            if (!body.contains(reference.uri())) {
                *error = QStringLiteral("Media manifest contains an attachment not referenced by Markdown");
                return false;
            }
            // Pre-chunking managed blobs still require legacy whole-object
            // decryption by MediaSource. Bound this compatibility fallback
            // until those blobs can be converted to managed chunks.
            if (!reference.blobId.isEmpty() && store->containsManagedBlob(reference.blobId)
                && !store->isChunkedManagedBlob(reference.blobId)
                && reference.size > 16LL * 1024 * 1024) {
                *error = QStringLiteral(
                    "Large legacy monolithic media must be migrated to chunked local storage");
                return false;
            }
            MediaStream source(createLocalMediaSource(reference, store));
            if (!source.open(QIODevice::ReadOnly)) {
                *error = QStringLiteral("Could not open local media: ") + source.errorString();
                return false;
            }
            if (source.size() != reference.size) {
                *error = QStringLiteral("Local media size changed before publication");
                return false;
            }
            QByteArray digest;
            const QString problem = hashStream(source, reference.size, &digest);
            if (!problem.isEmpty() || (reference.checksum.size() == 32 && reference.checksum != digest)) {
                *error = problem.isEmpty()
                    ? QStringLiteral("Local media SHA-256 checksum changed") : problem;
                return false;
            }
            probePaths.insert(reference.uri(), QStringLiteral("pending/") + reference.portableName);
            prepared->append({ reference, digest, reference.size });
        }

        QString linkError;
        (void)NextcloudMediaLinks::toNative(body, probePaths, &linkError);
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
                // Modern web-native Notes attachments live under a
                // .attachments.<noteId>/ directory. Notes <= 5.0.2's REST
                // attachment API instead publishes flat files beside the
                // note, so those safe relative links must be recoverable too.
                // Do not import links to other Markdown notes as attachments,
                // and never traverse into a different note's attachment dir.
                const QString prefix = QStringLiteral(".attachments.") + id + QLatin1Char('/');
                const auto paths = NextcloudMediaLinks::referencedNativePaths(restored.note.note.content);
                QStringList nativePaths;
                for (const auto &path : paths) {
                    if (!NextcloudMediaLinks::isSafeNativePath(path)
                        || path.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)
                        || (path.startsWith(QStringLiteral(".attachments.")) && !path.startsWith(prefix)))
                        continue;
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
                                const auto segment
                                    = worker_->readWebDavRange(davPath, bytes.size(), amount, first.etag);
                                if (!segment.ok || segment.totalSize != first.totalSize
                                    || segment.data.size() != amount
                                    || (!first.etag.isEmpty() && !segment.etag.isEmpty()
                                        && first.etag != segment.etag)) {
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
    note.setBackendValue(QStringLiteral("nextcloud.createToken"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces));
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

    const auto mediaReferences = note.media();
    const bool hasMedia = !mediaReferences.isEmpty();
    // Cheap manifest/body consistency checks must remain synchronous,
    // even though hashing and I/O preflight now run on the network worker.
    // This also prevents a malformed draft from reaching the create journal.
    for (const auto &reference : mediaReferences) {
        if (!reference.isValid() || !note.text().contains(reference.uri())) {
            job->fail({ StorageError::Other,
                        tr("Media manifest contains an invalid or unreferenced attachment"), false });
            return job;
        }
    }
    const QString rawToken = note.backendValue(QStringLiteral("anykeep.publicationDraftId")).toString()
        .isEmpty() ? note.backendValue(QStringLiteral("nextcloud.createToken")).toString()
                   : note.backendValue(QStringLiteral("anykeep.publicationDraftId")).toString();
    const QUuid creationUuid(rawToken);
    const QString creationToken = creationUuid.toString(QUuid::WithoutBraces);
    if (hasMedia && note.id().isEmpty() && creationUuid.isNull()) {
        job->fail({ StorageError::Other,
                    tr("Creating Nextcloud media requires a stable, recoverable draft identity"), false });
        return job;
    }
    // Resolve the keychain-backed local key on the application thread.
    // Opening, hashing and transferring large media runs on the worker.
    if (hasMedia) {
        QString keyError;
        if (!mediaStore_->initialize(&keyError)) {
            job->fail({ StorageError::Other, keyError, false });
            return job;
        }
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

    if (hasMedia && !note.id().isEmpty() && local.etag.isEmpty()) {
        job->fail({ StorageError::Conflict, tr("A known ETag is required before publishing Nextcloud media."),
                    false });
        return job;
    }

    const QString oldId = note.id();
    const QString canonicalText = note.text();
    QPointer<NoteSaveJob> guard(job);
    QMetaObject::invokeMethod(
        worker_,
        [this, guard, config, local, oldId, mediaReferences, hasMedia, canonicalText, creationToken]() {
            worker_->setConfig(config);
            MediaSaveResult publication;
            QList<PreparedAttachment> attachments;
            if (hasMedia) {
                QString preparationError;
                if (!prepareAttachments(canonicalText, mediaReferences, mediaStore_,
                                        &attachments, &preparationError)) {
                    publication.note.error = preparationError;
                    publication.localSourceFailure = true;
                }
            }
            QString publishingId = oldId;
            auto publishingLocal = local;
            const bool newMediaNote = hasMedia && oldId.isEmpty();
            if (publication.note.error.isEmpty() && newMediaNote) {
                const auto created = ensureCreatedMediaNote(worker_, config, local, creationToken);
                if (!created.ok) {
                    publication.note = created;
                } else {
                    publishingId = created.note.id;
                    publishingLocal.id = publishingId;
                    publishingLocal.etag = created.note.etag;
                }
            }
            if (!publication.note.error.isEmpty()) {
                // Missing/corrupt local plaintext and ambiguous creation are
                // both failure states: do not publish a partial Markdown body.
            } else if (!hasMedia) {
                publication.note = oldId.isEmpty() ? worker_->createNote(local) : worker_->updateNote(local);
            } else {
                // Check before staging data. Recheck the ETag again atomically
                // during the final updateNote() to handle concurrent edits.
                const auto current = worker_->getNote(publishingId);
                if (!current.ok) {
                    publication.note = current;
                } else if (current.note.etag.isEmpty()) {
                    publication.note.error = QStringLiteral(
                        "The Nextcloud note has no usable ETag for safe media publication");
                } else if (!publishingLocal.etag.isEmpty()
                           && current.note.etag != publishingLocal.etag) {
                    publication.note.conflict = true;
                    publication.note.httpStatus = 412;
                    publication.note.remoteOnConflict = current.note;
                    publication.note.error = QStringLiteral("The Nextcloud note changed before media publication");
                } else if (current.note.category != publishingLocal.category) {
                    // Staging inside the destination category before Notes has
                    // moved the source note risks splitting its attachments
                    // between the old and new directories. Require the caller
                    // to finish a separate safe category-move operation first.
                    publication.note.conflict = true;
                    publication.note.httpStatus = 412;
                    publication.note.remoteOnConflict = current.note;
                    publication.note.error = QStringLiteral(
                        "Move the Nextcloud note to its destination category before publishing media");
                } else {
                    QHash<QString, QString> nativePaths;
                    // A recovered POST may already have completed its final
                    // Markdown PUT before the last response was lost. Do not
                    // blindly update that document again (or overwrite an
                    // external edit). Compare with the deterministic native
                    // body, and still verify each remote attachment.
                    QHash<QString, QString> expectedPaths;
                    for (const auto &attachment : attachments) {
                        expectedPaths.insert(attachment.reference.uri(),
                            QStringLiteral(".attachments.") + publishingId + QLatin1Char('/')
                                + stableAttachmentFileName(attachment));
                    }
                    QString expectedError;
                    const QString expectedNative = NextcloudMediaLinks::toNative(
                        canonicalText, expectedPaths, &expectedError);
                    const bool alreadyCommitted = newMediaNote && expectedError.isEmpty()
                        && current.note.contentPresent && current.note.content == expectedNative
                        && current.note.favorite == publishingLocal.favorite;
                    if (newMediaNote && !alreadyCommitted
                        && (!current.note.contentPresent
                            || !current.note.content.contains(creationMarker(creationToken)))) {
                        publication.note.conflict = true;
                        publication.note.httpStatus = 412;
                        publication.note.remoteOnConflict = current.note;
                        publication.note.error = QStringLiteral(
                            "The recoverable Nextcloud note was changed before media publication");
                    }
                    NextcloudSettingsResult settings;
                    if (publication.note.error.isEmpty()) {
                        settings = worker_->getSettings();
                        if (!settings.ok) {
                            publication.note.error = settings.error;
                            publication.note.httpStatus = settings.httpStatus;
                        }
                    }
                    if (publication.note.error.isEmpty()) {
                        const QString davFolder = attachmentDirectory(settings.notesPath, publishingLocal.category, publishingId);
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
                                MediaStream source(createLocalMediaSource(attachment.reference, mediaStore_));
                                if (!source.open(QIODevice::ReadOnly) || source.size() != attachment.size) {
                                    publication.note.error = QStringLiteral(
                                        "Could not open the prepared local media stream or its size changed");
                                    publication.localSourceFailure = true;
                                    break;
                                }
                                QByteArray actualDigest;
                                const QString localError = hashStream(source, attachment.size, &actualDigest);
                                if (!localError.isEmpty() || actualDigest != attachment.sha256) {
                                    publication.note.error = localError.isEmpty()
                                        ? QStringLiteral("Local media changed after preflight")
                                        : localError;
                                    publication.localSourceFailure = true;
                                    break;
                                }
                                const QString receiptPath = attachment.reference.remoteData
                                    .value(QStringLiteral("nextcloud.path")).toString();
                                const QString receiptNote = attachment.reference.remoteData
                                    .value(QStringLiteral("nextcloud.noteId")).toString();
                                const QString ownedPrefix = QStringLiteral(".attachments.") + publishingId
                                    + QLatin1Char('/');
                                QString nativePath = ownedPrefix + stableAttachmentFileName(attachment);
                                bool verified = false;
                                if (receiptNote == publishingId && receiptPath.startsWith(ownedPrefix)
                                    && NextcloudMediaLinks::isSafeNativePath(receiptPath)) {
                                    const QString previousDav = attachmentDirectory(
                                        settings.notesPath, publishingLocal.category, publishingId)
                                        + QLatin1Char('/') + receiptPath.mid(ownedPrefix.size());
                                    bool missing = false;
                                    QString error = verifyNativeBytes(worker_, previousDav, source, attachment.size, attachment.sha256,
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
                                    const QString before = verifyNativeBytes(worker_, davPath, source, attachment.size, attachment.sha256,
                                                                             &missing, &publication.note.httpStatus);
                                    if (before.isEmpty()) {
                                        verified = true; // recover a previously staged upload
                                    } else if (!missing) {
                                        publication.note.conflict = true;
                                        publication.note.error = before;
                                        break;
                                    }

                                    if (!verified) {
                                        if (!source.seek(0)) {
                                            publication.note.error = QStringLiteral(
                                                "Could not rewind authenticated media before upload");
                                            publication.localSourceFailure = true;
                                            break;
                                        }
                                        const auto uploaded
                                            = worker_->uploadWebDavChunks(davPath, &source, attachment.size);
                                        if (!uploaded.ok) {
                                            // A racing identical upload might win the
                                            // atomic no-overwrite MOVE.
                                            const QString check = verifyNativeBytes(
                                                worker_, davPath, source, attachment.size, attachment.sha256, nullptr,
                                                &publication.note.httpStatus);
                                            if (!check.isEmpty()) {
                                                publication.note.error = uploaded.error;
                                                publication.note.httpStatus = uploaded.httpStatus;
                                                break;
                                            }
                                        }
                                        const QString check = verifyNativeBytes(worker_, davPath, source, attachment.size, attachment.sha256,
                                                                                 nullptr, &publication.note.httpStatus);
                                        if (!check.isEmpty()) {
                                            publication.note.error = check;
                                            break;
                                        }
                                    }
                                }

                                nativePaths.insert(attachment.reference.uri(), nativePath);
                                auto publishedReference = attachment.reference;
                                publishedReference.remoteData.insert(QStringLiteral("nextcloud.noteId"), publishingId);
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
                            if (alreadyCommitted) {
                                publication.note = current;
                            } else {
                                outgoing.id = publishingId;
                                // POST responses may omit an ETag. GET is the
                                // authoritative precondition in that case;
                                // never issue an unguarded initial PUT.
                                outgoing.etag = publishingLocal.etag.isEmpty()
                                    ? current.note.etag : publishingLocal.etag;
                                publication.note = worker_->updateNote(outgoing);
                            }
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
                        StorageError error {
                            result.conflict ? StorageError::Conflict
                                           : publication.localSourceFailure ? StorageError::Other
                                                                            : StorageError::Network,
                            result.error, !result.conflict && !publication.localSourceFailure
                        };
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
            // Notes 5.0.2 does not move sibling files uploaded by its old
            // attachment API. Such relative links would break on a category
            // change; reject the move before touching the remote note.
            const auto current = worker_->getNote(noteId);
            NextcloudNoteResult result;
            bool unsafeLegacyLinks = false;
            if (!current.ok) {
                result = current;
            } else if (current.note.etag != etag) {
                result.conflict = true;
                result.httpStatus = 412;
                result.remoteOnConflict = current.note;
                result.error = QStringLiteral("The note changed before its category move");
            } else {
                const QString prefix = QStringLiteral(".attachments.") + noteId + QLatin1Char('/');
                const auto referenced = NextcloudMediaLinks::referencedNativePaths(current.note.content);
                for (const QString &path : referenced) {
                    if (!path.startsWith(prefix)) {
                        unsafeLegacyLinks = true;
                        result.error = QStringLiteral(
                            "This note has legacy relative files that would break after a category move. "
                            "Move the attachments into its native per-note folder first.");
                        break;
                    }
                }
                if (!unsafeLegacyLinks)
                    result = worker_->updateNoteCategory(noteId, category, etag);
            }
            QMetaObject::invokeMethod(
                this,
                [this, guard, result, noteId, folderId, unsafeLegacyLinks]() {
                    if (!guard || guard->isFinished())
                        return;
                    if (!result.ok) {
                        if (result.remoteOnConflict) {
                            reconcileRemoteFolders({ *result.remoteOnConflict });
                            cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                        }
                        guard->fail({ unsafeLegacyLinks ? StorageError::Other
                                                       : result.conflict ? StorageError::Conflict
                                                                         : StorageError::Network,
                                      result.error, !result.conflict && !unsafeLegacyLinks });
                        return;
                    }

                    reconcileRemoteFolders({ result.note });
                    auto changed = fromRemote(result.note);
                    const auto cached = cache_.value(noteId);
                    if (!cached.isNull() && cached.isLoaded()) {
                        // A metadata-only category move must not downgrade the
                        // verified canonical media manifest into raw native URLs
                        // just because the Notes response happens to include
                        // the Markdown body.
                        QHash<QString, QString> nativePaths;
                        for (const auto &ref : cached.media()) {
                            const QString path = ref.remoteData.value(QStringLiteral("nextcloud.path")).toString();
                            if (NextcloudMediaLinks::isSafeNativePath(path))
                                nativePaths.insert(ref.uri(), path);
                        }
                        QString linkError;
                        const QString canonicalAsNative
                            = NextcloudMediaLinks::toNative(cached.text(), nativePaths, &linkError);
                        if (linkError.isEmpty() && (!result.note.contentPresent
                                                     || result.note.content == canonicalAsNative)) {
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
