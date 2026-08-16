#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "folderoperationscontroller.h"
#include "notedata.h"
#include "notemanager.h"
#include "notesindex.h"
#include "secureenvelope.h"

#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <memory>

using namespace AnyKeep;

class FolderOperationStorage final : public NoteStorage {
    Q_OBJECT
public:
    explicit FolderOperationStorage(QString id, bool nativeFolders, bool nativeCatalog, QObject *parent = nullptr) :
        NoteStorage(parent), id_(std::move(id)), nativeFolders_(nativeFolders), nativeCatalog_(nativeCatalog)
    {
    }

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
    void removeNote(const QString &) override {}

    bool                  supportsNativeFolders() const override { return nativeFolders_; }
    bool                  supportsNativeFolderCatalog() const override { return nativeCatalog_; }
    FolderCatalogSnapshot nativeFolderCatalog() const override { return catalog_; }
    NoteFolderChangeJob  *changeNoteFolderAsync(const Note &note, QObject *owner = nullptr) override
    {
        ++changeCalls;
        lastChangedNote = note;
        auto *job       = new NoteFolderChangeJob(owner ? owner : this);
        job->start();
        if (failChange) {
            job->fail({ StorageError::Network, QStringLiteral("Native folder request failed"), true });
            return job;
        }
        emit noteModified(note);
        job->complete(note);
        return job;
    }
    FolderCatalogJob *replaceNativeFolderCatalogAsync(const FolderCatalogSnapshot &snapshot,
                                                      QObject                     *owner = nullptr) override
    {
        ++replaceCalls;
        lastReplacedCatalog = snapshot;
        auto *job           = new FolderCatalogJob(owner ? owner : this);
        job->start();
        if (failReplace) {
            job->fail({ StorageError::Io, QStringLiteral("Native tree sync failed"), true });
            return job;
        }
        catalog_ = snapshot;
        job->complete();
        return job;
    }

    Note makeNote(const QString &id, const QString &title)
    {
        Note note(new NoteData(this));
        note.setId(id);
        note.setTitle(title);
        note.setText({}, Note::Markdown);
        note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        return note;
    }

    QList<Note>           notes;
    FolderCatalogSnapshot catalog_;
    FolderCatalogSnapshot lastReplacedCatalog;
    Note                  lastChangedNote;
    int                   replaceCalls { 0 };
    int                   changeCalls { 0 };
    bool                  failReplace { false };
    bool                  failChange { false };

private:
    QString id_;
    bool    nativeFolders_ { false };
    bool    nativeCatalog_ { false };
};

class FolderOperationsControllerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void storesOverlayForUnsupportedStorage();
    void preparesNativeTreeBeforeMetadataOnlyMove();
    void propagatesFolderTombstonesOverStaleNativeAssignments();
    void retainsOverlayWhenNativeMoveFails();
};

void FolderOperationsControllerTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

static std::unique_ptr<FileFolderCatalogStore> makeCatalogStore(QTemporaryDir &directory)
{
    return std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")),
                                                    SecureEnvelope::generateMasterKey());
}

static QUuid addFolder(FolderCatalogManager &manager)
{
    FolderRecord folder;
    folder.name       = QStringLiteral("Destination");
    const auto result = manager.addFolder(folder);
    Q_ASSERT(result);
    return result.value;
}

static FolderOperationStorage *registerStorage(std::unique_ptr<FolderOperationStorage> storage)
{
    auto *raw = storage.get();
    NoteManager::instance()->registerStorage(std::move(storage));
    return raw;
}

void FolderOperationsControllerTest::storesOverlayForUnsupportedStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    QVERIFY(catalog.initialize());
    const auto folder = addFolder(catalog);

    auto storage   = std::make_unique<FolderOperationStorage>(QStringLiteral("folder-overlay"), false, false);
    storage->notes = { storage->makeNote(QStringLiteral("note"), QStringLiteral("Overlay")) };
    auto *raw      = registerStorage(std::move(storage));
    QTRY_VERIFY(NoteManager::instance()->notesIndex()->hasSnapshot(raw->systemName()));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    FolderOperationsController controller(&catalog, NoteManager::instance());
    QSignalSpy                 finished(&controller, &FolderOperationsController::assignmentFinished);
    QVERIFY(controller.assignNoteFolder(raw->systemName(), QStringLiteral("note"), folder));
    QCOMPARE(raw->replaceCalls, 0);
    QCOMPARE(raw->changeCalls, 0);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("note")), folder);
}

