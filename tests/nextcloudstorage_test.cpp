#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "nextcloudstorage.h"
#include "mediarangeservice.h"
#include "localmediastore.h"
#include "secureenvelope.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QLockFile>
#include <QStandardPaths>
#include <QFile>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

using namespace AnyKeep;

namespace {

class ScriptedHttpServer final : public QObject {
public:
    explicit ScriptedHttpServer(QList<QByteArray> responses) : responses_(std::move(responses))
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            auto *socket = server_.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { readRequest(socket); });
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }

    const QList<QByteArray> &requests() const { return requests_; }

    // Hold an HTTP response until the test has changed storage state. This
    // avoids relying on races with a fast loopback WebDAV server.
    void holdResponseOnRequest(int ordinal) { holdAtRequest_ = ordinal; }
    bool hasHeldResponse() const { return !heldSocket_.isNull(); }
    void releaseHeldResponse()
    {
        if (!heldSocket_)
            return;
        QTcpSocket *socket = heldSocket_.data();
        heldSocket_.clear();
        writeResponse(socket, std::exchange(heldResponse_, {}));
    }

private:
    static void writeResponse(QTcpSocket *socket, const QByteArray &body)
    {
        if (body.startsWith("HTTP/1.1 "))
            socket->write(body);
        else
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    static qsizetype contentLength(const QByteArray &headers)
    {
        for (const auto &line : headers.split('\n')) {
            const auto trimmed    = line.trimmed();
            const auto headerName = QByteArrayLiteral("Content-Length:");
            if (trimmed.left(headerName.size()).compare(headerName, Qt::CaseInsensitive) == 0)
                return trimmed.mid(headerName.size()).trimmed().toLongLong();
        }
        return 0;
    }

    void readRequest(QTcpSocket *socket)
    {
        auto &request = pending_[socket];
        request += socket->readAll();
        const auto headerEnd = request.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const auto totalLength = headerEnd + 4 + contentLength(request.left(headerEnd));
        if (request.size() < totalLength)
            return;

        requests_.append(request.left(totalLength));
        pending_.remove(socket);
        QVERIFY2(!responses_.isEmpty(), "Unexpected Nextcloud request");
        const auto body = responses_.takeFirst();
        if (requests_.size() == holdAtRequest_) {
            heldSocket_ = socket;
            heldResponse_ = body;
            return;
        }
        writeResponse(socket, body);
    }

    QTcpServer                      server_;
    QList<QByteArray>               responses_;
    QList<QByteArray>               requests_;
    QHash<QTcpSocket *, QByteArray> pending_;
    int                            holdAtRequest_ { -1 };
    QPointer<QTcpSocket>           heldSocket_;
    QByteArray                    heldResponse_;
};

QByteArray noteJson(const QString &id, const QString &etag, const QString &category, qint64 modified)
{
    return QJsonDocument(QJsonObject { { QStringLiteral("id"), id },
                                       { QStringLiteral("etag"), etag },
                                       { QStringLiteral("title"), QStringLiteral("Remote note") },
                                       { QStringLiteral("category"), category },
                                       { QStringLiteral("favorite"), false },
                                       { QStringLiteral("readonly"), false },
                                       { QStringLiteral("modified"), modified } })
        .toJson(QJsonDocument::Compact);
}

QByteArray remoteByteRangeResponse(qint64 offset, qint64 count, qint64 total,
                                  const QByteArray &etag = QByteArrayLiteral("\"remote-revision\""))
{
    const QByteArray bytes(count, 'R');
    return QByteArrayLiteral("HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n")
        + QByteArrayLiteral("Accept-Ranges: bytes\r\n")
        + (etag.isEmpty() ? QByteArray() : QByteArrayLiteral("ETag: ") + etag + QByteArrayLiteral("\r\n"))
        + QByteArrayLiteral("Content-Range: bytes ") + QByteArray::number(offset)
        + '-' + QByteArray::number(offset + count - 1) + '/'
        + QByteArray::number(total) + QByteArrayLiteral("\r\nContent-Length: ")
        + QByteArray::number(count) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + bytes;
}

