#include "filefoldercatalogstore.h"
#include "secureenvelope.h"

#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace QtNote;

class FolderCatalogTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void maintainsValidatedTree();
    void movesFoldersRelativeToSiblings();
    void sortsFavoritesBeforeOtherSiblings();
    void sortsArchivedAndRecycleBinAfterNormalFolders();
    void buildsFolderPaths();
    void reconcilesPathOnlyProviderAssignments();
    void retainsAssignmentTombstones();
    void recyclesAndRestoresNotesWithOriginalFolder();
    void mergesNewerRecords();
    void detectsEqualRevisionConflict();
    void encryptedRoundTripAndBackupRecovery();
    void readsVersionOneCatalogWithoutPathHints();
    void rejectsWrongKey();
};

void FolderCatalogTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

static FolderRecord folder(const QString &name, const QUuid &parentId = {}, qint64 sortOrder = 0)
{
    FolderRecord record;
    record.id        = QUuid::createUuid();
    record.parentId  = parentId;
    record.name      = name;
    record.sortOrder = sortOrder;
    return record;
}

void FolderCatalogTest::maintainsValidatedTree()
{
    FolderCatalog catalog;
    const auto    root = catalog.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY2(root, qPrintable(root.error.message));
    const auto child = catalog.addFolder(folder(QStringLiteral("Receipts"), root.value, 10));
    QVERIFY2(child, qPrintable(child.error.message));

    QCOMPARE(catalog.children().size(), 1);
    QCOMPARE(catalog.children(root.value).size(), 1);
    QCOMPARE(catalog.children(root.value).first().id, child.value);

    const auto duplicate = catalog.addFolder(folder(QStringLiteral("  inbox  ")));
    QVERIFY(!duplicate);
    QCOMPARE(duplicate.error.code, FolderCatalogError::Conflict);

    const auto cycle = catalog.moveFolder(root.value, child.value, 0);
    QVERIFY(cycle);
    QCOMPARE(cycle.code, FolderCatalogError::Cycle);

    QVERIFY(!catalog.renameFolder(child.value, QStringLiteral("Invoices")));
    QVERIFY(!catalog.setFolderFlags(child.value, true, true));
    QVERIFY(!catalog.setFolderCollapsed(child.value, true));
    const auto *changed = catalog.folder(child.value);
    QVERIFY(changed);
    QCOMPARE(changed->name, QStringLiteral("Invoices"));
    QVERIFY(changed->favorite);
    QVERIFY(changed->archived);
    QVERIFY(changed->collapsed);
}

void FolderCatalogTest::movesFoldersRelativeToSiblings()
{
    FolderCatalog catalog;
    const auto    first  = catalog.addFolder(folder(QStringLiteral("First"), {}, 10));
    const auto    second = catalog.addFolder(folder(QStringLiteral("Second"), {}, 20));
    const auto    third  = catalog.addFolder(folder(QStringLiteral("Third"), {}, 30));
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(third);

    QVERIFY(!catalog.moveFolderRelative(third.value, {}, first.value));
    auto siblings = catalog.children();
    QCOMPARE(siblings.size(), 3);
    QCOMPARE(siblings.at(0).id, third.value);
    QCOMPARE(siblings.at(1).id, first.value);
    QCOMPARE(siblings.at(2).id, second.value);
    QCOMPARE(siblings.at(0).sortOrder, qint64(0));
    QCOMPARE(siblings.at(1).sortOrder, qint64(1024));
    QCOMPARE(siblings.at(2).sortOrder, qint64(2048));

    QVERIFY(!catalog.moveFolderRelative(first.value, third.value));
    siblings = catalog.children();
    QCOMPARE(siblings.size(), 2);
    QCOMPARE(siblings.at(0).id, third.value);
    QCOMPARE(siblings.at(1).id, second.value);
    const auto children = catalog.children(third.value);
    QCOMPARE(children.size(), 1);
    QCOMPARE(children.first().id, first.value);

    const auto cycle = catalog.moveFolderRelative(third.value, first.value);
    QVERIFY(cycle);
    QCOMPARE(cycle.code, FolderCatalogError::Cycle);
    QCOMPARE(catalog.folder(third.value)->parentId, QUuid {});
}

void FolderCatalogTest::sortsFavoritesBeforeOtherSiblings()
{
    FolderCatalog catalog;
    const auto    first    = catalog.addFolder(folder(QStringLiteral("First"), {}, 0));
    const auto    favorite = catalog.addFolder(folder(QStringLiteral("Favorite"), {}, 100));
    QVERIFY(first);
    QVERIFY(favorite);
    QVERIFY(!catalog.setFolderFlags(favorite.value, true, false));

    const auto siblings = catalog.children();
    QCOMPARE(siblings.size(), 2);
    QCOMPARE(siblings.first().id, favorite.value);
    QCOMPARE(siblings.last().id, first.value);
}

