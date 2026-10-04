#include "localmediastore.h"
#include "mediasource.h"
#include "mediastream.h"
#include "notedata.h"
#include "secureenvelope.h"
#include "utils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <thread>

using namespace AnyKeep;

class LocalMediaStoreTest : public QObject {
    Q_OBJECT

private slots:
    void encryptedRoundTripAndDeduplication();
    void concurrentReadsUseTheCachedKey();
    void mediaStreamReadsAndSeeks();
    void managedChunkedMediaStreamsVerifiedRanges();
    void legacyManagedEnvelopeRemainsReadable();
    void externalFileReferenceStreamsVerifiedRanges();
    void externalFileReferenceRejectsChangedChunks();
    void portableNames();
    void markdownDisplayTitle();
    void markdownHtmlImageDisplayTitle();
    void markdownAudioDisplayTitle();
};

void LocalMediaStoreTest::encryptedRoundTripAndDeduplication()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore  store(directory.path(), SecureEnvelope::generateMasterKey());
    const QByteArray plain("not really a png\0but binary", 27);

    const auto first = store.importData(plain, QStringLiteral("Схема: 1.png"), QStringLiteral("image/png"));
    QVERIFY2(first, qPrintable(first.error));
    const auto second = store.importData(plain, QStringLiteral("copy.png"), QStringLiteral("image/png"));
    QVERIFY2(second, qPrintable(second.error));
    QCOMPARE(first.value.blobId, second.value.blobId);
    QVERIFY(first.value.id != second.value.id);
    QCOMPARE(first.value.portableName, QStringLiteral("Схема_ 1.png"));
    QVERIFY(store.isChunkedManagedBlob(first.value.blobId));

    QDirIterator files(directory.path(), QDir::Files, QDirIterator::Subdirectories);
    int          blobCount = 0;
    QString      blobPath;
    while (files.hasNext()) {
        blobPath = files.next();
        ++blobCount;
    }
    QCOMPARE(blobCount, 1);
    QFile encrypted(blobPath);
    QVERIFY(encrypted.open(QIODevice::ReadOnly));
    QVERIFY(!encrypted.readAll().contains(plain));

    const auto opened = store.data(first.value.blobId);
    QVERIFY2(opened, qPrintable(opened.error));
    QCOMPARE(opened.value, plain);
}

void LocalMediaStoreTest::concurrentReadsUseTheCachedKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore  store(directory.path(), SecureEnvelope::generateMasterKey());
    const QByteArray plain("thread-safe local media");
    const auto       imported = store.importData(plain, QStringLiteral("image.png"), QStringLiteral("image/png"));
    QVERIFY2(imported, qPrintable(imported.error));

    std::array<LocalMediaDataResult, 8> results;
    std::array<std::thread, 8>          workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index]
            = std::thread([&store, &results, &imported, index] { results[index] = store.data(imported.value.blobId); });
    }
    for (auto &worker : workers)
        worker.join();
    for (const auto &result : results) {
        QVERIFY2(result, qPrintable(result.error));
        QCOMPARE(result.value, plain);
    }
}

void LocalMediaStoreTest::mediaStreamReadsAndSeeks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore  store(directory.path(), SecureEnvelope::generateMasterKey());
    const QByteArray plain("0123456789abcdef");
    const auto       imported = store.importData(plain, QStringLiteral("clip.bin"), QStringLiteral("video/mp4"));
    QVERIFY2(imported, qPrintable(imported.error));

    MediaStream stream(createLocalMediaSource(imported.value, &store));
    QVERIFY2(stream.open(QIODevice::ReadOnly), qPrintable(stream.errorString()));
    QCOMPARE(stream.size(), qint64(plain.size()));
    QCOMPARE(stream.read(4), QByteArray("0123"));
    QCOMPARE(stream.pos(), qint64(4));

    QVERIFY(stream.seek(10));
    QCOMPARE(stream.read(99), QByteArray("abcdef"));
    QCOMPARE(stream.pos(), qint64(plain.size()));
    QVERIFY(stream.atEnd());

    QVERIFY(stream.seek(2));
    QCOMPARE(stream.read(5), QByteArray("23456"));
    QVERIFY(!stream.seek(plain.size() + 1));
    QCOMPARE(stream.pos(), qint64(7));

    stream.close();
    QVERIFY(!stream.isOpen());
}

