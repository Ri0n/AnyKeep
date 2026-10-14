#include "mediachunkscheduler.h"
#include <QtTest>
#include <functional>
#include <utility>

using namespace AnyKeep;

class MediaChunkSchedulerTest : public QObject {
    Q_OBJECT
private slots:
    void oneFetchForMultipleWaiters()
    {
        MediaChunkScheduler scheduler;
        int starts = 0;
        int deliveries = 0;
        MediaChunkScheduler::Completion complete;
        const auto fetch = [&](MediaChunkScheduler::Completion done) {
            ++starts;
            complete = std::move(done);
        };
        const auto callback = [&](QByteArray bytes, QString error) {
            QVERIFY(error.isEmpty());
            QCOMPARE(bytes, QByteArrayLiteral("verified"));
            ++deliveries;
        };
        const quint64 first = scheduler.request(QStringLiteral("server/account/revision"), 5,
                                                QStringLiteral("webdav"), 10, fetch, callback);
        const quint64 second = scheduler.request(QStringLiteral("server/account/revision"), 5,
                                                 QStringLiteral("webdav"), 100, fetch, callback);
        QVERIFY(first && second && first != second);
        QTRY_COMPARE(starts, 1);
        QCOMPARE(scheduler.activeCount(), 1);
        QCOMPARE(scheduler.pendingCount(), 0);
        complete(QByteArrayLiteral("verified"), {});
        QTRY_COMPARE(deliveries, 2);
        QCOMPARE(scheduler.activeCount(), 0);
        QVERIFY(!scheduler.cancel(first));
        QVERIFY(!scheduler.cancel(second));
    }

