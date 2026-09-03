#ifndef ANYKEEP_IRISXMPPBACKEND_H
#define ANYKEEP_IRISXMPPBACKEND_H

#include "xmppbackend.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QUuid>

#include <memory>

namespace XMPP {
class AdvancedConnector;
class Client;
class ClientStream;
class Jid;
class OmemoEncryption;
class PubSubManager;
class QCATLSHandler;
class Task;
namespace Jingle {
    class Session;
}
}

namespace AnyKeep {

class IrisKeySyncTask;
struct IrisJingleCapability;
class IrisJinglePublicationProvider;
class IrisOmemoStorage;
class IrisTrustStorage;

/** Iris implementation of the backend-neutral private-notes XMPP service. */
class IrisXmppBackend final : public XmppBackend {
    Q_OBJECT

public:
    explicit IrisXmppBackend(QObject *parent = nullptr);
    ~IrisXmppBackend() override;

    bool supportsMedia() const override { return true; }

    void start() override;
    void setConfig(const XmppConfig &config) override;
    void shutdown() override;

    void probeAsync(StatusCallback callback) override;
    void listNotesAsync(ListCallback callback) override;
    void getNoteAsync(QString id, NoteCallback callback) override;
    void saveNoteAsync(XmppRemoteNote note, NoteCallback callback) override;
    void updateNoteIndexAsync(XmppRemoteNote note, NoteCallback callback) override;
    void deleteNoteAsync(QString id, StatusCallback callback) override;

    XmppDeviceInfo ownOmemoDevice() const override;
    void           ownOmemoDevicesAsync(DevicesCallback callback) override;
    void           ownOmemoBundleValidAsync(StatusCallback callback) override;
    void           repairOwnOmemoDeviceAsync(StatusCallback callback) override;
    void           removeOwnOmemoDeviceAsync(quint32 deviceId, StatusCallback callback) override;
    void           trustOwnOmemoDeviceAsync(QByteArray keyId, StatusCallback callback) override;
    void           trustOwnOmemoDevicesAsync(QList<QByteArray> keyIds, StatusCallback callback) override;

    void auditStorageKeysAsync(AuditCallback callback) override;
    void rekeyStorageAsync(QList<QByteArray> keys, QByteArray canonicalKey, RekeyCallback callback) override;
    void scanObsoleteItemsAsync(CleanupCallback callback) override;
    void deleteObsoleteItemsAsync(QStringList indexItemIds, QStringList contentItemIds,
                                  CleanupCallback callback) override;
    void approveKeySyncRequest(QString requestId) override;
    void rejectKeySyncRequest(QString requestId) override;

private:
    friend class IrisJinglePublicationProvider;
    struct ConnectionAttempt;
    struct ReadyAttempt;

    void resetClient();
    void destroyClientObjects();
    void markDisconnected();
    void createClient();
    void connectToServerAsync(StatusCallback callback);
    void ensureOmemoReadyAsync(StatusCallback callback);
    void ensureReadyAsync(StatusCallback callback);
    void verifyPrivateStorageSupportAsync(StatusCallback callback);
    void ensureNodeAsync(QString nodeName, StatusCallback callback, QString payloadType = {});
    void verifyNodeAsync(QString nodeName, StatusCallback callback);

    void                   requestIndexAsync(QString id, quint64 generation, NoteCallback callback);
    void                   requestNoteAsync(QString id, quint64 generation, NoteCallback callback, int attempt = 1);
    void                   prepareMediaAsync(XmppRemoteNote note, quint64 generation,
                                             std::function<void(XmppRemoteNote, XmppStatusResult)> callback);
    void                   hydrateMediaAsync(XmppRemoteNote note, quint64 generation,
                                             std::function<void(XmppRemoteNote, XmppStatusResult)> callback);
    void                   downloadMediaAsync(XmppRemoteMedia media, quint64 generation,
                                              std::function<void(XmppRemoteMedia, XmppStatusResult)> callback);
    void                   retractPublishedMediaForNoteAsync(QString noteId, StatusCallback callback);
    XMPP::Jingle::Session *createPublishedMediaSession(const IrisJingleCapability &capability,
                                                       const XMPP::Jid            &requester);
    void                   publishNoteAsync(XmppRemoteNote note, quint64 generation, NoteCallback callback);

    void listNodeItemIdsAsync(QString nodeName, std::function<void(QStringList, XmppStatusResult)> callback);
    void fetchPayloadAsync(QString nodeName, QString id,
                           std::function<void(std::optional<XmppEncryptedPayload>, XmppStatusResult)> callback);
    void publishPayloadAsync(QString nodeName, XmppEncryptedPayload payload, StatusCallback callback);
    void retractItemAsync(QString nodeName, QString id, StatusCallback callback);

    void scanNodeForObsoleteItemsAsync(QString nodeName, XmppEncryptedPayload::Kind kind, CleanupCallback callback);

    void refreshOwnOmemoFingerprintsAsync(std::function<void(QSet<quint32>, XmppStatusResult)> callback);
    void onlinePrivateNotesResourcesAsync(std::function<void(QStringList, QString)> callback);
    void requestStorageKeyFromResourceAsync(QString fullJid, AuditCallback callback);
    void handleKeySyncRequest(const QString &requestId, const QString &from, const QByteArray &senderKey);
    void handleKeySyncTrustRequest(const QString &requestId, const QString &from, const QByteArray &senderKey);
    void finishKeySyncTrustRequest(const QString &requestId, const QByteArray &senderKey);

    void             completeStatusForTask(XMPP::Task *task, StatusCallback callback, QString context = {});
    XmppStatusResult taskFailure(const XMPP::Task *task, QString context = {}) const;
    XmppStatusResult cancelledResult() const;

    static QString newUuid();
    static bool    sameConfig(const XmppConfig &left, const XmppConfig &right);

    XmppConfig config_;

    XMPP::AdvancedConnector       *connector_ { nullptr };
    XMPP::QCATLSHandler           *tlsHandler_ { nullptr };
    XMPP::ClientStream            *stream_ { nullptr };
    XMPP::Client                  *client_ { nullptr };
    XMPP::PubSubManager           *pubSub_ { nullptr };
    IrisKeySyncTask               *keySyncTask_ { nullptr };
    IrisJinglePublicationProvider *jinglePublicationProvider_ { nullptr };
    IrisOmemoStorage              *omemoStorage_ { nullptr };
    IrisTrustStorage              *trustStorage_ { nullptr };
    XMPP::OmemoEncryption         *omemo_ { nullptr };

    bool    acceptingWork_ { true };
    bool    connected_ { false };
    bool    omemoReady_ { false };
    bool    prepared_ { false };
    bool    freshClientRequired_ { false };
    quint64 generation_ { 0 };

    std::shared_ptr<ConnectionAttempt> connectionAttempt_;
    std::shared_ptr<ReadyAttempt>      omemoAttempt_;
    std::shared_ptr<ReadyAttempt>      readyAttempt_;

    struct PendingInboundKeyRequest {
        QByteArray senderKey;
        bool       trustBootstrap { false };
    };
    QHash<QString, PendingInboundKeyRequest> pendingInboundKeyRequests_;
};

} // namespace AnyKeep

#endif // ANYKEEP_IRISXMPPBACKEND_H
