#include "draftmanager.h"
#include "editortestsupport.h"
#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "secureenvelope.h"
#include "xmppbackend.h"
#include "xmppnotecodec.h"
#include "xmppsettingscontroller.h"
#include "xmppstorage.h"

#include <QHash>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
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

    bool supportsMedia() const override { return mediaSupported; }
    void checkMediaPublicationsAsync(XmppRemoteNote note) override { checkedMediaNotes.append(std::move(note)); }
    void start() override {}
    void setConfig(const XmppConfig &config) override { config_ = config; }
    void shutdown() override { ++shutdownCalls; }
    void checkConnectionAfterResume() override { ++wakeupProbes; }
    void simulateConnection(bool online) { emit connectionChanged(online); }
    void connectAsync(StatusCallback callback) override
    {
        ++connectCalls;
        if (deferConnect) {
            pendingConnects.append(std::move(callback));
            return;
        }
        callback({ true });
    }
    void probeAsync(StatusCallback callback) override
    {
        ++probeCalls;
        callback({ true });
    }
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
        if (deferGets) {
            pendingGets.append({ id, std::move(callback) });
            return;
        }
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
    void onlineResourcesAsync(ResourcesCallback callback) override { callback({}, {}); }
    void auditStorageKeysAsync(AuditCallback callback, XmppProgressCallback = {},
                               std::optional<QStringList> = std::nullopt) override
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

    int                                     connectCalls { 0 };
    int                                     probeCalls { 0 };
    bool                                    deferConnect { false };
    QList<StatusCallback>                   pendingConnects;
    int                                     fullSaveCount { 0 };
    int                                     shutdownCalls { 0 };
    int                                     wakeupProbes { 0 };
    int                                     indexUpdateCount { 0 };
    QList<XmppRemoteNote>                   indexRequests;
    QHash<QString, XmppRemoteNote>          remote_;
    XmppConfig                              config_;
    bool                                    deferLists { false };
    int                                     listCalls { 0 };
    QList<ListCallback>                     pendingLists;
    std::optional<XmppNoteResult>           loadFailure;
    std::optional<XmppNoteResult>           saveFailure;
    int                                     getNoteCalls { 0 };
    bool                                    deferGets { false };
    bool                                    mediaSupported { false };
    QList<std::pair<QString, NoteCallback>> pendingGets;
    QList<XmppRemoteNote>                   checkedMediaNotes;
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
    void foregroundResumeProbesOnlineStreamWithoutRestart();
    void offlineOpenRetainsLastCompleteBody_data();
    void offlineOpenRetainsLastCompleteBody();
    void backgroundRefreshRespectsLocalEdits_data();
    void backgroundRefreshRespectsLocalEdits();
    void lateBackgroundReplyDoesNotReplaceNewerSnapshot();
    void authenticationStartsWithoutEncryptionKeys();
    void reorderPublishesOnlyTheIndex();
    void restorePublishesIndexWithoutRecycleMarker_data();
    void restorePublishesIndexWithoutRecycleMarker();
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

void XmppStorageTest::authenticationStartsWithoutEncryptionKeys()
{
    // This account is deliberately missing both the remote-note key and the
    // local OMEMO key. The backend must still be asked to do TLS/SASL login.
    QSettings     settings;
    const QString jidSetting      = QStringLiteral("storage.xmpppubsub.jid");
    const QString passwordSetting = QStringLiteral("storage.xmpppubsub.password");
    const bool    hadJid          = settings.contains(jidSetting);
    const bool    hadPassword     = settings.contains(passwordSetting);
    const auto    oldJid          = settings.value(jidSetting);
    const auto    oldPassword     = settings.value(passwordSetting);
    const auto    restore         = qScopeGuard([&] {
        if (hadJid)
            settings.setValue(jidSetting, oldJid);
        else
            settings.remove(jidSetting);
        if (hadPassword)
            settings.setValue(passwordSetting, oldPassword);
        else
            settings.remove(passwordSetting);
    });
    settings.setValue(
        jidSetting,
        QStringLiteral("xmpp-auth-test-%1@example.test").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    settings.setValue(passwordSetting, QStringLiteral("stub-password"));

    auto *backend         = new FakeXmppBackend;
    backend->deferConnect = true;
    XmppStorage storage(nullptr, backend);
    auto       *job = storage.initAsync(&storage);
    QTRY_COMPARE(backend->connectCalls, 1);
    QCOMPARE(backend->probeCalls, 0);
    QVERIFY(!job->isFinished());

    // Authentication succeeded, but encrypted notes cannot yet be opened.
    backend->pendingConnects.takeFirst()({ true });
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Failed);
    QCOMPARE(job->error().code, StorageError::NotConfigured);
    QCOMPARE(backend->probeCalls, 0);
    QCOMPARE(storage.connectivityState(), NoteStorage::ConnectivityState::Online);
    QVERIFY(!storage.canAcceptWrites());
}

