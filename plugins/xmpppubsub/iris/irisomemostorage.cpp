#include "irisomemostorage.h"

#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace AnyKeep {
namespace {
    constexpr quint32 Magic   = 0x494e4f53; // INOS
    constexpr quint16 Version = 1;

    void writeProtocolState(QDataStream &out, const XMPP::OmemoStorage::DeviceProtocolState &state)
    {
        out << state.label << state.labelSignature << state.labelVerified << state.keyId << state.session
            << state.lastReceivedRatchetKey << state.unrespondedSentStanzasCount
            << state.unrespondedReceivedStanzasCount << state.removalFromDeviceListDate;
    }

    void readProtocolState(QDataStream &in, XMPP::OmemoStorage::DeviceProtocolState &state)
    {
        in >> state.label >> state.labelSignature >> state.labelVerified >> state.keyId >> state.session
            >> state.lastReceivedRatchetKey >> state.unrespondedSentStanzasCount
            >> state.unrespondedReceivedStanzasCount >> state.removalFromDeviceListDate;
    }
} // namespace

IrisOmemoStorage::IrisOmemoStorage(QString path, QByteArray encryptionKey, QString accountId) :
    path_(std::move(path)), encryptionKey_(std::move(encryptionKey)), accountId_(std::move(accountId))
{
    load();
}

void IrisOmemoStorage::load()
{
    if (!QFileInfo::exists(path_))
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        error_ = file.errorString();
        return;
    }
    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-iris-omemo-state"), accountId_, 1,
                                QStringLiteral("omemo-state") };
    const auto        opened = SecureEnvelope::open(file.readAll(), encryptionKey_, context);
    if (!opened) {
        error_ = opened.error.message;
        return;
    }

    QDataStream in(opened.value);
    in.setVersion(QDataStream::Qt_5_10);
    quint32 magic   = 0;
    quint16 version = 0;
    bool    hasOwn  = false;
    in >> magic >> version >> hasOwn;
    if (magic != Magic || version != Version) {
        error_ = QStringLiteral("Unsupported Iris OMEMO state format");
        return;
    }
    if (hasOwn) {
        OwnDevice own;
        in >> own.id >> own.label >> own.privateIdentityKey >> own.publicIdentityKey >> own.latestSignedPreKeyId
            >> own.latestPreKeyId;
        data_.ownDevice = std::move(own);
    }

    quint32 count = 0;
    in >> count;
    for (quint32 i = 0; i < count; ++i) {
        quint32          id = 0;
        SignedPreKeyPair pair;
        in >> id >> pair.creationDate >> pair.data;
        data_.signedPreKeyPairs.insert(id, pair);
    }
    in >> count;
    for (quint32 i = 0; i < count; ++i) {
        quint32    id = 0;
        QByteArray pair;
        in >> id >> pair;
        data_.preKeyPairs.insert(id, pair);
    }
    in >> count;
    for (quint32 i = 0; i < count; ++i) {
        QString owner;
        quint32 deviceCount = 0;
        in >> owner >> deviceCount;
        for (quint32 d = 0; d < deviceCount; ++d) {
            quint32 deviceId      = 0;
            quint32 protocolCount = 0;
            Device  device;
            in >> deviceId >> protocolCount;
            for (quint32 p = 0; p < protocolCount; ++p) {
                quint8              protocol = 0;
                DeviceProtocolState state;
                in >> protocol;
                readProtocolState(in, state);
                if (protocol == quint8(XMPP::OmemoProtocol::Legacy) || protocol == quint8(XMPP::OmemoProtocol::Omemo2))
                    device.protocols.insert(XMPP::OmemoProtocol(protocol), std::move(state));
            }
            data_.devices[owner].insert(deviceId, std::move(device));
        }
    }
    if (in.status() != QDataStream::Ok)
        error_ = QStringLiteral("Corrupt Iris OMEMO state");
}

