#include "xmppbackend.h"
#include "xmppnotecodec.h"
#include "xmppsettingscontroller.h"
#include "xmppstorage.h"

#include <QHash>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

#include <utility>

using namespace AnyKeep;

namespace {

class FakeXmppBackend final : public XmppBackend {
public:
    using XmppBackend::appendIndexPayload;
    using XmppBackend::completeIndexList;
    using XmppBackend::XmppBackend;

    void seed(XmppRemoteNote note)
    {
        remote_.insert(note.id, note);
        emit remoteNotePublished(note);
    }

    void start() override {}
    void setConfig(const XmppConfig &config) override { config_ = config; }
    void shutdown() override { ++shutdownCalls; }
    void probeAsync(StatusCallback callback) override { callback({ true }); }
    void listNotesAsync(ListCallback callback) override
    {
        ++listCalls;
        if (deferLists) {
            pendingLists.append(std::move(callback));
            return;
        }
        XmppListResult result;
        result.ok    = true;
        result.notes = remote_.values();
        callback(std::move(result));
    }
    void getNoteAsync(QString id, NoteCallback callback) override
    {
        ++getNoteCalls;
        if (loadFailure) {
            callback(*loadFailure);
            return;
        }
        XmppNoteResult result;
        result.ok   = remote_.contains(id);
        result.note = remote_.value(id);
        callback(std::move(result));
    }
    void saveNoteAsync(XmppRemoteNote, NoteCallback callback) override
    {
        ++fullSaveCount;
        if (saveFailure) {
            callback(*saveFailure);
            return;
        }
        XmppNoteResult result;
        result.error = QStringLiteral("Unexpected full save");
        callback(std::move(result));
    }
    void updateNoteIndexAsync(XmppRemoteNote requested, NoteCallback callback) override
    {
        ++indexUpdateCount;
        indexRequests.append(requested);
        XmppNoteResult result;
        if (!remote_.contains(requested.id)) {
            result.notFound = true;
            result.error    = QStringLiteral("Missing note");
            callback(std::move(result));
            return;
        }
        result.note
            = makeIndexUpdate(remote_.value(requested.id), requested,
                              QStringLiteral("index-revision-%1").arg(indexUpdateCount), QStringLiteral("test-device"));
        result.ok = true;
        remote_.insert(result.note.id, result.note);
        callback(std::move(result));
    }
    void deleteNoteAsync(QString id, StatusCallback callback) override
    {
        remote_.remove(id);
        callback({ true });
    }
    XmppDeviceInfo ownOmemoDevice() const override { return {}; }
    void           ownOmemoDevicesAsync(DevicesCallback callback) override { callback({}, {}); }
    void           ownOmemoBundleValidAsync(StatusCallback callback) override { callback({ true }); }
    void           repairOwnOmemoDeviceAsync(StatusCallback callback) override { callback({ true }); }
    void           removeOwnOmemoDeviceAsync(quint32, StatusCallback callback) override { callback({ true }); }
    void           trustOwnOmemoDeviceAsync(QByteArray, StatusCallback callback) override { callback({ true }); }
    void trustOwnOmemoDevicesAsync(QList<QByteArray>, StatusCallback callback) override { callback({ true }); }
    void auditStorageKeysAsync(AuditCallback callback, XmppProgressCallback = {}) override
    {
        XmppKeyAuditResult result;
        result.ok = true;
        callback(std::move(result));
    }
    void rekeyStorageAsync(QList<QByteArray>, QByteArray, RekeyCallback callback, XmppProgressCallback = {}) override
    {
        XmppRekeyResult result;
        result.ok = true;
        callback(std::move(result));
    }
    void scanObsoleteItemsAsync(CleanupCallback callback) override
    {
        XmppCleanupResult result;
        result.ok = true;
        callback(std::move(result));
    }
    void deleteObsoleteItemsAsync(QStringList, QStringList, CleanupCallback callback) override
    {
        XmppCleanupResult result;
        result.ok = true;
        callback(std::move(result));
    }
    void approveKeySyncRequest(QString) override {}
    void rejectKeySyncRequest(QString) override {}