void LocalMediaStoreTest::managedChunkedMediaStreamsVerifiedRanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray masterKey = SecureEnvelope::generateMasterKey();
    LocalMediaStore store(QDir(directory.path()).filePath(QStringLiteral("store")), masterKey);

    QByteArray plain(int(LocalMediaStore::ManagedChunkSize * 2 + 137), Qt::Uninitialized);
    for (int i = 0; i < plain.size(); ++i)
        plain[i] = char('A' + (i % 19));

    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("large-video.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(plain), qint64(plain.size()));
    source.close();

    const auto imported = store.importFile(sourcePath);
    QVERIFY2(imported, qPrintable(imported.error));
    QVERIFY(store.isChunkedManagedBlob(imported.value.blobId));
    QCOMPARE(imported.value.size, qint64(plain.size()));

    const qint64 boundary = LocalMediaStore::ManagedChunkSize;
    const auto range = store.readManagedRange(imported.value.blobId, boundary - 31, 96);
    QVERIFY2(range, qPrintable(range.error));
    QCOMPARE(range.totalSize, qint64(plain.size()));
    QCOMPARE(range.value, plain.mid(int(boundary - 31), 96));

    MediaStream stream(createLocalMediaSource(imported.value, &store));
    QVERIFY2(stream.open(QIODevice::ReadOnly), qPrintable(stream.errorString()));
    QVERIFY(stream.seek(boundary * 2 - 11));
    QCOMPARE(stream.read(64), plain.mid(int(boundary * 2 - 11), 64));
    stream.close();

    QDirIterator files(QDir(directory.path()).filePath(QStringLiteral("store")), QStringList() << QStringLiteral("*.blob"),
                       QDir::Files, QDirIterator::Subdirectories);
    QVERIFY(files.hasNext());
    const QString blobPath = files.next();
    QVERIFY(!files.hasNext());

    QFile encrypted(blobPath);
    QVERIFY(encrypted.open(QIODevice::ReadWrite));
    QVERIFY(encrypted.size() > 32);
    QVERIFY(encrypted.seek(encrypted.size() - 1));
    char tail = 0;
    QCOMPARE(encrypted.read(&tail, 1), qint64(1));
    tail = char(uchar(tail) ^ 0x01);
    QVERIFY(encrypted.seek(encrypted.size() - 1));
    QCOMPARE(encrypted.write(&tail, 1), qint64(1));
    encrypted.close();

    MediaStream corrupted(createLocalMediaSource(imported.value, &store));
    QVERIFY2(corrupted.open(QIODevice::ReadOnly), qPrintable(corrupted.errorString()));
    QCOMPARE(corrupted.read(32), plain.left(32));
    QVERIFY(corrupted.seek(boundary * 2));
    QVERIFY(corrupted.read(16).isEmpty());
    QVERIFY(!corrupted.errorString().isEmpty());
}

