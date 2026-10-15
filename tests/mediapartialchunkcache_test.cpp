#include "localmediastore.h"
#include "mediapartialchunkcache.h"
#include "secureenvelope.h"

#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QtTest>

using namespace AnyKeep;

class MediaPartialChunkCacheTest : public QObject {
    Q_OBJECT
private slots:
    void sealedPartialSurvivesRestartAndRejectsTampering();
    void independentRemoteRevisionsCannotShareCachedBytes();
    void legacyOpaqueRecordsRemainInPlaceAndRequireVerification();
    void sharedOpaqueCacheUsesProfileNamespaceAndIgnoresOldNightlyFiles();
};

void MediaPartialChunkCacheTest::sealedPartialSurvivesRestartAndRejectsTampering()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray key = SecureEnvelope::generateMasterKey();
    LocalMediaStore store(dir.path(), key);
    QString error;
    auto cache = store.partialMediaCache(QStringLiteral("nextcloud"), QStringLiteral("account/note/movie/etag-v1"),
                                         2 * LocalMediaStore::ManagedChunkSize + 10, &error);
    QVERIFY2(cache, qPrintable(error));
    QCOMPARE(cache->restoreVerifiedBytes(), 0);
    const QByteArray first(LocalMediaStore::ManagedChunkSize, 'p');
    const QByteArray last(10, 'q');
    QVERIFY(cache->write(0, first, first.size(), {}, &error));
    QVERIFY2(cache->write(2, last, last.size(), {}, &error), qPrintable(error));
    QCOMPARE(cache->restoreVerifiedBytes(), qint64(first.size() + last.size()));
    QVERIFY(cache->read(1, LocalMediaStore::ManagedChunkSize).isEmpty());
    QCOMPARE(cache->read(2, 10), last);

    // Rebuild the cache through LocalMediaStore to emulate an application
    // restart; the profile key and persistent identity must recover records.
    LocalMediaStore reopened(dir.path(), key);
    auto resumed = reopened.partialMediaCache(QStringLiteral("nextcloud"),
                                               QStringLiteral("account/note/movie/etag-v1"),
                                               2 * LocalMediaStore::ManagedChunkSize + 10);
    QVERIFY(resumed);
    QCOMPARE(resumed->read(0, first.size()), first);
    QCOMPARE(resumed->restoreVerifiedBytes(), qint64(first.size() + last.size()));

    // Stored ciphertext cannot be treated as plaintext by an attacker.
    QVERIFY(!resumed->storedChunkNames().isEmpty());
    // Accessing a record through its opaque directory is test-only.
    QDirIterator it(dir.path() + QStringLiteral("/partial"), QDir::Files, QDirIterator::Subdirectories);
    QVERIFY(it.hasNext());
    const QString path = it.next();
    QFile raw(path);
    QVERIFY(raw.open(QIODevice::ReadOnly));
    const QByteArray ciphertext = raw.readAll();
    QVERIFY(!ciphertext.contains(QByteArray(64, 'p')));
    raw.close();
    bool validIndex = false;
    const quint64 index = QFileInfo(path).fileName().toULongLong(&validIndex);
    QVERIFY(validIndex);
    const qint64 expected = resumed->expectedPlainSize(index);
    QVERIFY(expected > 0);
    QVERIFY(raw.open(QIODevice::ReadWrite));
    const qint64 tamperOffset = raw.size() / 2;
    QVERIFY(raw.seek(tamperOffset));
    const char changed = char(ciphertext.at(tamperOffset) ^ 0x7f);
    QCOMPARE(raw.write(&changed, 1), qint64(1));
    raw.close();

    // Existing readers validate the requested chunk when it is used, not
    // on every call to the memoized restoration scan.
    QVERIFY(resumed->read(index, expected).isEmpty());
    QCOMPARE(resumed->verifiedBytes(), qint64(first.size() + last.size()) - expected);

    // The same corruption must also be rejected during a *new* restoration.
    LocalMediaStore reloaded(dir.path(), key);
    auto afterRestart = reloaded.partialMediaCache(QStringLiteral("nextcloud"),
                             QStringLiteral("account/note/movie/etag-v1"),
                             2 * LocalMediaStore::ManagedChunkSize + 10);
    QVERIFY(afterRestart);
    QCOMPARE(afterRestart->restoreVerifiedBytes(), qint64(first.size() + last.size()) - expected);
}