    int                            fullSaveCount { 0 };
    int                            shutdownCalls { 0 };
    int                            indexUpdateCount { 0 };
    QList<XmppRemoteNote>          indexRequests;
    QHash<QString, XmppRemoteNote> remote_;
    XmppConfig                     config_;
    bool                           deferLists { false };
    int                            listCalls { 0 };
    QList<ListCallback>            pendingLists;
    std::optional<XmppNoteResult>  loadFailure;
    std::optional<XmppNoteResult>  saveFailure;
    int                            getNoteCalls { 0 };
};

XmppRemoteNote remoteNote(const QString &id, const QDateTime &modified)
{
    XmppRemoteNote note;
    note.id              = id;
    note.revision        = id + QStringLiteral("-index-1");
    note.contentRevision = id + QStringLiteral("-content-1");
    note.originId        = QStringLiteral("seed-device");
    note.title           = id;
    note.content         = id + QStringLiteral(" body");
    note.modified        = modified;
    note.contentPresent  = true;
    return note;
}

} // namespace

class XmppStorageTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void reorderPublishesOnlyTheIndex();
    void syncActivityReportsIndexStages();
    void sharedRefreshStaysBusyUntilTheLastCallerCancels();
    void destroyedRefreshCallerClearsActivity();
    void cancelledRefreshIgnoresLateResult();
    void failedRefreshReportsTerminalActivity();
    void syncHistoryIsBoundedAndAvailableToNewSettings();
    void shutdownCancelsRefreshAndRejectsNewJobs();
    void mixedKeyIndexRequiresRecovery_data();
    void mixedKeyIndexRequiresRecovery();
    void sameKeyAuthenticationFailureIsNotForeignKeyRecovery();
    void noteBodyKeyMismatchRequiresRecovery_data();
    void noteBodyKeyMismatchRequiresRecovery();
    void saveWithForeignKeyRequiresRecovery();
};

