#include "conflictresolver.h"
#include "filedraftstore.h"
#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace AnyKeep;

class FileDraftStoreTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void roundTrip();
    void deletionRoundTrip();
    void copyConflictResolution();
    void readsVersionFiveWithoutFolder();
    void readsVersionSevenWithoutFolderOverride();
    void rejectsWrongKey();
    void rejectsTampering();
};

void FileDraftStoreTest::initTestCase() { QVERIFY2(FileDraftStore::cryptoAvailable(), "AES-256-GCM unavailable"); }

static DraftRecord sampleRecord()
{
    DraftRecord record;
    record.id                    = QUuid::createUuid();
    record.storageId             = QStringLiteral("nextcloud");
    record.remoteNoteId          = QStringLiteral("remote-42");
    record.title                 = QStringLiteral("Sensitive title");
    record.body                  = QStringLiteral("Sensitive body that must not occur in the ciphertext");
    record.format                = Note::Markdown;
    record.tags                  = QStringList { QStringLiteral("private"), QStringLiteral("work") };
    record.folderId              = QUuid::createUuid();
    record.folderUserOverride    = true;
    record.removeSourceStorageId = QStringLiteral("tomboy");
    record.removeSourceNoteId    = QStringLiteral("original-note");
    record.backendData.insert(QStringLiteral("etag"), QStringLiteral("base-etag"));
    record.backendData.insert(QStringLiteral("revision"), QStringLiteral("base-revision"));
    record.revision = 7;
    MediaReference media;
    media.id           = QUuid::createUuid();
    media.blobId       = QByteArray::fromHex("00112233");
    media.originalName = QStringLiteral("Схема.png");
    media.portableName = QStringLiteral("Схема.png");
    media.mediaType    = QStringLiteral("image/png");
    media.size         = 42;
    media.checksum     = QByteArray::fromHex("aabbccdd");
    record.media.append(media);
    record.updatedAt = QDateTime::currentDateTimeUtc();
    return record;
}

static QString draftPath(const QString &root, const QUuid &id)
{
    return QDir(root).filePath(id.toString(QUuid::WithoutBraces) + QStringLiteral(".draft"));
}

void FileDraftStoreTest::roundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto key = FileDraftStore::generateMasterKey();
    QCOMPARE(key.size(), FileDraftStore::MasterKeySize);
    FileDraftStore store(directory.path(), key);
    auto           record = sampleRecord();
    QVERIFY(!store.write(record));

    QFile file(draftPath(directory.path(), record.id));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto encrypted = file.readAll();
    QVERIFY(!encrypted.contains(record.title.toUtf8()));
    QVERIFY(!encrypted.contains(record.body.toUtf8()));

    auto loaded = store.load(record.id);
    QVERIFY2(loaded, qPrintable(loaded.error.message));
    QCOMPARE(loaded.value.id, record.id);
    QCOMPARE(loaded.value.storageId, record.storageId);
    QCOMPARE(loaded.value.remoteNoteId, record.remoteNoteId);
    QCOMPARE(loaded.value.title, record.title);
    QCOMPARE(loaded.value.body, record.body);
    QCOMPARE(loaded.value.format, record.format);
    QCOMPARE(loaded.value.tags, record.tags);
    QCOMPARE(loaded.value.folderId, record.folderId);
    QCOMPARE(loaded.value.folderUserOverride, record.folderUserOverride);
    QCOMPARE(loaded.value.removeSourceStorageId, record.removeSourceStorageId);
    QCOMPARE(loaded.value.removeSourceNoteId, record.removeSourceNoteId);
    QCOMPARE(loaded.value.backendData, record.backendData);
    QCOMPARE(loaded.value.revision, record.revision);
    QCOMPARE(loaded.value.media.size(), 1);
    QCOMPARE(loaded.value.media.first().id, record.media.first().id);
    QCOMPARE(loaded.value.media.first().originalName, record.media.first().originalName);
    QCOMPARE(loaded.value.media.first().blobId, record.media.first().blobId);
    QCOMPARE(loaded.value.operation, DraftRecord::Publish);

    QVERIFY(!store.transition(record.id, DraftRecord::Ready));
    loaded = store.load(record.id);
    QVERIFY(loaded);
    QCOMPARE(loaded.value.state, DraftRecord::Ready);
}

void FileDraftStoreTest::readsVersionFiveWithoutFolder()
{
    constexpr quint32 PayloadMagic = 0x514e4450; // QNDP
    QTemporaryDir     directory;
    QVERIFY(directory.isValid());
    const auto key    = FileDraftStore::generateMasterKey();
    auto       record = sampleRecord();

    QByteArray  bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << PayloadMagic << quint16(5) << record.id << quint8(record.state) << record.storageId << record.remoteNoteId
        << record.title << record.body << quint8(record.format) << record.tags << record.revision << record.updatedAt
        << record.lastError << record.retryAt << quint8(record.operation) << record.backendData << quint32(0);
    const AeadContext context { KeyDomain::LocalDraft, QStringLiteral("anykeep-local-drafts"),
                                record.id.toString(QUuid::WithoutBraces), 1, QStringLiteral("draft") };
    const auto        sealed = SecureEnvelope::seal(bytes, key, context);
    QVERIFY2(sealed, qPrintable(sealed.error.message));

    QFile file(draftPath(directory.path(), record.id));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(sealed.value), sealed.value.size());
    file.close();

    FileDraftStore store(directory.path(), key);
    const auto     loaded = store.load(record.id);
    QVERIFY2(loaded, qPrintable(loaded.error.message));
    QCOMPARE(loaded.value.title, record.title);
    QVERIFY(loaded.value.folderId.isNull());
    QVERIFY(!loaded.value.folderUserOverride);
    QCOMPARE(loaded.value.revision, record.revision);
}