void FolderCatalogTest::sortsArchivedAndRecycleBinAfterNormalFolders()
{
    FolderCatalog catalog;
    const auto    normal         = catalog.addFolder(folder(QStringLiteral("Normal"), {}, 100));
    FolderRecord  archivedRecord = folder(QStringLiteral("Archived"), {}, -100);
    archivedRecord.archived      = true;
    const auto archived          = catalog.addFolder(archivedRecord);
    QVERIFY(normal);
    QVERIFY(archived);

    QVERIFY(!catalog.recycleNote(QStringLiteral("ptf"), QStringLiteral("discarded"), {}));
    const auto siblings = catalog.children();
    QCOMPARE(siblings.size(), 3);
    QCOMPARE(siblings.at(0).id, normal.value);
    QCOMPARE(siblings.at(1).id, archived.value);
    QCOMPARE(siblings.at(2).id, FolderCatalog::recycleBinId());
    QVERIFY(FolderCatalog::isRecycleBinId(siblings.at(2).id));
}

void FolderCatalogTest::buildsFolderPaths()
{
    FolderCatalog catalog;
    const auto    projects = catalog.addFolder(folder(QStringLiteral("Projects")));
    QVERIFY(projects);
    const auto current = catalog.addFolder(folder(QStringLiteral("2026"), projects.value));
    QVERIFY(current);
    const auto invoices = catalog.addFolder(folder(QStringLiteral("Invoices"), current.value));
    QVERIFY(invoices);

    QCOMPARE(catalog.pathForFolder(projects.value), QStringList({ QStringLiteral("Projects") }));
    QCOMPARE(catalog.pathForFolder(invoices.value),
             QStringList({ QStringLiteral("Projects"), QStringLiteral("2026"), QStringLiteral("Invoices") }));
    QVERIFY(catalog.pathForFolder(QUuid::createUuid()).isEmpty());
}

void FolderCatalogTest::reconcilesPathOnlyProviderAssignments()
{
    FolderCatalog                catalog;
    const auto                   remoteTime = QDateTime::currentDateTimeUtc().addSecs(-60);
    ProviderFolderPathAssignment imported;
    imported.noteId     = QStringLiteral("note-1");
    imported.path       = { QStringLiteral("Projects"), QStringLiteral("2026") };
    imported.modifiedAt = remoteTime;
    QVERIFY(!catalog.reconcileProviderFolderPaths(QStringLiteral("nextcloud"), { imported }));

    const auto projects = catalog.children();
    QCOMPARE(projects.size(), 1);
    QCOMPARE(projects.first().name, QStringLiteral("Projects"));
    const auto years = catalog.children(projects.first().id);
    QCOMPARE(years.size(), 1);
    const auto importedFolder = years.first().id;
    QCOMPARE(catalog.folderForNote(QStringLiteral("nextcloud"), QStringLiteral("note-1")), importedFolder);
    const auto *hint
        = catalog.pathHint(QStringLiteral("nextcloud"), { QStringLiteral("PROJECTS"), QStringLiteral("2026") });
    QVERIFY(hint);
    QCOMPARE(hint->folderId, importedFolder);

    QVERIFY(!catalog.renameFolder(projects.first().id, QStringLiteral("Work")));
    ProviderFolderPathAssignment stale;
    stale.noteId     = QStringLiteral("note-2");
    stale.path       = imported.path;
    stale.modifiedAt = remoteTime;
    QVERIFY(!catalog.reconcileProviderFolderPaths(QStringLiteral("nextcloud"), { stale }));
    QCOMPARE(catalog.folderForNote(QStringLiteral("nextcloud"), QStringLiteral("note-2")), importedFolder);
    QCOMPARE(catalog.children().first().name, QStringLiteral("Work"));

    FolderRecord localFolder;
    localFolder.name = QStringLiteral("Local");
    const auto local = catalog.addFolder(localFolder);
    QVERIFY(local);
    QVERIFY(!catalog.assignNote(QStringLiteral("nextcloud"), QStringLiteral("note-1"), local.value));
    QVERIFY(!catalog.reconcileProviderFolderPaths(QStringLiteral("nextcloud"), { imported }));
    QCOMPARE(catalog.folderForNote(QStringLiteral("nextcloud"), QStringLiteral("note-1")), local.value);

    ProviderFolderPathAssignment external;
    external.noteId     = QStringLiteral("note-1");
    external.path       = { QStringLiteral("External") };
    external.modifiedAt = QDateTime::currentDateTimeUtc().addSecs(60);
    QVERIFY(!catalog.reconcileProviderFolderPaths(QStringLiteral("nextcloud"), { external }));
    const auto externalFolder = catalog.folderForNote(QStringLiteral("nextcloud"), QStringLiteral("note-1"));
    QVERIFY(!externalFolder.isNull());
    QCOMPARE(catalog.pathForFolder(externalFolder), QStringList({ QStringLiteral("External") }));
}