void XmppStorageTest::offlineOpenRetainsLastCompleteBody_data()
{
    QTest::addColumn<bool>("event");
    QTest::newRow("index-event") << true;
    QTest::newRow("index-refresh") << false;
}

void XmppStorageTest::offlineOpenRetainsLastCompleteBody()
{
    QFETCH(bool, event);
    auto      *backend  = new FakeXmppBackend;
    const auto original = remoteNote(QStringLiteral("cached-note"), QDateTime::currentDateTimeUtc());
    backend->remote_.insert(original.id, original);
    XmppStorage storage(nullptr, backend);
    backend->simulateConnection(true);
    auto *ready = storage.refreshNotesAsync();
    QTRY_VERIFY(ready->isFinished());
    QCOMPARE(ready->state(), StorageJob::Succeeded);
    QVERIFY(storage.note(original.id).isLoaded());
    auto latest            = original;
    latest.revision        = QStringLiteral("new-index");
    latest.contentRevision = QStringLiteral("new-body");
    latest.contentPresent  = false;
    latest.content.clear();
    if (event) {
        emit backend->remoteNotePublished(latest);
    } else {
        backend->remote_.insert(latest.id, latest);
        emit  backend->remoteNodeInvalidated();
        auto *refresh = storage.refreshNotesAsync();
        QTRY_VERIFY(refresh->isFinished());
        QCOMPARE(refresh->state(), StorageJob::Succeeded);
    }
    backend->deferGets      = true;
    backend->mediaSupported = true;
    backend->simulateConnection(false);
    QObject subscriber;
    storage.observeNote(storage.note(original.id), &subscriber);
    QVERIFY(backend->checkedMediaNotes.isEmpty());
    const int reads = backend->getNoteCalls;
    auto     *load  = storage.loadNoteAsync(original.id);
    QTRY_VERIFY_WITH_TIMEOUT(load->isFinished(), 500);
    QCOMPARE(load->state(), StorageJob::Succeeded);
    QCOMPARE(load->result().text(), original.content);
    // The old body keeps its real base token, never the newer index token.
    QCOMPARE(load->result().backendValue(QStringLiteral("revision")).toString(), original.revision);
    QCOMPARE(backend->getNoteCalls, reads);
    QVERIFY(storage.isAccessible());
}

void XmppStorageTest::backgroundRefreshRespectsLocalEdits_data()
{
    QTest::addColumn<QString>("edit");
    for (const auto *kind : { "untouched", "typing", "checkpoint", "undo", "metadata" })
        QTest::newRow(kind) << QString::fromLatin1(kind);
}

void XmppStorageTest::backgroundRefreshRespectsLocalEdits()
{
    QFETCH(QString, edit);
    auto *backend           = new FakeXmppBackend;
    backend->mediaSupported = true;
    auto original           = remoteNote(QStringLiteral("background-note"), QDateTime::currentDateTimeUtc());
    original.content        = QStringLiteral("Keep this paragraph.\n\n- Stable list\n\nOld tail.");
    backend->remote_.insert(original.id, original);
    XmppStorage storage(nullptr, backend);
    backend->simulateConnection(true);
    auto *ready = storage.refreshNotesAsync();
    QTRY_VERIFY(ready->isFinished());
    auto latest            = original;
    latest.revision        = QStringLiteral("fresh-index");
    latest.contentRevision = QStringLiteral("fresh-body");
    latest.content         = QStringLiteral("Keep this paragraph.\n\n- Stable list\n\nNew tail.");
    backend->remote_.insert(latest.id, latest);
    backend->deferGets = true;
    DraftManager          drafts(std::make_unique<TestSupport::MemoryDraftStore>());
    NoteEditor            editor(storage.note(original.id), drafts);
    QSignalSpy            resets(editor.model(), &QAbstractItemModel::modelReset);
    QPersistentModelIndex stable(editor.model()->index(1));
    QTRY_COMPARE(backend->pendingGets.size(), 1);
    QTRY_COMPARE(backend->checkedMediaNotes.size(), 1);
    QCOMPARE(backend->checkedMediaNotes.first().id, original.id);
    QCOMPARE(backend->checkedMediaNotes.first().contentRevision, original.contentRevision);
    auto *open = storage.loadNoteAsync(original.id);
    QTRY_VERIFY_WITH_TIMEOUT(open->isFinished(), 500);
    QCOMPARE(open->state(), StorageJob::Succeeded);
    QCOMPARE(open->result().text(), original.content);
    if (edit == QLatin1String("typing") || edit == QLatin1String("checkpoint") || edit == QLatin1String("undo")) {
        editor.setText(editor.text() + QStringLiteral(" Local change."));
        if (edit == QLatin1String("checkpoint")) {
            QVERIFY(editor.save());
            QVERIFY(!editor.isDirty());
        } else if (edit == QLatin1String("undo")) {
            editor.setText(original.title + QStringLiteral("\n\n") + original.content);
        }
    } else if (edit == QLatin1String("metadata")) {
        editor.setFavorite(true);
    }
    const auto     localText = editor.text();
    XmppNoteResult result;
    result.ok   = true;
    result.note = latest;
    backend->pendingGets.takeFirst().second(std::move(result));
    QTRY_COMPARE(storage.note(original.id).backendValue(QStringLiteral("revision")).toString(), latest.revision);
    if (edit == QLatin1String("untouched")) {
        QTRY_COMPARE(editor.text(), latest.title + QStringLiteral("\n\n") + latest.content);
        QCOMPARE(editor.note().backendValue(QStringLiteral("revision")).toString(), latest.revision);
        QVERIFY(!editor.isDirty());
        QVERIFY(!editor.canUndo());
        QVERIFY(stable.isValid());
        QCOMPARE(stable.data(NoteBlockModel::TextRole).toString(), QStringLiteral("Keep this paragraph."));
        QCOMPARE(resets.count(), 0);
    } else {
        QCOMPARE(editor.text(), localText);
        QCOMPARE(editor.note().backendValue(QStringLiteral("revision")).toString(), original.revision);
    }
}