QByteArray noteWithContent(const QString &id, const QString &etag, const QString &title,
                           const QString &body, const QString &category = {})
{
    auto object = QJsonDocument::fromJson(noteJson(id, etag, category, 1700000000)).object();
    object.insert(QStringLiteral("title"), title);
    object.insert(QStringLiteral("content"), body);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray httpError(int status, const QByteArray &label)
{
    return QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' + label
        + QByteArrayLiteral("\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

QByteArray byteRangeResponse(const QByteArray &bytes, qint64 start, qint64 count,
                             const QByteArray &etag = {})
{
    const QByteArray data = bytes.mid(start, count);
    return QByteArrayLiteral("HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n")
        + QByteArrayLiteral("Content-Range: bytes ") + QByteArray::number(start) + '-'
        + QByteArray::number(start + data.size() - 1) + '/' + QByteArray::number(bytes.size())
        + QByteArrayLiteral("\r\nContent-Length: ") + QByteArray::number(data.size())
        + (etag.isEmpty() ? QByteArray() : QByteArrayLiteral("\r\nETag: ") + etag)
        + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + data;
}

FolderCatalogResult<QUuid> addFolder(FolderCatalogManager &manager, const QString &name, const QUuid &parent = {})
{
    FolderRecord record;
    record.name     = name;
    record.parentId = parent;
    return manager.addFolder(record);
}

} // namespace

class NextcloudStorageTest : public QObject {
    Q_OBJECT

private slots:
    void metadataOnlyFolderMoveUsesCategoryPath();
    void legacyMediaLinksPreventUnsafeFolderMove();
    void dirtyPublishIncludesFolderCategory();
    void mediaIsUploadedAndVerifiedBeforeMarkdownCommit();
    void newNoteWithMediaGetsServerIdBeforeAttachmentUpload();
    void ambiguousCreateIsRecoveredAfterStorageRestart();
    void ambiguousCreateNeverRetriesPostWhenIdentityIsNotVisible();
    void interruptedWebDavUploadResumesWithoutDuplicateNote();
    void simultaneousCreateClaimCannotIssueSecondPost();
    void successfulRemoteCommitSurvivesLostMarkdownAcknowledgment();
    void recoveredCreationRejectsUnrelatedNativeEdit_data();
    void recoveredCreationRejectsUnrelatedNativeEdit();
    void streamPublishWithoutWholePlaintextBuffer_data();
    void streamPublishWithoutWholePlaintextBuffer();
    void encryptedNextcloudPartialCacheSurvivesRestartAndTamper();
    void shutdownRejectsPreviouslyIssuedMediaRangeUrl();
    void concurrentRangeReadsReuseOneVerifiedWebDavChunk();
    void accountSwitchDuringRangeReadRejectsStaleBytes();
    void reentrantStorageOperationCannotChangePinnedWebDavAccount();
    void oversizedNativeAttachmentStreamsRemoteWebDavRanges_data();
    void oversizedNativeAttachmentStreamsRemoteWebDavRanges();
    void nativeLinksRestoreEncryptedLocalMediaViaWebDavRanges_data();
    void nativeLinksRestoreEncryptedLocalMediaViaWebDavRanges();
    void staleEtagPreventsAttachmentStaging_data();
    void staleEtagPreventsAttachmentStaging();
    void existingMediaReceiptIsReusedOrRejected_data();
    void existingMediaReceiptIsReusedOrRejected();
};

void NextcloudStorageTest::metadataOnlyFolderMoveUsesCategoryPath()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(catalog.initialize());
    const auto projects = addFolder(catalog, QStringLiteral("Projects"));
    QVERIFY(projects);
    const auto destination = addFolder(catalog, QStringLiteral("2026"), projects.value);
    QVERIFY(destination);

    ScriptedHttpServer server(
        { QByteArrayLiteral("[]"), QByteArrayLiteral("[]"),
          QByteArrayLiteral("[")
              + noteJson(QStringLiteral("note-1"), QStringLiteral("old-etag"), QStringLiteral("Inbox"), 1700000000)
              + QByteArrayLiteral("]"),
          noteJson(QStringLiteral("note-1"), QStringLiteral("old-etag"), QStringLiteral("Inbox"), 1700000000),
          noteJson(QStringLiteral("note-1"), QStringLiteral("new-etag"), QStringLiteral("Projects/2026"),
                   2100000000) });
    QVERIFY(server.listen());

    QSettings  settings;
    const auto clearSettings = [&settings]() {
        settings.remove(QStringLiteral("storage.nextcloud.url"));
        settings.remove(QStringLiteral("storage.nextcloud.username"));
        settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
        settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
    };
    clearSettings();
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    NextcloudStorage storage(nullptr, &catalog);
    auto            *init = storage.initAsync();
    QTRY_VERIFY(init->isFinished());
    QCOMPARE(init->state(), StorageJob::Succeeded);
    init->deleteLater();

    auto *listed = storage.refreshNotesAsync();
    QTRY_VERIFY(listed->isFinished());
    QCOMPARE(listed->state(), StorageJob::Succeeded);
    QCOMPARE(listed->result().size(), 1);
    auto note = listed->result().first();
    listed->deleteLater();
    QVERIFY(!note.folderId().isNull());
    note.setFolderId(destination.value);

    auto *moved = storage.changeNoteFolderAsync(note);
    QTRY_VERIFY(moved->isFinished());
    QCOMPARE(moved->state(), StorageJob::Succeeded);
    QCOMPARE(moved->result().folderId(), destination.value);
    QCOMPARE(catalog.catalog().folderForNote(storage.systemName(), QStringLiteral("note-1")), destination.value);
    moved->deleteLater();

    QCOMPARE(server.requests().size(), 5);
    const auto request   = server.requests().last();
    const auto headerEnd = request.indexOf("\r\n\r\n");
    QVERIFY(headerEnd >= 0);
    const auto headers = request.left(headerEnd);
    QVERIFY(headers.startsWith("PUT /index.php/apps/notes/api/v1/notes/note-1 HTTP/1.1\r\n"));
    QVERIFY(headers.contains("If-Match: \"old-etag\"\r\n"));
    const auto body = QJsonDocument::fromJson(request.mid(headerEnd + 4));
    QVERIFY(body.isObject());
    QCOMPARE(body.object().keys(), QStringList({ QStringLiteral("category") }));
    QCOMPARE(body.object().value(QStringLiteral("category")).toString(), QStringLiteral("Projects/2026"));

    clearSettings();
}

void NextcloudStorageTest::legacyMediaLinksPreventUnsafeFolderMove()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(catalog.initialize());
    const auto target = addFolder(catalog, QStringLiteral("Destination"));
    QVERIFY(target);

    QJsonObject remote = QJsonDocument::fromJson(
        noteJson(QStringLiteral("42"), QStringLiteral("old-etag"), QStringLiteral("Origin"), 1700000000)).object();
    remote.insert(QStringLiteral("content"),
                  QStringLiteral("# With legacy API image\n![image](old-api-image.png)"));
    ScriptedHttpServer server({ QJsonDocument(remote).toJson(QJsonDocument::Compact) });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    NextcloudStorage storage(nullptr, &catalog);
    auto note = storage.createNote();
    note.setId(QStringLiteral("42"));
    note.setBackendValue(QStringLiteral("etag"), QStringLiteral("old-etag"));
    note.setFolderId(target.value);
    auto *job = storage.changeNoteFolderAsync(note);
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Failed);
    QCOMPARE(job->error().code, StorageError::Other);
    QVERIFY(job->error().message.contains(QStringLiteral("legacy")));
    QCOMPARE(server.requests().size(), 1);
    QVERIFY(server.requests().first().startsWith("GET /index.php/apps/notes/api/v1/notes/42 "));
    job->deleteLater();

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::dirtyPublishIncludesFolderCategory()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(catalog.initialize());
    const auto projects = addFolder(catalog, QStringLiteral("Projects"));
    QVERIFY(projects);
    const auto destination = addFolder(catalog, QStringLiteral("2026"), projects.value);
    QVERIFY(destination);

    ScriptedHttpServer server({ noteJson(QStringLiteral("new-note"), QStringLiteral("new-etag"),
                                         QStringLiteral("Projects/2026"), 2100000000) });
    QVERIFY(server.listen());

    QSettings settings;
    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    NextcloudStorage storage(nullptr, &catalog);
    QVERIFY(storage.supportsFavorite());
    auto note = storage.createNote();
    note.setTitle(QStringLiteral("Draft title"));
    note.setText(QStringLiteral("Draft body"), Note::Markdown);
    note.setFolderId(destination.value);
    note.setFavorite(true);
    auto *saved = storage.saveNoteAsync(note);
    QTRY_VERIFY(saved->isFinished());
    QCOMPARE(saved->state(), StorageJob::Succeeded);
    QCOMPARE(saved->result().folderId(), destination.value);
    QCOMPARE(catalog.catalog().folderForNote(storage.systemName(), QStringLiteral("new-note")), destination.value);
    saved->deleteLater();

    QCOMPARE(server.requests().size(), 1);
    const auto request   = server.requests().first();
    const auto headerEnd = request.indexOf("\r\n\r\n");
    QVERIFY(headerEnd >= 0);
    QVERIFY(request.left(headerEnd).startsWith("POST /index.php/apps/notes/api/v1/notes HTTP/1.1\r\n"));
    const auto body = QJsonDocument::fromJson(request.mid(headerEnd + 4));
    QVERIFY(body.isObject());
    QCOMPARE(body.object().value(QStringLiteral("category")).toString(), QStringLiteral("Projects/2026"));
    QCOMPARE(body.object().value(QStringLiteral("content")).toString(), QStringLiteral("Draft body"));
    QVERIFY(body.object().value(QStringLiteral("favorite")).toBool());

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::mediaIsUploadedAndVerifiedBeforeMarkdownCommit()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());

    const QByteArray bytes = QByteArrayLiteral("image-content-verified");
    const auto imported = mediaStore.importData(bytes, QStringLiteral("image.png"),
                                                QStringLiteral("image/png"));
    QVERIFY2(imported, qPrintable(imported.error));
    const auto reference = imported.value;
    const QString nativePath = QStringLiteral(".attachments.42/")
        + reference.id.toString(QUuid::WithoutBraces) + QLatin1Char('-')
        + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                                  .toHex().left(24)) + QStringLiteral(".png");

    ScriptedHttpServer server({
        noteJson(QStringLiteral("42"), QStringLiteral("old-etag"), QString(), 1700000000),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        QByteArrayLiteral("{}"), // MKCOL note's attachment directory
        httpError(404, QByteArrayLiteral("Not Found")), // no previous staged content
        QByteArrayLiteral("{}"), // MKCOL chunk staging collection
        QByteArrayLiteral("{}"), // PUT the chunk
        QByteArrayLiteral("{}"), // MOVE the assembled file
        byteRangeResponse(bytes, 0, bytes.size()), // verify published plaintext
        noteJson(QStringLiteral("42"), QStringLiteral("new-etag"), QString(), 2100000000)
    });
    QVERIFY(server.listen());

    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto note = storage.createNote();
    note.setId(QStringLiteral("42"));
    note.setBackendValue(QStringLiteral("etag"), QStringLiteral("old-etag"));
    const QString canonical = QStringLiteral("![image](") + reference.uri() + QLatin1Char(')');
    note.setText(canonical, Note::Markdown);
    note.setMedia({ reference });

    auto *saved = storage.saveNoteAsync(note);
    QTRY_VERIFY(saved->isFinished());
    QCOMPARE(saved->state(), StorageJob::Succeeded);
    QCOMPARE(saved->result().text(), canonical);
    QCOMPARE(saved->result().media().size(), 1);
    QCOMPARE(saved->result().media().first().remoteData.value(QStringLiteral("nextcloud.path")).toString(),
             nativePath);
    saved->deleteLater();

    QCOMPARE(server.requests().size(), 9);
    QVERIFY(server.requests().at(0).startsWith("GET /index.php/apps/notes/api/v1/notes/42 "));
    QVERIFY(server.requests().at(1).startsWith("GET /index.php/apps/notes/api/v1/settings "));
    QVERIFY(server.requests().at(2).startsWith(
        "MKCOL /remote.php/dav/files/alice/Notes/.attachments.42 "));
    QVERIFY(server.requests().at(3).startsWith(
        "GET /remote.php/dav/files/alice/Notes/.attachments.42/"));
    QVERIFY(server.requests().at(4).startsWith("MKCOL /remote.php/dav/uploads/alice/"));
    QVERIFY(server.requests().at(5).startsWith("PUT /remote.php/dav/uploads/alice/"));
    QVERIFY(server.requests().at(6).startsWith("MOVE /remote.php/dav/uploads/alice/"));
    QVERIFY(server.requests().at(7).contains("Range: bytes=0-") );
    const QByteArray request = server.requests().at(8);
    QVERIFY(request.startsWith("PUT /index.php/apps/notes/api/v1/notes/42 "));
    QVERIFY(request.contains("If-Match: \"old-etag\"\r\n"));
    const auto marker = request.indexOf("\r\n\r\n");
    QVERIFY(marker > 0);
    const auto payload = QJsonDocument::fromJson(request.mid(marker + 4));
    QVERIFY(payload.isObject());
    QCOMPARE(payload.object().value(QStringLiteral("content")).toString(),
             QStringLiteral("![image](") + nativePath + QLatin1Char(')'));

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::newNoteWithMediaGetsServerIdBeforeAttachmentUpload()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const QByteArray bytes = QByteArrayLiteral("first-new-note-media");
    const auto media = mediaStore.importData(bytes, QStringLiteral("new.png"), QStringLiteral("image/png"));
    QVERIFY2(media, qPrintable(media.error));

    // Stable creation identity is generated before the async save and belongs
    // to the draft rather than to the eventual server-assigned note ID.
    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto note = storage.createNote();
    const QString token = note.backendValue(QStringLiteral("nextcloud.createToken")).toString();
    QVERIFY(!QUuid(token).isNull());
    const QString marker = QStringLiteral("<!-- anykeep-media-create:") + token + QStringLiteral(" -->");
    const QString pendingTitle = QStringLiteral("AnyKeep pending media ") + token;
    const QString pendingBody = QStringLiteral("# ") + pendingTitle + QStringLiteral("\n\n") + marker;
    const QString nativePath = QStringLiteral(".attachments.42/")
        + media.value.id.toString(QUuid::WithoutBraces) + QLatin1Char('-')
        + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                              .toHex().left(24)) + QStringLiteral(".png");
    const QString canonical = QStringLiteral("![new](") + media.value.uri() + QLatin1Char(')');
    const QString nativeBody = QStringLiteral("![new](") + nativePath + QLatin1Char(')');
    ScriptedHttpServer server({
        noteWithContent(QStringLiteral("42"), QStringLiteral("created-etag"), pendingTitle, pendingBody), // POST
        noteWithContent(QStringLiteral("42"), QStringLiteral("created-etag"), pendingTitle, pendingBody), // GET
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"), // settings
        QByteArrayLiteral("{}"), // MKCOL native attachment folder
        httpError(404, QByteArrayLiteral("Not Found")), // existing deterministic file absent
        QByteArrayLiteral("{}"), // MKCOL chunk staging
        QByteArrayLiteral("{}"), // PUT chunk
        QByteArrayLiteral("{}"), // MOVE staged upload
        byteRangeResponse(bytes, 0, bytes.size()), // verify file
        noteWithContent(QStringLiteral("42"), QStringLiteral("final-etag"),
                        QStringLiteral("Final note"), nativeBody) // PUT Markdown
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);
    note.setText(canonical, Note::Markdown);
    note.setMedia({ media.value });
    auto *job = storage.saveNoteAsync(note);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 20000);
    QVERIFY2(job->state() == StorageJob::Succeeded, qPrintable(job->error().message));
    QCOMPARE(job->result().id(), QStringLiteral("42"));
    QCOMPARE(job->result().text(), canonical);
    QCOMPARE(job->result().media().first().remoteData.value(QStringLiteral("nextcloud.path")).toString(),
             nativePath);
    job->deleteLater();

    QCOMPARE(server.requests().size(), 10);
    QVERIFY(server.requests().at(0).startsWith("POST /index.php/apps/notes/api/v1/notes "));
    QVERIFY(server.requests().at(0).contains(marker.toUtf8()));
    QVERIFY(!server.requests().at(0).contains(media.value.uri().toUtf8()));
    QVERIFY(server.requests().at(1).startsWith("GET /index.php/apps/notes/api/v1/notes/42 "));
    QVERIFY(server.requests().at(3).startsWith("MKCOL /remote.php/dav/files/alice/Notes/.attachments.42 "));
    QVERIFY(server.requests().last().startsWith("PUT /index.php/apps/notes/api/v1/notes/42 "));
    QVERIFY(server.requests().last().contains(nativePath.toUtf8()));

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::ambiguousCreateIsRecoveredAfterStorageRestart()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const QByteArray bytes = QByteArrayLiteral("crash-recovery-image");
    const auto media = mediaStore.importData(bytes, QStringLiteral("recover.png"),
                                            QStringLiteral("image/png"));
    QVERIFY2(media, qPrintable(media.error));
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString marker = QStringLiteral("<!-- anykeep-media-create:") + token + QStringLiteral(" -->");
    const QString pendingTitle = QStringLiteral("AnyKeep pending media ") + token;
    const QString pendingBody = QStringLiteral("# ") + pendingTitle + QStringLiteral("\n\n") + marker;
    const QString nativePath = QStringLiteral(".attachments.74/")
        + media.value.id.toString(QUuid::WithoutBraces) + QLatin1Char('-')
        + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                              .toHex().left(24)) + QStringLiteral(".png");
    const QByteArray remote = noteWithContent(QStringLiteral("74"), QStringLiteral("etag-start"),
                                              pendingTitle, pendingBody);
    const QByteArray listed = QByteArrayLiteral("[") + remote + QByteArrayLiteral("]");
    ScriptedHttpServer server({
        httpError(503, QByteArrayLiteral("Service Unavailable")), // ambiguous POST
        listed, // metadata search after restart
        remote, // verify candidate body marker
        remote, // normal ETag guard
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        QByteArrayLiteral("{}"), // MKCOL native folder
        httpError(404, QByteArrayLiteral("Not Found")),
        QByteArrayLiteral("{}"), // upload collection
        QByteArrayLiteral("{}"), // chunk
        QByteArrayLiteral("{}"), // MOVE
        byteRangeResponse(bytes, 0, bytes.size()),
        noteWithContent(QStringLiteral("74"), QStringLiteral("etag-done"),
                        QStringLiteral("Recovered"), QStringLiteral("![x](") + nativePath + QLatin1Char(')'))
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    const QString canonical = QStringLiteral("![x](") + media.value.uri() + QLatin1Char(')');
    {
        NextcloudStorage storage(nullptr, nullptr, &mediaStore);
        auto draft = storage.createNote();
        draft.setBackendValue(QStringLiteral("anykeep.publicationDraftId"), token);
        draft.setText(canonical, Note::Markdown);
        draft.setMedia({ media.value });
        auto *job = storage.saveNoteAsync(draft);
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
        QCOMPARE(job->state(), StorageJob::Failed);
        QCOMPARE(job->error().code, StorageError::Network);
        job->deleteLater();
    }
    {
        NextcloudStorage restarted(nullptr, nullptr, &mediaStore);
        auto draft = restarted.createNote();
        draft.setBackendValue(QStringLiteral("anykeep.publicationDraftId"), token);
        draft.setText(canonical, Note::Markdown);
        draft.setMedia({ media.value });
        auto *job = restarted.saveNoteAsync(draft);
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 20000);
        QVERIFY2(job->state() == StorageJob::Succeeded, qPrintable(job->error().message));
        QCOMPARE(job->result().id(), QStringLiteral("74"));
        QCOMPARE(job->result().text(), canonical);
        job->deleteLater();
    }
    int creates = 0;
    for (const auto &request : server.requests())
        if (request.startsWith("POST /index.php/apps/notes/api/v1/notes "))
            ++creates;
    QCOMPARE(creates, 1); // an ambiguous POST can NEVER be repeated

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::ambiguousCreateNeverRetriesPostWhenIdentityIsNotVisible()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const auto media = mediaStore.importData(QByteArrayLiteral("a"), QStringLiteral("x.png"),
                                            QStringLiteral("image/png"));
    QVERIFY2(media, qPrintable(media.error));
    ScriptedHttpServer server({ httpError(503, QByteArrayLiteral("Service Unavailable")),
                                QByteArrayLiteral("[]") });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);
    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto draft = storage.createNote();
    draft.setText(QStringLiteral("![x](") + media.value.uri() + QLatin1Char(')'), Note::Markdown);
    draft.setMedia({ media.value });
    for (int index = 0; index < 2; ++index) {
        auto *job = storage.saveNoteAsync(draft);
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
        QCOMPARE(job->state(), StorageJob::Failed);
        QCOMPARE(job->error().code, StorageError::Network);
        job->deleteLater();
    }
    QCOMPARE(server.requests().size(), 2);
    QVERIFY(server.requests().first().startsWith("POST /index.php/apps/notes/api/v1/notes "));
    QVERIFY(server.requests().last().startsWith("GET /index.php/apps/notes/api/v1/notes?"));
    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::interruptedWebDavUploadResumesWithoutDuplicateNote()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const QByteArray bytes = QByteArrayLiteral("retry-after-upload-failure");
    const auto media = mediaStore.importData(bytes, QStringLiteral("retry.png"),
                                            QStringLiteral("image/png"));
    QVERIFY2(media, qPrintable(media.error));

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString pendingTitle = QStringLiteral("AnyKeep pending media ") + token;
    const QString placeholder = QStringLiteral("# ") + pendingTitle + QStringLiteral("\n\n")
        + QStringLiteral("<!-- anykeep-media-create:") + token + QStringLiteral(" -->");
    const QByteArray pending = noteWithContent(QStringLiteral("91"), QStringLiteral("etag-1"),
                                               pendingTitle, placeholder);
    const QString nativePath = QStringLiteral(".attachments.91/")
        + media.value.id.toString(QUuid::WithoutBraces) + QLatin1Char('-')
        + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                              .toHex().left(24)) + QStringLiteral(".png");
    const QString canonical = QStringLiteral("![retry](") + media.value.uri() + QLatin1Char(')');
    const QString native = QStringLiteral("![retry](") + nativePath + QLatin1Char(')');
    ScriptedHttpServer server({
        pending, // POST creates remote identity
        pending, // GET ETag preflight
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        QByteArrayLiteral("{}"), // MKCOL native folder
        httpError(404, QByteArrayLiteral("Not Found")), // no remote file yet
        httpError(503, QByteArrayLiteral("Service Unavailable")), // staging MKCOL fails
        httpError(404, QByteArrayLiteral("Not Found")), // retry read-back of failed upload
        pending, // retry finds previously journaled server ID
        pending, // guarded ETag check
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        QByteArrayLiteral("{}"), // native folder already exists
        httpError(404, QByteArrayLiteral("Not Found")),
        QByteArrayLiteral("{}"), // new upload staging MKCOL
        QByteArrayLiteral("{}"), // PUT chunk
        QByteArrayLiteral("{}"), // MOVE final file
        byteRangeResponse(bytes, 0, bytes.size()),
        noteWithContent(QStringLiteral("91"), QStringLiteral("etag-2"),
                        QStringLiteral("Recovered note"), native)
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto draft = storage.createNote();
    draft.setBackendValue(QStringLiteral("anykeep.publicationDraftId"), token);
    draft.setText(canonical, Note::Markdown);
    draft.setMedia({ media.value });
    auto *first = storage.saveNoteAsync(draft);
    QTRY_VERIFY_WITH_TIMEOUT(first->isFinished(), 12000);
    QCOMPARE(first->state(), StorageJob::Failed);
    QCOMPARE(first->error().code, StorageError::Network);
    first->deleteLater();

    auto *again = storage.saveNoteAsync(draft);
    QTRY_VERIFY_WITH_TIMEOUT(again->isFinished(), 20000);
    QVERIFY2(again->state() == StorageJob::Succeeded, qPrintable(again->error().message));
    QCOMPARE(again->result().id(), QStringLiteral("91"));
    QCOMPARE(again->result().text(), canonical);
    QCOMPARE(again->result().media().first().remoteData
                 .value(QStringLiteral("nextcloud.path")).toString(), nativePath);
    again->deleteLater();

    QCOMPARE(server.requests().size(), 17);
    int creates = 0, nativeWrites = 0;
    for (const auto &request : server.requests()) {
        if (request.startsWith("POST /index.php/apps/notes/api/v1/notes "))
            ++creates;
        if (request.startsWith("PUT /index.php/apps/notes/api/v1/notes/91 "))
            ++nativeWrites;
    }
    QCOMPARE(creates, 1);
    QCOMPARE(nativeWrites, 1);

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::simultaneousCreateClaimCannotIssueSecondPost()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const auto media = mediaStore.importData(QByteArrayLiteral("picture"), QStringLiteral("image.png"),
                                            QStringLiteral("image/png"));
    QVERIFY2(media, qPrintable(media.error));

    ScriptedHttpServer server({ httpError(503, QByteArrayLiteral("Service Unavailable")) });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    // Claim the exact account+draft lock the worker will use. A different
    // process holding it is never permission to send a speculative POST.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString account = server.url().toString(QUrl::RemoveQuery | QUrl::RemoveFragment)
        + QLatin1Char('\n') + QStringLiteral("alice");
    const QByteArray accountHash = QCryptographicHash::hash(account.toUtf8(),
                                                           QCryptographicHash::Sha256).toHex();
    const QString key = QStringLiteral("storage.nextcloud.creationJournal/")
        + QString::fromLatin1(accountHash) + QLatin1Char('/') + token;
    const QByteArray lockHash = QCryptographicHash::hash(key.toUtf8(),
                                                       QCryptographicHash::Sha256).toHex();
    const QString lockDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/nextcloud-create-locks");
    QVERIFY(QDir().mkpath(lockDir));
    QLockFile firstClaim(lockDir + QLatin1Char('/') + QString::fromLatin1(lockHash)
                         + QStringLiteral(".lock"));
    firstClaim.setStaleLockTime(0);
    QVERIFY2(firstClaim.tryLock(), qPrintable(firstClaim.error() == QLockFile::NoError
        ? QStringLiteral("Failed to acquire test creation lock") : QString::number(firstClaim.error())));

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto draft = storage.createNote();
    draft.setBackendValue(QStringLiteral("anykeep.publicationDraftId"), token);
    draft.setText(QStringLiteral("![picture](") + media.value.uri() + QLatin1Char(')'), Note::Markdown);
    draft.setMedia({ media.value });
    auto *job = storage.saveNoteAsync(draft);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
    QCOMPARE(job->state(), StorageJob::Failed);
    QVERIFY(job->error().message.contains(QStringLiteral("Another process")));
    QCOMPARE(server.requests().size(), 0);
    job->deleteLater();
    firstClaim.unlock();

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::successfulRemoteCommitSurvivesLostMarkdownAcknowledgment()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const QByteArray bytes = QByteArrayLiteral("already-on-server");
    const auto media = mediaStore.importData(bytes, QStringLiteral("file.png"),
                                            QStringLiteral("image/png"));
    QVERIFY2(media, qPrintable(media.error));
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString pendingTitle = QStringLiteral("AnyKeep pending media ") + token;
    const QString pendingBody = QStringLiteral("# ") + pendingTitle + QStringLiteral("\n\n")
        + QStringLiteral("<!-- anykeep-media-create:") + token + QStringLiteral(" -->");
    const QString nativePath = QStringLiteral(".attachments.88/")
        + media.value.id.toString(QUuid::WithoutBraces) + QLatin1Char('-')
        + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                              .toHex().left(24)) + QStringLiteral(".png");
    const QString nativeBody = QStringLiteral("![file](") + nativePath + QLatin1Char(')');
    const QString canonical = QStringLiteral("![file](") + media.value.uri() + QLatin1Char(')');
    const auto pending = noteWithContent(QStringLiteral("88"), QStringLiteral("first-etag"),
                                         pendingTitle, pendingBody);
    const auto committed = noteWithContent(QStringLiteral("88"), QStringLiteral("final-etag"),
                                           QStringLiteral("Published"), nativeBody);

    ScriptedHttpServer server({
        pending, // initial POST
        pending, // initial ETag preflight GET
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        QByteArrayLiteral("{}"), // MKCOL native folder
        httpError(404, QByteArrayLiteral("Not Found")),
        QByteArrayLiteral("{}"), // MKCOL upload collection
        QByteArrayLiteral("{}"), // chunk
        QByteArrayLiteral("{}"), // atomic MOVE
        byteRangeResponse(bytes, 0, bytes.size()),
        httpError(503, QByteArrayLiteral("Service Unavailable")), // final PUT applied, ACK lost
        committed, // reload persisted remote ID from journal
        committed, // normal GET guard
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        QByteArrayLiteral("{}"), // native folder already exists
        byteRangeResponse(bytes, 0, bytes.size()) // verify and skip repeated PUT
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto draft = storage.createNote();
    draft.setBackendValue(QStringLiteral("anykeep.publicationDraftId"), token);
    draft.setText(canonical, Note::Markdown);
    draft.setMedia({ media.value });
    auto *first = storage.saveNoteAsync(draft);
    QTRY_VERIFY_WITH_TIMEOUT(first->isFinished(), 15000);
    QCOMPARE(first->state(), StorageJob::Failed);
    QCOMPARE(first->error().code, StorageError::Network);
    first->deleteLater();

    auto *retry = storage.saveNoteAsync(draft);
    QTRY_VERIFY_WITH_TIMEOUT(retry->isFinished(), 15000);
    QVERIFY2(retry->state() == StorageJob::Succeeded, qPrintable(retry->error().message));
    QCOMPARE(retry->result().id(), QStringLiteral("88"));
    QCOMPARE(retry->result().text(), canonical);
    retry->deleteLater();
    QCOMPARE(server.requests().size(), 15);
    int creates = 0, contentWrites = 0;
    for (const auto &request : server.requests()) {
        if (request.startsWith("POST /index.php/apps/notes/api/v1/notes "))
            ++creates;
        if (request.startsWith("PUT /index.php/apps/notes/api/v1/notes/88 "))
            ++contentWrites;
    }
    QCOMPARE(creates, 1);
    QCOMPARE(contentWrites, 1);

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::recoveredCreationRejectsUnrelatedNativeEdit_data()
{
    QTest::addColumn<bool>("preserveMarker");
    QTest::addColumn<bool>("changeFavorite");
    QTest::newRow("native-client-replaces-placeholder") << false << false;
    QTest::newRow("native-client-appends-but-keeps-marker") << true << false;
    QTest::newRow("native-client-changes-favorite") << true << true;
}

void NextcloudStorageTest::recoveredCreationRejectsUnrelatedNativeEdit()
{
    QFETCH(bool, preserveMarker);
    QFETCH(bool, changeFavorite);
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const auto media = mediaStore.importData(QByteArrayLiteral("img"), QStringLiteral("file.png"),
                                            QStringLiteral("image/png"));
    QVERIFY2(media, qPrintable(media.error));
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString pendingTitle = QStringLiteral("AnyKeep pending media ") + token;
    const QString pendingBody = QStringLiteral("# ") + pendingTitle + QStringLiteral("\n\n")
        + QStringLiteral("<!-- anykeep-media-create:") + token + QStringLiteral(" -->");
    const auto pending = noteWithContent(QStringLiteral("88"), QStringLiteral("etag-1"),
                                         pendingTitle, pendingBody);
    auto editedBody = preserveMarker ? pendingBody : QStringLiteral("# Native client's unrelated edit");
    if (preserveMarker && !changeFavorite)
        editedBody += QStringLiteral("\nNative client's addition");
    auto editedObject = QJsonDocument::fromJson(
        noteWithContent(QStringLiteral("88"), QStringLiteral("etag-2"),
                        preserveMarker ? pendingTitle : QStringLiteral("Edited by another client"),
                        editedBody)).object();
    if (changeFavorite)
        editedObject.insert(QStringLiteral("favorite"), true);
    const auto edited = QJsonDocument(editedObject).toJson(QJsonDocument::Compact);
    ScriptedHttpServer server({
        pending, // original create note
        httpError(503, QByteArrayLiteral("Service Unavailable")), // initial get fails
        edited, // persisted ID lookup on retry
        edited // ETag preflight sees unauthorized marker replacement
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto draft = storage.createNote();
    draft.setBackendValue(QStringLiteral("anykeep.publicationDraftId"), token);
    draft.setText(QStringLiteral("![file](") + media.value.uri() + QLatin1Char(')'), Note::Markdown);
    draft.setMedia({ media.value });
    auto *first = storage.saveNoteAsync(draft);
    QTRY_VERIFY_WITH_TIMEOUT(first->isFinished(), 10000);
    QCOMPARE(first->state(), StorageJob::Failed);
    first->deleteLater();

    auto *second = storage.saveNoteAsync(draft);
    QTRY_VERIFY_WITH_TIMEOUT(second->isFinished(), 10000);
    QCOMPARE(second->state(), StorageJob::Failed);
    QVERIFY2(second->error().code == StorageError::Conflict, qPrintable(second->error().message));
    QCOMPARE(second->error().retryable, false);
    second->deleteLater();
    QCOMPARE(server.requests().size(), 4);

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::streamPublishWithoutWholePlaintextBuffer_data()
{
    QTest::addColumn<bool>("externalFile");
    QTest::newRow("large-encrypted-managed-file") << false;
    QTest::newRow("lazy-linked-external-file") << true;
}

void NextcloudStorageTest::streamPublishWithoutWholePlaintextBuffer()
{
    QFETCH(bool, externalFile);
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    // The managed variant is deliberately larger than the old 16 MiB cap;
    // production publishing must not materialize its entire plaintext.
    const qint64 dataSize = externalFile ? 57 * 1024 : 17 * 1024 * 1024 + 31;
    QByteArray bytes(dataSize, char('a'));
    if (dataSize > 1024 * 1024)
        bytes.replace(1024 * 1024 - 4, 8, QByteArrayLiteral("CHUNKBND"));
    bytes.replace(bytes.size() - 9, 9, QByteArrayLiteral("FINALTAIL"));

    LocalMediaResult source;
    if (externalFile) {
        const QString original = directory.filePath(QStringLiteral("linked-image.png"));
        QFile file(original);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        source = mediaStore.referenceFile(original);
    } else {
        source = mediaStore.importData(bytes, QStringLiteral("large-video.mp4"), QStringLiteral("video/mp4"));
    }
    QVERIFY2(source, qPrintable(source.error));
    QVERIFY(source.value.size > 0);
    const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    const QString ext = externalFile ? QStringLiteral("png") : QStringLiteral("mp4");
    const QString nativePath = QStringLiteral(".attachments.42/")
        + source.value.id.toString(QUuid::WithoutBraces) + QLatin1Char('-')
        + QString::fromLatin1(digest.toHex().left(24)) + QLatin1Char('.') + ext;

    QList<QByteArray> responses {
        noteJson(QStringLiteral("42"), QStringLiteral("old-etag"), QString(), 1700000000),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        QByteArrayLiteral("{}"), // MKCOL native per-note folder
        httpError(404, QByteArrayLiteral("Not Found")), // missing staged object
        QByteArrayLiteral("{}"), // MKCOL staging folder
    };
    const qint64 uploadChunks = (dataSize + 5 * 1024 * 1024 - 1) / (5 * 1024 * 1024);
    for (qint64 i = 0; i < uploadChunks; ++i)
        responses.append(QByteArrayLiteral("{}")); // streamed PUT body
    responses.append(QByteArrayLiteral("{}")); // MOVE assembled file
    for (qint64 offset = 0; offset < dataSize; offset += 1024 * 1024) {
        const qint64 count = qMin<qint64>(1024 * 1024, dataSize - offset);
        responses.append(byteRangeResponse(bytes, offset, count, QByteArrayLiteral("\"verified\"")));
    }
    responses.append(noteJson(QStringLiteral("42"), QStringLiteral("new-etag"), QString(), 2100000000));
    ScriptedHttpServer server(std::move(responses));
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 5000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto note = storage.createNote();
    note.setId(QStringLiteral("42"));
    note.setBackendValue(QStringLiteral("etag"), QStringLiteral("old-etag"));
    note.setText(QStringLiteral("[media](") + source.value.uri() + QLatin1Char(')'), Note::Markdown);
    note.setMedia({ source.value });
    auto *saved = storage.saveNoteAsync(note);
    QTRY_VERIFY_WITH_TIMEOUT(saved->isFinished(), 60000);
    QCOMPARE(saved->state(), StorageJob::Succeeded);
    QCOMPARE(saved->result().media().size(), 1);
    QCOMPARE(saved->result().media().first().remoteData.value(QStringLiteral("nextcloud.path")).toString(),
             nativePath);
    QCOMPARE(saved->result().text(), note.text());
    saved->deleteLater();

    const auto &requests = server.requests();
    QCOMPARE(requests.size(), 7 + uploadChunks + (dataSize + 1024 * 1024 - 1) / (1024 * 1024));
    int putChunks = 0;
    int rangeGets = 0;
    int markdownWrites = 0;
    for (const auto &request : requests) {
        if (request.startsWith("PUT /remote.php/dav/uploads/alice/")) {
            ++putChunks;
            const auto bodyPos = request.indexOf("\r\n\r\n");
            QVERIFY(bodyPos >= 0);
            QVERIFY(request.size() - bodyPos - 4 <= 5 * 1024 * 1024);
        }
        if (request.startsWith("GET /remote.php/dav/files/alice/Notes/.attachments.42/"))
            ++rangeGets;
        if (request.startsWith("PUT /index.php/apps/notes/api/v1/notes/42 "))
            ++markdownWrites;
    }
    QCOMPARE(putChunks, int(uploadChunks));
    QCOMPARE(rangeGets, int((dataSize + 1024 * 1024 - 1) / (1024 * 1024) + 1));
    QCOMPARE(markdownWrites, 1);
    QCOMPARE(requests.last().contains(nativePath.toUtf8()), true);
    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}


void NextcloudStorageTest::shutdownRejectsPreviouslyIssuedMediaRangeUrl()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    constexpr qint64 remoteSize = 20LL * 1024 * 1024;
    const QString nativePath = QStringLiteral(".attachments.shut-1/clip.mp4");
    ScriptedHttpServer server({
        noteWithContent(QStringLiteral("shut-1"), QStringLiteral("note-etag"),
                        QStringLiteral("Video"), QStringLiteral("![video](") + nativePath + QLatin1Char(')')),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, 1, remoteSize)
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto *load = storage.loadNoteAsync(QStringLiteral("shut-1"));
    QTRY_VERIFY_WITH_TIMEOUT(load->isFinished(), 10000);
    QCOMPARE(load->state(), StorageJob::Succeeded);
    QCOMPARE(load->result().media().size(), 1);
    const MediaReference reference = load->result().media().first();
    load->deleteLater();

    const QUrl existingUrl = MediaRangeService::urlFor(reference);
    QVERIFY(!existingUrl.isEmpty());
    storage.shutdown();
    storage.shutdown(); // Idempotent and never restart a stopped worker.
    QVERIFY(MediaRangeService::urlFor(reference).isEmpty());

    QNetworkAccessManager client;
    QNetworkRequest request(existingUrl);
    request.setRawHeader("Range", "bytes=1024-1055");
    auto *reply = client.get(request);
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 503);
    QVERIFY(reply->readAll().isEmpty());
    reply->deleteLater();
    QCOMPARE(server.requests().size(), 3); // Shutdown must not reach WebDAV.
    MediaRangeService::releaseUrl(existingUrl);

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::concurrentRangeReadsReuseOneVerifiedWebDavChunk()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    constexpr qint64 remoteSize = 20LL * 1024 * 1024;
    const QString nativePath = QStringLiteral(".attachments.concurrent-1/movie.mp4");
    ScriptedHttpServer server({
        noteWithContent(QStringLiteral("concurrent-1"), QStringLiteral("note-etag"),
                        QStringLiteral("Video"), QStringLiteral("![movie](") + nativePath + QLatin1Char(')')),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, 1, remoteSize),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, LocalMediaStore::ManagedChunkSize, remoteSize)
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto *load = storage.loadNoteAsync(QStringLiteral("concurrent-1"));
    QTRY_VERIFY_WITH_TIMEOUT(load->isFinished(), 10000);
    QCOMPARE(load->state(), StorageJob::Succeeded);
    QCOMPARE(load->result().media().size(), 1);
    const QUrl url = MediaRangeService::urlFor(load->result().media().first());
    QVERIFY(!url.isEmpty());
    load->deleteLater();

    QNetworkAccessManager client;
    QNetworkRequest firstRequest(url);
    firstRequest.setRawHeader("Range", "bytes=1024-1055");
    QNetworkRequest secondRequest(url);
    secondRequest.setRawHeader("Range", "bytes=4096-4127");
    auto *first = client.get(firstRequest);
    auto *second = client.get(secondRequest);
    QTRY_VERIFY_WITH_TIMEOUT(first->isFinished() && second->isFinished(), 15000);
    QCOMPARE(first->error(), QNetworkReply::NoError);
    QCOMPARE(second->error(), QNetworkReply::NoError);
    QCOMPARE(first->readAll(), QByteArray(32, 'R'));
    QCOMPARE(second->readAll(), QByteArray(32, 'R'));
    first->deleteLater();
    second->deleteLater();
    QCOMPARE(server.requests().size(), 5);
    QVERIFY(server.requests().at(4).contains("Range: bytes=0-1048575"));
    MediaRangeService::releaseUrl(url);

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::accountSwitchDuringRangeReadRejectsStaleBytes()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    constexpr qint64 remoteSize = 20LL * 1024 * 1024;
    const QString nativePath = QStringLiteral(".attachments.switch-1/movie.mp4");
    ScriptedHttpServer server({
        noteWithContent(QStringLiteral("switch-1"), QStringLiteral("note-etag"),
                        QStringLiteral("Video"), QStringLiteral("![movie](") + nativePath + QLatin1Char(')')),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, 1, remoteSize),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, LocalMediaStore::ManagedChunkSize, remoteSize)
    });
    server.holdResponseOnRequest(5);
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 5000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto *load = storage.loadNoteAsync(QStringLiteral("switch-1"));
    QTRY_VERIFY_WITH_TIMEOUT(load->isFinished(), 10000);
    QCOMPARE(load->state(), StorageJob::Succeeded);
    QCOMPARE(load->result().media().size(), 1);
    const auto reference = load->result().media().first();
    load->deleteLater();

    const QUrl url = MediaRangeService::urlFor(reference);
    QVERIFY(!url.isEmpty());
    QNetworkAccessManager client;
    QNetworkRequest request(url);
    request.setRawHeader("Range", "bytes=1024-1055");
    auto *reply = client.get(request);
    QTRY_VERIFY_WITH_TIMEOUT(server.requests().size() == 5 && server.hasHeldResponse(), 10000);
    QVERIFY(!reply->isFinished());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("bob"));
    server.releaseHeldResponse();
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 15000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 503);
    QVERIFY(reply->readAll().isEmpty());
    QVERIFY(MediaRangeService::urlFor(reference).isEmpty());
    reply->deleteLater();
    MediaRangeService::releaseUrl(url);

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}


void NextcloudStorageTest::reentrantStorageOperationCannotChangePinnedWebDavAccount()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    constexpr qint64 remoteSize = 20LL * 1024 * 1024;
    const QString nativePath = QStringLiteral(".attachments.pin-1/movie.mp4");
    ScriptedHttpServer server({
        noteWithContent(QStringLiteral("pin-1"), QStringLiteral("note-etag"),
                        QStringLiteral("Video"), QStringLiteral("![movie](") + nativePath + QLatin1Char(')')),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, 1, remoteSize),
        // Pause Alice's settings request. A nested worker event loop then
        // allows a different note request to switch the worker to Bob.
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        noteWithContent(QStringLiteral("other"), QStringLiteral("other-etag"),
                        QStringLiteral("Other"), QStringLiteral("Nothing attached")),
        remoteByteRangeResponse(0, LocalMediaStore::ManagedChunkSize, remoteSize)
    });
    server.holdResponseOnRequest(4);
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 5000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto *load = storage.loadNoteAsync(QStringLiteral("pin-1"));
    QTRY_VERIFY_WITH_TIMEOUT(load->isFinished(), 10000);
    QCOMPARE(load->state(), StorageJob::Succeeded);
    QCOMPARE(load->result().media().size(), 1);
    const MediaReference reference = load->result().media().first();
    load->deleteLater();

    const QUrl url = MediaRangeService::urlFor(reference);
    QVERIFY(!url.isEmpty());
    QNetworkAccessManager client;
    QNetworkRequest request(url);
    request.setRawHeader("Range", "bytes=1024-1055");
    auto *reply = client.get(request);
    QTRY_VERIFY_WITH_TIMEOUT(server.requests().size() == 4 && server.hasHeldResponse(), 10000);

    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("bob"));
    auto *other = storage.loadNoteAsync(QStringLiteral("other"));
    QTRY_VERIFY_WITH_TIMEOUT(other->isFinished(), 10000);
    QCOMPARE(other->state(), StorageJob::Succeeded);
    other->deleteLater();
    QCOMPARE(server.requests().size(), 5);
    QVERIFY(server.requests().at(4).startsWith("GET /index.php/apps/notes/api/v1/notes/other "));

    server.releaseHeldResponse();
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 15000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 503);
    QVERIFY(reply->readAll().isEmpty());
    reply->deleteLater();

    // clearConnectionCache() is allowed to abort the outstanding settings
    // GET when Bob takes over. If Qt leaves the request alive, its subsequent
    // WebDAV GET must still be pinned to Alice. Neither path may disclose
    // bytes from the other account or cache them under the wrong identity.
    const qsizetype requestCount = server.requests().size();
    QVERIFY(requestCount == 5 || requestCount == 6);
    if (requestCount == 6) {
        QVERIFY(server.requests().at(5).startsWith(
            "GET /remote.php/dav/files/alice/Notes/.attachments.pin-1/movie.mp4 "));
        QVERIFY(server.requests().at(5).contains("Range: bytes=0-1048575"));
        settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));

        // The verified cache is keyed to Alice's object, never Bob's account.
        auto *cached = client.get(request);
        QTRY_VERIFY_WITH_TIMEOUT(cached->isFinished(), 10000);
        QCOMPARE(cached->error(), QNetworkReply::NoError);
        QCOMPARE(cached->readAll(), QByteArray(32, 'R'));
        cached->deleteLater();
        QCOMPARE(server.requests().size(), 6);
    }
    MediaRangeService::releaseUrl(url);

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::encryptedNextcloudPartialCacheSurvivesRestartAndTamper()
{
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray profileKey = SecureEnvelope::generateMasterKey();
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")), profileKey);
    constexpr qint64 size = 20LL * 1024 * 1024;
    const QString nativePath = QStringLiteral(".attachments.stream-1/clip.mp4");
    const QString body = QStringLiteral("![video](") + nativePath + QLatin1Char(')');
    ScriptedHttpServer server({
        noteWithContent(QStringLiteral("stream-1"), QStringLiteral("note-etag"),
                        QStringLiteral("Video"), body),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, 1, size),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, LocalMediaStore::ManagedChunkSize, size),
        // After a corrupted local record is rejected, the worker reloads the
        // same pinned revision instead of sending unauthenticated bytes.
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, LocalMediaStore::ManagedChunkSize, size)
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    MediaReference reference;
    auto requestBytes = [](QNetworkAccessManager &client, const QUrl &url) {
        QNetworkRequest request(url);
        request.setRawHeader("Range", "bytes=1024-1055");
        return client.get(request);
    };
    {
        NextcloudStorage storage(nullptr, nullptr, &mediaStore);
        auto *load = storage.loadNoteAsync(QStringLiteral("stream-1"));
        QTRY_VERIFY_WITH_TIMEOUT(load->isFinished(), 10000);
        QCOMPARE(load->state(), StorageJob::Succeeded);
        QCOMPARE(load->result().media().size(), 1);
        reference = load->result().media().first();
        load->deleteLater();

        QNetworkAccessManager client;
        const QUrl url = MediaRangeService::urlFor(reference);
        QVERIFY(!url.isEmpty());
        auto *first = requestBytes(client, url);
        QTRY_VERIFY_WITH_TIMEOUT(first->isFinished(), 10000);
        QCOMPARE(first->error(), QNetworkReply::NoError);
        QCOMPARE(first->readAll(), QByteArray(32, 'R'));
        first->deleteLater();
        QCOMPARE(server.requests().size(), 5);
        auto *again = requestBytes(client, url);
        QTRY_VERIFY_WITH_TIMEOUT(again->isFinished(), 10000);
        QCOMPARE(again->error(), QNetworkReply::NoError);
        QCOMPARE(again->readAll(), QByteArray(32, 'R'));
        again->deleteLater();
        QCOMPARE(server.requests().size(), 5); // Cache hit: no WebDAV.
        MediaRangeService::releaseUrl(url);
    }
    // Destroying and recreating the storage object must not erase encrypted
    // chunks; no call to getNote() is necessary to resume playback.
    {
        NextcloudStorage restarted(nullptr, nullptr, &mediaStore);
        QNetworkAccessManager client;
        const QUrl url = MediaRangeService::urlFor(reference);
        QVERIFY(!url.isEmpty());
        auto *offline = requestBytes(client, url);
        QTRY_VERIFY_WITH_TIMEOUT(offline->isFinished(), 10000);
        QCOMPARE(offline->error(), QNetworkReply::NoError);
        QCOMPARE(offline->readAll(), QByteArray(32, 'R'));
        offline->deleteLater();
        QCOMPARE(server.requests().size(), 5);

        // The only downloaded chunk is index 0. Select it explicitly:
        // directory traversal order is platform-dependent.
        QDirIterator files(directory.filePath(QStringLiteral("media/partial")), QDir::Files,
                           QDirIterator::Subdirectories);
        QString recordPath;
        while (files.hasNext()) {
            const QString candidate = files.next();
            if (QFileInfo(candidate).fileName() == QStringLiteral("0")) {
                recordPath = candidate;
                break;
            }
        }
        QVERIFY2(!recordPath.isEmpty(), "Missing encrypted record for chunk 0");
        QFile encrypted(recordPath);
        QVERIFY(encrypted.open(QIODevice::ReadWrite));
        const QByteArray ciphertext = encrypted.readAll();
        QVERIFY(!ciphertext.contains(QByteArray(64, 'R')));
        QVERIFY(!ciphertext.isEmpty());
        const qint64 offset = ciphertext.size() / 2;
        // Writing a constant can be a no-op when ciphertext already contains
        // that byte (probability 1/256). Always flip bits instead.
        const char corrupt = char(quint8(ciphertext.at(offset)) ^ 0x5e);
        QVERIFY(corrupt != ciphertext.at(offset));
        QVERIFY(encrypted.seek(offset));
        QCOMPARE(encrypted.write(&corrupt, 1), qint64(1));
        encrypted.close();

        // Confirm the on-disk record was actually modified before retrying.
        QVERIFY(encrypted.open(QIODevice::ReadOnly));
        QVERIFY(encrypted.seek(offset));
        QCOMPARE(encrypted.read(1), QByteArray(1, corrupt));
        encrypted.close();

        auto *repaired = requestBytes(client, url);
        QTRY_VERIFY_WITH_TIMEOUT(repaired->isFinished(), 10000);
        QCOMPARE(repaired->error(), QNetworkReply::NoError);
        QCOMPARE(repaired->readAll(), QByteArray(32, 'R'));
        repaired->deleteLater();
        QCOMPARE(server.requests().size(), 7);
        QVERIFY(server.requests().at(6).contains("Range: bytes=0-1048575"));
        QVERIFY(server.requests().at(6).toLower().contains("if-match: \"remote-revision\""));
        MediaRangeService::releaseUrl(url);
    }
    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::oversizedNativeAttachmentStreamsRemoteWebDavRanges_data()
{
    QTest::addColumn<int>("revisionScenario");
    QTest::newRow("same-webdav-revision") << 0;
    QTest::newRow("webdav-revision-replaced") << 1;
    QTest::newRow("webdav-response-omits-etag") << 2;
}

