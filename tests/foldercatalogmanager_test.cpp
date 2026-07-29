#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "secureenvelope.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace QtNote;

class FolderCatalogManagerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void persistsMutationsAndProviderImports();
    void persistsReconciledProviderPaths();
    void collapsesAllFoldersInOneCatalogUpdate();
    void rejectsForeignProviderAssignments();
    void rejectsForeignProviderPathHints();
    void requiresExplicitRecoveryForCorruptCatalog();
};

void FolderCatalogManagerTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

static FolderRecord folder(const QString &name)
{
    FolderRecord record;
    record.id   = QUuid::createUuid();
    record.name = name;
    return record;
}

void FolderCatalogManagerTest::persistsMutationsAndProviderImports()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("folders.bin"));
    const auto key  = SecureEnvelope::generateMasterKey();

    FolderCatalogManager manager(std::make_unique<FileFolderCatalogStore>(path, key));
    QVERIFY(manager.initialize());
    QVERIFY(manager.isAvailable());

    QSignalSpy changed(&manager, &FolderCatalogManager::catalogChanged);
    const auto inbox = manager.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY2(inbox, qPrintable(inbox.error.message));
    QVERIFY(!manager.assignNote(QStringLiteral("tomboy"), QStringLiteral("note-1"), inbox.value));

    FolderCatalog provider;
    const auto    imported = provider.addFolder(folder(QStringLiteral("Imported")));
    QVERIFY(imported);
    QVERIFY(!provider.assignNote(QStringLiteral("ptf"), QStringLiteral("note-2"), imported.value));
    QVERIFY(!manager.mergeProviderCatalog(QStringLiteral("ptf"), provider.snapshot()));
    QCOMPARE(manager.catalog().folderForNote(QStringLiteral("ptf"), QStringLiteral("note-2")), imported.value);
    QCOMPARE(changed.count(), 3);

    FolderCatalogManager reloaded(std::make_unique<FileFolderCatalogStore>(path, key));
    QVERIFY(reloaded.initialize());
    QCOMPARE(reloaded.catalog().folderForNote(QStringLiteral("tomboy"), QStringLiteral("note-1")), inbox.value);
    QCOMPARE(reloaded.catalog().folderForNote(QStringLiteral("ptf"), QStringLiteral("note-2")), imported.value);
    QCOMPARE(reloaded.catalog().folders().size(), 2);
}

void FolderCatalogManagerTest::persistsReconciledProviderPaths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("folders.bin"));
    const auto key  = SecureEnvelope::generateMasterKey();

    FolderCatalogManager manager(std::make_unique<FileFolderCatalogStore>(path, key));
    QVERIFY(manager.initialize());
    ProviderFolderPathAssignment assignment;
    assignment.noteId     = QStringLiteral("remote-1");
    assignment.path       = { QStringLiteral("Imported"), QStringLiteral("Path") };
    assignment.modifiedAt = QDateTime::currentDateTimeUtc();
    QVERIFY(!manager.reconcileProviderFolderPaths(QStringLiteral("nextcloud"), { assignment }));
    const auto folderId = manager.catalog().folderForNote(QStringLiteral("nextcloud"), assignment.noteId);
    QVERIFY(!folderId.isNull());
    QVERIFY(manager.catalog().pathHint(QStringLiteral("nextcloud"), assignment.path));

    FolderCatalogManager reloaded(std::make_unique<FileFolderCatalogStore>(path, key));
    QVERIFY(reloaded.initialize());
    QCOMPARE(reloaded.catalog().folderForNote(QStringLiteral("nextcloud"), assignment.noteId), folderId);
    QCOMPARE(reloaded.catalog().pathForFolder(folderId), assignment.path);
}

