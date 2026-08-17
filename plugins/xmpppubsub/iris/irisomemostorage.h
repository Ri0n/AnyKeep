#ifndef ANYKEEP_IRISOMEMOSTORAGE_H
#define ANYKEEP_IRISOMEMOSTORAGE_H

#include <iris/xmpp_omemostorage.h>

namespace AnyKeep {

/** Encrypted persistent Iris OMEMO state, separate from the QXmpp state file. */
class IrisOmemoStorage final : public XMPP::OmemoStorage {
public:
    IrisOmemoStorage(QString path, QByteArray encryptionKey, QString accountId);

    bool       isValid() const { return error_.isEmpty(); }
    QString    errorString() const { return error_; }
    uint32_t   ownDeviceId() const { return data_.ownDevice ? data_.ownDevice->id : 0; }
    QString    ownDeviceLabel() const { return data_.ownDevice ? data_.ownDevice->label : QString {}; }
    QByteArray ownIdentityKey() const { return data_.ownDevice ? data_.ownDevice->publicIdentityKey : QByteArray {}; }
    bool       resetSessions();

    OmemoData allData() const override;
    bool      setOwnDevice(const std::optional<OwnDevice> &device) override;
    bool      addSignedPreKeyPair(uint32_t keyId, const SignedPreKeyPair &keyPair) override;
    bool      removeSignedPreKeyPair(uint32_t keyId) override;
    bool      addPreKeyPairs(const QHash<uint32_t, QByteArray> &keyPairs) override;
    bool      removePreKeyPair(uint32_t keyId) override;
    bool      addDevice(const QString &jid, uint32_t deviceId, const Device &device) override;
    bool      removeDevice(const QString &jid, uint32_t deviceId) override;
    bool      removeDevices(const QString &jid) override;
    bool      resetAll() override;

private:
    void load();
    bool persist();

    QString    path_;
    QByteArray encryptionKey_;
    QString    accountId_;
    OmemoData  data_;
    QString    error_;
};

} // namespace AnyKeep

#endif // ANYKEEP_IRISOMEMOSTORAGE_H