bool IrisOmemoStorage::persist()
{
    QByteArray  plain;
    QDataStream out(&plain, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << Magic << Version << data_.ownDevice.has_value();
    if (data_.ownDevice) {
        const auto &own = *data_.ownDevice;
        out << own.id << own.label << own.privateIdentityKey << own.publicIdentityKey << own.latestSignedPreKeyId
            << own.latestPreKeyId;
    }
    out << quint32(data_.signedPreKeyPairs.size());
    for (auto it = data_.signedPreKeyPairs.cbegin(); it != data_.signedPreKeyPairs.cend(); ++it)
        out << it.key() << it.value().creationDate << it.value().data;
    out << quint32(data_.preKeyPairs.size());
    for (auto it = data_.preKeyPairs.cbegin(); it != data_.preKeyPairs.cend(); ++it)
        out << it.key() << it.value();
    out << quint32(data_.devices.size());
    for (auto owner = data_.devices.cbegin(); owner != data_.devices.cend(); ++owner) {
        out << owner.key() << quint32(owner.value().size());
        for (auto device = owner.value().cbegin(); device != owner.value().cend(); ++device) {
            out << device.key() << quint32(device.value().protocols.size());
            for (auto protocol = device.value().protocols.cbegin(); protocol != device.value().protocols.cend();
                 ++protocol) {
                out << quint8(protocol.key());
                writeProtocolState(out, protocol.value());
            }
        }
    }

    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-iris-omemo-state"), accountId_, 1,
                                QStringLiteral("omemo-state") };
    const auto        sealed = SecureEnvelope::seal(plain, encryptionKey_, context);
    if (!sealed) {
        error_ = sealed.error.message;
        return false;
    }
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(sealed.value) != sealed.value.size() || !file.commit()) {
        error_ = file.errorString();
        return false;
    }
    QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    error_.clear();
    return true;
}

bool IrisOmemoStorage::resetSessions()
{
    for (auto owner = data_.devices.begin(); owner != data_.devices.end(); ++owner) {
        for (auto device = owner->begin(); device != owner->end(); ++device) {
            for (auto protocol = device->protocols.begin(); protocol != device->protocols.end(); ++protocol) {
                protocol->session.clear();
                protocol->lastReceivedRatchetKey.clear();
                protocol->unrespondedSentStanzasCount     = 0;
                protocol->unrespondedReceivedStanzasCount = 0;
            }
        }
    }
    return persist();
}

XMPP::OmemoStorage::OmemoData IrisOmemoStorage::allData() const { return data_; }

bool IrisOmemoStorage::setOwnDevice(const std::optional<OwnDevice> &device)
{
    auto merged = device;
    if (merged && data_.ownDevice) {
        if (merged->privateIdentityKey.isEmpty())
            merged->privateIdentityKey = data_.ownDevice->privateIdentityKey;
        if (merged->publicIdentityKey.isEmpty())
            merged->publicIdentityKey = data_.ownDevice->publicIdentityKey;
        if (merged->label.isEmpty())
            merged->label = data_.ownDevice->label;
    }
    data_.ownDevice = std::move(merged);
    return persist();
}

bool IrisOmemoStorage::addSignedPreKeyPair(uint32_t keyId, const SignedPreKeyPair &keyPair)
{
    data_.signedPreKeyPairs.insert(keyId, keyPair);
    return persist();
}

bool IrisOmemoStorage::removeSignedPreKeyPair(uint32_t keyId)
{
    data_.signedPreKeyPairs.remove(keyId);
    return persist();
}

bool IrisOmemoStorage::addPreKeyPairs(const QHash<uint32_t, QByteArray> &keyPairs)
{
    for (auto it = keyPairs.cbegin(); it != keyPairs.cend(); ++it)
        data_.preKeyPairs.insert(it.key(), it.value());
    return persist();
}

bool IrisOmemoStorage::removePreKeyPair(uint32_t keyId)
{
    data_.preKeyPairs.remove(keyId);
    return persist();
}

bool IrisOmemoStorage::addDevice(const QString &jid, uint32_t deviceId, const Device &device)
{
    data_.devices[jid][deviceId] = device;
    return persist();
}

bool IrisOmemoStorage::removeDevice(const QString &jid, uint32_t deviceId)
{
    auto owner = data_.devices.find(jid);
    if (owner != data_.devices.end()) {
        owner->remove(deviceId);
        if (owner->isEmpty())
            data_.devices.erase(owner);
    }
    return persist();
}

bool IrisOmemoStorage::removeDevices(const QString &jid)
{
    data_.devices.remove(jid);
    return persist();
}

bool IrisOmemoStorage::resetAll()
{
    data_ = {};
    return persist();
}

} // namespace AnyKeep
