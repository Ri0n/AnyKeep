#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "foldernotesmodel.h"
#include "notedata.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notesworkspacecontroller.h"
#include "secureenvelope.h"

#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace QtNote;

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

void NotesWorkspaceFoldersTest::createsUnnamedFoldersForInlineRename()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager     catalog(makeCatalogStore(directory));
    NotesWorkspaceController workspace(&catalog, nullptr);
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

    NotesWorkspaceController workspace(&catalog, nullptr);
    const auto               projects = workspace.createFolder(QStringLiteral("Projects"));
    QVERIFY(!projects.isEmpty());
    const auto child = workspace.createFolder(QStringLiteral("QtNote"), projects);
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

    QVERIFY(workspace.collapseAllFolders());
    QVERIFY(catalog.catalog().folder(QUuid(projects))->collapsed);
    QVERIFY(catalog.catalog().folder(QUuid(child))->collapsed);
}

QTEST_MAIN(NotesWorkspaceFoldersTest)
#include "notesworkspacefolders_test.moc"