    void foregroundPriorityAndSingleLaneAdmission()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(2);
        scheduler.setLaneMaxActive(QStringLiteral("dav"), 1);
        QStringList started;
        QHash<QString, MediaChunkScheduler::Completion> finish;
        const auto fetch = [&](QString key) {
            return [&, key](MediaChunkScheduler::Completion done) {
                started.append(key);
                finish.insert(key, std::move(done));
            };
        };
        const auto callback = [](QByteArray, QString error) { QVERIFY(error.isEmpty()); };
        scheduler.request(QStringLiteral("active"), 0, QStringLiteral("dav"), 0,
                          fetch(QStringLiteral("active")), callback);
        QTRY_COMPARE(started, QStringList({QStringLiteral("active")}));
        scheduler.request(QStringLiteral("low"), 0, QStringLiteral("dav"), 1,
                          fetch(QStringLiteral("low")), callback);
        const quint64 high = scheduler.request(QStringLiteral("high"), 0, QStringLiteral("dav"), 2,
                                                fetch(QStringLiteral("high")), callback);
        QVERIFY(high);
        QVERIFY(scheduler.reprioritize(high, 50));
        scheduler.request(QStringLiteral("xmpp"), 0, QStringLiteral("jingle"), 5,
                          fetch(QStringLiteral("xmpp")), callback);
        QTRY_COMPARE(started.size(), 2);
        QVERIFY(started.contains(QStringLiteral("xmpp")));
        QCOMPARE(scheduler.pendingCount(), 2);
        finish.take(QStringLiteral("active"))({}, {});
        QTRY_COMPARE(started.size(), 3);
        QCOMPARE(started[2], QStringLiteral("high"));
        finish.take(QStringLiteral("high"))({}, {});
        QTRY_COMPARE(started.size(), 4);
        QCOMPARE(started[3], QStringLiteral("low"));
        finish.take(QStringLiteral("low"))({}, {});
        finish.take(QStringLiteral("xmpp"))({}, {});
        QTRY_COMPARE(scheduler.activeCount(), 0);
    }

    void cancellationDetachesOnlyOneWaiter()
    {
        MediaChunkScheduler scheduler;
        int starts = 0;
        MediaChunkScheduler::Completion finish;
        int canceled = 0, received = 0;
        const auto fetch = [&](MediaChunkScheduler::Completion callback) {
            ++starts;
            finish = std::move(callback);
        };
        const auto first = scheduler.request(QStringLiteral("rev"), 1, QStringLiteral("xmpp"), 10, fetch,
                                             [&](QByteArray bytes, QString error) {
                                                 QVERIFY(bytes.isEmpty());
                                                 QVERIFY(!error.isEmpty());
                                                 ++canceled;
                                             });
        scheduler.request(QStringLiteral("rev"), 1, QStringLiteral("xmpp"), 10, fetch,
                          [&](QByteArray bytes, QString error) {
                              QVERIFY(error.isEmpty());
                              QCOMPARE(bytes, QByteArrayLiteral("ok"));
                              ++received;
                          });
        QTRY_COMPARE(starts, 1);
        QVERIFY(scheduler.cancel(first));
        QCOMPARE(canceled, 1);
        QCOMPARE(starts, 1);
        finish(QByteArrayLiteral("ok"), {});
        QTRY_COMPARE(received, 1);
        QCOMPARE(canceled, 1);
        QCOMPARE(starts, 1);
    }

    void canceledQueueDoesNotStartAndShutdownSettlesAll()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(1);
        int starts = 0, failures = 0;
        MediaChunkScheduler::Completion staleCompletion;
        auto fetch = [&](MediaChunkScheduler::Completion done) {
            ++starts;
            staleCompletion = std::move(done);
        };
        const auto failed = [&](QByteArray bytes, QString error) {
            QVERIFY(bytes.isEmpty());
            QVERIFY(!error.isEmpty());
            ++failures;
        };
        scheduler.request(QStringLiteral("first"), 0, QStringLiteral("dav"), 0, fetch, failed);
        QTRY_COMPARE(starts, 1);
        auto queued = scheduler.request(QStringLiteral("second"), 0, QStringLiteral("dav"), 10, fetch, failed);
        QVERIFY(queued);
        QVERIFY(scheduler.cancel(queued));
        QCOMPARE(failures, 1);
        QCOMPARE(scheduler.pendingCount(), 0);
        scheduler.shutdown();
        QCOMPARE(failures, 2);
        QCOMPARE(scheduler.activeCount(), 0);
        staleCompletion(QByteArrayLiteral("late"), {});
        QTest::qWait(20);
        QCOMPARE(failures, 2);
        QCOMPARE(starts, 1);
        QVERIFY(!scheduler.request(QStringLiteral("third"), 0, QStringLiteral("dav"), 0, fetch, failed));
        QCOMPARE(failures, 3);
    }

    void immutableKeysAndTransportGroupsAreIsolated()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(3);
        scheduler.setLaneMaxActive(QStringLiteral("dav"), 3);
        int starts = 0;
        QList<MediaChunkScheduler::Completion> finishes;
        const auto fetch = [&](MediaChunkScheduler::Completion done) {
            ++starts;
            finishes.append(std::move(done));
        };
        int completions = 0, mismatches = 0;
        const auto okay = [&](QByteArray, QString error) {
            QVERIFY(error.isEmpty());
            ++completions;
        };
        scheduler.request(QStringLiteral("accountA:etag1"), 0, QStringLiteral("dav"), 1, fetch, okay);
        scheduler.request(QStringLiteral("accountB:etag1"), 0, QStringLiteral("dav"), 1, fetch, okay);
        scheduler.request(QStringLiteral("accountA:etag2"), 0, QStringLiteral("dav"), 1, fetch, okay);
        scheduler.request(QStringLiteral("accountA:etag1"), 0, QStringLiteral("xmpp"), 1, fetch,
                          [&](QByteArray, QString error) {
                              QVERIFY(!error.isEmpty());
                              ++mismatches;
                          });
        QCOMPARE(mismatches, 1);
        QTRY_COMPARE(starts, 3);
        for (auto &finish : finishes)
            finish({}, {});
        QTRY_COMPARE(completions, 3);
    }
};

QTEST_GUILESS_MAIN(MediaChunkSchedulerTest)
#include "mediachunkscheduler_test.moc"
