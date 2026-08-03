#include "draftmanager.h"
#include "filedraftstore.h"
#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "foldernotesmodel.h"
#include "notedata.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notesmodel.h"
#include "notessearchmodel.h"
#include "notesworkspacecontroller.h"
#include "secureenvelope.h"

#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace AnyKeep;

class WorkspaceFolderStorage final : public NoteStorage {
    Q_OBJECT
public:
    explicit WorkspaceFolderStorage(QString id, QObject *parent = nullptr) : NoteStorage(parent), id_(std::move(id)) { }

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
        for (const auto &candidate : notes) {
            if (candidate.id() == id)
                return candidate;
        }
        return {};
    }
    Note createNote() override { return {}; }
    bool saveNote(const Note &) override { return false; }
    void removeNote(const QString &) override { }

    Note makeNote(const QString &id, const QString &title)
    {
        Note note(new NoteData(this));
        note.setId(id);
        note.setTitle(title);
        note.setText({}, Note::Markdown);
        note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        return note;
    }

    QList<Note> notes;

private:
    QString id_;
};

class NotesWorkspaceFoldersTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void createsUnnamedFoldersForInlineRename();
    void exposesFoldersAndMovesCleanEditorMetadata();
    void recycleBinHidesNotesUntilRestored();
    void deletesFolderBranchesWithSessionUndo();
    void recentReorderRejectsCrossStorageMove();
    void exposesBodySearchMatchesForEditorFind();
};

void NotesWorkspaceFoldersTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

static std::unique_ptr<FileFolderCatalogStore> makeCatalogStore(QTemporaryDir &directory)
{
    return std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")),
                                                    SecureEnvelope::generateMasterKey());
}

static std::unique_ptr<FileDraftStore> makeDraftStore(QTemporaryDir &directory)
{
    return std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("drafts")),
                                            SecureEnvelope::generateMasterKey());
}

void NotesWorkspaceFoldersTest::createsUnnamedFoldersForInlineRename()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager     catalog(makeCatalogStore(directory));
    DraftManager             drafts(makeDraftStore(directory));
    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);
    QVERIFY(catalog.initialize());

    const auto first  = workspace.createFolder({});
    const auto second = workspace.createFolder({});
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QCOMPARE(catalog.catalog().folder(QUuid(first))->name, QStringLiteral("New folder"));
    QCOMPARE(catalog.catalog().folder(QUuid(second))->name, QStringLiteral("New folder 2"));
}

void NotesWorkspaceFoldersTest::exposesFoldersAndMovesCleanEditorMetadata()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    DraftManager         drafts(makeDraftStore(directory));
    QVERIFY(catalog.initialize());

    auto storage   = std::make_unique<WorkspaceFolderStorage>(QStringLiteral("workspace-folders"));
    storage->notes = { storage->makeNote(QStringLiteral("note"), QStringLiteral("Workspace note")) };
    auto *raw      = storage.get();
    auto *manager  = NoteManager::instance();
    manager->registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([manager, raw]() {
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });
    QTRY_VERIFY(manager->notesIndex()->hasSnapshot(raw->systemName()));

    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);
    const auto               projects = workspace.createFolder(QStringLiteral("Projects"));
    QVERIFY(!projects.isEmpty());
    const auto child = workspace.createFolder(QStringLiteral("AnyKeep"), projects);
    QVERIFY(!child.isEmpty());
    const auto archive = workspace.createFolder(QStringLiteral("Archive"));
    QVERIFY(!archive.isEmpty());
    QVERIFY(workspace.moveFolderBefore(archive, {}, projects));
    const auto rootFolders = catalog.catalog().children();
    QCOMPARE(rootFolders.size(), 2);
    QCOMPARE(rootFolders.first().id, QUuid(archive));
    QCOMPARE(rootFolders.last().id, QUuid(projects));
    QCOMPARE(workspace.folderNotesModel()->rowCount(), 5);
    QCOMPARE(workspace.folderNotesModel()->index(0, 0).data(FolderNotesModel::TitleRole).toString(),
             QStringLiteral("Archive"));

    QVERIFY(workspace.openNote(raw->systemName(), QStringLiteral("note")));
    QTRY_VERIFY(workspace.currentEditor());
    QVERIFY(workspace.assignCurrentNoteFolder(child));
    QCOMPARE(workspace.currentFolderId(), child);
    QCOMPARE(workspace.folderIdForNote(raw->systemName(), QStringLiteral("note")), child);
    QVERIFY(!workspace.editor()->isDirty());
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("note")), QUuid(child));

    // Drag boundaries can transiently serialize an empty folder as a null
    // UUID. It must retain the same "Unsorted" meaning for the active editor.
    QVERIFY(workspace.assignNoteFolder(raw->systemName(), QStringLiteral("note"),
                                       QStringLiteral("00000000-0000-0000-0000-000000000000")));
    QCOMPARE(workspace.currentFolderId(), QString());
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("note")), QUuid {});

    QVERIFY(workspace.collapseAllFolders());
    QVERIFY(catalog.catalog().folder(QUuid(projects))->collapsed);
    QVERIFY(catalog.catalog().folder(QUuid(child))->collapsed);

    QVERIFY(workspace.setUnsortedCollapsed(true));
    const auto unsortedRow = workspace.folderNotesModel()->rowCount() - 1;
    QCOMPARE(workspace.folderNotesModel()->index(unsortedRow, 0).data(FolderNotesModel::RowKindRole).toInt(),
             int(FolderNotesModel::UnsortedRow));
    QVERIFY(workspace.folderNotesModel()->index(unsortedRow, 0).data(FolderNotesModel::CollapsedRole).toBool());
}

