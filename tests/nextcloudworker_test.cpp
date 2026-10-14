#include "nextcloudworker.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace AnyKeep;

class NextcloudWorkerTest : public QObject {
    Q_OBJECT

private slots:
    void categoryUpdateSendsOnlyCategoryAndEtag();
    void asyncWebDavRangeUsesPinnedAccountDuringConfigChange();
    void asyncSettingsLookupIsPinnedAndNonBlocking();
};

namespace {

class HttpCapture final : public QObject {
public:
    HttpCapture()
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this]() { readRequest(); });
        });
    }

    bool listen() { return server.listen(QHostAddress::LocalHost); }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }

    QByteArray request;

private:
    void readRequest()
    {
        request += socket->readAll();
        const auto headerEnd = request.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;

        qsizetype contentLength = 0;
        for (const auto &line : request.left(headerEnd).split('\n')) {
            const auto trimmed    = line.trimmed();
            const auto headerName = QByteArrayLiteral("Content-Length:");
            if (trimmed.left(headerName.size()).compare(headerName, Qt::CaseInsensitive) == 0) {
                contentLength = trimmed.mid(headerName.size()).trimmed().toLongLong();
                break;
            }
        }
        if (request.size() < headerEnd + 4 + contentLength)
            return;

        const QJsonObject note { { QStringLiteral("id"), QStringLiteral("42") },
                                 { QStringLiteral("etag"), QStringLiteral("new-etag") },
                                 { QStringLiteral("title"), QStringLiteral("Existing title") },
                                 { QStringLiteral("category"), QStringLiteral("Projects/2026") },
                                 { QStringLiteral("favorite"), false },
                                 { QStringLiteral("readonly"), false },
                                 { QStringLiteral("modified"), 1720000000 } };
        const auto        body = QJsonDocument(note).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer  server;
    QTcpSocket *socket { nullptr };
};

} // namespace

void NextcloudWorkerTest::categoryUpdateSendsOnlyCategoryAndEtag()
{
    HttpCapture server;
    QVERIFY(server.listen());

    NextcloudWorker worker;
    NextcloudConfig config;
    config.serverUrl   = server.url();
    config.userName    = QStringLiteral("alice");
    config.appPassword = QStringLiteral("secret");
    config.timeoutMs   = 1000;
    worker.setConfig(config);

    const auto result
        = worker.updateNoteCategory(QStringLiteral("42"), QStringLiteral("Projects/2026"), QStringLiteral("old-etag"));
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.note.id, QStringLiteral("42"));
    QCOMPARE(result.note.etag, QStringLiteral("new-etag"));

    const auto headerEnd = server.request.indexOf("\r\n\r\n");
    QVERIFY(headerEnd >= 0);
    const auto headers = server.request.left(headerEnd);
    QVERIFY(headers.startsWith("PUT /index.php/apps/notes/api/v1/notes/42 HTTP/1.1\r\n"));
    QVERIFY(headers.contains("If-Match: \"old-etag\"\r\n"));
    const auto      body = server.request.mid(headerEnd + 4);
    QJsonParseError parseError;
    const auto      document = QJsonDocument::fromJson(body, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    const auto object = document.object();
    QCOMPARE(object.keys(), QStringList({ QStringLiteral("category") }));
    QCOMPARE(object.value(QStringLiteral("category")).toString(), QStringLiteral("Projects/2026"));
}


void NextcloudWorkerTest::asyncWebDavRangeUsesPinnedAccountDuringConfigChange()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QUrl url(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    QByteArray request;
    QPointer<QTcpSocket> peer;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, &server, [&] {
            request += peer->readAll();
            if (!request.contains("\r\n\r\n"))
                return;
            // Intentionally leave the network request unanswered. The worker
            // must be able to process other operations and account changes.
        });
    });

    NextcloudWorker worker;
    NextcloudConfig alice;
    alice.serverUrl = url;
    alice.userName = QStringLiteral("alice");
    alice.appPassword = QStringLiteral("alice-password");
    alice.timeoutMs = 3000;
    int completions = 0;
    NextcloudWebDavRangeResult received;
    worker.readWebDavRangeAsync(alice, QStringLiteral("Notes/42/video.mp4"), 0, 8,
                                QStringLiteral("\"revision-1\""),
                                [&](NextcloudWebDavRangeResult value) {
                                    received = std::move(value);
                                    ++completions;
                                });
    QTRY_VERIFY(request.contains("\r\n\r\n"));
    QCOMPARE(completions, 0);
    QVERIFY(request.startsWith("GET /remote.php/dav/files/alice/Notes/42/video.mp4 HTTP/1.1\r\n"));
    QVERIFY(request.contains("Range: bytes=0-7\r\n"));
    QVERIFY(request.contains("If-Match: \"revision-1\"\r\n"));
    const QByteArray expectedAuthorization = "Authorization: Basic "
        + QByteArray("alice:alice-password").toBase64() + "\r\n";
    QVERIFY(request.contains(expectedAuthorization));

    NextcloudConfig bob = alice;
    bob.userName = QStringLiteral("bob");
    bob.appPassword = QStringLiteral("bob-password");
    worker.setConfig(bob); // Must not rebind the in-flight immutable request.
    QCOMPARE(completions, 0);
    QVERIFY(peer);
    peer->write("HTTP/1.1 206 Partial Content\r\n"
                "Content-Range: bytes 0-7/64\r\n"
                "ETag: \"revision-1\"\r\n"
                "Content-Length: 8\r\n"
                "Connection: close\r\n\r\n"
                "abcdefgh");
    peer->disconnectFromHost();
    QTRY_COMPARE(completions, 1);
    QVERIFY2(received.ok, qPrintable(received.error));
    QCOMPARE(received.data, QByteArrayLiteral("abcdefgh"));
    QCOMPARE(received.totalSize, qint64(64));
    QCOMPARE(received.etag, QStringLiteral("\"revision-1\""));
}

void NextcloudWorkerTest::asyncSettingsLookupIsPinnedAndNonBlocking()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QUrl url(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    QByteArray request;
    QPointer<QTcpSocket> peer;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, &server, [&] {
            request += peer->readAll();
        });
    });

    NextcloudWorker worker;
    NextcloudConfig alice;
    alice.serverUrl = url;
    alice.userName = QStringLiteral("alice");
    alice.appPassword = QStringLiteral("alice-password");
    alice.timeoutMs = 3000;
    int completions = 0;
    NextcloudSettingsResult received;
    worker.getSettingsAsync(alice, [&](NextcloudSettingsResult result) {
        received = std::move(result);
        ++completions;
    });
    QTRY_VERIFY(request.contains("\r\n\r\n"));
    QVERIFY(request.startsWith("GET /index.php/apps/notes/api/v1/settings HTTP/1.1\r\n"));
    const QByteArray expectedAuthorization = "Authorization: Basic "
        + QByteArray("alice:alice-password").toBase64() + "\r\n";
    QVERIFY(request.contains(expectedAuthorization));
    QCOMPARE(completions, 0);

    NextcloudConfig bob = alice;
    bob.userName = QStringLiteral("bob");
    bob.appPassword = QStringLiteral("bob-password");
    worker.setConfig(bob);
    QVERIFY(peer);
    const QByteArray body = QByteArrayLiteral("{\"notesPath\":\"Notes\"}");
    peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    peer->disconnectFromHost();
    QTRY_COMPARE(completions, 1);
    QVERIFY2(received.ok, qPrintable(received.error));
    QCOMPARE(received.notesPath, QStringLiteral("Notes"));
}

QTEST_GUILESS_MAIN(NextcloudWorkerTest)
#include "nextcloudworker_test.moc"
