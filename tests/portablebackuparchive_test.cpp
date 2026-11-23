#include "portablebackuparchive.h"

#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace AnyKeep;

class PortableBackupArchiveTest final : public QObject {
    Q_OBJECT

    static PortableBackupSource makeSource(QBuffer *buffer, const QString &path, const QString &kind,
                                           const QString &mimeType)
    {
        PortableBackupSource source;
        source.entry.path = path;
        source.entry.kind = kind;
        source.entry.mimeType = mimeType;
        source.entry.size = buffer->size();
        source.device = buffer;
        return source;
    }

    static QByteArray validArchive()
    {
        QByteArray noteBytes = QByteArrayLiteral("# Example\n\n![Diagram](anykeep-media:/sample/diagram.png)");
        QByteArray mediaBytes = QByteArrayLiteral("\x01\x00\x7f\x02");
        QBuffer note(&noteBytes);
        QBuffer media(&mediaBytes);
        if (!note.open(QIODevice::ReadOnly) || !media.open(QIODevice::ReadOnly))
            return {};
        auto noteSource = makeSource(&note, QStringLiteral("notes/sample.md"), QStringLiteral("note"),
                                     QStringLiteral("text/markdown"));
        noteSource.entry.id = QStringLiteral("sample");
        auto mediaSource = makeSource(&media, QStringLiteral("media/sample/diagram.png"),
                                      QStringLiteral("attachment"), QStringLiteral("image/png"));
        mediaSource.entry.id = QStringLiteral("asset");
        mediaSource.entry.noteId = QStringLiteral("sample");
        QByteArray result;
        QBuffer archive(&result);
        if (!archive.open(QIODevice::WriteOnly)
            || !PortableBackupArchive::writeReadable(&archive, { noteSource, mediaSource }))
            return {};
        return result;
    }

private slots:
    void readableArchiveRoundTrip()
    {
        QByteArray data = validArchive();
        QVERIFY(!data.isEmpty());
        QVERIFY(data.startsWith("notes/sample.md"));
        QBuffer archive(&data);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        PortableBackupManifest manifest;
        QString error;
        QVERIFY2(PortableBackupArchive::inspectReadable(&archive, &manifest, &error), qPrintable(error));
        QCOMPARE(manifest.entries.size(), 2);
        QCOMPARE(manifest.entries.at(0).kind, QStringLiteral("note"));
        QCOMPARE(manifest.entries.at(0).id, QStringLiteral("sample"));
        QCOMPARE(manifest.entries.at(1).kind, QStringLiteral("attachment"));
        QCOMPARE(manifest.entries.at(1).noteId, QStringLiteral("sample"));
        QCOMPARE(manifest.entries.at(1).sha256.size(), 32);
    }

