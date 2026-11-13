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
        // Different encryption domains and previously quarantined drafts must
        // not be discarded by a reset of this local master key.
        QVERIFY(touch(dir.filePath(QStringLiteral("xmpp-omemo-001.state"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("drafts-unrecoverable-2026/123.draft"))));
        QVERIFY(!containsProtectedFiles(dir.path()));
    }

    void protectedDataLocations_data()
    {
        QTest::addColumn<QString>("relativePath");
        QTest::addColumn<QString>("category");
        QTest::newRow("draft") << "drafts/123.draft" << "drafts";
        QTest::newRow("encrypted-media") << "media/0a/abcd" << "media";
        QTest::newRow("remote-cache") << "remote-cache/xmpppubsub/note.cache" << "remote-cache";
        QTest::newRow("catalog") << "folder-catalog.bin" << "folder-catalog";
        QTest::newRow("catalog-backup") << "folder-catalog.bin.bak" << "folder-catalog";
        QTest::newRow("rules") << "note-rules.bin" << "note-rules";
        QTest::newRow("rules-backup") << "note-rules.bin.bak" << "note-rules";
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

    void quarantinePreservesPtfAndCanRollback()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto ptfPath = dir.filePath(QStringLiteral("ptf/Hello.md"));
        QVERIFY(touch(ptfPath));
        QVERIFY(touch(dir.filePath(QStringLiteral("drafts/123.draft"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("folder-catalog.bin"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("media/aa/blob.blob"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("other-plugin/config.json"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("xmpp-omemo-001.state"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("drafts-unrecoverable-2026/older.draft"))));
        QString backupPath, error;
        QVERIFY2(AnyKeep::LocalKeyRecoveryPolicy::quarantineProtectedFiles(dir.path(), &backupPath, &error),
                 qPrintable(error));
        QVERIFY(!backupPath.isEmpty());
        QVERIFY(!containsProtectedFiles(dir.path()));
        QVERIFY(QFile::exists(ptfPath));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("other-plugin/config.json"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("xmpp-omemo-001.state"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("drafts-unrecoverable-2026/older.draft"))));
        QVERIFY(QFile::exists(backupPath + QStringLiteral("/drafts/123.draft")));
        QVERIFY(QFile::exists(backupPath + QStringLiteral("/folder-catalog.bin")));
        QVERIFY(QFile::exists(backupPath + QStringLiteral("/media/aa/blob.blob")));

        QVERIFY(AnyKeep::LocalKeyRecoveryPolicy::restoreQuarantinedFiles(dir.path(), backupPath));
        QVERIFY(QFile::exists(ptfPath));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("drafts/123.draft"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("folder-catalog.bin"))));
        QVERIFY(containsProtectedFiles(dir.path()));
    }

    void successfulResetDiscardsOnlyStagedEncryptedState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto notePath = dir.filePath(QStringLiteral("ptf/Saved.md"));
        QVERIFY(touch(notePath));
        QVERIFY(touch(dir.filePath(QStringLiteral("drafts/unsaved.draft"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("media/aa/attachment.blob"))));
        QVERIFY(touch(dir.filePath(QStringLiteral("settings.json"))));
        QString stagedPath, error;
        QVERIFY2(AnyKeep::LocalKeyRecoveryPolicy::quarantineProtectedFiles(dir.path(), &stagedPath, &error),
                 qPrintable(error));
        QVERIFY(!stagedPath.isEmpty());
        QVERIFY(AnyKeep::LocalKeyRecoveryPolicy::discardStagedFiles(stagedPath));
        QVERIFY(!QFile::exists(stagedPath));
        QVERIFY(QFile::exists(notePath));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("settings.json"))));
        QVERIFY(!containsProtectedFiles(dir.path()));
        QVERIFY(AnyKeep::LocalKeyRecoveryPolicy::discardStagedFiles({}));
    }

    void quarantineLeavesOnlyPtfUntouched()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(touch(dir.filePath(QStringLiteral("ptf/Hello.md"))));
        QString backupPath, error;
        QVERIFY(AnyKeep::LocalKeyRecoveryPolicy::quarantineProtectedFiles(dir.path(), &backupPath, &error));
        QVERIFY(backupPath.isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("ptf/Hello.md"))));
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
