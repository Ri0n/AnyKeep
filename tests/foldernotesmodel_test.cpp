#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "foldernotesmodel.h"
#include "notedata.h"
#include "notemanager.h"
#include "notesindex.h"
#include "secureenvelope.h"

#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <memory>

using namespace QtNote;

class FolderModelStorage final : public NoteStorage {
    Q_OBJECT
public:
    explicit FolderModelStorage(QString id, QObject *parent = nullptr) : NoteStorage(parent), id_(std::move(id)) { }

    bool                init() override { return true; }
    const QString       systemName() const override { return id_; }
    const QString       name() const override { return id_; }
    QIcon               storageIcon() const override { return {}; }
    QIcon               noteIcon() const override { return {}; }
    bool                isAccessible() const override { return true; }
    QList<Note::Format> availableFormats() const override { return { Note::Markdown }; }
    QList<Note>         noteList(int limit = 0) override { return limit > 0 ? notes.mid(0, limit) : notes; }
    Note                note(const QString &id) override
    {
        for (const auto &note : notes) {
            if (note.id() == id)
                return note;
        }
        return {};
    }
    Note createNote() override { return {}; }
    bool saveNote(const Note &) override { return false; }
    void removeNote(const QString &) override { }

    Note makeNote(const QString &id, const QString &title, const QUuid &folderId = {})
    {
        Note note(new NoteData(this));
        note.setId(id);
        note.setTitle(title);
        note.setText({}, Note::Markdown);
        note.setFolderId(folderId);
        note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        return note;
    }

    QList<Note> notes;

private:
    QString id_;
};

class FolderNotesModelTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void projectsHierarchyAndUnsortedNotes();
    void placesArchivedFoldersAndRecycleBinAtTheEnd();
    void recyclingNoteUpdatesProjectionWithoutReset();
    void folderPickerIgnoresCollapsedAndArchivedBranches();
    void overlayTombstoneWinsOverProviderFolder();
};

void FolderNotesModelTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

static std::unique_ptr<FileFolderCatalogStore> makeCatalogStore(QTemporaryDir &directory)
{
    return std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")),
                                                    SecureEnvelope::generateMasterKey());
}

