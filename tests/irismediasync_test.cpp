#include "iris/irismediasync.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <iris/xmpp_client.h>

using namespace AnyKeep;
class IrisMediaSyncTest : public QObject {
    Q_OBJECT
    const QByteArray         plain = QByteArray("abcdefghijklmnopqrstuvwxyz0123456789!");
    MediaChunkWireParameters parameters() const
    {
        return *MediaChunkWire::generate(plain.size(), QCryptographicHash::hash(plain, QCryptographicHash::Sha256), 8);
    }
    QByteArray records(const MediaChunkWireParameters &p, quint64 first, quint64 count) const
    {
        QByteArray result;
        for (quint64 i = first; i < first + count; ++i)
            result += MediaChunkWire::encryptChunk(p, i, plain.mid(i * p.chunkSize, p.chunkSize)).value;
        return result;
    }
private slots:
    void cachedIslandsBoundEveryGap()
    {
        IrisMediaGapMap map(12);
        for (quint64 index : { 0, 1, 5, 6, 10, 11 })
            map.markAvailable(index);
        auto gap = map.nextGap(1);
        QVERIFY(gap);
        QCOMPARE(gap->first, quint64(2));
        QCOMPARE(gap->count, quint64(3));
        gap = map.nextGap(6);
        QVERIFY(gap);
        QCOMPARE(gap->first, quint64(7));
        QCOMPARE(gap->count, quint64(3));
        QVERIFY(!map.nextGap(10)); // Cached tail: retain the earlier background content.
        QVERIFY(!map.nextGap(12));
        QVERIFY(!map.nextGap(13));
        QVERIFY(!map.complete());
        for (quint64 index : { 2, 3, 4, 7, 8, 9 })
            map.markAvailable(index);
        QVERIFY(map.complete());
        map.invalidate(6);
        gap = map.nextGap(0);
        QVERIFY(gap);
        QCOMPARE(gap->first, quint64(6));
        QCOMPARE(gap->count, quint64(1));
    }
    void drainingChunkIsReservedWithoutBecomingCached()
    {
        IrisMediaGapMap map(6);
        map.markAvailable(0);
        const QSet<quint64> reserved { 2, 5 };
        auto                gap = map.nextGap(1, reserved);
        QVERIFY(gap);
        QCOMPARE(gap->first, quint64(1));
        QCOMPARE(gap->count, quint64(1));
        gap = map.nextGap(2, reserved);
        QVERIFY(gap);
        QCOMPARE(gap->first, quint64(3));
        QCOMPARE(gap->count, quint64(2));
        QVERIFY(!map.contains(2));
        QVERIFY(!map.complete());
    }
    void authenticatesFragmentedRecordsAndShortLastChunk()
    {
        const auto         p    = parameters();
        const auto         wire = records(p, 2, 3);
        QList<quint64>     committed;
        QByteArray         restored;
        bool               geometry = true;
        IrisMediaChunkSink sink(p, { 2, 3 }, [&](quint64 index, const QByteArray &record, const QByteArray &bytes) {
            geometry = geometry && record.size() == qint64(*MediaChunkWire::wireChunkSize(p, index));
            committed.append(index);
            restored += bytes;
            return IrisMediaChunkSink::Action::Continue;
        });
        QVERIFY(sink.isOpen());
        for (qint64 cursor = 0; cursor < wire.size(); cursor += 7) {
            const auto fragment = wire.mid(cursor, 7);
            QCOMPARE(sink.write(fragment), fragment.size());
        }
        QCOMPARE(committed, QList<quint64>({ 2, 3, 4 }));
        QCOMPARE(restored, plain.mid(16));
        QVERIFY(geometry);
        QVERIFY(sink.complete());
        QCOMPARE(sink.write("x", 1), qint64(-1));
    }
    void corruptLaterRecordPreservesEarlierVerifiedChunk()
    {
        const auto p    = parameters();
        auto       wire = records(p, 0, 2);
        wire[wire.size() - 1] ^= 1;
        QList<quint64>     committed;
        IrisMediaChunkSink sink(p, { 0, 2 }, [&](quint64 index, const QByteArray &, const QByteArray &) {
            committed.append(index);
            return IrisMediaChunkSink::Action::Continue;
        });
        QCOMPARE(sink.write(wire), qint64(-1));
        QCOMPARE(committed, QList<quint64>({ 0 }));
        QVERIFY(!sink.complete());
    }
    void seekDrainsExactlyOneAuthenticatedChunk()
    {
        const auto         p    = parameters();
        const auto         wire = records(p, 0, 3);
        QList<quint64>     committed;
        IrisMediaChunkSink sink(p, { 0, 3 }, [&](quint64 index, const QByteArray &, const QByteArray &) {
            committed.append(index);
            return IrisMediaChunkSink::Action::Pause;
        });
        QCOMPARE(sink.write(wire.left(9)), qint64(9));
        QVERIFY(sink.hasPartialChunk());
        QVERIFY(committed.isEmpty());
        QCOMPARE(sink.write(wire.mid(9)), qint64(wire.size() - 9));
        QCOMPARE(committed, QList<quint64>({ 0 }));
        QVERIFY(!sink.hasPartialChunk());
        QVERIFY(!sink.complete());
    }
    void failedCacheCommitCannotConfirmRange()
    {
        const auto         p = parameters();
        IrisMediaChunkSink sink(p, { 0, 1 }, [](quint64, const QByteArray &, const QByteArray &) {
            return IrisMediaChunkSink::Action::Error;
        });
        QCOMPARE(sink.write(records(p, 0, 1)), qint64(-1));
        QVERIFY(!sink.complete());
    }
#ifdef IRIS_FT_DEFERRED_RECEIPTS
    void completedCacheNeedsNoPublicationSession()
    {
        const auto    p = parameters();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (quint64 i = 0; i < p.chunkCount(); ++i) {
            QFile file(directory.filePath(QString::number(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            const auto record = records(p, i, 1);
            QCOMPARE(file.write(record), record.size());
        }
        XMPP::Client client;
        int          stanzas = 0;
        connect(&client, &XMPP::Client::xmlOutgoing, this, [&] { ++stanzas; });
        IrisMediaSync job(&client, {}, p, directory.path(), 1000);
        QByteArray    result;
        QString       error;
        int           completions = 0;
        job.readWireChunk(2, [&](QByteArray data, QString failure) {
            result = std::move(data);
            error  = std::move(failure);
            ++completions;
        });
        QTRY_COMPARE(completions, 1);
        QCOMPARE(result, records(p, 2, 1));
        QVERIFY(error.isEmpty());
        QTest::qWait(25);
        QCOMPARE(stanzas, 0);
    }
    void cancellationWinsOverQueuedCachedDelivery()
    {
        const auto    p = parameters();
        QTemporaryDir directory;
        QFile         file(directory.filePath(QStringLiteral("0")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto record = records(p, 0, 1);
        QCOMPARE(file.write(record), record.size());
        file.close();
        IrisMediaSync job(nullptr, {}, p, directory.path(), 1000);
        int           completions = 0;
        QString       error;
        QByteArray    result;
        job.readWireChunk(0, [&](QByteArray bytes, QString failure) {
            ++completions;
            result = std::move(bytes);
            error  = std::move(failure);
        });
        job.cancel();
        QTRY_COMPARE(completions, 1);
        QVERIFY(result.isEmpty());
        QVERIFY(!error.isEmpty());
        QTest::qWait(25);
        QCOMPARE(completions, 1);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), record);
    }
#endif
};
QTEST_MAIN(IrisMediaSyncTest)
#include "irismediasync_test.moc"