void FolderCatalogTest::retainsAssignmentTombstones()
{
    FolderCatalog catalog;
    const auto    inbox = catalog.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY(inbox);

    QVERIFY(!catalog.assignNote(QStringLiteral("tomboy"), QStringLiteral("note-1"), inbox.value));
    QCOMPARE(catalog.folderForNote(QStringLiteral("tomboy"), QStringLiteral("note-1")), inbox.value);
    QVERIFY(!catalog.clearNoteAssignment(QStringLiteral("tomboy"), QStringLiteral("note-1")));
    QVERIFY(catalog.folderForNote(QStringLiteral("tomboy"), QStringLiteral("note-1")).isNull());
    const auto *assignment = catalog.assignment(QStringLiteral("tomboy"), QStringLiteral("note-1"));
    QVERIFY(assignment);
    QVERIFY(assignment->tombstone);
    QVERIFY(assignment->folderId.isNull());
    QCOMPARE(assignment->revision, quint64(2));
}

void FolderCatalogTest::recyclesAndRestoresNotesWithOriginalFolder()
{
    FolderCatalog catalog;
    const auto    inbox = catalog.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY(inbox);

    QVERIFY(!catalog.recycleNote(QStringLiteral("ptf"), QStringLiteral("note-1"), inbox.value));
    const auto  trash       = FolderCatalog::recycleBinId();
    const auto *trashFolder = catalog.folder(trash);
    QVERIFY(trashFolder);
    QVERIFY(trashFolder->archived);
    QVERIFY(catalog.isRecycled(QStringLiteral("ptf"), QStringLiteral("note-1")));
    const auto *recycled = catalog.assignment(QStringLiteral("ptf"), QStringLiteral("note-1"));
    QVERIFY(recycled);
    QCOMPARE(recycled->folderId, trash);
    QCOMPARE(recycled->previousFolderId, inbox.value);
    QVERIFY(recycled->recycledAt.isValid());

    const auto restored = catalog.restoreRecycledNote(QStringLiteral("ptf"), QStringLiteral("note-1"));
    QVERIFY(restored);
    QCOMPARE(restored.value, inbox.value);
    QCOMPARE(catalog.folderForNote(QStringLiteral("ptf"), QStringLiteral("note-1")), inbox.value);
    QVERIFY(!catalog.isRecycled(QStringLiteral("ptf"), QStringLiteral("note-1")));
    const auto *assignment = catalog.assignment(QStringLiteral("ptf"), QStringLiteral("note-1"));
    QVERIFY(assignment);
    QVERIFY(assignment->previousFolderId.isNull());
    QVERIFY(!assignment->recycledAt.isValid());
}

void FolderCatalogTest::mergesNewerRecords()
{
    FolderCatalog catalog;
    const auto    inbox = catalog.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY(inbox);

    auto incomingRecord = *catalog.folder(inbox.value);
    incomingRecord.name = QStringLiteral("Archive");
    incomingRecord.revision += 1;
    incomingRecord.modifiedAt = QDateTime::currentDateTimeUtc().addSecs(1);
    FolderCatalogSnapshot incoming;
    incoming.folders.append(incomingRecord);
    QVERIFY(!catalog.merge(incoming));
    QCOMPARE(catalog.folder(inbox.value)->name, QStringLiteral("Archive"));

    incomingRecord.name       = QStringLiteral("Older name");
    incomingRecord.revision   = 1;
    incomingRecord.modifiedAt = QDateTime::currentDateTimeUtc().addSecs(-1);
    incoming.folders          = { incomingRecord };
    QVERIFY(!catalog.merge(incoming));
    QCOMPARE(catalog.folder(inbox.value)->name, QStringLiteral("Archive"));
}

void FolderCatalogTest::detectsEqualRevisionConflict()
{
    FolderCatalog catalog;
    const auto    inbox = catalog.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY(inbox);

    auto conflictRecord = *catalog.folder(inbox.value);
    conflictRecord.name = QStringLiteral("Different Inbox");
    FolderCatalogSnapshot incoming;
    incoming.folders.append(conflictRecord);
    const auto result = catalog.merge(incoming);
    QVERIFY(result);
    QCOMPARE(result.code, FolderCatalogError::Conflict);
    QCOMPARE(catalog.folder(inbox.value)->name, QStringLiteral("Inbox"));
}

