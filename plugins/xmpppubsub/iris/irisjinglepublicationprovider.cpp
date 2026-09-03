#include "irisjinglepublicationprovider.h"

#include "irisxmppbackend.h"
#include "secureenvelope.h"

#include <iris/jingle-ft.h>

#include <QDataStream>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace AnyKeep {
namespace {

    constexpr quint32 Magic       = 0x414b4a50; // AKJP
    constexpr quint16 Version     = 1;
    constexpr quint32 MaximumSize = 100000;

    AeadContext storeContext(const XmppConfig &config)
    {
        return { KeyDomain::LocalRemoteCache, QStringLiteral("anykeep-jingle-publications"),
                 config.instanceId + QLatin1Char('|') + config.jid, 1, QStringLiteral("jingle-publications") };
    }

    bool sameCiphertext(const IrisJingleCapability &left, const IrisJingleCapability &right)
    {
        return left.from == right.from && left.node == right.node && left.noteId == right.noteId
            && left.reference.id == right.reference.id && left.reference.blobId == right.reference.blobId
            && left.reference.size == right.reference.size && left.reference.checksum == right.reference.checksum
            && left.cipher == right.cipher && left.key == right.key && left.iv == right.iv
            && left.cipherHash == right.cipherHash && left.wireSize == right.wireSize;
    }

    bool publicationFile(const XMPP::Jingle::JinglePub &publication, XMPP::Jingle::FileTransfer::File *file)
    {
        if (!file)
            return false;
        for (const auto &description : publication.descriptions()) {
            if (description.namespaceURI() != XMPP::Jingle::FileTransfer::NS)
                continue;
            const auto                       fileElement = description.firstChildElement(QStringLiteral("file"));
            XMPP::Jingle::FileTransfer::File parsed(fileElement);
            if (parsed.isValid()) {
                *file = parsed;
                return true;
            }
        }
        return false;
    }

    XMPP::Hash sha256Hash(const QList<XMPP::Hash> &hashes)
    {
        for (const auto &hash : hashes) {
            if (hash.type() == XMPP::Hash::Sha256)
                return hash;
        }
        return {};
    }

} // namespace

bool IrisJingleCapability::isValid() const
{
    const XMPP::Jid publisher(from);
    const auto      expectedSize = reference.size >= 0
        ? XMPP::StatelessFileSharing::encryptedSize(cipher, std::uint64_t(reference.size))
        : std::nullopt;
    return !publicationId.isEmpty() && !itemId.isEmpty() && publisher.isValid() && !publisher.resource().isEmpty()
        && !node.isEmpty() && !noteId.isEmpty() && !contentRevision.isEmpty() && reference.isValid()
        && reference.size >= 0 && reference.checksum.size() == 32
        && cipher == XMPP::StatelessFileSharing::Cipher::Aes256Gcm && key.size() == 32 && iv.size() == 12
        && cipherHash.size() == 32 && expectedSize && *expectedSize == wireSize;
}

IrisJinglePublicationProvider::IrisJinglePublicationProvider(IrisXmppBackend *backend, XmppConfig config, QString path,
                                                             QByteArray encryptionKey, QObject *parent) :
    PublishedSessionProvider(parent), backend_(backend), config_(std::move(config)), path_(std::move(path)),
    encryptionKey_(std::move(encryptionKey))
{
    load();
}

QList<XMPP::Jingle::PublishedSessionEndpoint> IrisJinglePublicationProvider::publishedSessionEndpoints() const
{
    return { { XMPP::Jid(config_.jid).withResource({}), config_.jinglePubNodeName(), true } };
}

XMPP::Jingle::JinglePub IrisJinglePublicationProvider::publication(const IrisJingleCapability &capability) const
{
    if (!capability.isValid())
        return {};

    XMPP::Jingle::FileTransfer::File file;
    file.setName(capability.reference.portableName + QStringLiteral(".encrypted"));
    file.setMediaType(QStringLiteral("application/octet-stream"));
    file.setSize(capability.wireSize);
    file.addHash(XMPP::Hash(XMPP::Hash::Sha256, capability.cipherHash));

    QDomDocument document;
    auto         description = document.createElementNS(XMPP::Jingle::FileTransfer::NS, QStringLiteral("description"));
    const auto   fileElement = file.toXml(&document);
    if (fileElement.isNull())
        return {};
    description.appendChild(fileElement);
    document.appendChild(description);

    XMPP::Jingle::JinglePub result;
    result.setFrom(XMPP::Jid(capability.from));
    result.setId(capability.publicationId);
    result.setUri(QUrl(QStringLiteral("urn:uuid:%1").arg(capability.reference.id.toString(QUuid::WithoutBraces))));
    result.addDescription(description);
    return result.isValid() ? result : XMPP::Jingle::JinglePub();
}