void XmppStorageTest::init()
{
    // A Windows keychain enables persistent caching. Each scenario must have
    // its own cache, regardless of whether the previous scenario wrote notes.
    QSettings().setValue(QStringLiteral("storage.xmpppubsub.instanceId"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void XmppStorageTest::mixedKeyIndexRequiresRecovery_data()
{
    QTest::addColumn<int>("readableCount");
    QTest::addColumn<int>("foreignCount");
    QTest::addColumn<bool>("freshStart");
    QTest::addColumn<bool>("needsRecovery");
    QTest::newRow("mixed-keys") << 2 << 1 << false << true;
    QTest::newRow("after-peer-migration") << 0 << 3 << false << true;
    QTest::newRow("canonical-key-installed") << 3 << 0 << false << false;
    QTest::newRow("empty-storage") << 0 << 0 << false << false;
    QTest::newRow("explicit-fresh-start") << 0 << 3 << true << false;
    QTest::newRow("fresh-start-with-new-notes") << 2 << 3 << true << false;
}

void XmppStorageTest::mixedKeyIndexRequiresRecovery()
{
    QFETCH(int, readableCount);
    QFETCH(int, foreignCount);
    QFETCH(bool, freshStart);
    QFETCH(bool, needsRecovery);
    XmppConfig config;
    config.masterKey              = SecureEnvelope::generateMasterKey();
    config.allowForeignKeyIndices = freshStart;
    const auto     foreignKey     = SecureEnvelope::generateMasterKey();
    XmppListResult result;
    for (int i = 0; i < readableCount + foreignCount; ++i) {
        const auto source  = remoteNote(QStringLiteral("note-%1").arg(i), QDateTime::currentDateTimeUtc());
        const auto encoded = XmppNoteCodec::encodeIndex(source, i < readableCount ? config.masterKey : foreignKey,
                                                        config.indexNodeName());
        QVERIFY(encoded);
        QVERIFY(!FakeXmppBackend::appendIndexPayload(config, encoded.value, result));
    }
    FakeXmppBackend::completeIndexList(config, result);
    QCOMPARE(result.notes.size(), readableCount);
    QCOMPARE(result.foreignKeyNotes, foreignCount);
    QCOMPARE(result.ok, !needsRecovery);
    if (needsRecovery) {
        QCOMPARE(result.errorKind, XmppErrorKind::Security);
        QVERIFY(result.error.contains(QStringLiteral("storage key mismatch")));
        // The actual storage adapter must stop normal operations for this
        // result; it must keep the backend alive for OMEMO recovery requests.
        auto *backend       = new FakeXmppBackend;
        backend->deferLists = true;
        XmppStorage storage(nullptr, backend);
        auto       *job = storage.refreshNotesAsync();
        QTRY_COMPARE(backend->pendingLists.size(), 1);
        backend->pendingLists.takeFirst()(result);
        QTRY_VERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Failed);
        QCOMPARE(storage.connectivityState(), NoteStorage::ConnectivityState::Error);
        QCOMPARE(backend->shutdownCalls, 0);
    }
}

void XmppStorageTest::sameKeyAuthenticationFailureIsNotForeignKeyRecovery()
{
    XmppConfig config;
    config.masterKey   = SecureEnvelope::generateMasterKey();
    const auto source  = remoteNote(QStringLiteral("damaged"), QDateTime::currentDateTimeUtc());
    auto       encoded = XmppNoteCodec::encodeIndex(source, config.masterKey, config.indexNodeName());
    QVERIFY(encoded);
    encoded.value.cipherText[0] = char(encoded.value.cipherText.at(0) ^ 1);
    XmppListResult result;
    QVERIFY(FakeXmppBackend::appendIndexPayload(config, encoded.value, result));
    FakeXmppBackend::completeIndexList(config, result);
    QVERIFY(result.ok);
    QVERIFY(result.partial);
    QCOMPARE(result.foreignKeyNotes, 0);
    QVERIFY(result.notes.isEmpty());
}

void XmppStorageTest::noteBodyKeyMismatchRequiresRecovery_data()
{
    QTest::addColumn<QString>("error");
    QTest::addColumn<bool>("needsRecovery");
    QTest::newRow("foreign-body-key") << QStringLiteral("Encrypted private-note storage key mismatch") << true;
    QTest::newRow("damaged-same-key-ciphertext") << QStringLiteral("Encrypted data authentication failed") << false;
}

void XmppStorageTest::noteBodyKeyMismatchRequiresRecovery()
{
    QFETCH(QString, error);
    QFETCH(bool, needsRecovery);
    auto          *backend = new FakeXmppBackend;
    XmppNoteResult failed;
    failed.error         = error;
    backend->loadFailure = failed;
    XmppStorage storage(nullptr, backend);
    emit        backend->connectionChanged(true);
    auto       *job = storage.loadNoteAsync(QStringLiteral("note-with-unreadable-body"));
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Failed);
    // A key mismatch blocks further ordinary loads until recovery; isolated
    // same-key authentication damage leaves other note loads available.
    auto *second = storage.loadNoteAsync(QStringLiteral("another-note"));
    QTRY_VERIFY(second->isFinished());
    QCOMPARE(backend->getNoteCalls, needsRecovery ? 1 : 2);
    QCOMPARE(backend->shutdownCalls, 0);
}

void XmppStorageTest::saveWithForeignKeyRequiresRecovery()
{
    auto          *backend = new FakeXmppBackend;
    XmppNoteResult failed;
    failed.error         = QStringLiteral("Encrypted private-note storage key mismatch");
    backend->saveFailure = failed;
    XmppStorage storage(nullptr, backend);
    emit        backend->connectionChanged(true);
    const auto  remote = remoteNote(QStringLiteral("editable"), QDateTime::currentDateTimeUtc());
    backend->seed(remote);
    const auto local = storage.note(remote.id);
    QVERIFY(local.isLoaded());
    auto *job = storage.saveNoteAsync(local);
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(backend->fullSaveCount, 1);
    QCOMPARE(job->state(), StorageJob::Failed);
    auto *blocked = storage.loadNoteAsync(QStringLiteral("different-remote-note"));
    QTRY_VERIFY(blocked->isFinished());
    QCOMPARE(backend->getNoteCalls, 0);
    QCOMPARE(storage.connectivityState(), NoteStorage::ConnectivityState::Error);
    QCOMPARE(backend->shutdownCalls, 0);
}

void XmppStorageTest::reorderPublishesOnlyTheIndex()
{
    auto       *backend = new FakeXmppBackend;
    XmppStorage storage(nullptr, backend);
    const auto  newer = QDateTime::currentDateTimeUtc().addSecs(-120);
    const auto  older = newer.addSecs(-60);
    backend->seed(remoteNote(QStringLiteral("newer"), newer));
    backend->seed(remoteNote(QStringLiteral("older"), older));

    auto *job = storage.reorderNotesAsync({ QStringLiteral("older") }, {});
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Succeeded);
    QCOMPARE(backend->fullSaveCount, 0);
    QCOMPARE(backend->indexUpdateCount, 1);
    QCOMPARE(backend->indexRequests.size(), 1);
    QVERIFY(backend->indexRequests.constFirst().preserveModified);
    QVERIFY(backend->indexRequests.constFirst().modified > newer);

    const auto reordered = backend->remote_.value(QStringLiteral("older"));
    QCOMPARE(reordered.content, QStringLiteral("older body"));
    QCOMPARE(reordered.contentRevision, QStringLiteral("older-content-1"));
    QCOMPARE(reordered.modified, backend->indexRequests.constFirst().modified);
    QVERIFY(!reordered.contentPresent);
}