void FolderCatalogTest::encryptedRoundTripAndBackupRecovery()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto             key = SecureEnvelope::generateMasterKey();
    FileFolderCatalogStore store(directory.filePath(QStringLiteral("folders.bin")), key);

    FolderCatalog first;
    const auto    inbox = first.addFolder(folder(QStringLiteral("Private receipts")));
    QVERIFY(inbox);
    QVERIFY(!store.save(first.snapshot()));

    QFile encrypted(store.filePath());
    QVERIFY(encrypted.open(QIODevice::ReadOnly));
    QVERIFY(!encrypted.readAll().contains(QByteArrayLiteral("Private receipts")));
    encrypted.close();

    FolderCatalog second;
    QVERIFY(!second.replaceSnapshot(first.snapshot()));
    QVERIFY(!second.renameFolder(inbox.value, QStringLiteral("Tax receipts")));
    QVERIFY(!store.save(second.snapshot()));

    const auto backup = store.loadBackup();
    QVERIFY2(backup, qPrintable(backup.error.message));
    QCOMPARE(backup.value.folders.size(), 1);
    QCOMPARE(backup.value.folders.first().name, QStringLiteral("Private receipts"));

    QVERIFY(encrypted.open(QIODevice::ReadWrite));
    auto bytes = encrypted.readAll();
    QVERIFY(bytes.size() > 8);
    bytes[bytes.size() / 2] = char(uchar(bytes.at(bytes.size() / 2)) ^ 1U);
    QVERIFY(encrypted.resize(0));
    QCOMPARE(encrypted.write(bytes), bytes.size());
    encrypted.close();

    const auto broken = store.load();
    QVERIFY(!broken);
    QCOMPARE(broken.error.code, FolderCatalogError::Corrupt);
    const auto refusedWrite = store.save(second.snapshot());
    QVERIFY(refusedWrite);
    QCOMPARE(refusedWrite.code, FolderCatalogError::Corrupt);

    QString preserved;
    QVERIFY(!store.restoreBackup(&preserved));
    QVERIFY(!preserved.isEmpty());
    QVERIFY(QFileInfo::exists(preserved));
    const auto restored = store.load();
    QVERIFY2(restored, qPrintable(restored.error.message));
    QCOMPARE(restored.value.folders.first().name, QStringLiteral("Private receipts"));

    QString recreatedPreserved;
    QVERIFY(!store.recreate(&recreatedPreserved));
    QVERIFY(!recreatedPreserved.isEmpty());
    QVERIFY(QFileInfo::exists(recreatedPreserved));
    const auto recreated = store.load();
    QVERIFY2(recreated, qPrintable(recreated.error.message));
    QVERIFY(recreated.value.folders.isEmpty());
    QVERIFY(recreated.value.assignments.isEmpty());
}

void FolderCatalogTest::readsVersionOneCatalogWithoutPathHints()
{
    constexpr quint32 PayloadMagic = 0x514e4643; // QNFC
    QTemporaryDir     directory;
    QVERIFY(directory.isValid());
    const auto key  = SecureEnvelope::generateMasterKey();
    const auto path = directory.filePath(QStringLiteral("folders.bin"));

    const auto  record = folder(QStringLiteral("Legacy"));
    QByteArray  payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << PayloadMagic << quint16(1) << quint32(1) << quint32(0) << record.id << record.parentId << record.name
        << record.sortOrder << record.collapsed << record.favorite << record.archived << record.revision
        << record.modifiedAt << record.tombstone;

    const AeadContext context { KeyDomain::LocalFolderCatalog, QStringLiteral("qtnote-folder-catalog"),
                                QStringLiteral("global"), 1, QStringLiteral("catalog") };
    const auto        sealed = SecureEnvelope::seal(payload, key, context);
    QVERIFY2(sealed, qPrintable(sealed.error.message));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(sealed.value), sealed.value.size());
    file.close();

    FileFolderCatalogStore store(path, key);
    const auto             loaded = store.load();
    QVERIFY2(loaded, qPrintable(loaded.error.message));
    QCOMPARE(loaded.value.folders.size(), 1);
    QCOMPARE(loaded.value.folders.first().name, QStringLiteral("Legacy"));
    QVERIFY(loaded.value.pathHints.isEmpty());
}

void FolderCatalogTest::rejectsWrongKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalog catalog;
    const auto    inbox = catalog.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY(inbox);

    const auto             path = directory.filePath(QStringLiteral("folders.bin"));
    FileFolderCatalogStore writer(path, SecureEnvelope::generateMasterKey());
    QVERIFY(!writer.save(catalog.snapshot()));
    FileFolderCatalogStore reader(path, SecureEnvelope::generateMasterKey());
    const auto             loaded = reader.load();
    QVERIFY(!loaded);
    QCOMPARE(loaded.error.code, FolderCatalogError::Corrupt);
}

QTEST_GUILESS_MAIN(FolderCatalogTest)
#include "foldercatalog_test.moc"