bool IrisJinglePublicationProvider::cacheCapability(const IrisJingleCapability &capability)
{
    if (!manager())
        return false;
    const QPointer<IrisJinglePublicationProvider> guard(this);
    const auto cached = cachePublishedSession({ XMPP::Jid(config_.jid).withResource({}), capability.node, true },
                                              capability.itemId, publication(capability),
                                              [guard, id = capability.publicationId](const XMPP::Jid &requester) {
                                                  if (!guard || !guard->backend_)
                                                      return static_cast<XMPP::Jingle::Session *>(nullptr);
                                                  const auto it = guard->capabilities_.constFind(id);
                                                  return it == guard->capabilities_.cend()
                                                      ? static_cast<XMPP::Jingle::Session *>(nullptr)
                                                      : guard->backend_->createPublishedMediaSession(*it, requester);
                                              });
    return cached.isValid();
}

void IrisJinglePublicationProvider::restoreCachedPublishedSessions()
{
    if (!error_.isEmpty())
        return;
    const auto records = capabilities_.values();
    for (const auto &capability : records)
        cacheCapability(capability);
}

void IrisJinglePublicationProvider::synchronizePublishedSessions()
{
    // Remote offers are discovery data, not durable local authority. Rebuild
    // the view from this connection's snapshot and buffered live events.
    observed_.clear();
    XMPP::Jingle::PublishedSessionProvider::synchronizePublishedSessions();
}

IrisJinglePublicationProvider::PrepareResult IrisJinglePublicationProvider::prepare(IrisJingleCapability capability)
{
    if (!writable_)
        return { {}, error_.isEmpty() ? QStringLiteral("The Jingle capability store is read-only") : error_ };

    capability.node = config_.jinglePubNodeName();
    auto existing   = std::find_if(capabilities_.cbegin(), capabilities_.cend(), [&capability](const auto &candidate) {
        return sameCiphertext(candidate, capability);
    });
    if (existing != capabilities_.cend()) {
        capability.publicationId = existing->publicationId;
        capability.itemId        = existing->itemId;
    } else {
        capability.publicationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        capability.itemId        = capability.publicationId;
    }
    if (!capability.isValid())
        return { {}, QStringLiteral("Invalid durable Jingle media capability") };

    const auto old    = capabilities_.value(capability.publicationId);
    const bool hadOld = capabilities_.contains(capability.publicationId);
    capabilities_.insert(capability.publicationId, capability);
    if (!persist()) {
        if (hadOld)
            capabilities_.insert(old.publicationId, old);
        else
            capabilities_.remove(capability.publicationId);
        return { {}, error_ };
    }
    if (!cacheCapability(capability)) {
        if (hadOld)
            capabilities_.insert(old.publicationId, old);
        else
            capabilities_.remove(capability.publicationId);
        persist();
        return { {}, QStringLiteral("Could not register the durable Jingle media capability") };
    }
    return { publication(capability), {} };
}

QList<XMPP::Jingle::JinglePub> IrisJinglePublicationProvider::matchingPublications(const QByteArray &cipherHash,
                                                                                   quint64           wireSize) const
{
    QList<XMPP::Jingle::JinglePub> result;
    QSet<QString>                  seen;
    for (const auto &publication : observed_) {
        if (!publication.isValid() || !publication.from().compare(XMPP::Jid(config_.jid), false))
            continue;
        XMPP::Jingle::FileTransfer::File file;
        if (!publicationFile(publication, &file) || !file.size() || *file.size() != wireSize)
            continue;
        const auto hash = sha256Hash(file.computedHashes());
        const auto key  = publication.from().full() + QLatin1Char('\n') + publication.id();
        if (!hash.isValid() || hash.data() != cipherHash || seen.contains(key))
            continue;
        seen.insert(key);
        result.append(publication);
    }
    return result;
}

QStringList IrisJinglePublicationProvider::publicationIdsForNote(const QString &noteId) const
{
    QStringList result;
    for (auto it = capabilities_.cbegin(); it != capabilities_.cend(); ++it) {
        if (it->noteId == noteId)
            result.append(it.key());
    }
    return result;
}