    void unsafePaths_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("traversal") << QStringLiteral("../notes/secret.md");
        QTest::newRow("nested traversal") << QStringLiteral("notes/../secret.md");
        QTest::newRow("dot") << QStringLiteral("notes/./secret.md");
        QTest::newRow("absolute") << QStringLiteral("/notes/secret.md");
        QTest::newRow("windows absolute") << QStringLiteral("C:/notes/secret.md");
        QTest::newRow("backslash") << QStringLiteral("notes\\secret.md");
        QTest::newRow("double slash") << QStringLiteral("notes//secret.md");
        QTest::newRow("manifest reserved") << QStringLiteral("manifest.json");
        QTest::newRow("unicode") << QStringLiteral("notes/заметка.md");
        QTest::newRow("too long") << QStringLiteral("notes/") + QString(101, QLatin1Char('a'));
    }

    void unsafePaths()
    {
        QFETCH(QString, path);
        QVERIFY(!PortableBackupArchive::safeEntryPath(path));
        QByteArray data("note");
        QBuffer note(&data);
        QVERIFY(note.open(QIODevice::ReadOnly));
        auto source = makeSource(&note, path, QStringLiteral("note"), QStringLiteral("text/markdown"));
        QByteArray output;
        QBuffer archive(&output);
        QVERIFY(archive.open(QIODevice::WriteOnly));
        QVERIFY(!PortableBackupArchive::writeReadable(&archive, { source }));
    }

    void contentTamperDetected()
    {
        QByteArray data = validArchive();
        QVERIFY(data.size() > 512);
        data[512] = char(data.at(512) ^ 1);
        QBuffer archive(&data);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        PortableBackupManifest manifest;
        QVERIFY(!PortableBackupArchive::inspectReadable(&archive, &manifest));
    }

    void truncatedArchiveDetected()
    {
        QByteArray data = validArchive();
        data.chop(512);
        QBuffer archive(&data);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        PortableBackupManifest manifest;
        QVERIFY(!PortableBackupArchive::inspectReadable(&archive, &manifest));
    }

    void wrongVersionRejected()
    {
        QByteArray data = validArchive();
        const auto oldVersion = QByteArrayLiteral("\"version\":1");
        const auto newVersion = QByteArrayLiteral("\"version\":2");
        const qsizetype index = data.indexOf(oldVersion);
        QVERIFY(index >= 0);
        data.replace(index, oldVersion.size(), newVersion);
        QBuffer archive(&data);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        PortableBackupManifest manifest;
        QVERIFY(!PortableBackupArchive::inspectReadable(&archive, &manifest));
    }

    void symlinkEntryRejected()
    {
        QByteArray data = validArchive();
        QVERIFY(data.size() > 512);
        data[156] = '2'; // USTAR symlink type
        QBuffer archive(&data);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        PortableBackupManifest manifest;
        QVERIFY(!PortableBackupArchive::inspectReadable(&archive, &manifest));
    }

    void duplicatePathsRejected()
    {
        QByteArray payload("first");
        QBuffer first(&payload);
        QBuffer second(&payload);
        QVERIFY(first.open(QIODevice::ReadOnly));
        QVERIFY(second.open(QIODevice::ReadOnly));
        auto source = makeSource(&first, QStringLiteral("notes/same.md"), QStringLiteral("note"),
                                 QStringLiteral("text/markdown"));
        auto duplicate = makeSource(&second, QStringLiteral("notes/same.md"), QStringLiteral("note"),
                                    QStringLiteral("text/markdown"));
        QByteArray result;
        QBuffer archive(&result);
        QVERIFY(archive.open(QIODevice::WriteOnly));
        QVERIFY(!PortableBackupArchive::writeReadable(&archive, { source, duplicate }));
    }

    void sourceDisappearingIsNotSuccess()
    {
        QByteArray payload("short");
        QBuffer sourceDevice(&payload);
        QVERIFY(sourceDevice.open(QIODevice::ReadOnly));
        auto source = makeSource(&sourceDevice, QStringLiteral("notes/a.md"), QStringLiteral("note"),
                                 QStringLiteral("text/markdown"));
        source.entry.size += 2;
        QByteArray result;
        QBuffer archive(&result);
        QVERIFY(archive.open(QIODevice::WriteOnly));
        QVERIFY(!PortableBackupArchive::writeReadable(&archive, { source }));
    }

    void emptyArchiveHasValidManifest()
    {
        QByteArray result;
        QBuffer writer(&result);
        QVERIFY(writer.open(QIODevice::WriteOnly));
        QVERIFY(PortableBackupArchive::writeReadable(&writer, {}));
        writer.close();
        QBuffer reader(&result);
        QVERIFY(reader.open(QIODevice::ReadOnly));
        PortableBackupManifest manifest;
        QVERIFY(PortableBackupArchive::inspectReadable(&reader, &manifest));
        QVERIFY(manifest.entries.isEmpty());
    }
};

QTEST_GUILESS_MAIN(PortableBackupArchiveTest)
#include "portablebackuparchive_test.moc"
