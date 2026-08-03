#include "xmppomemostorage.h"

#include "secureenvelope.h"

#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QXmppPromise.h>

namespace AnyKeep {
namespace {
    constexpr quint32 Magic   = 0x514e4f53; // QNOS
    constexpr quint16 Version = 1;

    template <typename T> QXmppTask<T> ready(T value)
    {
        QXmppPromise<T> promise;
        auto            task = promise.task();
        promise.finish(std::move(value));
        return task;
    }

    QXmppTask<void> done()
    {
        QXmppPromise<void> promise;
        auto               task = promise.task();
        promise.finish();
        return task;
    }

    void writeDevice(QDataStream &out, const QXmppOmemoStorage::Device &device)
    {
        out << device.label << device.keyId << device.session << device.unrespondedSentStanzasCount
            << device.unrespondedReceivedStanzasCount << device.removalFromDeviceListDate;
    }

    void readDevice(QDataStream &in, QXmppOmemoStorage::Device &device)
    {
        in >> device.label >> device.keyId >> device.session >> device.unrespondedSentStanzasCount
            >> device.unrespondedReceivedStanzasCount >> device.removalFromDeviceListDate;
    }
}

XmppOmemoStorage::XmppOmemoStorage(QString path, QByteArray encryptionKey, QString accountId) :
    path_(std::move(path)), encryptionKey_(std::move(encryptionKey)), accountId_(std::move(accountId))
{
    load();
}

void XmppOmemoStorage::load()
{
    if (!QFileInfo::exists(path_))
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        error_ = file.errorString();
        return;
    }
    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-omemo-state"), accountId_, 1,
                                QStringLiteral("omemo-state") };
    auto              opened = SecureEnvelope::open(file.readAll(), encryptionKey_, context);
    if (!opened) {
        error_ = opened.error.message;
        return;
    }
    QDataStream in(opened.value);
    in.setVersion(QDataStream::Qt_5_10);
    quint32 magic;
    quint16 version;
    bool    hasOwn;
    in >> magic >> version >> hasOwn;
    if (magic != Magic || version != Version) {
        error_ = QStringLiteral("Unsupported OMEMO state format");
        return;
    }
    if (hasOwn) {
        OwnDevice own;
        in >> own.id >> own.label >> own.privateIdentityKey >> own.publicIdentityKey >> own.latestSignedPreKeyId
            >> own.latestPreKeyId;
        data_.ownDevice = own;
    }
    quint32 count;
    in >> count;
    for (quint32 i = 0; i < count; ++i) {
        quint32          id;
        SignedPreKeyPair pair;
        in >> id >> pair.creationDate >> pair.data;
        data_.signedPreKeyPairs.insert(id, pair);
    }
    in >> count;
    for (quint32 i = 0; i < count; ++i) {
        quint32    id;
        QByteArray pair;
        in >> id >> pair;
        data_.preKeyPairs.insert(id, pair);
    }
    in >> count;
    for (quint32 i = 0; i < count; ++i) {
        QString jid;
        quint32 deviceCount;
        in >> jid >> deviceCount;
        for (quint32 j = 0; j < deviceCount; ++j) {
            quint32 id;
            Device  device;
            in >> id;
            readDevice(in, device);
            data_.devices[jid].insert(id, device);
        }
    }
    if (in.status() != QDataStream::Ok)
        error_ = QStringLiteral("Corrupt OMEMO state");
}

void XmppOmemoStorage::persist()
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
        for (auto it = owner.value().cbegin(); it != owner.value().cend(); ++it) {
            out << it.key();
            writeDevice(out, it.value());
        }
    }
    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-omemo-state"), accountId_, 1,
                                QStringLiteral("omemo-state") };
    auto              sealed = SecureEnvelope::seal(plain, encryptionKey_, context);
    if (!sealed) {
        error_ = sealed.error.message;
        return;
    }
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(sealed.value) != sealed.value.size() || !file.commit()) {
        error_ = file.errorString();
        return;
    }
    QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    error_.clear();
}

QXmppTask<QXmppOmemoStorage::OmemoData> XmppOmemoStorage::allData() { return ready(data_); }
QXmppTask<void>                         XmppOmemoStorage::setOwnDevice(const std::optional<OwnDevice> &value)
{
    auto merged = value;
    if (merged && data_.ownDevice) {
        // Identity keys are immutable for the lifetime of an OMEMO device. Some QXmpp update paths
        // only modify pre-key counters and pass an OwnDevice value without filling the identity
        // fields. Never let such a partial update destroy the keys and produce a bundle without <ik>.
        if (merged->privateIdentityKey.isEmpty())
            merged->privateIdentityKey = data_.ownDevice->privateIdentityKey;
        if (merged->publicIdentityKey.isEmpty())
            merged->publicIdentityKey = data_.ownDevice->publicIdentityKey;
        if (merged->label.isEmpty())
            merged->label = data_.ownDevice->label;
    }
    qInfo() << "OMEMO own device state write: id=" << (merged ? merged->id : 0)
            << "private-key-size=" << (merged ? merged->privateIdentityKey.size() : 0)
            << "public-key-size=" << (merged ? merged->publicIdentityKey.size() : 0);
    data_.ownDevice = std::move(merged);
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::addSignedPreKeyPair(uint32_t id, const SignedPreKeyPair &value)
{
    data_.signedPreKeyPairs[id] = value;
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::removeSignedPreKeyPair(uint32_t id)
{
    data_.signedPreKeyPairs.remove(id);
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::addPreKeyPairs(const QHash<uint32_t, QByteArray> &values)
{
    data_.preKeyPairs.insert(values);
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::removePreKeyPair(uint32_t id)
{
    data_.preKeyPairs.remove(id);
    persist();
    if (preKeyRemovedHandler_)
        preKeyRemovedHandler_(id);
    return done();
}
QXmppTask<void> XmppOmemoStorage::addDevice(const QString &jid, uint32_t id, const Device &value)
{
    qInfo().noquote() << "OMEMO peer state write: jid=" << jid << "device=" << id
                      << "key-id-size=" << value.keyId.size() << "session-size=" << value.session.size();
    data_.devices[jid][id] = value;
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::removeDevice(const QString &jid, uint32_t id)
{
    data_.devices[jid].remove(id);
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::removeDevices(const QString &jid)
{
    data_.devices.remove(jid);
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::removeAllDevices()
{
    qsizetype deviceCount = 0;
    for (auto owner = data_.devices.cbegin(); owner != data_.devices.cend(); ++owner)
        deviceCount += owner.value().size();
    qInfo() << "Clearing all cached OMEMO peer states: owners=" << data_.devices.size() << "devices=" << deviceCount;
    data_.devices.clear();
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::resetAllSessions()
{
    qsizetype deviceCount = 0;
    for (auto owner = data_.devices.begin(); owner != data_.devices.end(); ++owner) {
        for (auto device = owner.value().begin(); device != owner.value().end(); ++device) {
            device->session.clear();
            device->unrespondedSentStanzasCount     = 0;
            device->unrespondedReceivedStanzasCount = 0;
            ++deviceCount;
        }
    }
    qInfo() << "Reset cached OMEMO peer sessions while preserving fingerprints: devices=" << deviceCount;
    persist();
    return done();
}
QXmppTask<void> XmppOmemoStorage::resetAll()
{
    data_ = {};
    persist();
    return done();
}

} // namespace AnyKeep
