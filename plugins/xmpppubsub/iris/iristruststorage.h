#ifndef ANYKEEP_IRISTRUSTSTORAGE_H
#define ANYKEEP_IRISTRUSTSTORAGE_H

#include <iris/xmpp_encryption.h>

#include <QList>

namespace AnyKeep {

/** Encrypted persistent implementation of Iris' generic encryption trust store. */
class IrisTrustStorage final : public XMPP::EncryptionTrustStorage {
public:
    IrisTrustStorage(QString path, QByteArray encryptionKey, QString accountId);

    bool    isValid() const { return error_.isEmpty(); }
    QString errorString() const { return error_; }

    XMPP::EncryptionTrustLevel trustLevel(const QString &methodId, const XMPP::Jid &owner,
                                          const QByteArray &keyId) const override;
    bool                       setTrustLevel(const QString &methodId, const XMPP::Jid &owner, const QByteArray &keyId,
                                             XMPP::EncryptionTrustLevel level) override;
    bool removeTrust(const QString &methodId, const XMPP::Jid &owner, const QByteArray &keyId) override;

private:
    struct Entry {
        QString                    methodId;
        QString                    owner;
        QByteArray                 keyId;
        XMPP::EncryptionTrustLevel level { XMPP::EncryptionTrustLevel::Undecided };
    };

    void load();
    bool persist();

    QString      path_;
    QByteArray   encryptionKey_;
    QString      accountId_;
    QList<Entry> entries_;
    QString      error_;
};

} // namespace AnyKeep

#endif // ANYKEEP_IRISTRUSTSTORAGE_H