void LocalMediaStoreTest::legacyManagedEnvelopeRemainsReadable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray masterKey = SecureEnvelope::generateMasterKey();
    LocalMediaStore store(directory.path(), masterKey);
    const QByteArray plain("legacy whole-envelope media");
    const QByteArray idKey = SecureEnvelope::deriveKey(masterKey, KeyDomain::LocalMedia);
    const QByteArray blobId = QMessageAuthenticationCode::hash(plain, idKey, QCryptographicHash::Sha256);
    const QByteArray hex = blobId.toHex();
    const QString blobPath = directory.path() + QLatin1Char('/') + QString::fromLatin1(hex.left(2)) + QLatin1Char('/')
        + QString::fromLatin1(hex.mid(2, 2)) + QLatin1Char('/') + QString::fromLatin1(hex) + QStringLiteral(".blob");
    QVERIFY(QDir().mkpath(QFileInfo(blobPath).absolutePath()));

    const AeadContext context { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media"),
                                QString::fromLatin1(blobId.toHex()), 1, QStringLiteral("attachment") };
    const auto sealed = SecureEnvelope::seal(plain, masterKey, context);
    QVERIFY2(sealed, qPrintable(sealed.error.message));
    QFile file(blobPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(sealed.value), qint64(sealed.value.size()));
    file.close();

    QVERIFY(store.containsManagedBlob(blobId));
    QVERIFY(!store.isChunkedManagedBlob(blobId));
    const auto opened = store.data(blobId);
    QVERIFY2(opened, qPrintable(opened.error));
    QCOMPARE(opened.value, plain);

    MediaReference reference;
    reference.id           = QUuid::createUuid();
    reference.blobId       = blobId;
    reference.originalName = QStringLiteral("legacy.bin");
    reference.portableName = QStringLiteral("legacy.bin");
    reference.mediaType    = QStringLiteral("application/octet-stream");
    reference.size         = plain.size();
    reference.checksum     = QCryptographicHash::hash(plain, QCryptographicHash::Sha256);
    MediaStream stream(createLocalMediaSource(reference, &store));
    QVERIFY2(stream.open(QIODevice::ReadOnly), qPrintable(stream.errorString()));
    QVERIFY(stream.seek(7));
    QCOMPARE(stream.read(5), plain.mid(7, 5));
}

void LocalMediaStoreTest::externalFileReferenceStreamsVerifiedRanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("store")));

    QByteArray plain(int(LocalMediaStore::ExternalChunkSize + 64), 'a');
    for (int i = 0; i < plain.size(); ++i)
        plain[i] = char('a' + (i % 23));

    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("large-video.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(plain), qint64(plain.size()));
    source.close();

    LocalMediaStore store(QDir(directory.path()).filePath(QStringLiteral("store")),
                          SecureEnvelope::generateMasterKey());
    const auto referenced = store.referenceFile(sourcePath);
    QVERIFY2(referenced, qPrintable(referenced.error));
    QVERIFY(!store.containsManagedBlob(referenced.value.blobId));
    QVERIFY(store.contains(referenced.value.blobId));
    QCOMPARE(referenced.value.size, qint64(plain.size()));
    QCOMPARE(referenced.value.checksum, QCryptographicHash::hash(plain, QCryptographicHash::Sha256));

    const auto external = store.externalSource(referenced.value);
    QVERIFY2(external, qPrintable(external.error));
    QCOMPARE(external.value.chunkSize, quint32(LocalMediaStore::ExternalChunkSize));
    QCOMPARE(external.value.chunkHashes.size(), 2);

    MediaStream stream(createLocalMediaSource(referenced.value, &store));
    QVERIFY2(stream.open(QIODevice::ReadOnly), qPrintable(stream.errorString()));
    const qint64 boundary = LocalMediaStore::ExternalChunkSize;
    QVERIFY(stream.seek(boundary - 16));
    QCOMPARE(stream.read(48), plain.mid(int(boundary - 16), 48));
    QVERIFY(stream.seek(12345));
    QCOMPARE(stream.read(777), plain.mid(12345, 777));

    const auto materialized = store.data(referenced.value.blobId);
    QVERIFY2(materialized, qPrintable(materialized.error));
    QCOMPARE(materialized.value, plain);

    const auto managed = store.importData(plain, QStringLiteral("managed-video.bin"), QStringLiteral("video/mp4"));
    QVERIFY2(managed, qPrintable(managed.error));
    QCOMPARE(managed.value.blobId, referenced.value.blobId);
    QVERIFY(store.containsManagedBlob(referenced.value.blobId));
}