void FolderNotesModelTest::projectsHierarchyAndUnsortedNotes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(makeCatalogStore(directory));
    QVERIFY(manager.initialize());

    FolderRecord root;
    root.name             = QStringLiteral("Projects");
    const auto rootResult = manager.addFolder(root);
    QVERIFY(rootResult);
    FolderRecord child;
    child.name             = QStringLiteral("QtNote");
    child.parentId         = rootResult.value;
    const auto childResult = manager.addFolder(child);
    QVERIFY(childResult);

    auto  storage = std::make_unique<FolderModelStorage>(QStringLiteral("folder-model"));
    auto *raw     = storage.get();
    raw->notes    = {
        raw->makeNote(QStringLiteral("root"), QStringLiteral("Root note"), rootResult.value),
        raw->makeNote(QStringLiteral("child"), QStringLiteral("Child note"), childResult.value),
        raw->makeNote(QStringLiteral("unsorted"), QStringLiteral("Loose note")),
    };
    auto *notes = NoteManager::instance();
    notes->registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([notes, raw]() {
        if (notes->storage(raw->systemName()) == raw)
            notes->unregisterStorage(raw);
    });
    QTRY_COMPARE(notes->notesIndex()->noteCount(raw->systemName()), 3);

    FolderNotesModel model(&manager);
    QCOMPARE(model.rowCount(), 6);
    QCOMPARE(model.index(0, 0).data(FolderNotesModel::RowKindRole).toInt(), int(FolderNotesModel::FolderRow));
    QCOMPARE(model.index(0, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("Projects"));
    QCOMPARE(model.index(0, 0).data(FolderNotesModel::DepthRole).toInt(), 0);
    QCOMPARE(model.index(1, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("QtNote"));
    QCOMPARE(model.index(1, 0).data(FolderNotesModel::DepthRole).toInt(), 1);
    QCOMPARE(model.index(2, 0).data(FolderNotesModel::NoteIdRole).toString(), QStringLiteral("child"));
    QCOMPARE(model.index(2, 0).data(FolderNotesModel::DepthRole).toInt(), 2);
    QCOMPARE(model.index(3, 0).data(FolderNotesModel::NoteIdRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.index(3, 0).data(FolderNotesModel::DepthRole).toInt(), 1);
    QCOMPARE(model.index(4, 0).data(FolderNotesModel::RowKindRole).toInt(), int(FolderNotesModel::UnsortedRow));
    QCOMPARE(model.index(4, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("Unsorted"));
    QCOMPARE(model.index(5, 0).data(FolderNotesModel::NoteIdRole).toString(), QStringLiteral("unsorted"));
    QCOMPARE(model.rowForFolder(rootResult.value.toString(QUuid::WithoutBraces)), 0);

    QVERIFY(model.setUnsortedCollapsed(true));
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(model.index(4, 0).data(FolderNotesModel::RowKindRole).toInt(), int(FolderNotesModel::UnsortedRow));
    QVERIFY(model.index(4, 0).data(FolderNotesModel::CollapsedRole).toBool());
    QVERIFY(model.setUnsortedCollapsed(false));
    QCOMPARE(model.rowCount(), 6);

    QVERIFY(!manager.setFolderCollapsed(rootResult.value, true));
    QTRY_COMPARE(model.rowCount(), 3);
    QCOMPARE(model.index(0, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("Projects"));
    QCOMPARE(model.index(1, 0).data(FolderNotesModel::RowKindRole).toInt(), int(FolderNotesModel::UnsortedRow));
    QCOMPARE(model.index(2, 0).data(FolderNotesModel::NoteIdRole).toString(), QStringLiteral("unsorted"));
}

void FolderNotesModelTest::placesArchivedFoldersAndRecycleBinAtTheEnd()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(makeCatalogStore(directory));
    QVERIFY(manager.initialize());

    FolderRecord inbox;
    inbox.name = QStringLiteral("Inbox");
    QVERIFY(manager.addFolder(inbox));
    FolderRecord archive;
    archive.name     = QStringLiteral("Archive");
    archive.archived = true;
    QVERIFY(manager.addFolder(archive));
    QVERIFY(!manager.recycleNote(QStringLiteral("ptf"), QStringLiteral("discarded"), {}));

    FolderNotesModel model(&manager);
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.index(0, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("Inbox"));
    QCOMPARE(model.index(1, 0).data(FolderNotesModel::RowKindRole).toInt(), int(FolderNotesModel::UnsortedRow));
    QCOMPARE(model.index(2, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("Archive"));
    QCOMPARE(model.index(3, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("Recycle Bin"));
    QVERIFY(model.index(3, 0).data(FolderNotesModel::SystemFolderRole).toBool());
}

void FolderNotesModelTest::recyclingNoteUpdatesProjectionWithoutReset()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(makeCatalogStore(directory));
    QVERIFY(manager.initialize());

    FolderRecord inbox;
    inbox.name             = QStringLiteral("Inbox");
    const auto inboxResult = manager.addFolder(inbox);
    QVERIFY(inboxResult);

    auto  storage = std::make_unique<FolderModelStorage>(QStringLiteral("folder-model-recycle"));
    auto *raw     = storage.get();
    raw->notes    = { raw->makeNote(QStringLiteral("discarded"), QStringLiteral("Discarded"), inboxResult.value) };
    auto *notes   = NoteManager::instance();
    notes->registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([notes, raw]() {
        if (notes->storage(raw->systemName()) == raw)
            notes->unregisterStorage(raw);
    });
    QTRY_COMPARE(notes->notesIndex()->noteCount(raw->systemName()), 1);

    FolderNotesModel model(&manager);
    QCOMPARE(model.rowCount(), 3);
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy moveSpy(&model, &QAbstractItemModel::rowsMoved);

    QVERIFY(!manager.recycleNote(raw->systemName(), QStringLiteral("discarded"), inboxResult.value));
    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(insertSpy.count() >= 1);
    QVERIFY(moveSpy.count() >= 1);
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.index(2, 0).data(FolderNotesModel::TitleRole).toString(), QStringLiteral("Recycle Bin"));
    QCOMPARE(model.index(3, 0).data(FolderNotesModel::NoteIdRole).toString(), QStringLiteral("discarded"));
}

void FolderNotesModelTest::folderPickerIgnoresCollapsedAndArchivedBranches()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(makeCatalogStore(directory));
    QVERIFY(manager.initialize());

    FolderRecord projects;
    projects.name      = QStringLiteral("Projects");
    projects.collapsed = true;
    const auto root    = manager.addFolder(projects);
    QVERIFY(root);
    FolderRecord child;
    child.name        = QStringLiteral("QtNote");
    child.parentId    = root.value;
    const auto nested = manager.addFolder(child);
    QVERIFY(nested);
    FolderRecord archive;
    archive.name        = QStringLiteral("Archive");
    archive.archived    = true;
    const auto archived = manager.addFolder(archive);
    QVERIFY(archived);
    FolderRecord hiddenChild;
    hiddenChild.name     = QStringLiteral("Hidden child");
    hiddenChild.parentId = archived.value;
    QVERIFY(manager.addFolder(hiddenChild));

    FolderNotesModel model(&manager);
    const auto       visible = model.folderPickerItems();
    QCOMPARE(visible.size(), 2);
    QCOMPARE(visible.at(0).toMap().value(QStringLiteral("folderId")).toString(),
             root.value.toString(QUuid::WithoutBraces));
    QCOMPARE(visible.at(0).toMap().value(QStringLiteral("depth")).toInt(), 0);
    QCOMPARE(visible.at(1).toMap().value(QStringLiteral("folderId")).toString(),
             nested.value.toString(QUuid::WithoutBraces));
    QCOMPARE(visible.at(1).toMap().value(QStringLiteral("depth")).toInt(), 1);

    const auto all = model.folderPickerItems(true);
    QCOMPARE(all.size(), 4);
}

void FolderNotesModelTest::overlayTombstoneWinsOverProviderFolder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager manager(makeCatalogStore(directory));
    QVERIFY(manager.initialize());

    FolderRecord folder;
    folder.name             = QStringLiteral("Assigned");
    const auto folderResult = manager.addFolder(folder);
    QVERIFY(folderResult);

    auto  storage = std::make_unique<FolderModelStorage>(QStringLiteral("folder-tombstone"));
    auto *raw     = storage.get();
    raw->notes    = { raw->makeNote(QStringLiteral("note"), QStringLiteral("Provider folder"), folderResult.value) };
    auto *notes   = NoteManager::instance();
    notes->registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([notes, raw]() {
        if (notes->storage(raw->systemName()) == raw)
            notes->unregisterStorage(raw);
    });
    QTRY_COMPARE(notes->notesIndex()->noteCount(raw->systemName()), 1);

    FolderNotesModel model(&manager);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.index(1, 0).data(FolderNotesModel::NoteIdRole).toString(), QStringLiteral("note"));

    QVERIFY(!manager.clearNoteAssignment(raw->systemName(), QStringLiteral("note")));
    QTRY_COMPARE(model.rowCount(), 3);
    QCOMPARE(model.index(1, 0).data(FolderNotesModel::RowKindRole).toInt(), int(FolderNotesModel::UnsortedRow));
    QCOMPARE(model.index(2, 0).data(FolderNotesModel::NoteIdRole).toString(), QStringLiteral("note"));
}

QTEST_GUILESS_MAIN(FolderNotesModelTest)
#include "foldernotesmodel_test.moc"
