#include "xmpppersistenttruststorage.h"

#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QXmppTask.h>

#include <algorithm>

namespace AnyKeep {
namespace {
    constexpr quint32 Magic   = 0x514e5453; // QNTS
    constexpr quint16 Version = 1;
}

XmppPersistentTrustStorage::XmppPersistentTrustStorage(QString path, QByteArray encryptionKey, QString accountId) :
    path_(std::move(path)), encryptionKey_(std::move(encryptionKey)), accountId_(std::move(accountId))
{
    load();
}

void XmppPersistentTrustStorage::load()
{
    if (!QFileInfo::exists(path_))
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        error_ = file.errorString();
        return;
    }
    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-omemo-trust"), accountId_, 1,
                                QStringLiteral("omemo-trust") };
    auto              opened = SecureEnvelope::open(file.readAll(), encryptionKey_, context);
    if (!opened) {
        error_ = opened.error.message;
        return;
    }
    QDataStream in(opened.value);
    in.setVersion(QDataStream::Qt_5_10);
    quint32 magic;
    quint16 version;
    quint32 count;
    in >> magic >> version >> count;
    if (magic != Magic || version != Version) {
        error_ = QStringLiteral("Unsupported OMEMO trust format");
        return;
    }
    for (quint32 i = 0; i < count; ++i) {
        Entry entry;
        int   level;
        in >> entry.encryption >> entry.owner >> entry.keyId >> level;
        entry.level = QXmpp::TrustLevel(level);
        entries_.append(entry);
        QXmppTrustMemoryStorage::addKeys(entry.encryption, entry.owner, { entry.keyId }, entry.level);
    }
    if (in.status() != QDataStream::Ok)
        error_ = QStringLiteral("Corrupt OMEMO trust state");
}

void XmppPersistentTrustStorage::persist()
{
    QByteArray  plain;
    QDataStream out(&plain, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << Magic << Version << quint32(entries_.size());
    for (const auto &entry : entries_)
        out << entry.encryption << entry.owner << entry.keyId << int(entry.level);
    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-omemo-trust"), accountId_, 1,
                                QStringLiteral("omemo-trust") };
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

QXmppTask<QHash<QString, QMultiHash<QString, QByteArray>>>
XmppPersistentTrustStorage::setTrustLevel(const QString &encryption, const QMultiHash<QString, QByteArray> &keyIds,
                                          QXmpp::TrustLevel trustLevel)
{
    for (auto it = keyIds.cbegin(); it != keyIds.cend(); ++it) {
        auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &value) {
            return value.encryption == encryption && value.owner == it.key() && value.keyId == it.value();
        });
        if (entry == entries_.end())
            entries_.append({ encryption, it.key(), it.value(), trustLevel });
        else
            entry->level = trustLevel;
    }
    persist();
    return QXmppTrustMemoryStorage::setTrustLevel(encryption, keyIds, trustLevel);
}

QXmppTask<QHash<QString, QMultiHash<QString, QByteArray>>>
XmppPersistentTrustStorage::setTrustLevel(const QString &encryption, const QList<QString> &owners,
                                          QXmpp::TrustLevel oldLevel, QXmpp::TrustLevel newLevel)
{
    for (auto &entry : entries_) {
        if (entry.encryption == encryption && owners.contains(entry.owner) && entry.level == oldLevel)
            entry.level = newLevel;
    }
    persist();
    return QXmppTrustMemoryStorage::setTrustLevel(encryption, owners, oldLevel, newLevel);
}

QXmppTask<void> XmppPersistentTrustStorage::removeKeys(const QString &encryption, const QList<QByteArray> &keyIds)
{
    entries_.removeIf(
        [&](const Entry &entry) { return entry.encryption == encryption && keyIds.contains(entry.keyId); });
    persist();
    return QXmppTrustMemoryStorage::removeKeys(encryption, keyIds);
}

QXmppTask<void> XmppPersistentTrustStorage::removeKeys(const QString &encryption, const QString &owner)
{
    entries_.removeIf([&](const Entry &entry) { return entry.encryption == encryption && entry.owner == owner; });
    persist();
    return QXmppTrustMemoryStorage::removeKeys(encryption, owner);
}

QXmppTask<void> XmppPersistentTrustStorage::removeKeys(const QString &encryption)
{
    entries_.removeIf([&](const Entry &entry) { return entry.encryption == encryption; });
    persist();
    return QXmppTrustMemoryStorage::removeKeys(encryption);
}

QXmppTask<void> XmppPersistentTrustStorage::resetAll(const QString &encryption)
{
    entries_.removeIf([&](const Entry &entry) { return entry.encryption == encryption; });
    persist();
    return QXmppTrustMemoryStorage::resetAll(encryption);
}

} // namespace AnyKeep
