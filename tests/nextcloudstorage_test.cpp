#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "nextcloudstorage.h"
#include "localmediastore.h"
#include "secureenvelope.h"
#include <QCryptographicHash>
#include <QFile>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
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

private:
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
        if (body.startsWith("HTTP/1.1 "))
            socket->write(body);
        else
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer                      server_;
    QList<QByteArray>               responses_;
    QList<QByteArray>               requests_;
    QHash<QTcpSocket *, QByteArray> pending_;
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
    void streamPublishWithoutWholePlaintextBuffer_data();
    void streamPublishWithoutWholePlaintextBuffer();
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
    QCOMPARE(requests.size(), 6 + uploadChunks + (dataSize + 1024 * 1024 - 1) / (1024 * 1024));
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
