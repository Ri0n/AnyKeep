#include "draftmanager.h"
#include "draftstore.h"
#include "filefoldercatalogstore.h"
#include "filenoterulestore.h"
#include "foldercatalogmanager.h"
#include "notedata.h"
#include "notemanager.h"
#include "noteruleapplicationcontroller.h"
#include "noterulemanager.h"
#include "secureenvelope.h"
#include "storagejob.h"

#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace QtNote;

namespace {

class MemoryDraftStore final : public DraftStore {
public:
    DraftStoreError write(const DraftRecord &record) override
    {
        records_.insert(record.id, record);
        return {};
    }

    DraftStoreResult<DraftRecord> load(const QUuid &id) const override
    {
        const auto record = records_.constFind(id);
        if (record == records_.cend())
            return { {}, { DraftStoreError::NotFound, QStringLiteral("not found") } };
        return { record.value(), {} };
    }

    DraftStoreResult<QList<DraftRecord>> records() const override { return { records_.values(), {} }; }

    DraftStoreError transition(const QUuid &id, DraftRecord::State state) override
    {
        auto record = records_.find(id);
        if (record == records_.end())
            return { DraftStoreError::NotFound, QStringLiteral("not found") };
        record->state = state;
        return {};
    }

    DraftStoreError remove(const QUuid &id) override
    {
        return records_.remove(id) ? DraftStoreError {}
                                   : DraftStoreError { DraftStoreError::NotFound, QStringLiteral("not found") };
    }

    QHash<QUuid, DraftRecord> records_;
};

class RuleStorage final : public NoteStorage {
public:
    explicit RuleStorage(QString id, QObject *parent = nullptr) : NoteStorage(parent), id_(std::move(id)) { }

