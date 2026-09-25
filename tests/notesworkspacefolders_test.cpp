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
    explicit WorkspaceFolderStorage(QString id, QObject *parent = nullptr) : NoteStorage(parent), id_(std::move(id)) {}

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
    Note createNote() override
    {
        Note note(new NoteData(this));
        note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        return note;
    }
    bool saveNote(const Note &) override { return true; }
    void removeNote(const QString &) override {}

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
    void sharesLiveModelAcrossViewsAndRetargetsMove();
    void opensDurableDraftWithoutTargetStorage();
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
    QTRY_COMPARE(workspace.recentNotesModel()->rowCount(), 1);
    QVERIFY(workspace.trashNote(raw->systemName(), QStringLiteral("note")));
    QVERIFY(workspace.canUndoTrash());
    QCOMPARE(workspace.lastTrashedItemName(), QStringLiteral("Recyclable note"));
    QVERIFY(catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("note")));
    QTRY_COMPARE(workspace.sourceModel()->rowCount(workspace.sourceModel()->index(0, 0)), 0);
    QTRY_COMPARE(workspace.recentNotesModel()->rowCount(), 0);
    QVERIFY(workspace.undoTrash());
    QVERIFY(!workspace.canUndoTrash());
    QVERIFY(!catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("note")));
    QTRY_COMPARE(workspace.sourceModel()->rowCount(workspace.sourceModel()->index(0, 0)), 1);
    QTRY_COMPARE(workspace.recentNotesModel()->rowCount(), 1);

    QVERIFY(workspace.trashNote(raw->systemName(), QStringLiteral("note")));
    QVERIFY(workspace.canUndoTrash());
    QVERIFY(workspace.restoreRecycledNote(raw->systemName(), QStringLiteral("note")));
    QVERIFY(!workspace.canUndoTrash());
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

    QVERIFY(workspace.trashNote(raw->systemName(), QStringLiteral("one")));
    QCOMPARE(workspace.lastTrashedItemName(), QStringLiteral("Parent note"));
    QVERIFY(workspace.trashFolder(parent));
    QVERIFY(workspace.canUndoTrash());
    QCOMPARE(workspace.lastTrashedItemName(), QStringLiteral("Projects"));
    QVERIFY(workspace.canUndoFolderTrash());
    QCOMPARE(workspace.lastTrashedFolderName(), QStringLiteral("Projects"));
    QVERIFY(!catalog.catalog().folder(QUuid(parent)));
    QVERIFY(!catalog.catalog().folder(QUuid(child)));
    QVERIFY(catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("one")));
    QVERIFY(catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("two")));

    QVERIFY(workspace.undoTrash());
    QVERIFY(workspace.canUndoTrash());
    QVERIFY(!workspace.canUndoFolderTrash());
    QVERIFY(catalog.catalog().folder(QUuid(parent)));
    QVERIFY(catalog.catalog().folder(QUuid(child)));
    QVERIFY(catalog.catalog().isRecycled(raw->systemName(), QStringLiteral("one")));
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("two")), QUuid(child));

    QVERIFY(workspace.undoTrash());
    QVERIFY(!workspace.canUndoTrash());
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("one")), QUuid(parent));
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

