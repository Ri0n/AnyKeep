#include "fileremotecachestore.h"
#include "secureenvelope.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto TimeZoneUTC = QTimeZone::Initialization::UTC;
#else
constexpr auto TimeZoneUTC = Qt::UTC;
#endif

using namespace QtNote;

class FileRemoteCacheStoreTest : public QObject {
    Q_OBJECT

private slots:
    void roundTrip();
    void rejectsWrongInstance();
    void rejectsTampering();
};

static RemoteCacheRecord sampleRecord()
{
    RemoteCacheRecord record;
    record.id          = QStringLiteral("note-1");
    record.title       = QStringLiteral("Cached note");
    record.tags        = { QStringLiteral("offline") };
    record.modified    = QDateTime::fromSecsSinceEpoch(1700000000, TimeZoneUTC);
    record.format      = Note::Markdown;
    record.body        = QStringLiteral("# Cached note\nBody");
    record.bodyPresent = true;
    record.backendData.insert(QStringLiteral("revision"), QStringLiteral("r1"));
    record.syncState    = RemoteCacheRecord::Synced;
    record.lastOpenedAt = QDateTime::fromSecsSinceEpoch(1700000100, TimeZoneUTC);
    MediaReference media;
    media.id           = QUuid::createUuid();
    media.blobId       = QByteArray::fromHex("01020304");
    media.originalName = QStringLiteral("diagram.png");
    media.portableName = media.originalName;
    media.mediaType    = QStringLiteral("image/png");
    media.size         = 123;
    record.media.append(media);
    return record;
}

void FileRemoteCacheStoreTest::roundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FileRemoteCacheStore store(directory.filePath(QStringLiteral("cache.bin")), QStringLiteral("instance-1"), key);
    const auto           expected = sampleRecord();
    QVERIFY(!store.replaceRecords({ expected }));

    const auto loaded = store.records();
    QVERIFY(loaded);
    QCOMPARE(loaded.value.size(), 1);
    const auto &record = loaded.value.first();
    QCOMPARE(record.id, QStringLiteral("note-1"));
    QCOMPARE(record.body, QStringLiteral("# Cached note\nBody"));
    QVERIFY(record.bodyPresent);
    QCOMPARE(record.backendData.value(QStringLiteral("revision")).toString(), QStringLiteral("r1"));
    QVERIFY(record.cachedAt.isValid());
    QCOMPARE(record.media.size(), 1);
    QCOMPARE(record.media.first().id, expected.media.first().id);

    auto changed = record;
    changed.body = QStringLiteral("changed");
    QVERIFY(!store.put(changed));
    QCOMPARE(store.records().value.first().body, QStringLiteral("changed"));
    QVERIFY(!store.remove(record.id));
    QVERIFY(store.records().value.isEmpty());
}

void FileRemoteCacheStoreTest::rejectsWrongInstance()
{
    QTemporaryDir        directory;
    const auto           path = directory.filePath(QStringLiteral("cache.bin"));
    const auto           key  = SecureEnvelope::generateMasterKey();
    FileRemoteCacheStore writer(path, QStringLiteral("instance-1"), key);
    QVERIFY(!writer.put(sampleRecord()));
    FileRemoteCacheStore reader(path, QStringLiteral("instance-2"), key);
    QCOMPARE(reader.records().error.code, RemoteCacheError::Corrupt);
}

void FileRemoteCacheStoreTest::rejectsTampering()
{
    QTemporaryDir        directory;
    const auto           path = directory.filePath(QStringLiteral("cache.bin"));
    FileRemoteCacheStore store(path, QStringLiteral("instance-1"), SecureEnvelope::generateMasterKey());
    QVERIFY(!store.put(sampleRecord()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    auto bytes              = file.readAll();
    bytes[bytes.size() / 2] = char(uchar(bytes.at(bytes.size() / 2)) ^ 1U);
    file.resize(0);
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    QCOMPARE(store.records().error.code, RemoteCacheError::Corrupt);
}

QTEST_GUILESS_MAIN(FileRemoteCacheStoreTest)
#include "fileremotecachestore_test.moc"
