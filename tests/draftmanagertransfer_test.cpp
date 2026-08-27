#include "draftmanager.h"
#include "draftstore.h"
#include "notedata.h"
#include "noteeditor.h"
#include "notemanager.h"

#include <QScopeGuard>
#include <QSignalSpy>
#include <QtTest>

#include <algorithm>
#include <memory>

using namespace AnyKeep;

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

class TransferStorage final : public NoteStorage {
    Q_OBJECT
public:
    explicit TransferStorage(QString id, QObject *parent = nullptr) : NoteStorage(parent), id_(std::move(id)) {}

    bool                init() override { return true; }
    const QString       systemName() const override { return id_; }
    const QString       name() const override { return id_; }
    QIcon               storageIcon() const override { return {}; }
    QIcon               noteIcon() const override { return {}; }
    bool                isAccessible() const override { return true; }
    QList<Note::Format> availableFormats() const override { return formats_; }
    bool                supportsMedia() const override { return supportsMedia_; }
    bool                supportsFavorite() const override { return supportsFavorite_; }
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
        Note result(new NoteData(this));
        result.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        return result;
    }

    bool saveNote(const Note &note) override
    {
        if (failSaves_)
            return false;
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
            const auto removed = notes_.takeAt(index);
            ++removeCalls_;
            emit noteRemoved(removed);
            return;
        }
    }

    Note addStored(const QString &id, const QString &title, const QString &body)
    {
        Note result(new NoteData(this));
        result.setId(id);
        result.setTitle(title);
        result.setText(body, Note::Markdown);
        result.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        notes_.append(result);
        return result;
    }

    QList<Note::Format> formats_ { Note::Markdown, Note::PlainText };
    QList<Note>         notes_;
    bool                supportsMedia_ { true };
    bool                supportsFavorite_ { false };
    bool                failSaves_ { false };
    int                 saveCalls_ { 0 };
    int                 removeCalls_ { 0 };

private:
    QString id_;
    int     nextId_ { 0 };
};

TransferStorage *registerStorage(std::unique_ptr<TransferStorage> storage)
{
    auto *raw = storage.get();
    NoteManager::instance()->registerStorage(std::move(storage));
    return raw;
}

} // namespace

class DraftManagerTransferTest : public QObject {
    Q_OBJECT

private slots:
    void publishesDestinationBeforeDeletingSource();
    void preservesSourceWhenDestinationPublicationFails();
    void resumesPublishingDraftForEditing();
    void excludesActiveEditingSessionFromCrashRecovery();
    void prepareForShutdownRequeuesPublishingDraft();
    void exposesPendingPublicationDrafts();
    void publishesFavoriteOnlyChangesForMultipleNotesAndAllowsRemoval();
    void retargetsPublishedDraftWithoutLosingSourceIdentity();
    void movesUnpublishedDraftWithoutCreatingSourceRemoval();
};

void DraftManagerTransferTest::publishesFavoriteOnlyChangesForMultipleNotesAndAllowsRemoval()
{
    auto storage               = std::make_unique<TransferStorage>(QStringLiteral("favorite-publication"));
    storage->supportsFavorite_ = true;
    const auto first
        = storage->addStored(QStringLiteral("favorite-note-1"), QStringLiteral("First"), QStringLiteral("First body"));
    const auto second  = storage->addStored(QStringLiteral("favorite-note-2"), QStringLiteral("Second"),
                                            QStringLiteral("Second body"));
    auto      *raw     = registerStorage(std::move(storage));
    const auto cleanup = qScopeGuard([raw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(raw->systemName()) == raw)
            manager->unregisterStorage(raw);
    });

    DraftManager drafts(std::make_unique<MemoryDraftStore>());

    {
        NoteEditor editor(first, drafts);
        editor.setFavorite(true);
        QVERIFY(editor.isDirty());
        QVERIFY(editor.close());
    }
    QTRY_COMPARE(raw->saveCalls_, 1);
    QTRY_VERIFY(raw->note(first.id()).isFavorite());

    {
        NoteEditor editor(second, drafts);
        editor.setFavorite(true);
        QVERIFY(editor.isDirty());
        QVERIFY(editor.close());
    }
    QTRY_COMPARE(raw->saveCalls_, 2);
    QTRY_VERIFY(raw->note(first.id()).isFavorite());
    QTRY_VERIFY(raw->note(second.id()).isFavorite());

    {
        NoteEditor editor(raw->note(first.id()), drafts);
        editor.setFavorite(false);
        QVERIFY(editor.isDirty());
        QVERIFY(editor.close());
    }
    QTRY_COMPARE(raw->saveCalls_, 3);
    QTRY_VERIFY(!raw->note(first.id()).isFavorite());
    QTRY_VERIFY(raw->note(second.id()).isFavorite());
}