void XmppStorageTest::lateBackgroundReplyDoesNotReplaceNewerSnapshot()
{
    auto      *backend  = new FakeXmppBackend;
    const auto original = remoteNote(QStringLiteral("racing-note"), QDateTime::currentDateTimeUtc());
    backend->remote_.insert(original.id, original);
    XmppStorage storage(nullptr, backend);
    backend->simulateConnection(true);
    auto *ready = storage.refreshNotesAsync();
    QTRY_VERIFY(ready->isFinished());
    backend->deferGets = true;
    QObject subscriber;
    storage.observeNote(storage.note(original.id), &subscriber);
    QTRY_COMPARE(backend->pendingGets.size(), 1);
    auto newer            = original;
    newer.revision        = QStringLiteral("newest-revision");
    newer.contentRevision = newer.revision;
    newer.content         = QStringLiteral("Newer event body");
    backend->seed(newer);
    XmppNoteResult delayed;
    delayed.ok   = true;
    delayed.note = original;
    backend->pendingGets.takeFirst().second(std::move(delayed));
    QTest::qWait(20);
    QCOMPARE(storage.note(original.id).text(), newer.content);
    QCOMPARE(storage.note(original.id).backendValue(QStringLiteral("revision")).toString(), newer.revision);
}

void XmppStorageTest::foregroundResumeProbesOnlineStreamWithoutRestart()
{
    auto       *backend = new FakeXmppBackend;
    XmppStorage storage(nullptr, backend);
    backend->simulateConnection(true);

    // The lifecycle event is delivered to the storage adapter (not to a
    // mobile-only copy of the XMPP plugin). It must test the existing socket,
    // not tear down an otherwise healthy authenticated session.
    QVERIFY(QMetaObject::invokeMethod(&storage, "onApplicationResumed", Qt::DirectConnection));
    QCOMPARE(backend->wakeupProbes, 1);
    QCOMPARE(backend->shutdownCalls, 0);
    QVERIFY(QMetaObject::invokeMethod(&storage, "onApplicationResumed", Qt::DirectConnection));
    QCOMPARE(backend->wakeupProbes, 2);
    QCOMPARE(backend->shutdownCalls, 0);
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
    // The SASL connection alone no longer grants encrypted note access.
    // Complete a successful index preparation to make the storage ready.
    auto *ready = storage.refreshNotesAsync();
    QTRY_VERIFY(ready->isFinished());
    QCOMPARE(ready->state(), StorageJob::Succeeded);
    auto *job = storage.loadNoteAsync(QStringLiteral("note-with-unreadable-body"));
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
    // A storage-key mismatch blocks note operations, not the authenticated
    // XMPP transport; the server connection must remain visibly online.
    QCOMPARE(storage.connectivityState(), NoteStorage::ConnectivityState::Online);
    QCOMPARE(backend->shutdownCalls, 0);
}

