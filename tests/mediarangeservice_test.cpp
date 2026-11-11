#include "mediarangeservice.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <atomic>
#include <memory>
using namespace AnyKeep;
class MediaRangeServiceTest : public QObject {
    Q_OBJECT
private slots:
    void oldStreamContinuationDoesNotReprioritizeSeek()
    {
        QObject        owner;
        MediaReference reference;
        reference.id           = QUuid::createUuid();
        reference.portableName = QStringLiteral("video.avi");
        reference.size         = 8 * 65536;
        QList<qint64>                 priorities;
        QList<qint64>                 continuations;
        MediaRangeService::Completion oldRead;
        MediaRangeService::registerResolver(
            &owner, [reference](const MediaReference &r) { return r.id == reference.id; },
            [&](MediaReference, qint64 offset, qint64 length, bool prioritize, MediaRangeService::Completion done) {
                (prioritize ? priorities : continuations).append(offset);
                if (offset == 0) {
                    oldRead = std::move(done);
                    return;
                }
                done(QByteArray(qsizetype(length), 'v'), {});
            });
        const auto            url = MediaRangeService::urlFor(reference);
        QNetworkAccessManager manager;
        QNetworkRequest       request(url);
        request.setRawHeader("Range", "bytes=0-131071");
        auto      *oldReply = manager.get(request);
        QSignalSpy oldFinished(oldReply, &QNetworkReply::finished);
        QTRY_VERIFY(bool(oldRead));
        request.setRawHeader("Range", "bytes=262144-262159");
        auto      *seekReply = manager.get(request);
        QSignalSpy seekFinished(seekReply, &QNetworkReply::finished);
        QVERIFY(seekFinished.wait(5000));
        QCOMPARE(seekReply->readAll(), QByteArray(16, 'v'));
        oldRead(QByteArray(65536, 'v'), {});
        QVERIFY(oldFinished.wait(5000));
        QCOMPARE(oldReply->readAll(), QByteArray(131072, 'v'));
        QCOMPARE(priorities, QList<qint64>({ 0, 262144 }));
        QCOMPARE(continuations, QList<qint64>({ 65536 }));
        oldReply->deleteLater();
        seekReply->deleteLater();
        MediaRangeService::releaseUrl(url);
    }
    void rangedReadIsAsyncAndBounded()
    {
        QObject        owner;
        MediaReference reference;
        reference.id           = QUuid::createUuid();
        reference.portableName = QStringLiteral("video.avi");
        reference.size         = 100 * 1024 * 1024;
        int calls              = 0;
        MediaRangeService::registerResolver(
            &owner, [reference](const MediaReference &r) { return r.id == reference.id; },
            [&calls, &owner](MediaReference, qint64 offset, qint64 length, MediaRangeService::Completion done) {
                ++calls;
                QVERIFY(length <= 64 * 1024);
                QTimer::singleShot(10, &owner, [offset, length, done] {
                    QByteArray bytes(qsizetype(length), char(offset / 65536));
                    done(bytes, {});
                });
            });
        const auto url = MediaRangeService::urlFor(reference);
        QVERIFY(!url.isEmpty());
        QNetworkAccessManager manager;
        QNetworkRequest       request(url);
        request.setRawHeader("Range", "bytes=65536-196607");
        auto      *reply = manager.get(request);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 206);
        QCOMPARE(reply->rawHeader("Content-Range"), QByteArray("bytes 65536-196607/104857600"));
        QCOMPARE(reply->readAll(), QByteArray(65536, '\1') + QByteArray(65536, '\2'));
        QCOMPARE(calls, 2); // 128 KiB of a 100 MiB object, without downloading its prefix.
        reply->deleteLater();
        request.setRawHeader("Range", "bytes=-17");
        reply = manager.get(request);
        QSignalSpy suffix(reply, &QNetworkReply::finished);
        QVERIFY(suffix.wait(5000));
        QCOMPARE(reply->readAll().size(), 17);
        reply->deleteLater();
        MediaRangeService::releaseUrl(url);
        reply = manager.get(QNetworkRequest(url));
        QSignalSpy revoked(reply, &QNetworkReply::finished);
        QVERIFY(revoked.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
        reply->deleteLater();
    }
    void resolverUsesOwnerThreadAndCancelsOnDestruction()
    {
        struct Worker {
            QThread thread;
            ~Worker()
            {
                thread.quit();
                thread.wait();
            }
        } worker;
        auto *owner = new QObject;
        owner->moveToThread(&worker.thread);
        connect(&worker.thread, &QThread::finished, owner, &QObject::deleteLater);
        MediaReference reference;
        reference.id                    = QUuid::createUuid();
        reference.portableName          = QStringLiteral("remote.bin");
        reference.size                  = 128;
        bool              registered    = false;
        std::atomic<bool> correctThread = false;
        QMetaObject::invokeMethod(
            owner,
            [&, owner] {
                MediaRangeService::registerResolver(
                    owner, [reference](const MediaReference &r) { return r.id == reference.id; },
                    [&, owner](MediaReference, qint64, qint64 length, MediaRangeService::Completion done) {
                        correctThread = QThread::currentThread() == owner->thread();
                        done(QByteArray(qsizetype(length), 'v'), {});
                    });
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(), [&] { registered = true; }, Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
        worker.thread.start();
        QTRY_VERIFY(registered);
        const auto url = MediaRangeService::urlFor(reference);
        QVERIFY(!url.isEmpty());
        QNetworkAccessManager manager;
        auto                 *reply = manager.get(QNetworkRequest(url));
        QSignalSpy            finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->readAll(), QByteArray(128, 'v'));
        QVERIFY(correctThread.load());
        reply->deleteLater();
        worker.thread.quit();
        QVERIFY(worker.thread.wait(5000));
        QCoreApplication::processEvents();
        reply = manager.get(QNetworkRequest(url));
        QSignalSpy removed(reply, &QNetworkReply::finished);
        QVERIFY(removed.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
        reply->deleteLater();
        MediaRangeService::releaseUrl(url);
    }

    void abortedPlayerSocketNotifiesCancelableReader()
    {
        QObject owner;
        MediaReference reference;
        reference.id = QUuid::createUuid();
        reference.portableName = QStringLiteral("cancel.mp4");
        reference.size = 1024 * 1024;
        int started = 0;
        int canceled = 0;
        MediaRangeService::Completion pending;
        MediaRangeService::registerResolver(
            &owner, [reference](const MediaReference &r) { return r.id == reference.id; },
            MediaRangeService::CancelableRequestReader(
                [&](MediaReference, qint64, qint64, bool, MediaRangeService::Completion done)
                    -> MediaRangeService::Cancel {
                    ++started;
                    pending = std::move(done);
                    return [&] { ++canceled; };
                }));
        const auto url = MediaRangeService::urlFor(reference);
        QVERIFY(!url.isEmpty());
        QNetworkAccessManager manager;
        QNetworkRequest req(url);
        req.setRawHeader("Range", "bytes=4096-8191");
        auto *reply = manager.get(req);
        QTRY_COMPARE(started, 1);
        QVERIFY(pending);
        QCOMPARE(canceled, 0);
        reply->abort(); // A seek or decoder teardown closes its HTTP GET.
        QTRY_COMPARE(canceled, 1);
        reply->deleteLater();
        MediaRangeService::releaseUrl(url);
    }

    void directExportReadsVerifiedCachedRangesWithoutHttp()
    {
        QObject owner;
        MediaReference reference;
        reference.id = QUuid::createUuid();
        reference.portableName = QStringLiteral("already-cached-image.png");
        reference.size = 3 * 65536;
        const QByteArray cached(qsizetype(reference.size), 'x');
        int cacheReads = 0;
        int networkTransfers = 0;
        QHash<qint64, QByteArray> verifiedCache;
        for (qint64 pos = 0; pos < reference.size; pos += 65536)
            verifiedCache.insert(pos, cached.mid(pos, 65536));
        QList<qint64> offsets;
        MediaRangeService::registerResolver(
            &owner, [id = reference.id](const MediaReference &r) { return r.id == id; },
            [&](MediaReference, qint64 offset, qint64 length, bool prioritize,
                MediaRangeService::Completion done) {
                QVERIFY(!prioritize); // Saving must not interrupt a playing video.
                QVERIFY(length <= 65536);
                offsets.append(offset);
                ++cacheReads;
                // Simulate the provider's existing cache-first range reader:
                // a missing chunk would use Jingle, but no remote request is
                // needed for a verified local record.
                if (!verifiedCache.contains(offset))
                    ++networkTransfers;
                const QByteArray bytes = verifiedCache.value(offset).left(length);
                QTimer::singleShot(0, &owner, [bytes, done = std::move(done)] {
                    done(bytes, {});
                });
            });
        QByteArray assembled;
        int completions = 0;
        QString error;
        auto next = std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> weakNext = next;
        *next = [&, weakNext] {
            if (assembled.size() == cached.size()) {
                ++completions;
                return;
            }
            const qint64 offset = assembled.size();
            MediaRangeService::readRangeAsync(
                reference, offset, qMin<qint64>(65536, reference.size - offset),
                [&, weakNext](QByteArray bytes, QString failure) {
                    error = std::move(failure);
                    if (!error.isEmpty()) {
                        ++completions;
                        return;
                    }
                    assembled += bytes;
                    if (auto resume = weakNext.lock())
                        (*resume)();
                });
        };
        (*next)();
        QTRY_COMPARE(completions, 1);
        QVERIFY(error.isEmpty());
        QCOMPARE(assembled, cached);
        QCOMPARE(offsets, QList<qint64>({ 0, 65536, 131072 }));
        QCOMPARE(cacheReads, 3);
        QCOMPARE(networkTransfers, 0);
    }

    void invalidDirectExportRangeIsRejected()
    {
        MediaReference reference;
        reference.id = QUuid::createUuid();
        reference.portableName = QStringLiteral("invalid.png");
        reference.size = 65536;
        int completions = 0;
        QString error;
        MediaRangeService::readRangeAsync(reference, reference.size, 10, [&](QByteArray bytes, QString failure) {
            QVERIFY(bytes.isEmpty());
            error = std::move(failure);
            ++completions;
        });
        QCOMPARE(completions, 1);
        QVERIFY(!error.isEmpty());
    }

    void failureDoesNotExposePlaintext()
    {
        QObject        owner;
        MediaReference r;
        r.id           = QUuid::createUuid();
        r.portableName = QStringLiteral("video");
        r.size         = 1024;
        MediaRangeService::registerResolver(
            &owner, [r](const MediaReference &v) { return v.id == r.id; },
            [](MediaReference, qint64, qint64, MediaRangeService::Completion done) {
                done(QByteArray("unverified"), QStringLiteral("Authentication failed"));
            });
        QNetworkAccessManager manager;
        const auto            url   = MediaRangeService::urlFor(r);
        auto                 *reply = manager.get(QNetworkRequest(url));
        QSignalSpy            finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 503);
        QVERIFY(reply->readAll().isEmpty());
        reply->deleteLater();
        MediaRangeService::releaseUrl(url);
    }
};
QTEST_GUILESS_MAIN(MediaRangeServiceTest)
#include "mediarangeservice_test.moc"