void MediaPartialChunkCacheTest::independentRemoteRevisionsCannotShareCachedBytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray key = SecureEnvelope::generateMasterKey();
    LocalMediaStore store(dir.path(), key);
    auto older = store.partialMediaCache(QStringLiteral("nextcloud"), QStringLiteral("etag-one"), 1024);
    auto newer = store.partialMediaCache(QStringLiteral("nextcloud"), QStringLiteral("etag-two"), 1024);
    QVERIFY(older && newer);
    QVERIFY(older->write(0, QByteArray(1024, '1'), 1024));
    QCOMPARE(older->restoreVerifiedBytes(), 1024);
    QVERIFY(newer->read(0, 1024).isEmpty());
    QCOMPARE(newer->restoreVerifiedBytes(), 0);
}

void MediaPartialChunkCacheTest::legacyOpaqueRecordsRemainInPlaceAndRequireVerification()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MediaPartialChunkCache opaque(MediaPartialChunkCache::Format::VerifiedOpaque, dir.path());
    QVERIFY(opaque.isValid());
    const QByteArray wire("original-encrypted-XMPP-record");
    auto verify = [](quint64 index, const QByteArray &bytes) {
        return index == 7 && bytes == QByteArrayLiteral("original-encrypted-XMPP-record");
    };
    QVERIFY(!opaque.write(7, wire, wire.size()));
    QVERIFY(opaque.write(7, wire, wire.size(), verify));
    QCOMPARE(opaque.storedChunkNames(), QStringList({QStringLiteral("7")}));
    QCOMPARE(opaque.read(7, wire.size(), verify), wire);
    QVERIFY(opaque.read(7, wire.size(), [](quint64, const QByteArray &) { return false; }).isEmpty());
    QVERIFY(opaque.storedChunkNames().isEmpty());
}

void MediaPartialChunkCacheTest::sharedOpaqueCacheUsesProfileNamespaceAndIgnoresOldNightlyFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore store(directory.path(), SecureEnvelope::generateMasterKey());
    QString error;
    auto first = store.opaqueMediaCache(QStringLiteral("xmpp"), QStringLiteral("media-revision-one"), &error);
    QVERIFY2(first, qPrintable(error));
    auto same = store.opaqueMediaCache(QStringLiteral("xmpp"), QStringLiteral("media-revision-one"));
    auto newer = store.opaqueMediaCache(QStringLiteral("xmpp"), QStringLiteral("media-revision-two"));
    auto other = store.opaqueMediaCache(QStringLiteral("nextcloud"), QStringLiteral("media-revision-one"));
    QVERIFY(same && newer && other);
    const QByteArray wire("authenticated-ciphertext-record");
    auto verify = [wire](quint64 index, const QByteArray &data) {
        return index == 4 && data == wire;
    };
    QVERIFY(first->write(4, wire, wire.size(), verify, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(same->read(4, wire.size(), verify), wire);
    QVERIFY(newer->read(4, wire.size(), verify).isEmpty());
    QVERIFY(other->read(4, wire.size(), verify).isEmpty());

    // No legacy import: obsolete nightly media-ranges data cannot be confused
    // with a verified record from the shared media-store namespace.
    QDir().mkpath(directory.filePath(QStringLiteral("media-ranges/old")));
    QFile obsolete(directory.filePath(QStringLiteral("media-ranges/old/4")));
    QVERIFY(obsolete.open(QIODevice::WriteOnly));
    QCOMPARE(obsolete.write(wire), wire.size());
    obsolete.close();
    auto reopened = LocalMediaStore(directory.path(), SecureEnvelope::generateMasterKey())
                        .opaqueMediaCache(QStringLiteral("xmpp"), QStringLiteral("media-revision-two"));
    QVERIFY(reopened);
    QVERIFY(reopened->read(4, wire.size(), verify).isEmpty());
    QCOMPARE(same->read(4, wire.size(), verify), wire);
}

QTEST_GUILESS_MAIN(MediaPartialChunkCacheTest)
#include "mediapartialchunkcache_test.moc"