    bool                init() override { return true; }
    const QString       systemName() const override { return id_; }
    const QString       name() const override { return id_; }
    QIcon               storageIcon() const override { return {}; }
    QIcon               noteIcon() const override { return {}; }
    bool                isAccessible() const override { return true; }
    QList<Note::Format> availableFormats() const override { return { Note::Markdown }; }
    bool                supportsMedia() const override { return true; }
    bool                supportsFolderRuleOverlayImport() const override { return importsFolderRules_; }
    QList<Note>         noteList(int limit = 0) override { return limit > 0 ? notes_.mid(0, limit) : notes_; }
    Note                note(const QString &id) override
    {
        for (const auto &candidate : notes_) {
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

    bool saveNote(const Note &note) override
    {
        ++saveCalls_;
        auto saved = note;
        if (saved.id().isEmpty())
            saved.setId(QStringLiteral("%1-%2").arg(id_).arg(++nextId_));
        saved.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        for (auto &candidate : notes_) {
            if (candidate.id() != saved.id())
                continue;
            candidate = saved;
            emit noteModified(saved);
            return true;
        }
        notes_.append(saved);
        emit noteAdded(saved);
        return true;
    }

    void removeNote(const QString &id) override
    {
        for (qsizetype index = 0; index < notes_.size(); ++index) {
            if (notes_.at(index).id() != id)
                continue;
            emit noteRemoved(notes_.takeAt(index));
            return;
        }
    }

    Note add(const QString &id, const QString &title, const QString &text)
    {
        Note note(new NoteData(this));
        note.setId(id);
        note.setTitle(title);
        note.setText(text, Note::Markdown);
        note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        notes_.append(note);
        return note;
    }

    QList<Note> notes_;
    bool        importsFolderRules_ { false };
    int         saveCalls_ { 0 };

private:
    QString id_;
    int     nextId_ { 0 };
};

RuleStorage *registerStorage(std::unique_ptr<RuleStorage> storage)
{
    auto *raw = storage.get();
    NoteManager::instance()->registerStorage(std::move(storage));
    return raw;
}

FolderRecord folder(const QString &name)
{
    FolderRecord result;
    result.name = name;
    return result;
}

NoteRuleEvaluationInput inputFor(const DraftRecord &record)
{
    NoteRuleEvaluationInput input;
    input.storageId     = record.storageId;
    input.noteId        = record.remoteNoteId;
    input.title         = record.title;
    input.tags          = record.tags;
    input.text          = record.body;
    input.textAvailable = true;
    return input;
}

DraftRecord readyDraft(const Note &note)
{
    DraftRecord record;
    record.id           = QUuid::createUuid();
    record.operation    = DraftRecord::Publish;
    record.state        = DraftRecord::Ready;
    record.storageId    = note.storageId();
    record.remoteNoteId = note.id();
    record.title        = note.title();
    record.body         = note.text();
    record.format       = note.format();
    record.tags         = note.tags();
    record.folderId     = note.folderId();
    record.backendData  = note.backendData();
    record.media        = note.media();
    record.revision     = 1;
    record.updatedAt    = QDateTime::currentDateTimeUtc();
    return record;
}

NoteRule folderRule(const QUuid &folderId, const QString &pattern)
{
    NoteRule rule;
    rule.name       = QStringLiteral("Folder rule");
    rule.conditions = { { NoteRuleConditionKind::TitleMatches, pattern, false } };
    NoteRuleAction action;
    action.kind     = NoteRuleActionKind::AssignFolder;
    action.folderId = folderId;
    rule.actions    = { action };
    return rule;
}

NoteRule storageRule(const QString &destinationStorageId, const QString &pattern)
{
    NoteRule rule;
    rule.name       = QStringLiteral("Storage rule");
    rule.conditions = { { NoteRuleConditionKind::TitleMatches, pattern, false } };
    NoteRuleAction action;
    action.kind      = NoteRuleActionKind::SelectStorage;
    action.storageId = destinationStorageId;
    rule.actions     = { action };
    return rule;
}

NoteRule textFolderRule(const QUuid &folderId, const QString &text)
{
    NoteRule rule;
    rule.name       = QStringLiteral("Text folder rule");
    rule.conditions = { { NoteRuleConditionKind::TextContains, text, false } };
    NoteRuleAction action;
    action.kind     = NoteRuleActionKind::AssignFolder;
    action.folderId = folderId;
    rule.actions    = { action };
    return rule;
}

} // namespace

class NoteRuleApplicationControllerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void routesFolderBeforePublicationAndRecordsMarker();
    void routesExistingDraftAcrossStorages();
    void routesNewDraftBeforeItsFirstSave();
    void preservesExplicitFolderChoiceDuringPublication();
    void loadingANoteDoesNotRunRules();
    void importsFolderOnlyFromOptInStorage();
    void applicationOwnerMayDestroyDraftManagerFirst();
};

void NoteRuleApplicationControllerTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
    QVERIFY2(FileNoteRuleStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

void NoteRuleApplicationControllerTest::routesFolderBeforePublicationAndRecordsMarker()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());
    const auto inbox = folders.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY(inbox);
    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    const auto added = rules.addRule(folderRule(inbox.value, QStringLiteral("Mail*")));
    QVERIFY(added);

    auto       storage = std::make_unique<RuleStorage>(QStringLiteral("rule-folder"));
    const auto note = storage->add(QStringLiteral("note"), QStringLiteral("Mail from Alice"), QStringLiteral("Body"));
    auto      *raw  = registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    auto                          store    = std::make_unique<MemoryDraftStore>();
    auto                         *storeRaw = store.get();
    DraftManager                  drafts(std::move(store));
    NoteRuleApplicationController controller(&rules, &folders, NoteManager::instance(), &drafts);
    controller.initialize();
    const auto record = readyDraft(note);
    QVERIFY(!storeRaw->write(record));

    drafts.publishPending();

    QTRY_COMPARE(raw->note(note.id()).folderId(), inbox.value);
    QTRY_COMPARE(folders.catalog().folderForNote(raw->systemName(), note.id()), inbox.value);
    QTRY_VERIFY(rules.wasApplied(added.value, inputFor(record)));
}

