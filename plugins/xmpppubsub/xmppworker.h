#ifndef XMPPWORKER_H
#define XMPPWORKER_H

#include "xmppbackend.h"
#include "xmppomemopubsubitems.h"

#include <QCoroTask>
#include <QFutureInterface>
#include <QObject>
#include <QSet>
#include <QXmppError.h>

#include <memory>

class QXmppClient;
class QXmppDiscoveryManager;
class QXmppPubSubManager;
class QXmppRosterManager;
class QXmppOmemoManager;
class QXmppTrustManager;
class QXmppTrustStorage;

namespace QtNote {

class XmppPepExtension;
class XmppOmemoStorage;
class XmppKeySyncExtension;

/**
 * @brief QXmpp implementation of the backend-neutral XmppBackend contract.
 *
 * The worker is event-driven and intentionally runs in one Qt event-loop
 * thread; QCoro is used to express ordered stanza exchanges without nested
 * QEventLoop instances. QXmpp managers are created and destroyed together by
 * resetClient().
 *
 * clientGeneration_ invalidates coroutine continuations when the QXmpp client
 * is replaced. This is the protocol-layer counterpart of XmppStorage's
 * configEpoch_: the former protects raw QXmpp object lifetimes, while the
 * latter protects storage/cache state across configuration changes.
 */
class XmppWorker final : public XmppBackend {
    Q_OBJECT

public:
    explicit XmppWorker(QObject *parent = nullptr);
    ~XmppWorker() override;

    void start() override;
    void setConfig(const XmppConfig &config) override;
    void shutdown() override;

    void           probeAsync(StatusCallback callback) override;
    void           listNotesAsync(ListCallback callback) override;
    void           getNoteAsync(QString id, NoteCallback callback) override;
    void           saveNoteAsync(XmppRemoteNote localNote, NoteCallback callback) override;
    void           deleteNoteAsync(QString id, StatusCallback callback) override;
    XmppDeviceInfo ownOmemoDevice() const override;
    void           ownOmemoDevicesAsync(DevicesCallback callback) override;
    void           ownOmemoBundleValidAsync(StatusCallback callback) override;
    void           repairOwnOmemoDeviceAsync(StatusCallback callback) override;
    void           trustOwnOmemoDeviceAsync(QByteArray keyId, StatusCallback callback) override;
    void           trustOwnOmemoDevicesAsync(QList<QByteArray> keyIds, StatusCallback callback) override;
    void           auditStorageKeysAsync(AuditCallback callback) override;
    void           rekeyStorageAsync(QList<QByteArray> keys, QByteArray canonicalKey, RekeyCallback callback) override;
    void           approveKeySyncRequest(QString requestId) override;
    void           rejectKeySyncRequest(QString requestId) override;

private:
    struct ConnectionAttempt;

    void resetClient();
    void createClient();
    void handleKeySyncRequest(const QString &requestId, const QString &from, const QByteArray &senderKey);
    void handleKeySyncTrustRequest(const QString &requestId, const QString &from, const QByteArray &senderKey);
    void finishKeySyncTrustRequest(const QString &requestId, const QByteArray &senderKey);
    void cacheOwnOmemoBundle();
    void scheduleOwnOmemoBundleRepair(uint32_t consumedPreKeyId);
    void repairOwnOmemoBundleAfterPreKeyUse(int attemptsRemaining = 8);
    void connectToServerAsync(StatusCallback callback);
    QCoro::Task<XmppStatusResult> connectToServerTask();
    QCoro::Task<XmppStatusResult> verifyPrivateStorageSupportTask();
    /** Coroutine arguments used after co_await are owned by the coroutine frame. */
    QCoro::Task<XmppStatusResult> verifyNodeTask(QString nodeName);
    QCoro::Task<XmppStatusResult> ensureNodeTask(QString nodeName);
    QCoro::Task<XmppStatusResult> ensureOmemoTask();
    QCoro::Task<XmppStatusResult> ensureOmemoReadyTask();
    QCoro::Task<XmppStatusResult> ensureReadyTask();

