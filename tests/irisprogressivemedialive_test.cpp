#include "iris/irismediasync.h"
#include "iris/irisxmppbackend.h"
#include "localmediastore.h"
#include "mediarangeservice.h"
#include "secureenvelope.h"
#include "utils.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTimer>
#include <QtCrypto>
#include <QtTest>
#include <iris/jingle-session.h>
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE)
#include <QMediaPlayer>
#include <QVideoFrame>
#include <QVideoSink>
#endif

namespace AnyKeep {
Q_LOGGING_CATEGORY(lcProgressiveLive, "anykeep.live")
// Opt-in live regression. Uses two processes with independent local profiles;
// the only common state is the XMPP account and an isolated test-directory key.
class IrisProgressiveMediaLiveTest : public QObject {
    Q_OBJECT
private slots:
    void progressivePublicationAndRanges()
    {
        if (!qEnvironmentVariableIsSet("ANYKEEP_LIVE_ROLE"))
            QSKIP("Live XMPP fixture is opt-in");
        const auto role      = qEnvironmentVariable("ANYKEEP_LIVE_ROLE");
        const auto directory = qEnvironmentVariable("ANYKEEP_LIVE_DIR");
        const auto video     = qEnvironmentVariable("ANYKEEP_LIVE_VIDEO");
        QVERIFY(!directory.isEmpty());
        QCoreApplication::setOrganizationName(QStringLiteral("AnyKeepLiveTest"));
        QCoreApplication::setApplicationName(QStringLiteral("progressive-") + role);
        qputenv("XDG_DATA_HOME", (directory + '/' + role + QStringLiteral("/data")).toUtf8());
        auto *store      = LocalMediaStore::instance();
        store->rootPath_ = directory + '/' + role + QStringLiteral("/media");
        QFile localKey(directory + QStringLiteral("/local-key-") + role);
        if (localKey.open(QIODevice::ReadOnly))
            store->masterKey_ = localKey.readAll();
        else {
            store->masterKey_ = SecureEnvelope::generateMasterKey();
            QVERIFY(localKey.open(QIODevice::WriteOnly));
            localKey.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            localKey.write(store->masterKey_);
            localKey.close();
        }
        XmppConfig config;
        config.instanceId = role;
        config.jid        = qEnvironmentVariable("ANYKEEP_LIVE_JID");
        config.password   = qEnvironmentVariable("ANYKEEP_LIVE_PASSWORD");
        config.host       = QStringLiteral("127.0.0.1");
        config.port       = qEnvironmentVariableIntValue("ANYKEEP_LIVE_PORT");
        config.resource   = QStringLiteral("anykeep-test-") + role;
        config.originId   = role;
        config.timeoutMs  = 45000;
        config.nodeName   = QStringLiteral("urn:anykeep:progressive-test:") + QFileInfo(directory).fileName();
        QFile keyFile(directory + QStringLiteral("/master-key"));
        QVERIFY(keyFile.open(QIODevice::ReadOnly));
        config.masterKey      = keyFile.readAll();
        config.omemoStateKey  = config.masterKey;
        config.omemoStatePath = directory + '/' + role + QStringLiteral("/omemo");
        IrisXmppBackend backend;
        backend.setConfig(config);
        backend.start();
        backend.createClient();
        const auto transportProfile = qEnvironmentVariable("ANYKEEP_LIVE_TRANSPORT");
        QVERIFY2(transportProfile.isEmpty() || transportProfile == QStringLiteral("ice"),
                 "Unsupported live transport profile");
        auto *jingle = backend.client_->jingleManager();
        if (transportProfile == QStringLiteral("ice")) {
            jingle->unregisterTransport(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));
            jingle->unregisterTransport(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));
        }
        connect(jingle, &XMPP::Jingle::Manager::incomingSession, this,
                [transportProfile](XMPP::Jingle::Session *session) {
                    for (auto *content : session->contentList()) {
                        QVERIFY(content->transport());
                        const auto ns = content->transport()->pad()->ns();
                        if (transportProfile == QStringLiteral("ice"))
                            // Iris v1.1.2 exposes these public protocol constants in headers but does not export
                            // the corresponding QString data symbols from the Windows DLL. Keep this live-test
                            // assertion source-compatible with that release; Iris #113 exports the symbols.
                            QVERIFY(ns == QStringLiteral("urn:xmpp:jingle:transports:ice:0")
                                    || ns == QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1"));
                        qCInfo(lcProgressiveLive) << "LIVE_RANGE_TRANSPORT" << ns;
                    }
                });
        const auto trust = QCA::CertificateCollection::fromFlatTextFile(qEnvironmentVariable("ANYKEEP_LIVE_CA"));
        QVERIFY(!trust.certificates().isEmpty());
        backend.tlsHandler_->tls()->setTrustedCertificates(trust);
        qInfo() << "TLS provider available" << QCA::isSupported("tls");
        connect(backend.tlsHandler_->tls(), &QCA::TLS::error, this,
                [&] { qInfo() << "TLS error code" << backend.tlsHandler_->tls()->errorCode(); });
        bool             done = false;
        XmppStatusResult ready;
        backend.probeAsync([&](XmppStatusResult status) {
            ready = status;
            done  = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        QVERIFY2(ready.ok, qPrintable(ready.error));
        if (role == QStringLiteral("publisher")) {
            const auto linked = store->referenceFile(video);
            QVERIFY2(linked, qPrintable(linked.error));
            XmppRemoteNote note;
            note.title    = QStringLiteral("Progressive video fixture");
            note.content  = QStringLiteral("<video src=\"") + linked.value.uri() + QStringLiteral("\"></video>");
            note.modified = QDateTime::currentDateTimeUtc();
            note.media.append({ linked.value, {} });
            done = false;
            XmppNoteResult saved;
            QElapsedTimer  elapsed;
            elapsed.start();
            backend.saveNoteAsync(note, [&](XmppNoteResult result) {
                saved = std::move(result);
                done  = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(done, 180000);
            QVERIFY2(saved.ok, qPrintable(saved.error));
            QVERIFY(saved.note.media[0].fileSharingXml.contains("encrypted-chunks"));
            qCInfo(lcProgressiveLive) << "LIVE_PUBLISHED_MS" << elapsed.elapsed() << "BYTES" << QFileInfo(video).size();
            QFile published(directory + QStringLiteral("/published"));
            QVERIFY(published.open(QIODevice::WriteOnly));
            published.write(saved.note.id.toUtf8());
            published.close();
            QElapsedTimer wait;
            wait.start();
            while (!QFile::exists(directory + QStringLiteral("/stop")) && wait.elapsed() < 600000)
                QTest::qWait(100);
            QVERIFY(QFile::exists(directory + QStringLiteral("/stop")));
            done = false;
            backend.deleteNoteAsync(saved.note.id, [&](XmppStatusResult) { done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
        } else {
            done = false;
            XmppNoteResult loaded;
            QElapsedTimer  elapsed;
            elapsed.start();
            QFile published(directory + QStringLiteral("/published"));
            QVERIFY(published.open(QIODevice::ReadOnly));
            backend.getNoteAsync(QString::fromUtf8(published.readAll()), [&](XmppNoteResult result) {
                loaded = std::move(result);
                done   = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(done, 60000);
            QVERIFY2(loaded.ok, qPrintable(loaded.error));
            qCInfo(lcProgressiveLive) << "LIVE_NOTE_LOADED_MS" << elapsed.elapsed();
            auto reference = loaded.note.media[0].reference;
            reference.remoteData.insert(QStringLiteral("xmpp.sfs"), loaded.note.media[0].fileSharingXml);
            reference.remoteData.insert(QStringLiteral("xmpp.instance"), config.instanceId);
            QVERIFY(!store->contains(reference));
#ifdef IRIS_FT_DEFERRED_RECEIPTS
            QDomDocument descriptorXml;
            QVERIFY(descriptorXml.setContent(loaded.note.media[0].fileSharingXml, true));
            XMPP::StatelessFileSharing::FileSharing sharing(descriptorXml.documentElement());
            IrisChunkedMediaSource                  chunked;
            for (const auto &source : sharing.sources().items()) {
                const auto parsed = IrisChunkedMediaSource::fromSource(source, reference.size, reference.checksum);
                if (parsed)
                    chunked = parsed.value;
            }
            QVERIFY(chunked.isValid());
            const auto parameters = chunked.parameters;
            QVERIFY(parameters.chunkCount() > 8);
            const QByteArray identity = parameters.rootKey + parameters.noncePrefix + parameters.plainChecksum
                + QByteArray::number(parameters.plainSize) + ':' + QByteArray::number(parameters.chunkSize);
            const QString cacheDirectory = Utils::anykeepDataDir() + QStringLiteral("/media-ranges/")
                + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
            QVERIFY(QDir().mkpath(cacheDirectory));
            const QSet<quint64> seeded { parameters.chunkCount() / 4, parameters.chunkCount() / 4 + 1,
                                         parameters.chunkCount() / 2, parameters.chunkCount() / 2 + 1,
                                         parameters.chunkCount() - 2, parameters.chunkCount() - 1 };
            const auto          seededTime = QDateTime::fromSecsSinceEpoch(946684800, Qt::UTC);
            QFile               seedOriginal(video);
            QVERIFY(seedOriginal.open(QIODevice::ReadOnly));
            for (const auto index : seeded) {
                QVERIFY(seedOriginal.seek(index * parameters.chunkSize));
                const auto record
                    = MediaChunkWire::encryptChunk(parameters, index, seedOriginal.read(parameters.chunkSize));
                QVERIFY(record);
                QFile file(cacheDirectory + '/' + QString::number(index));
                QVERIFY(file.open(QIODevice::WriteOnly));
                QCOMPARE(file.write(record.value), record.value.size());
                QVERIFY(file.setFileTime(seededTime, QFileDevice::FileModificationTime));
            }
            struct RequestedGap {
                quint64 first, count;
                QString sid;
            };
            QList<RequestedGap> requests;
            int                 added                    = 0;
            bool                sessionCompleted         = false;
            qint64              lastVerifiedBytes        = 0;
            bool                activeGapReportedWaiting = false;
            QObject             progressObserver;
            connect(MediaSyncService::instance(), &MediaSyncService::snapshotChanged, &progressObserver,
                    [&](const QString &key) {
                        if (key != MediaSyncService::key(reference))
                            return;
                        const auto status = MediaSyncService::instance()->snapshot(reference);
                        if (added > 0 && status.verifiedBytes > lastVerifiedBytes
                            && status.state != MediaSyncSnapshot::Complete
                            && status.state != MediaSyncSnapshot::Transferring)
                            activeGapReportedWaiting = true;
                        lastVerifiedBytes = status.verifiedBytes;
                    });
            connect(backend.client_, &XMPP::Client::xmlOutgoing, this, [&](const QString &xml) {
                QDomDocument doc;
                if (!doc.setContent(xml, true))
                    return;
                const auto jingle = doc.documentElement().firstChildElement(QStringLiteral("jingle"));
                const auto action = jingle.attribute(QStringLiteral("action"));
                if (action == QStringLiteral("session-terminate")
                    && !jingle.firstChildElement(QStringLiteral("reason"))
                            .firstChildElement(QStringLiteral("success"))
                            .isNull())
                    sessionCompleted = true;
                if (action != QStringLiteral("content-add") && action != QStringLiteral("session-accept"))
                    return;
                const auto                       content = jingle.firstChildElement(QStringLiteral("content"));
                XMPP::Jingle::FileTransfer::File file(
                    content.firstChildElement(QStringLiteral("description")).firstChildElement(QStringLiteral("file")));
                const auto range = file.range();
                if (!range.isValid() || !range.length)
                    return;
                const auto span = quint64(parameters.chunkSize) + MediaChunkWire::Overhead;
                requests.append(
                    { range.offset / span, (range.length + span - 1) / span, jingle.attribute(QStringLiteral("sid")) });
                if (action == QStringLiteral("content-add"))
                    ++added;
            });
#endif
            QNetworkAccessManager manager;
            const auto            url = MediaRangeService::urlFor(reference);
            QVERIFY(!url.isEmpty());
            for (const qint64 offset : { qint64(0), reference.size / 2, reference.size - 65536 }) {
                QNetworkRequest request(url);
                request.setRawHeader("Range",
                                     "bytes=" + QByteArray::number(offset) + '-' + QByteArray::number(offset + 65535));
                auto      *reply = manager.get(request);
                QSignalSpy finished(reply, &QNetworkReply::finished);
                QVERIFY(finished.wait(60000));
                QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 206);
                const auto bytes = reply->readAll();
                QFile      original(video);
                QVERIFY(original.open(QIODevice::ReadOnly));
                QVERIFY(original.seek(offset));
                QCOMPARE(bytes, original.read(65536));
                qCInfo(lcProgressiveLive) << "LIVE_RANGE_VERIFIED" << offset << bytes.size();
                reply->deleteLater();
            }
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE)
            QElapsedTimer playbackTime;
            playbackTime.start();
            QMediaPlayer player;
            QVideoSink   sink;
            int          frames        = 0;
            qint64       lastFrameTime = -1;
            connect(&sink, &QVideoSink::videoFrameChanged, this, [&](const QVideoFrame &frame) {
                if (frame.isValid()) {
                    ++frames;
                    lastFrameTime = frame.startTime();
                }
            });
            player.setVideoSink(&sink);
            player.setSource(url);
            player.play();
            QTRY_VERIFY_WITH_TIMEOUT(frames > 0 || player.error() != QMediaPlayer::NoError, 120000);
            QVERIFY2(frames > 0, qPrintable(player.errorString()));
            qCInfo(lcProgressiveLive) << "LIVE_FIRST_FRAME" << frames << "SIZE" << reference.size << "ELAPSED_MS"
                                      << playbackTime.elapsed();
            const auto duration = player.duration();
            QVERIFY(duration > 0);
            for (const auto seekPosition : { duration / 2, duration * 3 / 4, duration / 4, duration * 2 / 3 }) {
                frames        = 0;
                lastFrameTime = -1;
                player.setPosition(seekPosition);
                qCInfo(lcProgressiveLive)
                    << "LIVE_SEEK_REQUEST_MS" << seekPosition << "POSITION_MS" << player.position();
                QTRY_VERIFY_WITH_TIMEOUT((frames > 0 && lastFrameTime >= (seekPosition - 1000) * 1000
                                          && lastFrameTime <= (seekPosition + 5000) * 1000)
                                             || player.error() != QMediaPlayer::NoError,
                                         120000);
                QVERIFY2(frames > 0 && lastFrameTime >= (seekPosition - 1000) * 1000
                             && lastFrameTime <= (seekPosition + 5000) * 1000,
                         qPrintable(player.errorString()));
                qCInfo(lcProgressiveLive) << "LIVE_SEEK_FRAME_MS" << lastFrameTime / 1000;
            }
            player.stop();
            player.setSource(QUrl());
#endif
            qint64       cachedBytes = 0;
            QDirIterator cache(Utils::anykeepDataDir() + QStringLiteral("/media-ranges"), QDir::Files,
                               QDirIterator::Subdirectories);
            while (cache.hasNext()) {
                cache.next();
                cachedBytes += cache.fileInfo().size();
            }
            qCInfo(lcProgressiveLive) << "LIVE_CACHED_WIRE_BYTES" << cachedBytes;
#ifdef IRIS_FT_DEFERRED_RECEIPTS
            QTRY_COMPARE_WITH_TIMEOUT(QDir(cacheDirectory).entryList(QDir::Files).size(),
                                      qsizetype(parameters.chunkCount()), 180000);
            QTRY_VERIFY_WITH_TIMEOUT(sessionCompleted, 30000);
            QVERIFY2(!activeGapReportedWaiting, "Receiving a successor gap must remain transferring");
            for (const auto index : seeded)
                QCOMPARE(QFileInfo(cacheDirectory + '/' + QString::number(index)).lastModified().toSecsSinceEpoch(),
                         seededTime.toSecsSinceEpoch());
            QSet<QString> sessionIds;
            for (const auto &gap : requests) {
                sessionIds.insert(gap.sid);
                for (const auto index : seeded)
                    QVERIFY2(index < gap.first || index >= gap.first + gap.count, "Requested an already cached chunk");
            }
            if (transportProfile == QStringLiteral("ice")) {
                QVERIFY(added > 0);
                QCOMPARE(sessionIds.size(), 1);
            }
            qCInfo(lcProgressiveLive) << "LIVE_FULL_SYNC_GAPS" << requests.size() << "CONTENT_ADDS" << added
                                      << "SESSIONS" << sessionIds.size() << "CHUNKS" << parameters.chunkCount();
#else
            QVERIFY(cachedBytes > 0 && cachedBytes < reference.size / 10);
#endif
            // Cached records remain usable after disconnect; unauthenticated
            // bytes must never reach the decoder, even from persistent storage.
            backend.shutdown();
            QNetworkRequest offlineRequest(url);
            offlineRequest.setRawHeader("Range", "bytes=0-65535");
            auto      *offlineReply = manager.get(offlineRequest);
            QSignalSpy offlineFinished(offlineReply, &QNetworkReply::finished);
            QVERIFY(offlineFinished.wait(10000));
            QCOMPARE(offlineReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 206);
            QFile original(video);
            QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(offlineReply->readAll(), original.read(65536));
            offlineReply->deleteLater();
            const auto cachedPath = backend.cachedMediaChunk_;
            QVERIFY(!cachedPath.isEmpty());
            QFile corrupted(cachedPath);
            QVERIFY(corrupted.open(QIODevice::ReadWrite));
            auto byte = corrupted.read(1);
            QCOMPARE(byte.size(), 1);
            byte[0] = char(uchar(byte[0]) ^ 1);
            QVERIFY(corrupted.seek(0));
            QCOMPARE(corrupted.write(byte), qint64(1));
            corrupted.close();
            backend.cachedMediaChunk_.clear();
            backend.cachedMediaPlain_.clear();
            auto      *corruptReply = manager.get(offlineRequest);
            QSignalSpy corruptFinished(corruptReply, &QNetworkReply::finished);
            QVERIFY(corruptFinished.wait(10000));
            QCOMPARE(corruptReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 503);
            QVERIFY(corruptReply->readAll().isEmpty());
            corruptReply->deleteLater();
            qCInfo(lcProgressiveLive) << "LIVE_OFFLINE_CACHE_AND_CORRUPTION_VERIFIED";
            MediaRangeService::releaseUrl(url);
            QFile stop(directory + QStringLiteral("/stop"));
            QVERIFY(stop.open(QIODevice::WriteOnly));
            stop.close();
        }
        backend.shutdown();
    }
};
}
using AnyKeep::IrisProgressiveMediaLiveTest;
QTEST_MAIN(IrisProgressiveMediaLiveTest)
#include "irisprogressivemedialive_test.moc"
