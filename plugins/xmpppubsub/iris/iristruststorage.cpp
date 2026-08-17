#include "iristruststorage.h"

#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

namespace AnyKeep {
namespace {
    constexpr quint32 Magic   = 0x494e5453; // INTS
    constexpr quint16 Version = 1;

    QString normalizedOwner(const XMPP::Jid &owner) { return owner.bare().isEmpty() ? owner.full() : owner.bare(); }
} // namespace

IrisTrustStorage::IrisTrustStorage(QString path, QByteArray encryptionKey, QString accountId) :
    path_(std::move(path)), encryptionKey_(std::move(encryptionKey)), accountId_(std::move(accountId))
{
    load();
}

void IrisTrustStorage::load()
{
    if (!QFileInfo::exists(path_))
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        error_ = file.errorString();
        return;
    }
    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-iris-omemo-trust"), accountId_, 1,
                                QStringLiteral("omemo-trust") };
    const auto        opened = SecureEnvelope::open(file.readAll(), encryptionKey_, context);
    if (!opened) {
        error_ = opened.error.message;
        return;
    }
    QDataStream in(opened.value);
    in.setVersion(QDataStream::Qt_5_10);
    quint32 magic   = 0;
    quint16 version = 0;
    quint32 count   = 0;
    in >> magic >> version >> count;
    if (magic != Magic || version != Version) {
        error_ = QStringLiteral("Unsupported Iris OMEMO trust format");
        return;
    }
    for (quint32 i = 0; i < count; ++i) {
        Entry  entry;
        quint8 level = 0;
        in >> entry.methodId >> entry.owner >> entry.keyId >> level;
        switch (XMPP::EncryptionTrustLevel(level)) {
        case XMPP::EncryptionTrustLevel::Undecided:
        case XMPP::EncryptionTrustLevel::AutomaticallyTrusted:
        case XMPP::EncryptionTrustLevel::ManuallyTrusted:
        case XMPP::EncryptionTrustLevel::Authenticated:
        case XMPP::EncryptionTrustLevel::Distrusted:
            entry.level = XMPP::EncryptionTrustLevel(level);
            entries_.append(std::move(entry));
            break;
        }
    }
    if (in.status() != QDataStream::Ok)
        error_ = QStringLiteral("Corrupt Iris OMEMO trust state");
}

bool IrisTrustStorage::persist()
{
    QByteArray  plain;
    QDataStream out(&plain, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << Magic << Version << quint32(entries_.size());
    for (const auto &entry : entries_)
        out << entry.methodId << entry.owner << entry.keyId << quint8(entry.level);

    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("private-notes-iris-omemo-trust"), accountId_, 1,
                                QStringLiteral("omemo-trust") };
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

XMPP::EncryptionTrustLevel IrisTrustStorage::trustLevel(const QString &methodId, const XMPP::Jid &owner,
                                                        const QByteArray &keyId) const
{
    const auto ownerId = normalizedOwner(owner);
    const auto it      = std::find_if(entries_.cbegin(), entries_.cend(), [&](const Entry &entry) {
        return entry.methodId == methodId && entry.owner == ownerId && entry.keyId == keyId;
    });
    return it == entries_.cend() ? XMPP::EncryptionTrustLevel::Undecided : it->level;
}

bool IrisTrustStorage::setTrustLevel(const QString &methodId, const XMPP::Jid &owner, const QByteArray &keyId,
                                     XMPP::EncryptionTrustLevel level)
{
    const auto ownerId = normalizedOwner(owner);
    auto       it      = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) {
        return entry.methodId == methodId && entry.owner == ownerId && entry.keyId == keyId;
    });
    if (it == entries_.end())
        entries_.append({ methodId, ownerId, keyId, level });
    else
        it->level = level;
    return persist();
}

bool IrisTrustStorage::removeTrust(const QString &methodId, const XMPP::Jid &owner, const QByteArray &keyId)
{
    const auto ownerId = normalizedOwner(owner);
    entries_.removeIf([&](const Entry &entry) {
        return entry.methodId == methodId && entry.owner == ownerId && entry.keyId == keyId;
    });
    return persist();
}

} // namespace AnyKeep
