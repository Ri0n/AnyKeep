#include "mediarangeservice.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <atomic>
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