void XmppStorageTest::syncActivityReportsIndexStages()
{
    auto       *backend = new FakeXmppBackend;
    XmppStorage storage(nullptr, backend);
    const auto  now = QDateTime::currentDateTimeUtc();
    backend->remote_.insert(QStringLiteral("one"), remoteNote(QStringLiteral("one"), now));
    backend->remote_.insert(QStringLiteral("two"), remoteNote(QStringLiteral("two"), now));

    QSignalSpy progress(&storage, &XmppStorage::syncProgressChanged);
    auto      *job          = storage.refreshNotesAsync();
    bool       busyAtFinish = true;
    connect(job, &StorageJob::finished, &storage, [&] { busyAtFinish = progress.last().at(2).toBool(); });
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Succeeded);
    QVERIFY(progress.count() >= 3);
    QVERIFY(!busyAtFinish);
    QCOMPARE(backend->listCalls, 1);

    const auto first = progress.first();
    QCOMPARE(first.at(2).toBool(), true);
    QVERIFY(first.at(0).toString().contains(QStringLiteral("note index")));
    const auto last = progress.last();
    QCOMPARE(last.at(2).toBool(), false);
    QVERIFY(last.at(0).toString().contains(QStringLiteral("2 notes")));
    const QString joined = last.at(1).toStringList().join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QStringLiteral("encrypted local cache")));
    QVERIFY(!joined.contains(QStringLiteral("one body")));
    QVERIFY(!joined.contains(QStringLiteral("two body")));
}

void XmppStorageTest::sharedRefreshStaysBusyUntilTheLastCallerCancels()
{
    auto *backend       = new FakeXmppBackend;
    backend->deferLists = true;
    XmppStorage storage(nullptr, backend);
    QSignalSpy  progress(&storage, &XmppStorage::syncProgressChanged);
    auto       *first  = storage.refreshNotesAsync();
    auto       *second = storage.refreshNotesAsync(1);
    QTRY_COMPARE(backend->pendingLists.size(), 1);
    QCOMPARE(backend->listCalls, 1);
    first->cancel();
    QVERIFY(progress.last().at(2).toBool());
    second->cancel();
    QVERIFY(!progress.last().at(2).toBool());
    QVERIFY(progress.last().at(0).toString().contains(QStringLiteral("cancelled")));
}

void XmppStorageTest::destroyedRefreshCallerClearsActivity()
{
    auto *backend       = new FakeXmppBackend;
    backend->deferLists = true;
    XmppStorage storage(nullptr, backend);
    QSignalSpy  progress(&storage, &XmppStorage::syncProgressChanged);
    auto       *job = storage.refreshNotesAsync();
    delete job;
    QVERIFY(!progress.last().at(2).toBool());
    QCoreApplication::processEvents();
    QCOMPARE(backend->listCalls, 0);
}