    QCoro::Task<XmppListResult>   listNotesTask();
    QCoro::Task<XmppNoteResult>   requestNoteTask(QString id, quint64 clientGeneration);
    QCoro::Task<XmppNoteResult>   getNoteTask(QString id);
    QCoro::Task<XmppNoteResult>   publishNoteTask(XmppRemoteNote note, quint64 clientGeneration);
    QCoro::Task<XmppNoteResult>   saveNoteTask(XmppRemoteNote localNote);
    QCoro::Task<XmppStatusResult> deleteNoteTask(QString id);
    QCoro::Task<std::pair<QList<XmppDeviceInfo>, QString>> ownOmemoDevicesTask();
    QCoro::Task<XmppStatusResult>                          ownOmemoBundleValidTask();
    QCoro::Task<XmppStatusResult>                          repairOwnOmemoDeviceTask();
    QCoro::Task<XmppStatusResult>                          trustOwnOmemoDeviceTask(QByteArray keyId);
    QCoro::Task<XmppStatusResult>                          trustOwnOmemoDevicesTask(QList<QByteArray> keyIds);
    QCoro::Task<std::pair<QStringList, QString>>           onlineQtNoteResourcesTask();
    QCoro::Task<XmppKeyAuditResult>                        auditStorageKeysTask();
    QCoro::Task<XmppRekeyResult> rekeyStorageTask(QList<QByteArray> keys, QByteArray canonicalKey);
    QCoro::Task<>                approveKeySyncRequestTask(QString requestId);
    QCoro::Task<>                handleKeySyncRequestTask(QString requestId, QString from, QByteArray senderKey);
    QCoro::Task<>                cacheOwnOmemoBundleTask();
    QCoro::Task<>                repairOwnOmemoBundleAfterPreKeyUseTask(int attemptsRemaining);
    static QString               newUuid();
    static QString               errorText(const QXmppError &error);

    /// Configuration snapshot used to create the current client.
    XmppConfig config_;
    /// QXmpp client and extensions/managers belonging to the current generation.
    QXmppClient           *client_ { nullptr };
    QXmppDiscoveryManager *discovery_ { nullptr };
    QXmppRosterManager    *roster_ { nullptr };
    QXmppPubSubManager    *pubSub_ { nullptr };
    XmppPepExtension      *pepExtension_ { nullptr };
    XmppKeySyncExtension  *keySyncExtension_ { nullptr };
    XmppOmemoStorage      *omemoStorage_ { nullptr };
    QXmppTrustStorage     *trustStorage_ { nullptr };
    QXmppTrustManager     *trustManager_ { nullptr };
    QXmppOmemoManager     *omemoManager_ { nullptr };
    /// Whether connection and PubSub node preparation completed successfully.
    bool prepared_ { false };
    /// Whether the OMEMO manager and its local state are ready for encryption.
    bool omemoReady_ { false };
    /// False after shutdown(); start() is required before accepting more work.
    bool acceptingWork_ { true };
    /**
     * @brief Lifetime generation of client_ and all manager pointers.
     *
     * resetClient() increments this counter. Coroutines capture it before an
     * await point and abort if it changed, so they never dereference managers
     * from a destroyed QXmpp client.
     */
    quint64 clientGeneration_ { 0 };
    /**
     * One physical QXmpp connection attempt shared by every operation that
     * needs the account online. Without this fan-in, settings refresh and key
     * recovery can call connectToServer() concurrently on the same client.
     */
    std::shared_ptr<ConnectionAttempt> connectionAttempt_;
    /** Shared connection plus OMEMO initialization attempt. */
    std::shared_ptr<QFutureInterface<XmppStatusResult>> omemoReadinessAttempt_;
    /** Shared full connection/OMEMO/node preparation attempt. */
    std::shared_ptr<QFutureInterface<XmppStatusResult>> readinessAttempt_;
    /// Last valid own bundle, used to repair QXmpp publications missing fields.
    std::optional<XmppOmemoBundleItem> cachedOwnOmemoBundle_;
    /// Prekeys consumed locally and therefore excluded from a repaired bundle.
    QSet<uint32_t> consumedOwnPreKeyIds_;
    /// Coalesces several consumed-prekey notifications into one repair attempt.
    bool ownBundleRepairScheduled_ { false };
    /// Inbound recovery request waiting for explicit trust/user approval.
    struct PendingInboundKeyRequest {
        QByteArray senderKey;
        bool       trustBootstrap { false };
    };
    /// Pending requests keyed by the custom key-sync request ID.
    QHash<QString, PendingInboundKeyRequest> pendingInboundKeyRequests_;
};

} // namespace QtNote

#endif // XMPPWORKER_H
