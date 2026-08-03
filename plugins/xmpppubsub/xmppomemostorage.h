#ifndef XMPPOMEMOSTORAGE_H
#define XMPPOMEMOSTORAGE_H

#include <QXmppOmemoStorage.h>

#include <functional>

namespace AnyKeep {

/**
 * @brief Encrypted persistent implementation of QXmppOmemoStorage.
 *
 * All OMEMO state is kept in memory and atomically serialized to an encrypted
 * account-bound file after mutations. The storage is used from the XMPP event
 * loop thread. A prekey-removal hook allows the worker to repair affected
 * server bundles without coupling persistence code to PubSub.
 */
class XmppOmemoStorage final : public QXmppOmemoStorage {
public:
    XmppOmemoStorage(QString path, QByteArray encryptionKey, QString accountId);

    bool       isValid() const { return error_.isEmpty(); }
    QString    errorString() const { return error_; }
    uint32_t   ownDeviceId() const { return data_.ownDevice ? data_.ownDevice->id : 0; }
    QString    ownDeviceLabel() const { return data_.ownDevice ? data_.ownDevice->label : QString {}; }
    QByteArray ownIdentityKey() const { return data_.ownDevice ? data_.ownDevice->publicIdentityKey : QByteArray {}; }
    QXmppTask<void> removeAllDevices();
    QXmppTask<void> resetAllSessions();

    QXmppTask<OmemoData> allData() override;
    QXmppTask<void>      setOwnDevice(const std::optional<OwnDevice> &device) override;
    QXmppTask<void>      addSignedPreKeyPair(uint32_t keyId, const SignedPreKeyPair &keyPair) override;
    QXmppTask<void>      removeSignedPreKeyPair(uint32_t keyId) override;
    QXmppTask<void>      addPreKeyPairs(const QHash<uint32_t, QByteArray> &keyPairs) override;
    QXmppTask<void>      removePreKeyPair(uint32_t keyId) override;
    QXmppTask<void>      addDevice(const QString &jid, uint32_t deviceId, const Device &device) override;
    QXmppTask<void>      removeDevice(const QString &jid, uint32_t deviceId) override;
    QXmppTask<void>      removeDevices(const QString &jid) override;
    QXmppTask<void>      resetAll() override;
    void setPreKeyRemovedHandler(std::function<void(uint32_t)> handler) { preKeyRemovedHandler_ = std::move(handler); }

private:
    void load();
    void persist();

    QString                       path_;          ///< Encrypted state file path.
    QByteArray                    encryptionKey_; ///< Local at-rest encryption key.
    QString                       accountId_;     ///< Account binding included in authenticated data.
    OmemoData                     data_;          ///< In-memory authoritative QXmpp OMEMO state.
    QString                       error_;         ///< Load/validation error that makes the storage unusable.
    std::function<void(uint32_t)> preKeyRemovedHandler_;
};

} // namespace AnyKeep

#endif // XMPPOMEMOSTORAGE_H
