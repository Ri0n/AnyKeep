#include "mediachunkscheduler.h"
#include <QtTest>
#include <QThread>
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

    void sustainedForegroundDemandDoesNotStarveBackground()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(1);
        QStringList started;
        QHash<QString, MediaChunkScheduler::Completion> finishes;
        const auto fetch = [&](const QString &name) {
            return [&, name](MediaChunkScheduler::Completion done) {
                started.append(name);
                finishes.insert(name, std::move(done));
            };
        };
        const auto done = [](QByteArray, QString error) { QVERIFY(error.isEmpty()); };
        scheduler.request(QStringLiteral("active"), 0, QStringLiteral("dav"), 100,
                          fetch(QStringLiteral("active")), done);
        QTRY_COMPARE(started.size(), 1);
        scheduler.request(QStringLiteral("background"), 0, QStringLiteral("dav"), 0,
                          fetch(QStringLiteral("background")), done);
        for (int i = 0; i < 12; ++i) {
            const QString name = QStringLiteral("foreground-%1").arg(i);
            scheduler.request(name, 0, QStringLiteral("dav"), 100, fetch(name), done);
        }
        finishes.take(QStringLiteral("active"))({}, {});
        for (int i = 0; i < 4; ++i) {
            QTRY_COMPARE(started.size(), i + 2);
            QVERIFY(started.last().startsWith(QStringLiteral("foreground-")));
            finishes.take(started.last())({}, {});
        }
        QTRY_COMPARE(started.size(), 6);
        QCOMPARE(started.last(), QStringLiteral("background"));
        finishes.take(QStringLiteral("background"))({}, {});
        for (int i = 0; i < 8; ++i) {
            QTRY_COMPARE(started.size(), i + 7);
            finishes.take(started.last())({}, {});
        }
        QTRY_COMPARE(scheduler.activeCount(), 0);
    }

    void fairnessSkipsLaneBlockedOldestWithoutStalling()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(2);
        scheduler.setLaneMaxActive(QStringLiteral("dav"), 1);
        QStringList started;
        QHash<QString, MediaChunkScheduler::Completion> completions;
        const auto fetch = [&](const QString &name) {
            return [&, name](MediaChunkScheduler::Completion done) {
                started.append(name);
                completions.insert(name, std::move(done));
            };
        };
        const auto done = [](QByteArray, QString error) { QVERIFY(error.isEmpty()); };
        scheduler.request(QStringLiteral("active-dav"), 0, QStringLiteral("dav"), 100,
                          fetch(QStringLiteral("active-dav")), done);
        QTRY_COMPARE(started.size(), 1);
        scheduler.request(QStringLiteral("blocked-dav"), 0, QStringLiteral("dav"), 0,
                          fetch(QStringLiteral("blocked-dav")), done);
        scheduler.request(QStringLiteral("jingle"), 0, QStringLiteral("jingle"), 10,
                          fetch(QStringLiteral("jingle")), done);
        QTRY_COMPARE(started.size(), 2);
        QCOMPARE(started.last(), QStringLiteral("jingle"));
        QCOMPARE(scheduler.pendingCount(), 1);
        completions.take(QStringLiteral("active-dav"))({}, {});
        QTRY_COMPARE(started.size(), 3);
        QCOMPARE(started.last(), QStringLiteral("blocked-dav"));
        completions.take(QStringLiteral("jingle"))({}, {});
        completions.take(QStringLiteral("blocked-dav"))({}, {});
        QTRY_COMPARE(scheduler.activeCount(), 0);
    }

    void aggregateByteBudgetPacesNewAdmissionsWithoutInterruptingJingle()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(2);
        scheduler.setBandwidthLimitBytesPerSecond(4000);
        QCOMPARE(scheduler.bandwidthLimitBytesPerSecond(), qint64(4000));
        QObject liveJingle;
        scheduler.observeExternalTransfer(&liveJingle);
        QCOMPARE(scheduler.externalActiveCount(), 1);
        // An authenticated SCTP record used more than one second's byte
        // budget. Existing SCTP sessions are NEVER interrupted or slowed.
        scheduler.reportTransferResult(QStringLiteral("jingle"), 4400, false);
        QCOMPARE(scheduler.externalActiveCount(), 1);
        int starts = 0;
        MediaChunkScheduler::Completion completion;
        scheduler.request(QStringLiteral("dav"), 0, QStringLiteral("webdav"), 100,
                          [&](MediaChunkScheduler::Completion finish) {
                              ++starts;
                              completion = std::move(finish);
                          },
                          [](QByteArray, QString error) { QVERIFY(error.isEmpty()); });
        QTest::qWait(25);
        QCOMPARE(starts, 0);
        QCOMPARE(scheduler.activeCount(), 1);
        QTRY_COMPARE(starts, 1);
        QCOMPARE(scheduler.activeCount(), 2);
        completion({}, {});
        QTRY_COMPARE(scheduler.activeCount(), 1);
        scheduler.releaseExternalTransfer(&liveJingle);
        QCOMPARE(scheduler.activeCount(), 0);
        scheduler.setBandwidthLimitBytesPerSecond(0);
        QCOMPARE(scheduler.bandwidthLimitBytesPerSecond(), qint64(0));
    }

    void workerThreadTrafficAccountingIsMarshaledBeforeAdmission()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(1);
        scheduler.setBandwidthLimitBytesPerSecond(1000);
        auto *worker = QThread::create([&scheduler] {
            scheduler.reportTransferResult(QStringLiteral("jingle"), 2000, false);
        });
        worker->start();
        QVERIFY(worker->wait(5000));
        delete worker;
        // Drain the queued worker-thread accounting event on the owning thread.
        QCoreApplication::processEvents();
        int starts = 0;
        MediaChunkScheduler::Completion completion;
        scheduler.request(QStringLiteral("dav"), 0, QStringLiteral("webdav"), 10,
                          [&](MediaChunkScheduler::Completion finish) {
                              ++starts;
                              completion = std::move(finish);
                          },
                          [](QByteArray, QString error) { QVERIFY(error.isEmpty()); });
        QTest::qWait(40);
        QCOMPARE(starts, 0);
        QTRY_COMPARE(starts, 1);
        completion({}, {});
        QTRY_COMPARE(scheduler.activeCount(), 0);
    }

    void congestionBackoffIsLaneScopedAndSelfReleasing()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(2);
        QStringList started;
        QHash<QString, MediaChunkScheduler::Completion> completions;
        const auto fetch = [&](const QString &name) {
            return [&, name](MediaChunkScheduler::Completion done) {
                started.append(name);
                completions.insert(name, std::move(done));
            };
        };
        const auto done = [](QByteArray, QString error) { QVERIFY(error.isEmpty()); };
        scheduler.request(QStringLiteral("first"), 0, QStringLiteral("account-a"), 100,
                          fetch(QStringLiteral("first")), done);
        QTRY_COMPARE(started.size(), 1);
        scheduler.request(QStringLiteral("second"), 0, QStringLiteral("account-a"), 100,
                          fetch(QStringLiteral("second")), done);
        scheduler.reportTransferResult(QStringLiteral("account-a"), 0, true);
        completions.take(QStringLiteral("first"))({}, {});
        scheduler.request(QStringLiteral("other"), 0, QStringLiteral("account-b"), 0,
                          fetch(QStringLiteral("other")), done);
        QTRY_COMPARE(started.size(), 2);
        QCOMPARE(started.last(), QStringLiteral("other"));
        QTest::qWait(50);
        QCOMPARE(started.size(), 2); // A's retry cooldown must not block B.
        completions.take(QStringLiteral("other"))({}, {});
        QTRY_COMPARE(started.size(), 3);
        QCOMPARE(started.last(), QStringLiteral("second"));
        completions.take(QStringLiteral("second"))({}, {});
        QTRY_COMPARE(scheduler.activeCount(), 0);
    }

    void zeroBandwidthLimitDoesNotDelayUncappedDownloads()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(1);
        scheduler.reportTransferResult(QStringLiteral("jingle"), 10000000, false);
        int starts = 0;
        MediaChunkScheduler::Completion completion;
        scheduler.request(QStringLiteral("dav"), 0, QStringLiteral("dav"), 0,
                          [&](MediaChunkScheduler::Completion finish) {
                              ++starts;
                              completion = std::move(finish);
                          },
                          [](QByteArray, QString error) { QVERIFY(error.isEmpty()); });
        QTRY_COMPARE(starts, 1);
        completion({}, {});
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

    void cancelLastWaiterAbortsPhysicalFetchButHoldsNetworkSlot()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(1);
        int starts = 0, aborts = 0, cancellations = 0;
        QList<MediaChunkScheduler::Completion> completions;
        const auto fetch = [&](MediaChunkScheduler::Completion done) -> MediaChunkScheduler::Abort {
            ++starts;
            completions.append(std::move(done));
            return [&] { ++aborts; };
        };
        const quint64 ticket = scheduler.requestCancelable(
            QStringLiteral("file"), 0, QStringLiteral("dav"), 100, fetch,
            [&](QByteArray bytes, QString error) {
                QVERIFY(bytes.isEmpty());
                QVERIFY(!error.isEmpty());
                ++cancellations;
            });
        QVERIFY(ticket);
        QTRY_COMPARE(starts, 1);
        const quint64 next = scheduler.requestCancelable(
            QStringLiteral("another"), 0, QStringLiteral("dav"), 50, fetch,
            [&](QByteArray, QString error) { QVERIFY(error.isEmpty()); });
        QVERIFY(next);
        QVERIFY(scheduler.cancel(ticket));
        QCOMPARE(aborts, 1);
        QCOMPARE(cancellations, 1);
        QCOMPARE(scheduler.activeCount(), 1);
        QCOMPARE(scheduler.pendingCount(), 1);
        QCOMPARE(starts, 1); // Abort has NOT released physical admission.
        completions.takeFirst()({}, QStringLiteral("Aborted"));
        QTRY_COMPARE(starts, 2);
        completions.takeFirst()({}, {});
        QTRY_COMPARE(scheduler.activeCount(), 0);
        QCOMPARE(aborts, 1);
    }

    void lateWaiterRetriesAfterOrphanAbort()
    {
        MediaChunkScheduler scheduler;
        int starts = 0, aborts = 0, canceled = 0, delivered = 0;
        QList<MediaChunkScheduler::Completion> completions;
        auto fetch = [&](MediaChunkScheduler::Completion done) -> MediaChunkScheduler::Abort {
            ++starts;
            completions.append(std::move(done));
            return [&] { ++aborts; };
        };
        const quint64 oldTicket = scheduler.requestCancelable(
            QStringLiteral("rev"), 9, QStringLiteral("dav"), 10, fetch,
            [&](QByteArray, QString error) { QVERIFY(!error.isEmpty()); ++canceled; });
        QTRY_COMPARE(starts, 1);
        QVERIFY(scheduler.cancel(oldTicket));
        QCOMPARE(aborts, 1);
        scheduler.requestCancelable(
            QStringLiteral("rev"), 9, QStringLiteral("dav"), 100, fetch,
            [&](QByteArray bytes, QString error) {
                QVERIFY(error.isEmpty());
                QCOMPARE(bytes, QByteArrayLiteral("authenticated"));
                ++delivered;
            });
        QCOMPARE(starts, 1);
        completions.takeFirst()({}, QStringLiteral("Aborted previous waiter"));
        QTRY_COMPARE(starts, 2);
        QCOMPARE(delivered, 0);
        completions.takeFirst()(QByteArrayLiteral("authenticated"), {});
        QTRY_COMPARE(delivered, 1);
        QCOMPARE(canceled, 1);
        QCOMPARE(scheduler.activeCount(), 0);
    }

    void stoppedTransportCanReleaseAnOrphanedActiveSlot()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(1);
        int started = 0, failures = 0;
        MediaChunkScheduler::Completion stale;
        auto fetch = [&](MediaChunkScheduler::Completion finish) {
            ++started;
            stale = std::move(finish);
        };
        const quint64 ticket = scheduler.request(QStringLiteral("abandoned"), 0,
                                                 QStringLiteral("nextcloud"), 10, fetch,
                                                 [&](QByteArray, QString error) {
                                                     QVERIFY(!error.isEmpty());
                                                     ++failures;
                                                 });
        QTRY_COMPARE(started, 1);
        QVERIFY(scheduler.abandonStopped(ticket));
        QCOMPARE(failures, 1);
        QCOMPARE(scheduler.activeCount(), 0);
        // A late completion from a defunct worker must not resurrect state.
        stale(QByteArrayLiteral("obsolete"), {});
        QTest::qWait(10);
        QCOMPARE(scheduler.activeCount(), 0);
        QCOMPARE(failures, 1);
        QVERIFY(!scheduler.abandonStopped(ticket));
    }

    void observedJingleTransportDefersWebDavWithoutBreakingBundleHandover()
    {
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(2);
        auto *firstSession = new QObject;
        auto *secondSession = new QObject;
        // Multiple Jingle contents in one BUNDLE share a physical session.
        scheduler.observeExternalTransfer(firstSession);
        scheduler.observeExternalTransfer(firstSession);
        QCOMPARE(scheduler.externalActiveCount(), 1);
        scheduler.observeExternalTransfer(secondSession);
        QCOMPARE(scheduler.externalActiveCount(), 2);
        QCOMPARE(scheduler.activeCount(), 2);

        // SCTP/BUNDLE handover is not blocked or forcibly paused. It may
        // briefly oversubscribe if another provider already has a slot.
        auto *handoverSession = new QObject;
        scheduler.observeExternalTransfer(handoverSession);
        QCOMPARE(scheduler.externalActiveCount(), 3);
        int starts = 0, deliveries = 0;
        MediaChunkScheduler::Completion finish;
        scheduler.request(QStringLiteral("dav/account/rev"), 1, QStringLiteral("webdav"), 100,
                          [&](MediaChunkScheduler::Completion completion) {
                              ++starts;
                              finish = std::move(completion);
                          },
                          [&](QByteArray value, QString error) {
                              QVERIFY(error.isEmpty());
                              QCOMPARE(value, QByteArrayLiteral("verified"));
                              ++deliveries;
                          });
        QCoreApplication::processEvents();
        QCOMPARE(starts, 0);
        // Draining an old session frees its physical reservation.
        scheduler.releaseExternalTransfer(firstSession);
        QCOMPARE(scheduler.externalActiveCount(), 2);
        QCoreApplication::processEvents();
        QCOMPARE(starts, 0);
        delete firstSession;
        QCOMPARE(scheduler.externalActiveCount(), 2);
        delete secondSession; // QObject destruction also releases reservation.
        QTRY_COMPARE(starts, 1);
        QCOMPARE(scheduler.activeCount(), 2); // handover + WebDAV
        finish(QByteArrayLiteral("verified"), {});
        QTRY_COMPARE(deliveries, 1);
        QCOMPARE(scheduler.activeCount(), 1);
        delete handoverSession;
        QCOMPARE(scheduler.activeCount(), 0);
    }

    void retiringProviderReleasesOrphanedAndSharedFetches()
    {
        QObject workerOwner;
        QObject survivingOwner;
        MediaChunkScheduler scheduler;
        scheduler.setMaxActive(1);
        int networkStarts = 0;
        int canceled = 0;
        int survivingFailures = 0;
        MediaChunkScheduler::Completion abandonedNetwork;
        auto fetch = [&](MediaChunkScheduler::Completion completion) {
            ++networkStarts;
            abandonedNetwork = std::move(completion);
        };
        const auto staleTicket = scheduler.request(
            QStringLiteral("account:revisionA"), 2, QStringLiteral("dav"), 20, fetch,
            [&](QByteArray, QString error) {
                QVERIFY(!error.isEmpty());
                ++canceled;
            }, &workerOwner);
        QVERIFY(staleTicket);
        scheduler.request(
            QStringLiteral("account:revisionA"), 2, QStringLiteral("dav"), 20, fetch,
            [&](QByteArray, QString error) {
                QVERIFY(!error.isEmpty());
                ++survivingFailures;
            }, &survivingOwner);
        QTRY_COMPARE(networkStarts, 1);
        QVERIFY(scheduler.cancel(staleTicket));
        QCOMPARE(canceled, 1);
        QCOMPARE(scheduler.activeCount(), 1);
        // Even with no old ticket, the original fetch is still owned by the
        // stopped worker. Consumers from another owner fail once and may retry.
        scheduler.retireStoppedOwner(&workerOwner);
        QCOMPARE(scheduler.activeCount(), 0);
        QCOMPARE(survivingFailures, 1);
        QCOMPARE(canceled, 1);
        abandonedNetwork(QByteArrayLiteral("stale"), {});
        QTest::qWait(10);
        QCOMPARE(scheduler.activeCount(), 0);
        QCOMPARE(survivingFailures, 1);

        int retried = 0;
        auto finishRetry = MediaChunkScheduler::Completion();
        const auto retry = [&](MediaChunkScheduler::Completion completion) {
            ++networkStarts;
            finishRetry = std::move(completion);
        };
        scheduler.request(QStringLiteral("account:revisionA"), 2, QStringLiteral("dav"),
                          20, retry,
                          [&](QByteArray bytes, QString error) {
                              QVERIFY(error.isEmpty());
                              QCOMPARE(bytes, QByteArrayLiteral("fresh"));
                              ++retried;
                          }, &survivingOwner);
        QTRY_COMPARE(networkStarts, 2);
        finishRetry(QByteArrayLiteral("fresh"), {});
        QTRY_COMPARE(retried, 1);
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