void NotesWorkspaceFoldersTest::sharesLiveModelAcrossViewsAndRetargetsMove()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    DraftManager         drafts(makeDraftStore(directory));
    QVERIFY(catalog.initialize());

    auto source = std::make_unique<WorkspaceFolderStorage>(QStringLiteral("workspace-live-editor"));
    const auto note     = source->makeNote(QStringLiteral("note"), QStringLiteral("Shared note"));
    const auto removed  = source->makeNote(QStringLiteral("removed"), QStringLiteral("Delete me"));
    const auto recycled = source->makeNote(QStringLiteral("recycled"), QStringLiteral("Recycle me"));
    source->notes = { note, removed, recycled };
    auto *sourceRaw = source.get();

    auto destination = std::make_unique<WorkspaceFolderStorage>(QStringLiteral("workspace-destination"));
    auto *destinationRaw = destination.get();

    auto *manager = NoteManager::instance();
    manager->registerStorage(std::move(source));
    manager->registerStorage(std::move(destination));
    const auto cleanup = qScopeGuard([manager, sourceRaw, destinationRaw]() {
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
        if (manager->storage(sourceRaw->systemName()) == sourceRaw)
            manager->unregisterStorage(sourceRaw);
    });
    QTRY_VERIFY(manager->notesIndex()->hasSnapshot(sourceRaw->systemName()));

    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);
    QVERIFY(workspace.openNote(sourceRaw->systemName(), note.id()));
    QTRY_VERIFY(workspace.editor());

    auto *shared     = workspace.editor();
    auto *standalone = drafts.acquireEditor(note);
    QVERIFY(standalone);
    QCOMPARE(shared, standalone);
    QCOMPARE(shared->viewLeaseCount(), 2);
    QCOMPARE(drafts.editingSessionCount(shared->draftId()), 2);

    shared->setText(QStringLiteral("Shared note\n\nChanged through manager"));
    QCOMPARE(standalone->text(), QStringLiteral("Shared note\n\nChanged through manager"));

    const auto draftId = shared->draftId();
    QVERIFY(workspace.moveNote(sourceRaw->systemName(), note.id(), destinationRaw->systemName()));
    QCOMPARE(workspace.editor(), shared);
    QCOMPARE(shared->draftId(), draftId);
    QCOMPARE(shared->storageId(), destinationRaw->systemName());
    QVERIFY(shared->noteId().isEmpty());
    QCOMPARE(shared->viewLeaseCount(), 2);
    QCOMPARE(drafts.editingSessionCount(draftId), 2);

    const auto moved = drafts.pendingDraft(draftId);
    QVERIFY2(moved, qPrintable(moved.error.message));
    QCOMPARE(moved.value.state, DraftRecord::Editing);
    QCOMPARE(moved.value.storageId, destinationRaw->systemName());
    QVERIFY(moved.value.remoteNoteId.isEmpty());
    QCOMPARE(moved.value.removeSourceStorageId, sourceRaw->systemName());
    QCOMPARE(moved.value.removeSourceNoteId, note.id());

    // Closing one host releases only its lease. The same live model and draft
    // remain active for the standalone view.
    QVERIFY(workspace.closeCurrentNote());
    QVERIFY(!workspace.editor());
    QCOMPARE(standalone->draftId(), draftId);
    QCOMPARE(standalone->viewLeaseCount(), 1);
    QCOMPARE(drafts.editingSessionCount(draftId), 1);
    QCOMPARE(drafts.liveEditorForDraft(draftId), standalone);

    // Explicit deletion is different from move: it owns the lifecycle and
    // closes every view for the logical note before removing the source.
    QVERIFY(workspace.openNote(sourceRaw->systemName(), removed.id()));
    QTRY_VERIFY(workspace.editor());
    auto *deleteShared = workspace.editor();
    QCOMPARE(drafts.acquireEditor(removed), deleteShared);
    QCOMPARE(deleteShared->viewLeaseCount(), 2);
    QVERIFY(workspace.deleteNote(sourceRaw->systemName(), removed.id()));
    QVERIFY(!workspace.editor());
    QCOMPARE(drafts.editingSessionCountForNote(sourceRaw->systemName(), removed.id()), 0);

    QVERIFY(workspace.openNote(sourceRaw->systemName(), recycled.id()));
    QTRY_VERIFY(workspace.editor());
    auto *recycleShared = workspace.editor();
    QCOMPARE(drafts.acquireEditor(recycled), recycleShared);
    QCOMPARE(recycleShared->viewLeaseCount(), 2);
    QVERIFY(workspace.trashNote(sourceRaw->systemName(), recycled.id()));
    QVERIFY(!workspace.editor());
    QCOMPARE(drafts.editingSessionCountForNote(sourceRaw->systemName(), recycled.id()), 0);
    QVERIFY(catalog.catalog().isRecycled(sourceRaw->systemName(), recycled.id()));

    // Retargeting did not force publication/closure when the manager view
    // disappeared. Release the final test lease before unregistering storages.
    QCOMPARE(standalone->storageId(), destinationRaw->systemName());
    QPointer<NoteEditor> movedEditor(standalone);
    QVERIFY(standalone->close());
    QVERIFY(!drafts.liveEditorForDraft(draftId));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(movedEditor.isNull());
}

void NotesWorkspaceFoldersTest::opensDurableDraftWithoutTargetStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    DraftManager         drafts(makeDraftStore(directory));
    QVERIFY(catalog.initialize());

    auto storage = std::make_unique<WorkspaceFolderStorage>(QStringLiteral("offline-draft-target"));
    auto note    = storage->makeNote(QStringLiteral("remote-note"), QStringLiteral("Original"));
    auto *raw    = storage.get();
    auto *manager = NoteManager::instance();
    manager->registerStorage(std::move(storage));

    const auto draftId = drafts.acquireEditingSession(note);
    QVERIFY(!draftId.isNull());
    const auto saved = drafts.saveEditing(draftId, note, QStringLiteral("Recovered"),
                                          QStringLiteral("Durable body"), Note::Markdown);
    QVERIFY2(!saved, qPrintable(saved.message));
    QVERIFY(drafts.releaseEditingSession(draftId));

    manager->unregisterStorage(raw);
    QVERIFY(!manager->storage(QStringLiteral("offline-draft-target")));

    NotesWorkspaceController workspace(&catalog, &drafts, nullptr);
    QVERIFY(workspace.openNote(DraftManager::draftsStorageId(), draftId.toString(QUuid::WithoutBraces)));
    QTRY_VERIFY(workspace.editor());
    QCOMPARE(workspace.editor()->draftId(), draftId);
    QCOMPARE(workspace.editor()->text(), QStringLiteral("Recovered\n\nDurable body"));
    QVERIFY(workspace.editor()->storageId().isEmpty());
    QVERIFY(workspace.editor()->noteId() == QStringLiteral("remote-note"));
}

QTEST_MAIN(NotesWorkspaceFoldersTest)
#include "notesworkspacefolders_test.moc"