void NoteRuleApplicationControllerTest::routesExistingDraftAcrossStorages()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());
    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    const auto added = rules.addRule(storageRule(QStringLiteral("rule-destination"), QStringLiteral("Invoice*")));
    QVERIFY(added);

    auto       sourceStorage = std::make_unique<RuleStorage>(QStringLiteral("rule-source"));
    const auto note = sourceStorage->add(QStringLiteral("note"), QStringLiteral("Invoice 1"), QStringLiteral("Body"));
    auto      *sourceRaw          = registerStorage(std::move(sourceStorage));
    auto       destinationStorage = std::make_unique<RuleStorage>(QStringLiteral("rule-destination"));
    auto      *destinationRaw     = registerStorage(std::move(destinationStorage));
    const auto cleanup            = qScopeGuard([sourceRaw, destinationRaw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(sourceRaw->systemName()) == sourceRaw)
            manager->unregisterStorage(sourceRaw);
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
    });

    auto                          store    = std::make_unique<MemoryDraftStore>();
    auto                         *storeRaw = store.get();
    DraftManager                  drafts(std::move(store));
    NoteRuleApplicationController controller(&rules, &folders, NoteManager::instance(), &drafts);
    controller.initialize();
    const auto record = readyDraft(note);
    QVERIFY(!storeRaw->write(record));

    drafts.publishPending();

    QTRY_COMPARE(destinationRaw->notes_.size(), 1);
    QTRY_VERIFY(sourceRaw->note(note.id()).isNull());
    QCOMPARE(destinationRaw->notes_.constFirst().title(), note.title());
    QTRY_VERIFY(rules.wasApplied(added.value, inputFor(record)));
}

void NoteRuleApplicationControllerTest::routesNewDraftBeforeItsFirstSave()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());
    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    QVERIFY(rules.addRule(storageRule(QStringLiteral("rule-new-destination"), QStringLiteral("Invoice*"))));

    auto       sourceStorage      = std::make_unique<RuleStorage>(QStringLiteral("rule-new-source"));
    auto      *sourceRaw          = registerStorage(std::move(sourceStorage));
    auto       destinationStorage = std::make_unique<RuleStorage>(QStringLiteral("rule-new-destination"));
    auto      *destinationRaw     = registerStorage(std::move(destinationStorage));
    const auto cleanup            = qScopeGuard([sourceRaw, destinationRaw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(sourceRaw->systemName()) == sourceRaw)
            manager->unregisterStorage(sourceRaw);
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
    });

    auto                          store    = std::make_unique<MemoryDraftStore>();
    auto                         *storeRaw = store.get();
    DraftManager                  drafts(std::move(store));
    NoteRuleApplicationController controller(&rules, &folders, NoteManager::instance(), &drafts);
    controller.initialize();

    DraftRecord record;
    record.id        = QUuid::createUuid();
    record.operation = DraftRecord::Publish;
    record.state     = DraftRecord::Ready;
    record.storageId = sourceRaw->systemName();
    record.title     = QStringLiteral("Invoice draft");
    record.body      = QStringLiteral("Body");
    record.format    = Note::Markdown;
    record.revision  = 1;
    record.updatedAt = QDateTime::currentDateTimeUtc();
    QVERIFY(!storeRaw->write(record));

    drafts.publishPending();

    QTRY_COMPARE(destinationRaw->notes_.size(), 1);
    QCOMPARE(sourceRaw->notes_.size(), 0);
    QCOMPARE(destinationRaw->notes_.constFirst().title(), record.title);
}

void NoteRuleApplicationControllerTest::preservesExplicitFolderChoiceDuringPublication()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());
    const auto automaticFolder = folders.addFolder(folder(QStringLiteral("Automatic")));
    const auto explicitFolder  = folders.addFolder(folder(QStringLiteral("Explicit")));
    QVERIFY(automaticFolder);
    QVERIFY(explicitFolder);
    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    QVERIFY(rules.addRule(folderRule(automaticFolder.value, QStringLiteral("Mail*"))));

    auto storage = std::make_unique<RuleStorage>(QStringLiteral("rule-explicit-folder"));
    auto note    = storage->add(QStringLiteral("note"), QStringLiteral("Mail from Alice"), QStringLiteral("Body"));
    note.setFolderId(explicitFolder.value);
    QVERIFY(storage->saveNote(note));
    auto      *raw     = registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    auto                          store    = std::make_unique<MemoryDraftStore>();
    auto                         *storeRaw = store.get();
    DraftManager                  drafts(std::move(store));
    NoteRuleApplicationController controller(&rules, &folders, NoteManager::instance(), &drafts);
    controller.initialize();
    auto record               = readyDraft(note);
    record.folderUserOverride = true;
    QVERIFY(!storeRaw->write(record));

    drafts.publishPending();

    QTRY_COMPARE(raw->note(note.id()).folderId(), explicitFolder.value);
    QTRY_COMPARE(folders.catalog().folderForNote(raw->systemName(), note.id()), explicitFolder.value);
}

