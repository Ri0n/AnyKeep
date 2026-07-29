#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "nextcloudstorage.h"
#include "secureenvelope.h"

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

using namespace QtNote;

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
    void dirtyPublishIncludesFolderCategory();
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

    QCOMPARE(server.requests().size(), 4);
    const auto request   = server.requests().last();
    const auto headerEnd = request.indexOf("\r\n\r\n");
    QVERIFY(headerEnd >= 0);
    const auto headers = request.left(headerEnd);
    QVERIFY(headers.startsWith("PUT /index.php/apps/notes/api/v1/notes/note-1 HTTP/1.1\r\n"));
    QVERIFY(headers.contains("If-Match: old-etag\r\n"));
    const auto body = QJsonDocument::fromJson(request.mid(headerEnd + 4));
    QVERIFY(body.isObject());
    QCOMPARE(body.object().keys(), QStringList({ QStringLiteral("category") }));
    QCOMPARE(body.object().value(QStringLiteral("category")).toString(), QStringLiteral("Projects/2026"));

    clearSettings();
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
    auto             note = storage.createNote();
    note.setTitle(QStringLiteral("Draft title"));
    note.setText(QStringLiteral("Draft body"), Note::Markdown);
    note.setFolderId(destination.value);
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

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
    settings.remove(QStringLiteral("storage.nextcloud.timeoutMs"));
}

QTEST_GUILESS_MAIN(NextcloudStorageTest)
#include "nextcloudstorage_test.moc"