void DraftManagerTransferTest::publishesDestinationBeforeDeletingSource()
{
    auto       sourceStorage = std::make_unique<TransferStorage>(QStringLiteral("transfer-source"));
    const auto source
        = sourceStorage->addStored(QStringLiteral("source-note"), QStringLiteral("Source"), QStringLiteral("Body"));
    auto      *sourceRaw          = registerStorage(std::move(sourceStorage));
    auto       destinationStorage = std::make_unique<TransferStorage>(QStringLiteral("transfer-destination"));
    auto      *destinationRaw     = registerStorage(std::move(destinationStorage));
    const auto cleanup            = qScopeGuard([sourceRaw, destinationRaw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(sourceRaw->systemName()) == sourceRaw)
            manager->unregisterStorage(sourceRaw);
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
    });

    auto         store = std::make_unique<MemoryDraftStore>();
    auto        *data  = store.get();
    DraftManager drafts(std::move(store));
    QSignalSpy   published(&drafts, &DraftManager::draftPublished);
    const auto   folder = QUuid::createUuid();
    QUuid        draftId;
    const auto   error = drafts.stageTransfer(source, destinationRaw->systemName(), folder, &draftId);
    QVERIFY2(!error, qPrintable(error.message));
    QVERIFY(!draftId.isNull());
    QVERIFY(drafts.hasPendingTransferFrom(sourceRaw->systemName(), source.id()));

    const auto staged = data->records_.value(draftId);
    QCOMPARE(staged.removeSourceStorageId, sourceRaw->systemName());
    QCOMPARE(staged.removeSourceNoteId, source.id());
    QCOMPARE(staged.folderId, folder);

    QTRY_COMPARE(published.count(), 1);
    QCOMPARE(destinationRaw->notes_.size(), 1);
    QCOMPARE(destinationRaw->notes_.constFirst().title(), source.title());
    QCOMPARE(destinationRaw->notes_.constFirst().text(), source.text());
    QCOMPARE(destinationRaw->notes_.constFirst().folderId(), folder);
    QTRY_VERIFY(sourceRaw->note(source.id()).isNull());
    QTRY_COMPARE(sourceRaw->removeCalls_, 1);
    QTRY_VERIFY(data->records_.isEmpty());
}

void DraftManagerTransferTest::preservesSourceWhenDestinationPublicationFails()
{
    auto       sourceStorage = std::make_unique<TransferStorage>(QStringLiteral("transfer-failure-source"));
    const auto source
        = sourceStorage->addStored(QStringLiteral("source-note"), QStringLiteral("Source"), QStringLiteral("Body"));
    auto *sourceRaw                = registerStorage(std::move(sourceStorage));
    auto  destinationStorage       = std::make_unique<TransferStorage>(QStringLiteral("transfer-failure-destination"));
    destinationStorage->failSaves_ = true;
    auto      *destinationRaw      = registerStorage(std::move(destinationStorage));
    const auto cleanup             = qScopeGuard([sourceRaw, destinationRaw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(sourceRaw->systemName()) == sourceRaw)
            manager->unregisterStorage(sourceRaw);
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
    });

    auto         store = std::make_unique<MemoryDraftStore>();
    auto        *data  = store.get();
    DraftManager drafts(std::move(store));
    QUuid        draftId;
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(".*Draft publication job failed.*transfer-failure-destination.*")));
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(".*Draft publication retry/failure.*transfer-failure-destination.*")));
    const auto error = drafts.stageTransfer(source, destinationRaw->systemName(), {}, &draftId);
    QVERIFY2(!error, qPrintable(error.message));

    QTRY_VERIFY(data->records_.contains(draftId));
    QTRY_COMPARE(data->records_.value(draftId).state, DraftRecord::Retry);
    QVERIFY(!sourceRaw->note(source.id()).isNull());
    QCOMPARE(sourceRaw->removeCalls_, 0);
    QCOMPARE(destinationRaw->notes_.size(), 0);
}

