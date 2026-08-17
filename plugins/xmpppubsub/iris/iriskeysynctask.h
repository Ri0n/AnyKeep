#ifndef ANYKEEP_IRISKEYSYNCTASK_H
#define ANYKEEP_IRISKEYSYNCTASK_H

#include <iris/xmpp_encryption.h>
#include <iris/xmpp_jid.h>
#include <iris/xmpp_task.h>

#include <QByteArray>
#include <QHash>
#include <QString>

#include <optional>

namespace AnyKeep {

/** Receives AnyKeep key-sync IQs and keeps enough Iris metadata for deferred encrypted replies. */
class IrisKeySyncTask final : public XMPP::Task {
    Q_OBJECT

public:
    static const QString feature;

    explicit IrisKeySyncTask(XMPP::Client *client);

    void replyWithKey(const QString &requestId, const QString &recoveryKey);
    void replyTrustApproved(const QString &requestId);
    void reject(const QString &requestId);

signals:
    void requestReceived(const QString &requestId, const QString &from, const QByteArray &senderKey);
    void trustRequestReceived(const QString &requestId, const QString &from, const QByteArray &senderKey);

protected:
    bool take(const QDomElement &stanza) override;

private:
    struct PendingRequest {
        QString                                 from;
        QString                                 iqId;
        std::optional<XMPP::EncryptionMetadata> metadata;
    };

    QHash<QString, PendingRequest> pendingRequests_;
};

/** One full-JID request/response exchange for the AnyKeep key-sync protocol. */
class IrisKeySyncRequestTask final : public XMPP::Task {
    Q_OBJECT

public:
    enum class RequestType { TrustBootstrap, StorageKey };

    explicit IrisKeySyncRequestTask(XMPP::Task *parent);

    void request(const XMPP::Jid &to, RequestType type, const QString &requestId, const QByteArray &senderKey = {});

    QString recoveryKey() const { return recoveryKey_; }
    bool    trustApproved() const { return trustApproved_; }

protected:
    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    XMPP::Jid   to_;
    RequestType type_ { RequestType::StorageKey };
    QString     requestId_;
    QByteArray  senderKey_;
    QString     recoveryKey_;
    bool        trustApproved_ { false };
};

} // namespace AnyKeep

#endif // ANYKEEP_IRISKEYSYNCTASK_H