void FolderCatalogManagerTest::collapsesAllFoldersInOneCatalogUpdate()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(manager.initialize());

    const auto first  = manager.addFolder(folder(QStringLiteral("First")));
    const auto second = manager.addFolder(folder(QStringLiteral("Second")));
    QVERIFY(first);
    QVERIFY(second);
    QSignalSpy changed(&manager, &FolderCatalogManager::catalogChanged);

    QVERIFY(!manager.setAllFoldersCollapsed(true));
    QCOMPARE(changed.count(), 1);
    QVERIFY(manager.catalog().folder(first.value)->collapsed);
    QVERIFY(manager.catalog().folder(second.value)->collapsed);

    QVERIFY(!manager.setAllFoldersCollapsed(true));
    QCOMPARE(changed.count(), 1);
}

void FolderCatalogManagerTest::rejectsForeignProviderAssignments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(manager.initialize());

    FolderCatalog provider;
    const auto    imported = provider.addFolder(folder(QStringLiteral("Imported")));
    QVERIFY(imported);
    QVERIFY(!provider.assignNote(QStringLiteral("other"), QStringLiteral("note-2"), imported.value));
    const auto result = manager.mergeProviderCatalog(QStringLiteral("ptf"), provider.snapshot());
    QVERIFY(result);
    QCOMPARE(result.code, FolderCatalogError::InvalidArgument);
    QVERIFY(manager.catalog().folders().isEmpty());
}

void FolderCatalogManagerTest::rejectsForeignProviderPathHints()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(manager.initialize());

    FolderCatalog provider;
    const auto    imported = provider.addFolder(folder(QStringLiteral("Imported")));
    QVERIFY(imported);
    ProviderPathHint hint;
    hint.storageId  = QStringLiteral("other");
    hint.path       = { QStringLiteral("Imported") };
    hint.folderId   = imported.value;
    hint.revision   = 1;
    hint.modifiedAt = QDateTime::currentDateTimeUtc();
    auto snapshot   = provider.snapshot();
    snapshot.pathHints.append(hint);

    const auto result = manager.mergeProviderCatalog(QStringLiteral("ptf"), snapshot);
    QVERIFY(result);
    QCOMPARE(result.code, FolderCatalogError::InvalidArgument);
    QVERIFY(manager.catalog().folders().isEmpty());
}

void FolderCatalogManagerTest::requiresExplicitRecoveryForCorruptCatalog()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("folders.bin"));
    const auto key  = SecureEnvelope::generateMasterKey();

    {
        FolderCatalogManager writer(std::make_unique<FileFolderCatalogStore>(path, key));
        QVERIFY(writer.initialize());
        QVERIFY(writer.addFolder(folder(QStringLiteral("First"))));
        QVERIFY(writer.addFolder(folder(QStringLiteral("Second"))));
    }

    QFile encrypted(path);
    QVERIFY(encrypted.open(QIODevice::ReadWrite));
    auto bytes = encrypted.readAll();
    QVERIFY(bytes.size() > 8);
    bytes[bytes.size() / 2] = char(uchar(bytes.at(bytes.size() / 2)) ^ 1U);
    QVERIFY(encrypted.resize(0));
    QCOMPARE(encrypted.write(bytes), bytes.size());
    encrypted.close();

    FolderCatalogManager manager(std::make_unique<FileFolderCatalogStore>(path, key));
    QSignalSpy           recovery(&manager, &FolderCatalogManager::recoveryRequired);
    QString              error;
    QVERIFY(!manager.initialize(&error));
    QVERIFY(!manager.isAvailable());
    QVERIFY(manager.needsRecovery());
    QVERIFY(manager.hasRecoveryBackup());
    QVERIFY(!error.isEmpty());
    QVERIFY(manager.catalog().folders().isEmpty());
    QCOMPARE(recovery.count(), 1);

    QString preserved;
    QVERIFY(!manager.restoreBackup(&preserved));
    QVERIFY(manager.isAvailable());
    QVERIFY(!manager.needsRecovery());
    QVERIFY(!preserved.isEmpty());
    QVERIFY(QFileInfo::exists(preserved));
    QCOMPARE(manager.catalog().folders().size(), 1);
    QCOMPARE(manager.catalog().folders().first().name, QStringLiteral("First"));
}

QTEST_GUILESS_MAIN(FolderCatalogManagerTest)
#include "foldercatalogmanager_test.moc"
