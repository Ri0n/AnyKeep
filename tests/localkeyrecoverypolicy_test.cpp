#include <QtTest>

#include "../libanykeep/localkeyrecoverypolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using AnyKeep::LocalKeyRecoveryPolicy::containsProtectedFiles;

class LocalKeyRecoveryPolicyTest : public QObject {
    Q_OBJECT

    static bool touch(const QString &path)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write("test", 4) == 4;
    }

private slots:
    void emptyOrPtfOnlyInstallation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(!containsProtectedFiles(dir.path()));
        QVERIFY(touch(dir.filePath(QStringLiteral("ptf/Hello.md"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("ptf/.anykeep-folders.json"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("app-state/qt-settings.ini"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("other-plugin/runtime-state.json"))));
        QVERIFY(!containsProtectedFiles(dir.path()));
    }

    void protectedDataLocations_data()
    {
        QTest::addColumn<QString>("relativePath");
        QTest::addColumn<QString>("category");
        QTest::newRow("draft") << "drafts/123.draft" << "drafts";
        QTest::newRow("unrecoverable-draft") << "drafts-unrecoverable-2026/123.draft"
                                            << "drafts-unrecoverable-2026";
        QTest::newRow("encrypted-media") << "media/0a/abcd" << "media";
        QTest::newRow("remote-cache") << "remote-cache/xmpppubsub/note.cache" << "remote-cache";
        QTest::newRow("omemo") << "xmpp-omemo-001.state" << "xmpp-omemo-001.state";
        QTest::newRow("catalog") << "folder-catalog.bin" << "folder-catalog.bin";
        QTest::newRow("catalog-backup") << "folder-catalog.bin.bak" << "folder-catalog.bin.bak";
        QTest::newRow("rules") << "note-rules.bin" << "note-rules.bin";
        QTest::newRow("rules-backup") << "note-rules.bin.bak" << "note-rules.bin.bak";
    }

    void protectedDataLocations()
    {
        QFETCH(QString, relativePath);
        QFETCH(QString, category);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(touch(dir.filePath(relativePath)));
        QString detectedCategory;
        QVERIFY(containsProtectedFiles(dir.path(), &detectedCategory));
        QCOMPARE(detectedCategory, category);
    }

    void emptyProtectedDirectoriesAreNotData()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("drafts"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("media"))));
        QVERIFY(!containsProtectedFiles(dir.path()));
    }
};

QTEST_MAIN(LocalKeyRecoveryPolicyTest)
#include "localkeyrecoverypolicy_test.moc"
