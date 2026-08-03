#include "filefoldercatalogstore.h"
#include "filenoterulestore.h"
#include "foldercatalogmanager.h"
#include "folderoperationscontroller.h"
#include "notemanager.h"
#include "noteruleapplicationcontroller.h"
#include "noterulemanager.h"
#include "notesindex.h"
#include "secureenvelope.h"
#include "tomboystorage.h"

#include <QDomDocument>
#include <QFile>
#include <QList>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace AnyKeep;

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

QString formattedTomboyNoteXml()
{
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                          "<note version=\"0.3\" xmlns=\"http://beatniksoftware.com/tomboy\">"
                          "<title>New Note 2026-08-01</title>"
                          "<text xml:space=\"preserve\"><note-content version=\"0.1\">"
                          "New Note 2026-08-01\nnew note test\n\n"
                          "<list><list-item dir=\"ltr\">hello</list-item></list>\n"
                          "<list><list-item dir=\"ltr\">world</list-item></list>"
                          "</note-content></text>"
                          "<last-change-date>2026-08-01T08:52:39.885Z</last-change-date>"
                          "<last-metadata-change-date>2026-08-01T08:52:39.885Z</last-metadata-change-date>"
                          "<create-date>2026-08-01T08:52:39.885Z</create-date>"
                          "</note>");
}

QString headingTomboyNoteXml()
{
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                          "<note version=\"0.3\" xmlns=\"http://beatniksoftware.com/tomboy\" "
                          "xmlns:size=\"http://beatniksoftware.com/tomboy/size\">"
                          "<title>xxxx</title>"
                          "<text xml:space=\"preserve\"><note-content version=\"0.1\">"
                          "<size:huge><underline>xxxx</underline></size:huge>\n"
                          "<size:huge>test</size:huge>\n"
                          "<size:large>fgdg</size:large>\n"
                          "ordinary first line\nordinary second line\n\n"
                          "inline <size:huge>large text</size:huge> remains inline"
                          "</note-content></text>"
                          "<last-change-date>2026-08-01T10:00:00.000Z</last-change-date>"
                          "<last-metadata-change-date>2026-08-01T10:00:00.000Z</last-metadata-change-date>"
                          "<create-date>2026-08-01T10:00:00.000Z</create-date>"
                          "</note>");
}

QString domNodeText(const QDomNode &node)
{
    QString text;
    for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.isText())
            text += child.nodeValue();
        else if (child.isElement())
            text += domNodeText(child);
    }
    return text;
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
    void titleAndBulletListsRoundTrip();
    void richTextHeadingsAndLineBreaksRoundTrip();
    void folderAssignmentNeverWritesTomboyFolderData();
    void folderRulesCreateOnlyALocalOverlay();
};

void TomboyFolderOverlayTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
    QVERIFY2(FileNoteRuleStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

void TomboyFolderOverlayTest::titleAndBulletListsRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto notePath = directory.filePath(QStringLiteral("formatted.note"));
    QFile      noteFile(notePath);
    QVERIFY(noteFile.open(QIODevice::WriteOnly));
    QCOMPARE(noteFile.write(formattedTomboyNoteXml().toUtf8()), formattedTomboyNoteXml().toUtf8().size());
    noteFile.close();

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

    TomboyStorage storage(nullptr);
    QVERIFY(storage.init());

    auto note = storage.note(QStringLiteral("formatted"));
    QVERIFY(!note.isNull());
    QCOMPARE(note.title(), QStringLiteral("New Note 2026-08-01"));
    QCOMPARE(note.text(), QStringLiteral("new note test\n\n- hello\n- world"));

    note.setText(QStringLiteral("new note test\n\n- hello\n    - nested\n- world"), Note::Markdown);
    QVERIFY(storage.saveNote(note));

    const QByteArray savedBytes = readFile(notePath);
    QVERIFY(savedBytes.startsWith(QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>")));

    QFile savedFile(notePath);
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    QDomDocument savedDom;
    QVERIFY(savedDom.setContent(&savedFile));
    const auto root    = savedDom.documentElement();
    const auto content = root.namedItem(QStringLiteral("text")).namedItem(QStringLiteral("note-content")).toElement();
    QVERIFY(!content.isNull());
    const auto underlinedTitle = content.firstChildElement(QStringLiteral("underline"));
    QVERIFY(!underlinedTitle.isNull());
    QCOMPARE(domNodeText(underlinedTitle), QStringLiteral("New Note 2026-08-01"));
    QVERIFY(domNodeText(content).startsWith(QStringLiteral("New Note 2026-08-01\n\nnew note test\n")));
    QCOMPARE(domNodeText(root.namedItem(QStringLiteral("cursor-position"))), QStringLiteral("1"));
    QCOMPARE(domNodeText(root.namedItem(QStringLiteral("selection-bound-position"))), QStringLiteral("1"));
    QCOMPARE(domNodeText(root.namedItem(QStringLiteral("width"))), QStringLiteral("337"));
    QCOMPARE(domNodeText(root.namedItem(QStringLiteral("height"))), QStringLiteral("200"));
    QCOMPARE(domNodeText(root.namedItem(QStringLiteral("open-on-startup"))), QStringLiteral("False"));
    QList<QDomElement> topLevelLists;
    for (auto child = content.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.isElement() && child.toElement().tagName() == QLatin1String("list"))
            topLevelLists.append(child.toElement());
    }
    QCOMPARE(topLevelLists.size(), 3);
    QCOMPARE(domNodeText(topLevelLists.at(0)), QStringLiteral("hello"));
    QCOMPARE(domNodeText(topLevelLists.at(1)), QStringLiteral("nested"));
    QCOMPARE(domNodeText(topLevelLists.at(2)), QStringLiteral("world"));
    QCOMPARE(topLevelLists.at(0).elementsByTagName(QStringLiteral("list-item")).count(), 1);
    QCOMPARE(topLevelLists.at(1).elementsByTagName(QStringLiteral("list-item")).count(), 2);
    QCOMPARE(topLevelLists.at(2).elementsByTagName(QStringLiteral("list-item")).count(), 1);

    const auto reloaded = storage.note(QStringLiteral("formatted"));
    QVERIFY(!reloaded.isNull());
    QCOMPARE(reloaded.title(), QStringLiteral("New Note 2026-08-01"));
    QCOMPARE(reloaded.text(), QStringLiteral("new note test\n\n- hello\n    - nested\n- world"));
}

void TomboyFolderOverlayTest::richTextHeadingsAndLineBreaksRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto notePath = directory.filePath(QStringLiteral("headings.note"));
    QFile      noteFile(notePath);
    QVERIFY(noteFile.open(QIODevice::WriteOnly));
    QCOMPARE(noteFile.write(headingTomboyNoteXml().toUtf8()), headingTomboyNoteXml().toUtf8().size());
    noteFile.close();

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

    TomboyStorage storage(nullptr);
    QVERIFY(storage.init());

    auto note = storage.note(QStringLiteral("headings"));
    QVERIFY(!note.isNull());
    QCOMPARE(note.title(), QStringLiteral("xxxx"));
    QCOMPARE(note.text(),
             QStringLiteral("# test\n\n## fgdg\n\nordinary first line\n\nordinary second line\n\n"
                            "inline large text remains inline"));

    QVERIFY(storage.saveNote(note));

    QFile savedFile(notePath);
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    QDomDocument savedDom;
    QVERIFY(savedDom.setContent(&savedFile));
    const auto content = savedDom.documentElement()
                             .namedItem(QStringLiteral("text"))
                             .namedItem(QStringLiteral("note-content"))
                             .toElement();
    QVERIFY(!content.isNull());

    const auto hugeHeadings  = content.elementsByTagName(QStringLiteral("size:huge"));
    const auto largeHeadings = content.elementsByTagName(QStringLiteral("size:large"));
    QCOMPARE(hugeHeadings.count(), 1);
    QCOMPARE(largeHeadings.count(), 1);
    QCOMPARE(domNodeText(hugeHeadings.at(0)), QStringLiteral("test"));
    QCOMPARE(domNodeText(largeHeadings.at(0)), QStringLiteral("fgdg"));
    const auto reloaded = storage.note(QStringLiteral("headings"));
    QVERIFY(!reloaded.isNull());
    QCOMPARE(reloaded.text(), note.text());
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
    folder.name        = QStringLiteral("AnyKeep-only folder");
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
    QVERIFY(!savedXml.contains(QByteArrayLiteral("AnyKeep-only folder")));
    QVERIFY(savedXml.contains(QByteArrayLiteral("personal")));
    QVERIFY(savedXml.contains(QByteArrayLiteral("system:notebook:Existing Tomboy notebook")));

    const auto reloaded = raw->note(QStringLiteral("imported"));
    QVERIFY(!reloaded.isNull());
    QCOMPARE(reloaded.tags(), QStringList { QStringLiteral("personal") });
}

void TomboyFolderOverlayTest::folderRulesCreateOnlyALocalOverlay()
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

    QTemporaryDir catalogDirectory;
    QVERIFY(catalogDirectory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FolderCatalogManager catalog(
        std::make_unique<FileFolderCatalogStore>(catalogDirectory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(catalog.initialize());
    FolderRecord folder;
    folder.name        = QStringLiteral("Imported Tomboy");
    const auto created = catalog.addFolder(folder);
    QVERIFY(created);

    NoteRuleManager rules(
        std::make_unique<FileNoteRuleStore>(catalogDirectory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    NoteRule rule;
    rule.name       = QStringLiteral("Import Tomboy folder");
    rule.conditions = { { NoteRuleConditionKind::TitleMatches, QStringLiteral("Imported Tomboy*"), false } };
    NoteRuleAction folderAction;
    folderAction.kind     = NoteRuleActionKind::AssignFolder;
    folderAction.folderId = created.value;
    NoteRuleAction ignoredStorageAction;
    ignoredStorageAction.kind      = NoteRuleActionKind::SelectStorage;
    ignoredStorageAction.storageId = QStringLiteral("not-a-real-destination");
    rule.actions                   = { folderAction, ignoredStorageAction };
    QVERIFY(rules.addRule(rule));

    auto *manager = NoteManager::instance();
    QVERIFY(!manager->storage(TomboyStorage::storageId));
    NoteRuleApplicationController controller(&rules, &catalog, manager);
    controller.initialize();

    auto  storage = std::make_unique<TomboyStorage>(nullptr);
    auto *raw     = storage.get();
    QVERIFY(raw->init());
    QVERIFY(raw->supportsFolderRuleOverlayImport());
    QSignalSpy providerWrites(raw, &NoteStorage::noteModified);
    manager->registerStorage(std::move(storage));
    const auto unregisterStorage = qScopeGuard([manager, raw]() {
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    QTRY_COMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("imported")), created.value);
    QCOMPARE(providerWrites.count(), 0);
    QCOMPARE(readFile(notePath), originalXml);
}

QTEST_GUILESS_MAIN(TomboyFolderOverlayTest)
#include "tomboyfolderoverlay_test.moc"