void NoteRuleApplicationControllerTest::loadingANoteDoesNotRunRules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());
    const auto inbox = folders.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY(inbox);
    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    QVERIFY(rules.addRule(folderRule(inbox.value, QStringLiteral("Network*"))));

    auto       storage = std::make_unique<RuleStorage>(QStringLiteral("rule-load-only"));
    const auto note    = storage->add(QStringLiteral("note"), QStringLiteral("Network note"), QStringLiteral("Body"));
    auto      *raw     = registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    auto                          store = std::make_unique<MemoryDraftStore>();
    DraftManager                  drafts(std::move(store));
    NoteRuleApplicationController controller(&rules, &folders, NoteManager::instance(), &drafts);
    controller.initialize();

    auto *job = NoteManager::instance()->loadNoteAsync(raw->systemName(), note.id(), &controller);
    QTRY_COMPARE(job->state(), StorageJob::Succeeded);
    QTest::qWait(20);

    QVERIFY(folders.catalog().folderForNote(raw->systemName(), note.id()).isNull());
    QVERIFY(!rules.wasApplied(rules.rules().constFirst().id, inputFor(readyDraft(note))));
}

void NoteRuleApplicationControllerTest::importsFolderOnlyFromOptInStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto           key = SecureEnvelope::generateMasterKey();
    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());
    const auto imported = folders.addFolder(folder(QStringLiteral("Imported")));
    QVERIFY(imported);
    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    QVERIFY(rules.addRule(textFolderRule(imported.value, QStringLiteral("classified"))));
    QVERIFY(rules.addRule(storageRule(QStringLiteral("rule-overlay-destination"), QStringLiteral("*"))));

    auto sourceStorage                 = std::make_unique<RuleStorage>(QStringLiteral("rule-overlay-source"));
    sourceStorage->importsFolderRules_ = true;
    const auto note                    = sourceStorage->add(QStringLiteral("note"), QStringLiteral("Imported note"),
                                                            QStringLiteral("classified body"));
    auto       destinationStorage      = std::make_unique<RuleStorage>(QStringLiteral("rule-overlay-destination"));

    auto                          store = std::make_unique<MemoryDraftStore>();
    DraftManager                  drafts(std::move(store));
    NoteRuleApplicationController controller(&rules, &folders, NoteManager::instance(), &drafts);
    controller.initialize();

    auto      *sourceRaw      = registerStorage(std::move(sourceStorage));
    auto      *destinationRaw = registerStorage(std::move(destinationStorage));
    const auto cleanup        = qScopeGuard([sourceRaw, destinationRaw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(sourceRaw->systemName()) == sourceRaw)
            manager->unregisterStorage(sourceRaw);
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
    });

    QTRY_COMPARE(folders.catalog().folderForNote(sourceRaw->systemName(), note.id()), imported.value);
    QCOMPARE(sourceRaw->saveCalls_, 0);
    QCOMPARE(destinationRaw->notes_.size(), 0);
}

void NoteRuleApplicationControllerTest::applicationOwnerMayDestroyDraftManagerFirst()
{
    QObject owner;
    auto   *drafts = new DraftManager(std::make_unique<MemoryDraftStore>(), &owner);
    new NoteRuleApplicationController(nullptr, nullptr, nullptr, drafts, &owner);
}

QTEST_GUILESS_MAIN(NoteRuleApplicationControllerTest)
#include "noteruleapplicationcontroller_test.moc"