void FolderOperationsControllerTest::preparesNativeTreeBeforeMetadataOnlyMove()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    QVERIFY(catalog.initialize());
    const auto folder = addFolder(catalog);

    auto storage   = std::make_unique<FolderOperationStorage>(QStringLiteral("folder-native"), true, true);
    storage->notes = { storage->makeNote(QStringLiteral("note"), QStringLiteral("Native")) };
    auto *raw      = registerStorage(std::move(storage));
    QTRY_VERIFY(NoteManager::instance()->notesIndex()->hasSnapshot(raw->systemName()));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    FolderOperationsController controller(&catalog, NoteManager::instance());
    QSignalSpy                 finished(&controller, &FolderOperationsController::assignmentFinished);
    QVERIFY(controller.assignNoteFolder(raw->systemName(), QStringLiteral("note"), folder));
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(raw->replaceCalls, 1);
    QCOMPARE(raw->changeCalls, 1);
    QCOMPARE(raw->lastChangedNote.folderId(), folder);
    QCOMPARE(raw->lastReplacedCatalog.folders.size(), 1);
    QCOMPARE(raw->lastReplacedCatalog.folders.first().id, folder);
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("note")), folder);
    QVERIFY(finished.constFirst().at(3).toBool());
}

void FolderOperationsControllerTest::propagatesFolderTombstonesOverStaleNativeAssignments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    QVERIFY(catalog.initialize());
    const auto folder      = addFolder(catalog);
    const auto savedFolder = *catalog.catalog().folder(folder);
    const auto deleted     = catalog.trashFolderBranch(folder);
    QVERIFY2(deleted, qPrintable(deleted.error.message));

    auto storage   = std::make_unique<FolderOperationStorage>(QStringLiteral("folder-native-delete"), true, true);
    storage->notes = { storage->makeNote(QStringLiteral("note"), QStringLiteral("Native deleted folder")) };
    storage->notes.first().setFolderId(folder);
    storage->catalog_.folders = { savedFolder };
    NoteFolderAssignment staleAssignment;
    staleAssignment.storageId     = storage->systemName();
    staleAssignment.noteId        = QStringLiteral("note");
    staleAssignment.folderId      = folder;
    staleAssignment.revision      = 1;
    staleAssignment.modifiedAt    = savedFolder.modifiedAt;
    storage->catalog_.assignments = { staleAssignment };

    auto *raw = registerStorage(std::move(storage));
    QTRY_VERIFY(NoteManager::instance()->notesIndex()->hasSnapshot(raw->systemName()));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    FolderOperationsController controller(&catalog, NoteManager::instance());
    QSignalSpy                 prepared(&controller, &FolderOperationsController::nativeTreePrepared);
    QVERIFY(controller.prepareNativeFolderTree(raw->systemName()));
    QTRY_COMPARE(prepared.count(), 1);
    QCOMPARE(raw->replaceCalls, 1);

    const auto tombstone
        = std::find_if(raw->lastReplacedCatalog.folders.cbegin(), raw->lastReplacedCatalog.folders.cend(),
                       [folder](const FolderRecord &record) { return record.id == folder; });
    QVERIFY(tombstone != raw->lastReplacedCatalog.folders.cend());
    QVERIFY(tombstone->tombstone);
    QVERIFY(tombstone->name.isEmpty());
    QVERIFY(raw->lastReplacedCatalog.assignments.isEmpty());
    QVERIFY(prepared.constFirst().at(1).toBool());
}

void FolderOperationsControllerTest::retainsOverlayWhenNativeMoveFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(makeCatalogStore(directory));
    QVERIFY(catalog.initialize());
    const auto folder = addFolder(catalog);

    auto storage        = std::make_unique<FolderOperationStorage>(QStringLiteral("folder-failure"), true, false);
    storage->notes      = { storage->makeNote(QStringLiteral("note"), QStringLiteral("Failure")) };
    storage->failChange = true;
    auto *raw           = registerStorage(std::move(storage));
    QTRY_VERIFY(NoteManager::instance()->notesIndex()->hasSnapshot(raw->systemName()));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    FolderOperationsController controller(&catalog, NoteManager::instance());
    QSignalSpy                 finished(&controller, &FolderOperationsController::assignmentFinished);
    QTest::ignoreMessage(
        QtWarningMsg, QRegularExpression(QStringLiteral(".*Folder operation failed.*Native folder request failed.*")));
    QVERIFY(controller.assignNoteFolder(raw->systemName(), QStringLiteral("note"), folder));
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(raw->changeCalls, 1);
    QVERIFY(!finished.constFirst().at(3).toBool());
    QCOMPARE(catalog.catalog().folderForNote(raw->systemName(), QStringLiteral("note")), folder);
    QCOMPARE(controller.errorString(), QStringLiteral("Native folder request failed"));
}

QTEST_GUILESS_MAIN(FolderOperationsControllerTest)
#include "folderoperationscontroller_test.moc"