void XmppStorageTest::reorderPublishesOnlyTheIndex()
{
    FolderCatalogManager catalog;
    auto                *backend = new FakeXmppBackend;
    XmppStorage          storage(nullptr, backend, &catalog);
    const auto           newer = QDateTime::currentDateTimeUtc().addSecs(-120);
    const auto           older = newer.addSecs(-60);
    backend->seed(remoteNote(QStringLiteral("newer"), newer));
    auto recycled           = remoteNote(QStringLiteral("older"), older);
    recycled.folderPath     = { QStringLiteral("Recycle Bin") };
    recycled.systemFolderId = FolderCatalog::recycleBinId();
    backend->seed(recycled);

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
    QCOMPARE(reordered.folderPath, recycled.folderPath);
    QCOMPARE(reordered.systemFolderId, recycled.systemFolderId);
    QCOMPARE(reordered.modified, backend->indexRequests.constFirst().modified);
    QVERIFY(!reordered.contentPresent);
}

void XmppStorageTest::restorePublishesIndexWithoutRecycleMarker_data()
{
    QTest::addColumn<QStringList>("originalPath");
    QTest::newRow("unsorted") << QStringList();
    QTest::newRow("original-folder") << QStringList { QStringLiteral("Inbox") };
}

void XmppStorageTest::restorePublishesIndexWithoutRecycleMarker()
{
    QFETCH(QStringList, originalPath);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FolderCatalogManager catalog(std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(catalog.initialize());
    auto       *backend = new FakeXmppBackend;
    XmppStorage storage(nullptr, backend, &catalog);
    const auto  storageId = storage.systemName();

    auto  recycled = remoteNote(QStringLiteral("recycled"), QDateTime::currentDateTimeUtc().addSecs(-60));
    QUuid originalFolder;
    if (!originalPath.isEmpty()) {
        FolderRecord folder;
        folder.name        = originalPath.constFirst();
        const auto created = catalog.addFolder(folder);
        QVERIFY(created);
        originalFolder = created.value;
    }
    QVERIFY(!catalog.recycleNote(storageId, recycled.id, originalFolder));
    recycled.folderPath     = { QStringLiteral("Recycle Bin") };
    recycled.systemFolderId = FolderCatalog::recycleBinId();
    backend->seed(recycled);

    // The deleting device already stored the original folder and recycle time
    // before the server announced the recycle. Keep those fields in the peer.
    auto peerSnapshot = catalog.snapshot();
    QCOMPARE(peerSnapshot.assignments.size(), 1);
    peerSnapshot.assignments.first().modifiedAt = recycled.modified;
    peerSnapshot.assignments.first().recycledAt = recycled.modified;
    FolderCatalog peer;
    QVERIFY(!peer.replaceSnapshot(std::move(peerSnapshot)));

    const auto restored = catalog.restoreRecycledNote(storageId, recycled.id);
    QVERIFY(restored);
    QCOMPARE(restored.value, originalFolder);
    auto note = storage.note(recycled.id);
    QVERIFY(!note.isNull());
    note.setFolderId(restored.value);

    auto *job = storage.changeNoteFolderAsync(note);
    QTRY_VERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Succeeded);
    QCOMPARE(backend->fullSaveCount, 0);
    const auto published = backend->remote_.value(recycled.id);
    QCOMPARE(published.folderPath, originalPath);
    QVERIFY(published.systemFolderId.isNull());
    QCOMPARE(backend->indexUpdateCount, 1);
    QCOMPARE(published.contentRevision, recycled.contentRevision);
    QCOMPARE(published.content, recycled.content);
    QVERIFY(!published.contentPresent);
    QCOMPARE(catalog.catalog().folderForNote(storageId, recycled.id), originalFolder);

    QVERIFY(!peer.reconcileProviderFolderPaths(
        storageId, { { recycled.id, recycled.folderPath, recycled.systemFolderId, recycled.modified } }));
    const auto *before = peer.assignment(storageId, recycled.id);
    QVERIFY(before);
    QCOMPARE(before->previousFolderId, originalFolder);
    QCOMPARE(before->recycledAt, recycled.modified);
    QVERIFY(peer.isRecycled(storageId, recycled.id));

    const auto received = peer.reconcileProviderFolderPaths(
        storageId, { { published.id, published.folderPath, published.systemFolderId, published.modified } });
    QVERIFY2(!received, qPrintable(received.message));
    QVERIFY(!peer.isRecycled(storageId, recycled.id));
    QCOMPARE(peer.folderForNote(storageId, recycled.id), originalFolder);
    const auto *after = peer.assignment(storageId, recycled.id);
    QVERIFY(after);
    QCOMPARE(after->tombstone, originalFolder.isNull());
    QVERIFY(after->previousFolderId.isNull());
    QVERIFY(!after->recycledAt.isValid());
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
