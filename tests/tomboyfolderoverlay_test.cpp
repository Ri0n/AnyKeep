#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "folderoperationscontroller.h"
#include "notemanager.h"
#include "notesindex.h"
#include "secureenvelope.h"
#include "tomboystorage.h"

#include <QFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace QtNote;

namespace {

QString tomboyNoteXml()
{
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<note version=\"0.3\" xmlns=\"http://beatniksoftware.com/tomboy\">\n"
        "  <title>Imported Tomboy note</title>\n"
        "  <text xml:space=\"preserve\"><note-content version=\"0.1\">Original body</note-content></text>\n"
        "  <tags>\n"
        "    <tag>personal</tag>\n"
        "    <tag>system:notebook:Existing Tomboy notebook</tag>\n"
        "  </tags>\n"
        "  <last-change-date>2026-07-30T12:00:00.000Z</last-change-date>\n"
        "  <last-metadata-change-date>2026-07-30T12:00:00.000Z</last-metadata-change-date>\n"
        "  <create-date>2026-07-30T12:00:00.000Z</create-date>\n"
        "</note>\n");
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

class TomboyFolderOverlayTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void folderAssignmentNeverWritesTomboyFolderData();
};

void TomboyFolderOverlayTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

void TomboyFolderOverlayTest::folderAssignmentNeverWritesTomboyFolderData()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto notePath = directory.filePath(QStringLiteral("imported.note"));
    QFile      noteFile(notePath);
    QVERIFY(noteFile.open(QIODevice::WriteOnly));
    QCOMPARE(noteFile.write(tomboyNoteXml().toUtf8()), tomboyNoteXml().toUtf8().size());
    noteFile.close();
    const auto originalXml = readFile(notePath);
    QVERIFY(!originalXml.isEmpty());

    QSettings      settings;
    const auto     settingsKey     = QStringLiteral("storage.tomboy.path");
    const QVariant previousPath    = settings.value(settingsKey);
    const bool     hadPreviousPath = settings.contains(settingsKey);
    const auto     restoreSettings = qScopeGuard([settingsKey, previousPath, hadPreviousPath]() {
        QSettings restore;
        if (hadPreviousPath)
            restore.setValue(settingsKey, previousPath);
        else
            restore.remove(settingsKey);
    });
    settings.setValue(settingsKey, directory.path());

    auto *manager = NoteManager::instance();
    QVERIFY(!manager->storage(TomboyStorage::storageId));
    auto  storage = std::make_unique<TomboyStorage>(nullptr);
    auto *raw     = storage.get();
    QVERIFY(raw->init());
    QVERIFY(!raw->supportsNativeFolders());
    manager->registerStorage(std::move(storage));
    const auto unregisterStorage = qScopeGuard([manager, raw]() {
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });
    QTRY_VERIFY(manager->notesIndex()->hasSnapshot(raw->systemName()));

    QTemporaryDir catalogDirectory;
    QVERIFY(catalogDirectory.isValid());
    FolderCatalogManager catalog(std::make_unique<FileFolderCatalogStore>(
        catalogDirectory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(catalog.initialize());
    FolderRecord folder;
    folder.name        = QStringLiteral("QtNote-only folder");
    const auto created = catalog.addFolder(folder);
    QVERIFY(created);

    FolderOperationsController controller(&catalog, manager);
    QSignalSpy                 finished(&controller, &FolderOperationsController::assignmentFinished);
    QVERIFY(controller.assignNoteFolder(raw->systemName(), QStringLiteral("imported"), created.value));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("imported")), created.value);
    QCOMPARE(readFile(notePath), originalXml);

    // A later ordinary Tomboy save must still ignore the local folder ID and
    // preserve Tomboy's existing tags rather than inventing notebook/tag data.
    auto note = raw->note(QStringLiteral("imported"));
    QVERIFY(!note.isNull());
    note.setFolderId(created.value);
    QVERIFY(raw->saveNote(note));
    const auto savedXml = readFile(notePath);
    QVERIFY(!savedXml.contains(QByteArrayLiteral("QtNote-only folder")));
    QVERIFY(savedXml.contains(QByteArrayLiteral("personal")));
    QVERIFY(savedXml.contains(QByteArrayLiteral("system:notebook:Existing Tomboy notebook")));

    const auto reloaded = raw->note(QStringLiteral("imported"));
    QVERIFY(!reloaded.isNull());
    QCOMPARE(reloaded.tags(), QStringList { QStringLiteral("personal") });
}

QTEST_GUILESS_MAIN(TomboyFolderOverlayTest)
#include "tomboyfolderoverlay_test.moc"