void DraftManagerTransferTest::resumesPublishingDraftForEditing()
{
    auto        store = std::make_unique<MemoryDraftStore>();
    auto       *data  = store.get();
    DraftRecord record;
    record.id        = QUuid::createUuid();
    record.operation = DraftRecord::Publish;
    record.state     = DraftRecord::Publishing;
    record.storageId = QStringLiteral("resume-storage");
    record.retryAt   = QDateTime::currentDateTimeUtc().addSecs(60);
    data->records_.insert(record.id, record);

    DraftManager drafts(std::move(store));
    const auto   resumed = drafts.resumeEditingDraft(record.id);
    QVERIFY2(resumed, qPrintable(resumed.error.message));
    QCOMPARE(resumed.value.state, DraftRecord::Editing);
    QVERIFY(!resumed.value.retryAt.isValid());
    QCOMPARE(data->records_.value(record.id).state, DraftRecord::Editing);
    QCOMPARE(drafts.recoverableDrafts().size(), 1);
    QCOMPARE(drafts.recoverableDrafts().constFirst().id, record.id);
}

void DraftManagerTransferTest::excludesActiveEditingSessionFromCrashRecovery()
{
    TransferStorage storage(QStringLiteral("delayed-storage"));
    const auto      note
        = storage.addStored(QStringLiteral("open-note"), QStringLiteral("Open note"), QStringLiteral("Body"));
    DraftManager drafts(std::make_unique<MemoryDraftStore>());

    const auto draftId = drafts.acquireEditingSession(note);
    QVERIFY(!draftId.isNull());
    const auto saved = drafts.saveEditing(draftId, note, note.title(), note.text(), note.format());
    QVERIFY2(!saved, qPrintable(saved.message));

    QVERIFY(drafts.recoverableDrafts().isEmpty());

    QVERIFY(drafts.releaseEditingSession(draftId));
    const auto recoverable = drafts.recoverableDrafts();
    QCOMPARE(recoverable.size(), 1);
    QCOMPARE(recoverable.constFirst().id, draftId);
}

void DraftManagerTransferTest::prepareForShutdownRequeuesPublishingDraft()
{
    auto        store = std::make_unique<MemoryDraftStore>();
    auto       *data  = store.get();
    DraftRecord record;
    record.id           = QUuid::createUuid();
    record.operation    = DraftRecord::Publish;
    record.state        = DraftRecord::Publishing;
    record.storageId    = QStringLiteral("xmpp-pubsub");
    record.remoteNoteId = QStringLiteral("remote-note");
    record.lastError    = QStringLiteral("old error");
    record.retryAt      = QDateTime::currentDateTimeUtc().addSecs(60);
    data->records_.insert(record.id, record);

    DraftManager drafts(std::move(store));
    QSignalSpy   changed(&drafts, &DraftManager::draftsChanged);
    drafts.prepareForShutdown();

    const auto preserved = data->records_.value(record.id);
    QCOMPARE(preserved.state, DraftRecord::Ready);
    QCOMPARE(preserved.storageId, record.storageId);
    QCOMPARE(preserved.remoteNoteId, record.remoteNoteId);
    QVERIFY(preserved.lastError.isEmpty());
    QVERIFY(!preserved.retryAt.isValid());
    QCOMPARE(changed.count(), 1);
}