void NotesWorkspaceFoldersTest::recycleBinHidesNotesUntilRestored()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    DraftManager         drafts(makeDraftStore(directory));
    QVERIFY(catalog.initialize());

    auto storage   = std::make_unique<WorkspaceFolderStorage>(QStringLiteral("workspace-recycle"));
    storage->notes = { storage->makeNote(QStringLiteral("note"), QStringLiteral("Recyclable note")) };
    auto *raw      = storage.get();
    auto *manager  = NoteManager::instance();
    manager->registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([manager, raw] {
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });
    QTRY_VERIFY(manager->notesIndex()->hasSnapshot(raw->systemName()));

    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);
    QTRY_COMPARE(workspace.sourceModel()->rowCount(workspace.sourceModel()->index(0, 0)), 1);
    QVERIFY(workspace.trashNote(raw->systemName(), QStringLiteral("note")));
    QVERIFY(catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("note")));
    QTRY_COMPARE(workspace.sourceModel()->rowCount(workspace.sourceModel()->index(0, 0)), 0);
    QVERIFY(workspace.restoreRecycledNote(raw->systemName(), QStringLiteral("note")));
    QVERIFY(!catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("note")));
    QTRY_COMPARE(workspace.sourceModel()->rowCount(workspace.sourceModel()->index(0, 0)), 1);
}

void NotesWorkspaceFoldersTest::deletesFolderBranchesWithSessionUndo()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    DraftManager         drafts(makeDraftStore(directory));
    QVERIFY(catalog.initialize());

    auto storage   = std::make_unique<WorkspaceFolderStorage>(QStringLiteral("workspace-folder-delete"));
    storage->notes = {
        storage->makeNote(QStringLiteral("one"), QStringLiteral("Parent note")),
        storage->makeNote(QStringLiteral("two"), QStringLiteral("Child note")),
    };
    auto *raw     = storage.get();
    auto *manager = NoteManager::instance();
    manager->registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([manager, raw] {
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });
    QTRY_VERIFY(manager->notesIndex()->hasSnapshot(raw->systemName()));

    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);
    const auto               parent = workspace.createFolder(QStringLiteral("Projects"));
    const auto               child  = workspace.createFolder(QStringLiteral("AnyKeep"), parent);
    QVERIFY(!parent.isEmpty());
    QVERIFY(!child.isEmpty());
    QVERIFY(workspace.assignNoteFolder(raw->systemName(), QStringLiteral("one"), parent));
    QVERIFY(workspace.assignNoteFolder(raw->systemName(), QStringLiteral("two"), child));

    QVERIFY(workspace.trashFolder(parent));
    QVERIFY(workspace.canUndoFolderTrash());
    QCOMPARE(workspace.lastTrashedFolderName(), QStringLiteral("Projects"));
    QVERIFY(!catalog.catalog().folder(QUuid(parent)));
    QVERIFY(!catalog.catalog().folder(QUuid(child)));
    QVERIFY(catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("one")));
    QVERIFY(catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("two")));

    QVERIFY(workspace.undoFolderTrash());
    QVERIFY(!workspace.canUndoFolderTrash());
    QVERIFY(catalog.catalog().folder(QUuid(parent)));
    QVERIFY(catalog.catalog().folder(QUuid(child)));
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("one")), QUuid(parent));
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("two")), QUuid(child));
}

void NotesWorkspaceFoldersTest::exposesBodySearchMatchesForEditorFind()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    DraftManager         drafts(makeDraftStore(directory));
    QVERIFY(catalog.initialize());
    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);

    workspace.setSearchText(QStringLiteral("needle"));
    workspace.setSearchInBody(true);
    QVERIFY(!workspace.noteMatchesBodySearch(QStringLiteral("storage"), QStringLiteral("note")));
    QVERIFY(QMetaObject::invokeMethod(workspace.searchModel(), "noteFound", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("storage")),
                                      Q_ARG(QString, QStringLiteral("note"))));
    QVERIFY(workspace.noteMatchesBodySearch(QStringLiteral("storage"), QStringLiteral("note")));
    workspace.setSearchInBody(false);
    QVERIFY(!workspace.noteMatchesBodySearch(QStringLiteral("storage"), QStringLiteral("note")));
}

void NotesWorkspaceFoldersTest::recentReorderRejectsCrossStorageMove()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    DraftManager         drafts(makeDraftStore(directory));
    QVERIFY(catalog.initialize());
    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);

    const QVariantList notes {
        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("local") },
                      { QStringLiteral("noteId"), QStringLiteral("one") } },
        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("remote") },
                      { QStringLiteral("noteId"), QStringLiteral("two") } },
    };
    QVERIFY(!workspace.reorderRecentNotes(notes, QStringLiteral("local"), QStringLiteral("anchor"), false));
    QCOMPARE(workspace.errorString(), QStringLiteral("Recent notes can only be reordered within the same storage"));
}

QTEST_MAIN(NotesWorkspaceFoldersTest)
#include "notesworkspacefolders_test.moc"