void XmppStorageTest::cancelledRefreshIgnoresLateResult()
{
    auto *backend       = new FakeXmppBackend;
    backend->deferLists = true;
    XmppStorage storage(nullptr, backend);
    QSignalSpy  progress(&storage, &XmppStorage::syncProgressChanged);
    auto       *old = storage.refreshNotesAsync();
    QTRY_COMPARE(backend->pendingLists.size(), 1);
    old->cancel();
    auto *current = storage.refreshNotesAsync();
    QTRY_COMPARE(backend->pendingLists.size(), 2);
    XmppListResult stale;
    stale.ok = true;
    stale.notes.append(remoteNote(QStringLiteral("stale"), QDateTime::currentDateTimeUtc()));
    backend->pendingLists.takeFirst()(std::move(stale));
    QCoreApplication::processEvents();
    QVERIFY(storage.note(QStringLiteral("stale")).isNull());
    QVERIFY(!current->isFinished());
    QVERIFY(progress.last().at(2).toBool());
    XmppListResult fresh;
    fresh.ok = true;
    fresh.notes.append(remoteNote(QStringLiteral("fresh"), QDateTime::currentDateTimeUtc()));
    backend->pendingLists.takeFirst()(std::move(fresh));
    QTRY_VERIFY(current->isFinished());
    QCOMPARE(current->state(), StorageJob::Succeeded);
    QVERIFY(!storage.note(QStringLiteral("fresh")).isNull());
    QVERIFY(!progress.last().at(2).toBool());
}

void XmppStorageTest::failedRefreshReportsTerminalActivity()
{
    auto *backend       = new FakeXmppBackend;
    backend->deferLists = true;
    XmppStorage storage(nullptr, backend);
    QSignalSpy  progress(&storage, &XmppStorage::syncProgressChanged);
    auto       *job = storage.refreshNotesAsync();
    QTRY_COMPARE(backend->pendingLists.size(), 1);
    XmppListResult failed;
    failed.errorKind = XmppErrorKind::Transient;
    failed.error     = QStringLiteral("private remote diagnostic");
    backend->pendingLists.takeFirst()(std::move(failed));
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Failed);
    QVERIFY(!progress.last().at(2).toBool());
    QVERIFY(!progress.last()
                 .at(1)
                 .toStringList()
                 .join(QLatin1Char('\n'))
                 .contains(QStringLiteral("private remote diagnostic")));
}

void XmppStorageTest::syncHistoryIsBoundedAndAvailableToNewSettings()
{
    auto       *backend = new FakeXmppBackend;
    XmppStorage storage(nullptr, backend);
    QSignalSpy  progress(&storage, &XmppStorage::syncProgressChanged);
    for (int i = 0; i < 15; ++i) {
        emit  backend->remoteNodeInvalidated();
        auto *job = storage.refreshNotesAsync();
        QTRY_VERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
    }
    const auto last = progress.last();
    QCOMPARE(last.at(1).toStringList().size(), 40);
    auto *settings = qobject_cast<XmppSettingsController *>(storage.createSettingsController(&storage));
    QVERIFY(settings);
    QCOMPARE(settings->syncSummary(), last.at(0).toString());
    QCOMPARE(settings->syncEvents(), last.at(1).toStringList());
    QVERIFY(!settings->syncBusy());
    emit  backend->remoteNodeInvalidated();
    auto *job = storage.refreshNotesAsync();
    QVERIFY(settings->syncBusy());
    QTRY_VERIFY(job->isFinished());
    QVERIFY(!settings->syncBusy());
}

void XmppStorageTest::shutdownCancelsRefreshAndRejectsNewJobs()
{
    auto *backend       = new FakeXmppBackend;
    backend->deferLists = true;
    XmppStorage storage(nullptr, backend);
    QSignalSpy  progress(&storage, &XmppStorage::syncProgressChanged);
    auto       *job = storage.refreshNotesAsync();
    QTRY_COMPARE(backend->pendingLists.size(), 1);
    storage.shutdown();
    QCOMPARE(job->state(), StorageJob::Cancelled);
    QVERIFY(!progress.last().at(2).toBool());
    QCOMPARE(storage.refreshNotesAsync()->state(), StorageJob::Cancelled);
    QCOMPARE(storage.initAsync()->state(), StorageJob::Cancelled);
}

QTEST_MAIN(XmppStorageTest)
#include "xmppstorage_test.moc"