void FileDraftStoreTest::readsVersionSevenWithoutFolderOverride()
{
    constexpr quint32 PayloadMagic = 0x514e4450; // QNDP
    QTemporaryDir     directory;
    QVERIFY(directory.isValid());
    const auto key    = FileDraftStore::generateMasterKey();
    auto       record = sampleRecord();

    QByteArray  bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << PayloadMagic << quint16(7) << record.id << quint8(record.state) << record.storageId << record.remoteNoteId
        << record.title << record.body << quint8(record.format) << record.tags << record.folderId
        << record.removeSourceStorageId << record.removeSourceNoteId << record.revision << record.updatedAt
        << record.lastError << record.retryAt << quint8(record.operation) << record.backendData << quint32(0);
    const AeadContext context { KeyDomain::LocalDraft, QStringLiteral("anykeep-local-drafts"),
                                record.id.toString(QUuid::WithoutBraces), 1, QStringLiteral("draft") };
    const auto        sealed = SecureEnvelope::seal(bytes, key, context);
    QVERIFY2(sealed, qPrintable(sealed.error.message));

    QFile file(draftPath(directory.path(), record.id));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(sealed.value), sealed.value.size());
    file.close();

    FileDraftStore store(directory.path(), key);
    const auto     loaded = store.load(record.id);
    QVERIFY2(loaded, qPrintable(loaded.error.message));
    QCOMPARE(loaded.value.folderId, record.folderId);
    QVERIFY(!loaded.value.folderUserOverride);
    QCOMPARE(loaded.value.removeSourceStorageId, record.removeSourceStorageId);
    QCOMPARE(loaded.value.removeSourceNoteId, record.removeSourceNoteId);
}

void FileDraftStoreTest::copyConflictResolution()
{
    auto                 record = sampleRecord();
    CopyConflictResolver resolver;
    bool                 invoked = false;
    resolver.resolve({ record, {}, QStringLiteral("conflict") }, [&](ConflictResolution resolution) {
        invoked = true;
        QCOMPARE(resolution.action, ConflictResolution::CreateCopy);
        QVERIFY(resolution.copyTitle.startsWith(record.title + QStringLiteral(" (conflict ")));
        QVERIFY(resolution.notification.contains(resolution.copyTitle));
    });
    QVERIFY(invoked);
}

void FileDraftStoreTest::deletionRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FileDraftStore store(directory.path(), FileDraftStore::generateMasterKey());
    DraftRecord    record;
    record.id           = QUuid::createUuid();
    record.operation    = DraftRecord::Delete;
    record.state        = DraftRecord::Retry;
    record.storageId    = QStringLiteral("xmpp-pubsub");
    record.remoteNoteId = QStringLiteral("note-to-delete");
    QVERIFY(!store.write(record));

    const auto loaded = store.load(record.id);
    QVERIFY2(loaded, qPrintable(loaded.error.message));
    QCOMPARE(loaded.value.operation, DraftRecord::Delete);
    QCOMPARE(loaded.value.state, DraftRecord::Retry);
    QCOMPARE(loaded.value.storageId, record.storageId);
    QCOMPARE(loaded.value.remoteNoteId, record.remoteNoteId);
}

void FileDraftStoreTest::rejectsWrongKey()
{
    QTemporaryDir  directory;
    auto           record = sampleRecord();
    FileDraftStore writer(directory.path(), FileDraftStore::generateMasterKey());
    QVERIFY(!writer.write(record));
    FileDraftStore reader(directory.path(), FileDraftStore::generateMasterKey());
    auto           loaded = reader.load(record.id);
    QVERIFY(!loaded);
    QCOMPARE(loaded.error.code, DraftStoreError::Corrupt);
}

void FileDraftStoreTest::rejectsTampering()
{
    QTemporaryDir  directory;
    auto           record = sampleRecord();
    FileDraftStore store(directory.path(), FileDraftStore::generateMasterKey());
    QVERIFY(!store.write(record));
    QFile file(draftPath(directory.path(), record.id));
    QVERIFY(file.open(QIODevice::ReadWrite));
    auto bytes = file.readAll();
    QVERIFY(bytes.size() > 8);
    bytes[bytes.size() - 1] = char(bytes.back() ^ 0x01);
    QVERIFY(file.resize(0));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    auto loaded = store.load(record.id);
    QVERIFY(!loaded);
    QCOMPARE(loaded.error.code, DraftStoreError::Corrupt);
}

QTEST_GUILESS_MAIN(FileDraftStoreTest)
#include "filedraftstore_test.moc"