void LocalMediaStoreTest::externalFileReferenceRejectsChangedChunks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("store")));

    QByteArray plain(int(LocalMediaStore::ExternalChunkSize + 32), 'x');
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("mutable-video.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(plain), qint64(plain.size()));
    source.close();

    LocalMediaStore store(QDir(directory.path()).filePath(QStringLiteral("store")),
                          SecureEnvelope::generateMasterKey());
    const auto referenced = store.referenceFile(sourcePath);
    QVERIFY2(referenced, qPrintable(referenced.error));

    QVERIFY(source.open(QIODevice::ReadWrite));
    QVERIFY(source.seek(LocalMediaStore::ExternalChunkSize + 7));
    QCOMPARE(source.write("z", 1), qint64(1));
    source.close();

    MediaStream stream(createLocalMediaSource(referenced.value, &store));
    QVERIFY2(stream.open(QIODevice::ReadOnly), qPrintable(stream.errorString()));
    QVERIFY(stream.seek(LocalMediaStore::ExternalChunkSize));
    QVERIFY(stream.read(16).isEmpty());
    QVERIFY(!stream.errorString().isEmpty());

    const auto materialized = store.data(referenced.value.blobId);
    QVERIFY(!materialized);
    QVERIFY(materialized.error.contains(QStringLiteral("changed")));
}

void LocalMediaStoreTest::portableNames()
{
    QCOMPARE(Utils::portableFileName(QStringLiteral("CON.txt")), QStringLiteral("_CON.txt"));
    QCOMPARE(Utils::portableFileName(QFileInfo(QStringLiteral("folder/name?.jpg")).fileName()),
             QStringLiteral("name_.jpg"));
    QCOMPARE(Utils::portableFileName(QStringLiteral("trailing. ")), QStringLiteral("trailing"));
}

void LocalMediaStoreTest::markdownDisplayTitle()
{
    Note note(new NoteData(nullptr));
    note.setFormat(Note::Markdown);
    note.setTitle(
        QStringLiteral("![Screenshot_20240724_180235.png](%21%5BScreenshot_20240724_180235.png%5D%28anykeep-media__"
                       "e8338b20-71c6-45ed-aa25-425fc2e497e5_Screenshot_20240724_180235.png%20_Screenshot_20240724_"
                       "180235.png_%29/Screenshot_20240724_180235.png \"Screenshot_20240724_180235.png\")"));
    QCOMPARE(note.displayTitle(), QStringLiteral("Screenshot_20240724_180235.png"));
    QVERIFY(note.title().startsWith(QStringLiteral("![")));
}

void LocalMediaStoreTest::markdownHtmlImageDisplayTitle()
{
    Note note(new NoteData(nullptr));
    note.setFormat(Note::Markdown);
    note.setTitle(
        QStringLiteral("<p align=\"center\"><img src=\"anykeep-media:/11111111-1111-1111-1111-111111111111/photo.png\" "
                       "alt=\"Holiday photo\" width=\"320\" /></p>"));
    QCOMPARE(note.displayTitle(), QStringLiteral("Holiday photo"));
}

void LocalMediaStoreTest::markdownAudioDisplayTitle()
{
    Note note(new NoteData(nullptr));
    note.setFormat(Note::Markdown);
    note.setTitle(
        QStringLiteral("<audio controls src=\"anykeep-media:/11111111-1111-1111-1111-111111111111/audio_20260814.m4a\" "
                       "title=\"Voice &amp; memo\" data-anykeep-duration-ms=\"2500\"></audio>"));
    QCOMPARE(note.displayTitle(), QStringLiteral("Voice & memo"));

    note.setTitle(
        QStringLiteral("<audio controls src=\"anykeep-media:/11111111-1111-1111-1111-111111111111/audio_20260814.m4a\" "
                       "title=\"\" data-anykeep-duration-ms=\"2500\"></audio>"));
    QCOMPARE(note.displayTitle(), QStringLiteral("audio_20260814.m4a"));
}

QTEST_MAIN(LocalMediaStoreTest)
#include "localmediastore_test.moc"