void NextcloudStorageTest::oversizedNativeAttachmentStreamsRemoteWebDavRanges()
{
    QFETCH(int, revisionScenario);
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    constexpr qint64 remoteSize = 20LL * 1024 * 1024;
    const QString remotePath = QStringLiteral(".attachments.vid-1/movie.mp4");
    const QString markdown = QStringLiteral("![movie](") + remotePath + QLatin1Char(')');
    ScriptedHttpServer server({
        noteWithContent(QStringLiteral("vid-1"), QStringLiteral("note-etag"),
                        QStringLiteral("Video"), markdown),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, 1, remoteSize),
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        remoteByteRangeResponse(0, LocalMediaStore::ManagedChunkSize, remoteSize,
                                revisionScenario == 1 ? QByteArrayLiteral("\"changed-revision\"")
                                    : revisionScenario == 2 ? QByteArray() : QByteArrayLiteral("\"remote-revision\""))
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 2000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto *job = storage.loadNoteAsync(QStringLiteral("vid-1"));
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
    QVERIFY2(job->state() == StorageJob::Succeeded, qPrintable(job->error().message));
    const Note note = job->result();
    QVERIFY(note.text().contains(QStringLiteral("anykeep-media:/")));
    QCOMPARE(note.media().size(), 1);
    const auto reference = note.media().first();
    QVERIFY(reference.isValid());
    QCOMPARE(reference.size, remoteSize);
    QCOMPARE(reference.mediaType, QStringLiteral("video/mp4"));
    QVERIFY(reference.blobId.isEmpty());
    QVERIFY(reference.remoteData.value(QStringLiteral("nextcloud.remoteOnly")).toBool());
    QCOMPARE(reference.remoteData.value(QStringLiteral("nextcloud.etag")).toString(),
             QStringLiteral("\"remote-revision\""));
    QCOMPARE(reference.remoteData.value(QStringLiteral("nextcloud.path")).toString(), remotePath);
    QVERIFY(!mediaStore.contains(reference));
    job->deleteLater();

    // The existing Qt player/browser local-range bridge must request only
    // the actual sought segment, pinning the exact WebDAV ETag.
    const QUrl url = MediaRangeService::urlFor(reference);
    QVERIFY2(url.isValid() && !url.isEmpty(), "Nextcloud remote-only range resolver was not registered");
    QNetworkAccessManager client;
    QNetworkRequest request(url);
    request.setRawHeader("Range", "bytes=1024-1055");
    auto *reply = client.get(request);
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 15000);
    if (revisionScenario != 0) {
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 503);
        QVERIFY(reply->readAll().isEmpty()); // Never forward bytes from another file revision.
    } else {
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 206);
        QCOMPARE(reply->readAll(), QByteArray(32, 'R'));
    }
    reply->deleteLater();
    MediaRangeService::releaseUrl(url);

    QCOMPARE(server.requests().size(), 5);
    QVERIFY(server.requests().at(2).startsWith("GET /remote.php/dav/files/alice/Notes/.attachments.vid-1/movie.mp4 "));
    QVERIFY(server.requests().at(2).contains("Range: bytes=0-0"));
    const auto ranged = server.requests().at(4);
    // Decoder asks for 32 bytes, but the durable cache hydrates the
    // containing 1 MiB authenticated record just once.
    QVERIFY(ranged.contains("Range: bytes=0-1048575"));
    QVERIFY(ranged.toLower().contains("if-match: \"remote-revision\""));

    // References must never be routed to credentials of another configured
    // account, even if their remote note ID and pathname happen to match.
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("bob"));
    QVERIFY(MediaRangeService::urlFor(reference).isEmpty());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::nativeLinksRestoreEncryptedLocalMediaViaWebDavRanges_data()
{
    QTest::addColumn<QString>("nativePath");
    QTest::addColumn<bool>("racingEdit");
    QTest::newRow("web-native-directory") << QStringLiteral(".attachments.42/image.png") << false;
    QTest::newRow("notes-api-legacy-sibling") << QStringLiteral("legacy-upload.png") << false;
    QTest::newRow("concurrent-native-file-change") << QStringLiteral(".attachments.42/image.png") << true;
}