bool IrisJinglePublicationProvider::removePublication(const QString &publicationId)
{
    const auto it = capabilities_.find(publicationId);
    if (it == capabilities_.end())
        return true;
    const auto old = *it;
    capabilities_.erase(it);
    forgetPublishedSession(publicationId);
    if (persist())
        return true;
    capabilities_.insert(publicationId, old);
    cacheCapability(old);
    return false;
}

QString IrisJinglePublicationProvider::observedKey(const XMPP::Jingle::PublishedSessionEndpoint &endpoint,
                                                   const QString                                &itemId) const
{
    return endpoint.service.full() + QLatin1Char('\n') + endpoint.node + QLatin1Char('\n') + itemId;
}

void IrisJinglePublicationProvider::publishedSessionObserved(const XMPP::Jingle::PublishedSessionEndpoint &endpoint,
                                                             const QString                                &itemId,
                                                             const XMPP::Jingle::JinglePub                &publication)
{
    observed_.insert(observedKey(endpoint, itemId), publication);
}

void IrisJinglePublicationProvider::publishedSessionRetracted(const XMPP::Jingle::PublishedSessionEndpoint &endpoint,
                                                              const QString                                &itemId)
{
    observed_.remove(observedKey(endpoint, itemId));
}

void IrisJinglePublicationProvider::publishedSessionNodeInvalidated(
    const XMPP::Jingle::PublishedSessionEndpoint &endpoint, bool deleted)
{
    Q_UNUSED(endpoint)
    Q_UNUSED(deleted)
    observed_.clear();
}

bool IrisJinglePublicationProvider::load()
{
    if (path_.isEmpty() || encryptionKey_.size() != SecureEnvelope::MasterKeySize) {
        error_    = QStringLiteral("Invalid Jingle capability-store configuration");
        writable_ = false;
        return false;
    }
    if (!QFileInfo::exists(path_))
        return true;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        error_    = file.errorString();
        writable_ = false;
        return false;
    }
    const auto opened = SecureEnvelope::open(file.readAll(), encryptionKey_, storeContext(config_));
    if (!opened) {
        error_    = opened.error.message;
        writable_ = false;
        return false;
    }

    QDataStream in(opened.value);
    in.setVersion(QDataStream::Qt_5_10);
    quint32 magic   = 0;
    quint16 version = 0;
    quint32 count   = 0;
    in >> magic >> version >> count;
    if (magic != Magic || version != Version || count > MaximumSize) {
        error_    = QStringLiteral("Unsupported Jingle capability-store format");
        writable_ = false;
        return false;
    }
    QHash<QString, IrisJingleCapability> loaded;
    for (quint32 index = 0; index < count; ++index) {
        IrisJingleCapability capability;
        quint8               cipher = 0;
        in >> capability.publicationId >> capability.itemId >> capability.from >> capability.node >> capability.noteId
            >> capability.contentRevision >> capability.reference.id >> capability.reference.blobId
            >> capability.reference.originalName >> capability.reference.portableName >> capability.reference.mediaType
            >> capability.reference.size >> capability.reference.checksum >> capability.reference.remoteData >> cipher
            >> capability.key >> capability.iv >> capability.cipherHash >> capability.wireSize;
        capability.cipher = XMPP::StatelessFileSharing::Cipher(cipher);
        if (!capability.isValid() || loaded.contains(capability.publicationId)) {
            error_    = QStringLiteral("Corrupt Jingle capability-store record");
            writable_ = false;
            return false;
        }
        if (capability.node == config_.jinglePubNodeName())
            loaded.insert(capability.publicationId, std::move(capability));
    }
    if (in.status() != QDataStream::Ok || !in.atEnd()) {
        error_    = QStringLiteral("Corrupt Jingle capability-store payload");
        writable_ = false;
        return false;
    }
    capabilities_ = std::move(loaded);
    return true;
}

bool IrisJinglePublicationProvider::persist()
{
    if (!writable_)
        return false;
    QByteArray  plain;
    QDataStream out(&plain, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << Magic << Version << quint32(capabilities_.size());
    for (const auto &capability : capabilities_) {
        out << capability.publicationId << capability.itemId << capability.from << capability.node << capability.noteId
            << capability.contentRevision << capability.reference.id << capability.reference.blobId
            << capability.reference.originalName << capability.reference.portableName << capability.reference.mediaType
            << capability.reference.size << capability.reference.checksum << capability.reference.remoteData
            << quint8(capability.cipher) << capability.key << capability.iv << capability.cipherHash
            << capability.wireSize;
    }
    const auto sealed = SecureEnvelope::seal(plain, encryptionKey_, storeContext(config_));
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

} // namespace AnyKeep
