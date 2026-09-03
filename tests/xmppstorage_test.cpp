#include "xmppbackend.h"
#include "xmppstorage.h"

#include <QHash>
#include <QSignalSpy>
#include <QtTest>

#include <utility>

using namespace AnyKeep;

namespace {

class FakeXmppBackend final : public XmppBackend {
public:
    using XmppBackend::XmppBackend;

    void seed(XmppRemoteNote note)
    {
        remote_.insert(note.id, note);
        emit remoteNotePublished(note);
    }

    void start() override {}
    void setConfig(const XmppConfig &config) override { config_ = config; }
    void shutdown() override {}
    void probeAsync(StatusCallback callback) override { callback({ true }); }
    void listNotesAsync(ListCallback callback) override
    {
        XmppListResult result;
        result.ok    = true;
        result.notes = remote_.values();
        callback(std::move(result));
    }
    void getNoteAsync(QString id, NoteCallback callback) override
    {
        XmppNoteResult result;
        result.ok   = remote_.contains(id);
        result.note = remote_.value(id);
        callback(std::move(result));
    }
    void saveNoteAsync(XmppRemoteNote, NoteCallback callback) override
    {
        ++fullSaveCount;
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
    void auditStorageKeysAsync(AuditCallback callback) override
    {
        XmppKeyAuditResult result;
        result.ok = true;
        callback(std::move(result));
    }
    void rekeyStorageAsync(QList<QByteArray>, QByteArray, RekeyCallback callback) override
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
    int                            indexUpdateCount { 0 };
    QList<XmppRemoteNote>          indexRequests;
    QHash<QString, XmppRemoteNote> remote_;
    XmppConfig                     config_;
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
    void reorderPublishesOnlyTheIndex();
};

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

QTEST_MAIN(XmppStorageTest)
#include "xmppstorage_test.moc"