void NextcloudStorageTest::nativeLinksRestoreEncryptedLocalMediaViaWebDavRanges()
{
    QFETCH(QString, nativePath);
    QFETCH(bool, racingEdit);
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("restored")),
                               SecureEnvelope::generateMasterKey());

    const QByteArray bytes = QByteArrayLiteral("remotely-published-image");
    const QString remotePath = nativePath;
    const QString nativeMarkdown = QStringLiteral("![image](") + remotePath + QLatin1Char(')');
    QJsonObject json = QJsonDocument::fromJson(
        noteJson(QStringLiteral("42"), QStringLiteral("old-etag"), QString(), 1700000000)).object();
    json.insert(QStringLiteral("content"), nativeMarkdown);
    const QByteArray remoteNote = QJsonDocument(json).toJson(QJsonDocument::Compact);

    ScriptedHttpServer server({
        remoteNote,
        QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
        byteRangeResponse(bytes, 0, 1, QByteArrayLiteral("\"rev-one\"")),
        byteRangeResponse(bytes, 1, bytes.size() - 1,
                          racingEdit ? QByteArrayLiteral("\"rev-two\"") : QByteArrayLiteral("\"rev-one\""))
    });
    QVERIFY(server.listen());

    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto *loaded = storage.loadNoteAsync(QStringLiteral("42"));
    QTRY_VERIFY(loaded->isFinished());
    QCOMPARE(loaded->state(), StorageJob::Succeeded);
    const auto note = loaded->result();
    if (racingEdit) {
        QCOMPARE(note.media().size(), 0);
        QCOMPARE(note.text(), nativeMarkdown);
    } else {
        QCOMPARE(note.media().size(), 1);
        QVERIFY(note.text().startsWith(QStringLiteral("![image](anykeep-media:/")));
        QCOMPARE(note.media().first().remoteData.value(QStringLiteral("nextcloud.path")).toString(),
                 remotePath);
        QCOMPARE(mediaStore.data(note.media().first()).value, bytes);
    }
    loaded->deleteLater();

    QCOMPARE(server.requests().size(), 4);
    QVERIFY(server.requests().at(2).contains("Range: bytes=0-0\r\n"));
    QVERIFY(server.requests().at(3).contains("If-Match: \"rev-one\"\r\n"));
    QVERIFY(server.requests().at(3).contains(
        QByteArrayLiteral("Range: bytes=1-") + QByteArray::number(bytes.size() - 1) + "\r\n"));
    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