void DraftManagerTransferTest::exposesPendingPublicationDrafts()
{
    auto        store = std::make_unique<MemoryDraftStore>();
    auto       *data  = store.get();
    DraftRecord local;
    local.id        = QUuid::createUuid();
    local.operation = DraftRecord::Publish;
    local.state     = DraftRecord::Retry;
    local.storageId = QStringLiteral("xmpp-pubsub");
    local.title     = QStringLiteral("Local draft");
    local.lastError = QStringLiteral("Permanent upload failure");
    data->records_.insert(local.id, local);

    DraftRecord edit  = local;
    edit.id           = QUuid::createUuid();
    edit.remoteNoteId = QStringLiteral("published-note");
    edit.title        = QStringLiteral("Pending edit");
    data->records_.insert(edit.id, edit);

    DraftRecord deletion = edit;
    deletion.id          = QUuid::createUuid();
    deletion.operation   = DraftRecord::Delete;
    data->records_.insert(deletion.id, deletion);

    DraftManager drafts(std::move(store));
    const auto   pending = drafts.pendingDrafts();
    QCOMPARE(pending.size(), 2);
    QVERIFY(std::any_of(pending.cbegin(), pending.cend(),
                        [&](const DraftRecord &record) { return record.id == local.id; }));
    QVERIFY(
        std::any_of(pending.cbegin(), pending.cend(), [&](const DraftRecord &record) { return record.id == edit.id; }));

    const auto found = drafts.pendingDraftForNote(edit.storageId, edit.remoteNoteId);
    QVERIFY(found);
    QCOMPARE(found.value.id, edit.id);
}

void DraftManagerTransferTest::retargetsPublishedDraftWithoutLosingSourceIdentity()
{
    auto       destinationStorage = std::make_unique<TransferStorage>(QStringLiteral("retarget-destination"));
    auto      *destinationRaw     = registerStorage(std::move(destinationStorage));
    const auto cleanup            = qScopeGuard([destinationRaw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
    });

    auto        store = std::make_unique<MemoryDraftStore>();
    auto       *data  = store.get();
    DraftRecord record;
    record.id           = QUuid::createUuid();
    record.operation    = DraftRecord::Publish;
    record.state        = DraftRecord::Retry;
    record.storageId    = QStringLiteral("retarget-source");
    record.remoteNoteId = QStringLiteral("source-note");
    record.title        = QStringLiteral("Pending edit");
    record.body         = QStringLiteral("Body");
    record.format       = Note::Markdown;
    data->records_.insert(record.id, record);

    DraftManager drafts(std::move(store));
    const auto   error = drafts.moveDraft(record.id, destinationRaw->systemName());
    QVERIFY2(!error, qPrintable(error.message));

    const auto moved = data->records_.value(record.id);
    QCOMPARE(moved.storageId, destinationRaw->systemName());
    QVERIFY(moved.remoteNoteId.isEmpty());
    QCOMPARE(moved.removeSourceStorageId, record.storageId);
    QCOMPARE(moved.removeSourceNoteId, record.remoteNoteId);
    QCOMPARE(moved.state, DraftRecord::Ready);
    QVERIFY(moved.lastError.isEmpty());
    QVERIFY(!moved.retryAt.isValid());
}

void DraftManagerTransferTest::movesUnpublishedDraftWithoutCreatingSourceRemoval()
{
    auto       destinationStorage = std::make_unique<TransferStorage>(QStringLiteral("unpublished-destination"));
    auto      *destinationRaw     = registerStorage(std::move(destinationStorage));
    const auto cleanup            = qScopeGuard([destinationRaw]() {
        auto *manager = NoteManager::instance();
        if (manager->storage(destinationRaw->systemName()) == destinationRaw)
            manager->unregisterStorage(destinationRaw);
    });

    auto        store = std::make_unique<MemoryDraftStore>();
    auto       *data  = store.get();
    DraftRecord record;
    record.id        = QUuid::createUuid();
    record.operation = DraftRecord::Publish;
    record.state     = DraftRecord::Ready;
    record.storageId = QStringLiteral("old-publication-target");
    record.title     = QStringLiteral("Never published");
    record.body      = QStringLiteral("Body");
    record.format    = Note::Markdown;
    data->records_.insert(record.id, record);

    DraftManager drafts(std::move(store));
    const auto   error = drafts.moveDraft(record.id, destinationRaw->systemName());
    QVERIFY2(!error, qPrintable(error.message));

    const auto moved = data->records_.value(record.id);
    QCOMPARE(moved.storageId, destinationRaw->systemName());
    QVERIFY(moved.remoteNoteId.isEmpty());
    QVERIFY(moved.removeSourceStorageId.isEmpty());
    QVERIFY(moved.removeSourceNoteId.isEmpty());
    QCOMPARE(moved.state, DraftRecord::Ready);
}

QTEST_MAIN(DraftManagerTransferTest)
#include "draftmanagertransfer_test.moc"
