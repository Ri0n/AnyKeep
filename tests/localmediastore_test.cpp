#include "localmediastore.h"
#include "notedata.h"
#include "secureenvelope.h"
#include "utils.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <thread>

using namespace QtNote;

class LocalMediaStoreTest : public QObject {
    Q_OBJECT

private slots:
    void encryptedRoundTripAndDeduplication();
    void concurrentReadsUseTheCachedKey();
    void portableNames();
    void markdownDisplayTitle();
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
        QStringLiteral("![Screenshot_20240724_180235.png](%21%5BScreenshot_20240724_180235.png%5D%28qtnote-media__"
                       "e8338b20-71c6-45ed-aa25-425fc2e497e5_Screenshot_20240724_180235.png%20_Screenshot_20240724_"
                       "180235.png_%29/Screenshot_20240724_180235.png \"Screenshot_20240724_180235.png\")"));
    QCOMPARE(note.displayTitle(), QStringLiteral("Screenshot_20240724_180235.png"));
    QVERIFY(note.title().startsWith(QStringLiteral("![")));
}

QTEST_MAIN(LocalMediaStoreTest)
#include "localmediastore_test.moc"