void NextcloudStorageTest::staleEtagPreventsAttachmentStaging_data()
{
    QTest::addColumn<bool>("categoryWasChanged");
    QTest::newRow("stale-etag") << false;
    QTest::newRow("category-transition") << true;
}

void NextcloudStorageTest::staleEtagPreventsAttachmentStaging()
{
    QFETCH(bool, categoryWasChanged);
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const auto imported = mediaStore.importData(QByteArrayLiteral("unchanged-source"),
                                                QStringLiteral("image.png"), QStringLiteral("image/png"));
    QVERIFY2(imported, qPrintable(imported.error));

    ScriptedHttpServer server({
        noteJson(QStringLiteral("42"),
                 categoryWasChanged ? QStringLiteral("stale-etag") : QStringLiteral("changed-on-server"),
                 categoryWasChanged ? QStringLiteral("RemoteOtherCategory") : QString(), 2100000000)
    });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto note = storage.createNote();
    note.setId(QStringLiteral("42"));
    note.setBackendValue(QStringLiteral("etag"), QStringLiteral("stale-etag"));
    note.setMedia({ imported.value });
    note.setText(QStringLiteral("![image](") + imported.value.uri() + QLatin1Char(')'), Note::Markdown);

    auto *saved = storage.saveNoteAsync(note);
    QTRY_VERIFY(saved->isFinished());
    QCOMPARE(saved->state(), StorageJob::Failed);
    QCOMPARE(saved->error().code, StorageError::Conflict);
    QVERIFY(!saved->error().retryable);
    QCOMPARE(server.requests().size(), 1); // No upload, no destructive Markdown PUT.
    QVERIFY(server.requests().first().startsWith("GET /index.php/apps/notes/api/v1/notes/42 "));
    saved->deleteLater();

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}


