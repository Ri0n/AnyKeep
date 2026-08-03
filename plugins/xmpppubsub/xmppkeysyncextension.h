#ifndef XMPPKEYSYNCEXTENSION_H
#define XMPPKEYSYNCEXTENSION_H

#include <QHash>
#include <QXmppClientExtension.h>
#include <QXmppE2eeMetadata.h>
#include <QXmppIq.h>

#include <optional>

namespace AnyKeep {

/**
 * @brief Implements AnyKeep's private key-sync IQ protocol.
 *
 * Requests and responses are addressed to full JIDs. Recovery-key replies may
 * be OMEMO encrypted; QXmppE2eeMetadata retained in PendingRequest lets the
 * response follow the encryption context of the incoming request.
 */
class XmppKeySyncExtension final : public QXmppClientExtension {
    Q_OBJECT

public:
    static const QString feature;

    QStringList discoveryFeatures() const override;
    bool        handleStanza(const QDomElement &stanza, const std::optional<QXmppE2eeMetadata> &e2eeMetadata) override;

    QXmppIq makeRequest(const QString &to, const QString &requestId) const;
    QXmppIq makeTrustRequest(const QString &to, const QString &requestId, const QByteArray &senderKey) const;
    void    replyWithKey(const QString &requestId, const QString &recoveryKey);
    void    replyTrustApproved(const QString &requestId);
    void    reject(const QString &requestId);

    static QString responseRecoveryKey(const QDomElement &iq, const QString &requestId);
    static bool    isTrustApproved(const QDomElement &iq, const QString &requestId);

signals:
    void requestReceived(const QString &requestId, const QString &from, const QByteArray &senderKey);
    void trustRequestReceived(const QString &requestId, const QString &from, const QByteArray &senderKey);

private:
    /// Information needed to send a deferred result to an inbound IQ request.
    struct PendingRequest {
        QString                          from;
        QString                          iqId;
        std::optional<QXmppE2eeMetadata> metadata;
    };

    /// Deferred inbound requests keyed by application-level request ID.
    QHash<QString, PendingRequest> pendingRequests_;
};

} // namespace AnyKeep

#endif // XMPPKEYSYNCEXTENSION_H