void NextcloudStorageTest::existingMediaReceiptIsReusedOrRejected_data()
{
    QTest::addColumn<bool>("modifiedRemotely");
    QTest::newRow("identical-remote-content") << false;
    QTest::newRow("native-client-replaced-attachment") << true;
}

void NextcloudStorageTest::existingMediaReceiptIsReusedOrRejected()
{
    QFETCH(bool, modifiedRemotely);
    QVERIFY(FileFolderCatalogStore::cryptoAvailable());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("media")),
                               SecureEnvelope::generateMasterKey());
    const QByteArray bytes = QByteArrayLiteral("original-image-bytes");
    const auto imported = mediaStore.importData(bytes, QStringLiteral("image.png"),
                                                QStringLiteral("image/png"));
    QVERIFY2(imported, qPrintable(imported.error));
    auto reference = imported.value;
    const QString remotePath = QStringLiteral(".attachments.42/persisted.png");
    reference.remoteData.insert(QStringLiteral("nextcloud.noteId"), QStringLiteral("42"));
    reference.remoteData.insert(QStringLiteral("nextcloud.path"), remotePath);
    const QString body = QStringLiteral("![image](") + remotePath + QLatin1Char(')');
    QJsonObject response = QJsonDocument::fromJson(
        noteJson(QStringLiteral("42"), QStringLiteral("old-etag"), QString(), 1700000000)).object();
    response.insert(QStringLiteral("content"), body);
    const QByteArray initial = QJsonDocument(response).toJson(QJsonDocument::Compact);

    QByteArray remoteBytes = bytes;
    if (modifiedRemotely)
        remoteBytes[0] = 'X';
    ScriptedHttpServer server(modifiedRemotely
        ? QList<QByteArray> { initial, QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
                              QByteArrayLiteral("{}"),
                              byteRangeResponse(remoteBytes, 0, remoteBytes.size()) }
        : QList<QByteArray> { initial, QByteArrayLiteral("{\"notesPath\":\"Notes\"}"),
                              QByteArrayLiteral("{}"),
                              byteRangeResponse(remoteBytes, 0, remoteBytes.size()),
                              noteJson(QStringLiteral("42"), QStringLiteral("new-etag"), QString(), 2100000000) });
    QVERIFY(server.listen());
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), server.url().toString());
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));
    settings.setValue(QStringLiteral("storage.nextcloud.timeoutMs"), 1000);

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto note = storage.createNote();
    note.setId(QStringLiteral("42"));
    note.setBackendValue(QStringLiteral("etag"), QStringLiteral("old-etag"));
    note.setMedia({ reference });
    note.setText(QStringLiteral("A harmless text edit\n![image](") + reference.uri()
                 + QLatin1Char(')'), Note::Markdown);
    auto *saved = storage.saveNoteAsync(note);
    QTRY_VERIFY(saved->isFinished());

    if (modifiedRemotely) {
        QCOMPARE(saved->state(), StorageJob::Failed);
        QCOMPARE(saved->error().code, StorageError::Conflict);
        QCOMPARE(server.requests().size(), 4);
    } else {
        QCOMPARE(saved->state(), StorageJob::Succeeded);
        QCOMPARE(saved->result().media().size(), 1);
        QCOMPARE(saved->result().media().first().remoteData.value(QStringLiteral("nextcloud.path")).toString(),
                 remotePath);
        QCOMPARE(server.requests().size(), 5);
        const QByteArray put = server.requests().last();
        QVERIFY(put.startsWith("PUT /index.php/apps/notes/api/v1/notes/42 "));
        const auto headerEnd = put.indexOf("\r\n\r\n");
        QVERIFY(headerEnd >= 0);
        QCOMPARE(QJsonDocument::fromJson(put.mid(headerEnd + 4)).object()
                     .value(QStringLiteral("content")).toString(),
                 QStringLiteral("A harmless text edit\n![image](.attachments.42/persisted.png)"));
    }
    QVERIFY(server.requests().at(1).startsWith("GET /index.php/apps/notes/api/v1/settings "));
    QVERIFY(server.requests().at(2).startsWith("MKCOL /remote.php/dav/files/alice/Notes/.attachments.42 "));
    QVERIFY(server.requests().at(3).startsWith(
        "GET /remote.php/dav/files/alice/Notes/.attachments.42/persisted.png "));
    for (const auto &request : server.requests())
        QVERIFY(!request.startsWith("POST /index.php/apps/notes/api/v1.4/attachment/"));
    saved->deleteLater();

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

QTEST_GUILESS_MAIN(NextcloudStorageTest)
#include "nextcloudstorage_test.moc"
