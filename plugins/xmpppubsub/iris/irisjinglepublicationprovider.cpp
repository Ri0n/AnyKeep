#include "irisjinglepublicationprovider.h"

#include "irisxmppbackend.h"
#include "mediachunkwirestream.h"
#include "secureenvelope.h"

#include <iris/jingle-ft.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>

#include <QDataStream>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

Q_LOGGING_CATEGORY(lcIrisJingleMedia, "anykeep.xmpp.iris.media", QtInfoMsg)

namespace AnyKeep {
namespace {

    constexpr quint32 Magic         = 0x414b4a50; // AKJP
    constexpr quint16 LegacyVersion = 1;
    constexpr quint16 Version       = 2;
    constexpr quint32 MaximumSize   = 100000;

    AeadContext storeContext(const XmppConfig &config)
    {
        // Keep the outer envelope context stable so version 1 stores can be
        // decrypted and migrated to the explicit representation format.
        return { KeyDomain::LocalRemoteCache, QStringLiteral("anykeep-jingle-publications"),
                 config.instanceId + QLatin1Char('|') + config.jid, 1, QStringLiteral("jingle-publications") };
    }

    bool sameChunkedParameters(const MediaChunkWireParameters &left, const MediaChunkWireParameters &right)
    {
        return left.rootKey == right.rootKey && left.noncePrefix == right.noncePrefix
            && left.plainChecksum == right.plainChecksum && left.plainSize == right.plainSize
            && left.chunkSize == right.chunkSize;
    }

    bool sameCiphertext(const IrisJingleCapability &left, const IrisJingleCapability &right)
    {
        if (left.from != right.from || left.node != right.node || left.noteId != right.noteId
            || left.reference.id != right.reference.id || left.reference.blobId != right.reference.blobId
            || left.reference.size != right.reference.size || left.reference.checksum != right.reference.checksum
            || left.representation != right.representation || left.cipherHash != right.cipherHash
            || left.wireSize != right.wireSize) {
            return false;
        }
        switch (left.representation) {
        case IrisJingleMediaRepresentation::LegacyXep0448:
            return left.cipher == right.cipher && left.key == right.key && left.iv == right.iv;
        case IrisJingleMediaRepresentation::ChunkedAnyKeep:
            return sameChunkedParameters(left.chunked, right.chunked);
        }
        return false;
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

    QByteArray publicationFingerprint(const XMPP::Jingle::JinglePub &publication)
    {
        QDomDocument document;
        const auto   element = publication.toXml(&document);
        if (element.isNull())
            return {};
        document.appendChild(element);
        return document.toByteArray(-1);
    }

    void writeXmlIdentity(QDataStream &stream, const QDomElement &element)
    {
        const auto localName = element.localName().isEmpty() ? element.tagName() : element.localName();
        stream << quint8(1) << element.namespaceURI() << localName;
        QMap<QPair<QString, QString>, QString> attributes;
        const auto                             nodes = element.attributes();
        for (int i = 0; i < nodes.count(); ++i) {
            const auto attribute = nodes.item(i).toAttr();
            if (attribute.namespaceURI() == QLatin1String("http://www.w3.org/2000/xmlns/")
                || attribute.name() == QLatin1String("xmlns") || attribute.name().startsWith(QLatin1String("xmlns:")))
                continue;
            const auto name = attribute.localName().isEmpty() ? attribute.name() : attribute.localName();
            attributes.insert({ attribute.namespaceURI(), name }, attribute.value());
        }
        stream << attributes;

        // File-transfer containers have element-only content. Preserve leaf
        // text verbatim (including spaces in names) and all mixed content.
        bool mixedContent = false;
        for (auto child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if ((child.isText() || child.isCDATASection()) && !child.nodeValue().trimmed().isEmpty())
                mixedContent = true;
        }
        const bool ignoreIndentation = !element.firstChildElement().isNull() && !mixedContent;
        QString    text;
        auto       flushText = [&] {
            if (!text.isEmpty() && !(ignoreIndentation && text.trimmed().isEmpty()))
                stream << quint8(2) << text;
            text.clear();
        };
        for (auto child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (child.isText() || child.isCDATASection()) {
                text += child.nodeValue();
            } else if (child.isElement()) {
                flushText();
                writeXmlIdentity(stream, child.toElement());
            } else if (!child.isComment() && !child.isProcessingInstruction()) {
                flushText();
                stream << quint8(3) << int(child.nodeType()) << child.nodeValue();
            }
        }
        flushText();
        stream << quint8(0);
    }

    QByteArray publicationIdentity(const XMPP::Jingle::JinglePub &publication)
    {
        QDomDocument document;
        const auto   element = publication.toXml(&document);
        if (element.isNull())
            return {};
        QByteArray  bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        writeXmlIdentity(stream, element);
        return bytes;
    }

    bool samePublicationContent(const XMPP::Jingle::JinglePub &local, const XMPP::Jingle::JinglePub &observed)
    {
        return local.isValid() && observed.isValid() && publicationIdentity(local) == publicationIdentity(observed);
    }

} // namespace

QString IrisJingleCapability::invalidReason() const
{
    const XMPP::Jid publisher(from);
    if (publicationId.isEmpty())
        return QStringLiteral("publication id is empty");
    if (itemId.isEmpty())
        return QStringLiteral("item id is empty");
    if (!publisher.isValid())
        return QStringLiteral("publisher JID is invalid");
    if (publisher.resource().isEmpty())
        return QStringLiteral("publisher JID has no resource");
    if (node.isEmpty())
        return QStringLiteral("publication node is empty");
    if (noteId.isEmpty())
        return QStringLiteral("note id is empty");
    if (contentRevision.isEmpty())
        return QStringLiteral("content revision is empty");
    if (!reference.isValid())
        return QStringLiteral("media reference is invalid");
    if (reference.size < 0)
        return QStringLiteral("media size is negative");
    if (reference.checksum.size() != 32)
        return QStringLiteral("media checksum is not SHA-256");
    if (!cipherHash.isEmpty() && cipherHash.size() != 32)
        return QStringLiteral("ciphertext checksum is not SHA-256");
    if (wireSize > quint64(std::numeric_limits<qint64>::max()))
        return QStringLiteral("encrypted media size exceeds QIODevice limits");

    switch (representation) {
    case IrisJingleMediaRepresentation::LegacyXep0448: {
        if (cipher != XMPP::StatelessFileSharing::Cipher::Aes256Gcm)
            return QStringLiteral("media cipher is not AES-256-GCM");
        if (key.size() != 32)
            return QStringLiteral("media encryption key is not 32 bytes");
        if (iv.size() != 12)
            return QStringLiteral("media encryption IV is not 12 bytes");
        const auto expectedSize = XMPP::StatelessFileSharing::encryptedSize(cipher, std::uint64_t(reference.size));
        if (!expectedSize)
            return QStringLiteral("encrypted media size cannot be represented");
        if (*expectedSize != wireSize)
            return QStringLiteral("encrypted media size does not match the capability");
        return {};
    }
    case IrisJingleMediaRepresentation::ChunkedAnyKeep: {
        if (cipher != XMPP::StatelessFileSharing::Cipher::Unknown || !key.isEmpty() || !iv.isEmpty())
            return QStringLiteral("chunked media capability contains legacy cipher material");
        if (!chunked.isValid())
            return QStringLiteral("chunked media parameters are invalid");
        if (chunked.plainSize != quint64(reference.size) || chunked.plainChecksum != reference.checksum)
            return QStringLiteral("chunked media parameters do not match the plaintext reference");
        const auto expectedSize = MediaChunkWire::wireSize(chunked);
        if (!expectedSize || *expectedSize != wireSize)
            return QStringLiteral("chunked media wire size does not match the capability");
        return {};
    }
    }
    return QStringLiteral("unknown media representation");
}

IrisJinglePublicationProvider::IrisJinglePublicationProvider(IrisXmppBackend *backend, XmppConfig config, QString path,
                                                             QByteArray encryptionKey, QObject *parent) :
    PublishedSessionProvider(parent), backend_(backend), config_(std::move(config)), path_(std::move(path)),
    encryptionKey_(std::move(encryptionKey))
{
    load();
    connect(this, &XMPP::Jingle::PublishedSessionProvider::stateChanged, this,
            [](XMPP::Jingle::PublishedSessionProvider::State state) {
                qCDebug(lcIrisJingleMedia) << "Jingle publication provider state:" << state;
            });
}

void IrisJinglePublicationProvider::traceIncomingStart(const QString &xml) const
{
    if (!lcIrisJingleMedia().isDebugEnabled() || !xml.contains(QLatin1String("urn:xmpp:jinglepub:1")))
        return;
    QDomDocument document;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (!document.setContent(xml, QDomDocument::ParseOption::UseNamespaceProcessing))
#else
    if (!document.setContent(xml, true))
#endif
        return;
    const auto iq = document.documentElement();
    if (iq.tagName() != QLatin1String("iq") || iq.attribute(QStringLiteral("type")) != QLatin1String("get"))
        return;
    for (auto child = iq.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.localName() != QLatin1String("start")
            || child.namespaceURI() != QLatin1String("urn:xmpp:jinglepub:1"))
            continue;
        const auto id = child.attribute(QStringLiteral("id"));
        qCDebug(lcIrisJingleMedia).noquote()
            << "Jingle publication start received: iq=" << iq.attribute(QStringLiteral("id")) << "publication=" << id
            << "requester=" << iq.attribute(QStringLiteral("from"))
            << "durable-capability=" << capabilities_.contains(id) << "provider-state=" << state() << "registry-state="
            << (manager() ? manager()->publishedSessionState(id)
                          : XMPP::Jingle::PublicationManager::SessionState::Missing);
        const auto capability = capabilities_.constFind(id);
        if (capability == capabilities_.cend())
            continue;
        const auto serverItem = observed_.constFind(
            observedKey({ XMPP::Jid(config_.jid).withResource({}), capability->node, true }, capability->itemId));
        qCDebug(lcIrisJingleMedia) << "Jingle publication authority check: publication=" << id
                                   << "server-item-observed=" << (serverItem != observed_.cend());
        if (serverItem == observed_.cend() || !manager() || !manager()->client())
            continue;
        const auto local     = publication(*capability);
        const auto clientJid = manager()->client()->jid();
        const auto cached    = manager()->publishedSession(id);
        qCDebug(lcIrisJingleMedia) << "Jingle publication authority comparison: publication=" << id
                                   << "local-publisher=" << local.from().full()
                                   << "server-publisher=" << serverItem->from().full()
                                   << "bound-jid=" << clientJid.full()
                                   << "local-resource-matches=" << local.from().compare(clientJid)
                                   << "server-resource-matches=" << serverItem->from().compare(clientJid)
                                   << "server-publication-id=" << serverItem->id() << "descriptor-matches="
                                   << (cached.isValid() && serverItem->isValid()
                                       && publicationFingerprint(cached) == publicationFingerprint(*serverItem));
        XMPP::Jingle::FileTransfer::File localFile, serverFile;
        const bool                       localFileValid  = publicationFile(local, &localFile);
        const bool                       serverFileValid = publicationFile(*serverItem, &serverFile);
        qCDebug(lcIrisJingleMedia)
            << "Jingle publication content comparison: publication=" << id << "comparison-version=" << 2
            << "content-matches=" << samePublicationContent(local, *serverItem) << "generated-descriptor-matches="
            << (local.isValid() && publicationFingerprint(local) == publicationFingerprint(*serverItem))
            << "uri-matches=" << (local.uri() == serverItem->uri())
            << "local-description-count=" << local.descriptions().size()
            << "server-description-count=" << serverItem->descriptions().size() << "local-file-valid=" << localFileValid
            << "server-file-valid=" << serverFileValid
            << "file-name-matches=" << (localFileValid && serverFileValid && localFile.name() == serverFile.name())
            << "media-type-matches=" << (localFile.mediaType() == serverFile.mediaType())
            << "local-wire-size=" << (localFile.size() ? QString::number(*localFile.size()) : QStringLiteral("unknown"))
            << "server-wire-size="
            << (serverFile.size() ? QString::number(*serverFile.size()) : QStringLiteral("unknown"))
            << "local-hash-count=" << localFile.hashes().size() << "server-hash-count=" << serverFile.hashes().size()
            << "sha256-matches=" << (sha256Hash(localFile.hashes()).data() == sha256Hash(serverFile.hashes()).data());
    }
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
    // Empty data intentionally serializes as XEP-0300 <hash-used/>. Iris FT
    // then hashes the actual transfer incrementally and reports <checksum/>
    // after the payload instead of forcing a whole-file pre-pass.
    file.addHash(XMPP::Hash(XMPP::Hash::Sha256, capability.cipherHash));

    QDomDocument document;
    auto         description = document.createElementNS(XMPP::Jingle::FileTransfer::NS, QStringLiteral("description"));
    const auto   fileElement = file.toXml(&document);
    if (fileElement.isNull())
        return {};
    description.appendChild(fileElement);
    document.appendChild(description);

    // Iris File::toXml leaves some children without a DOM namespace even
    // though they inherit the file-transfer namespace on the wire. PubSub
    // parses them with that namespace and Qt then emits additional xmlns
    // declarations. Iris compares serialized publication bytes for authority,
    // so cache the same namespace-aware DOM that a server round trip produces.
    QDomDocument normalized;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (!normalized.setContent(document.toByteArray(-1), QDomDocument::ParseOption::UseNamespaceProcessing))
#else
    if (!normalized.setContent(document.toByteArray(-1), true))
#endif
        return {};

    XMPP::Jingle::JinglePub result;
    result.setFrom(XMPP::Jid(capability.from));
    result.setId(capability.publicationId);
    result.setUri(QUrl(QStringLiteral("urn:uuid:%1").arg(capability.reference.id.toString(QUuid::WithoutBraces))));
    result.addDescription(normalized.documentElement());
    return result.isValid() ? result : XMPP::Jingle::JinglePub();
}

bool IrisJinglePublicationProvider::cacheCapability(const IrisJingleCapability &capability)
{
    if (!manager())
        return false;
    const XMPP::Jingle::PublishedSessionEndpoint endpoint { XMPP::Jid(config_.jid).withResource({}), capability.node,
                                                            true };
    auto                                         descriptor = publication(capability);
    const auto observed = observed_.constFind(observedKey(endpoint, capability.itemId));
    if (observed != observed_.cend() && samePublicationContent(descriptor, *observed)) {
        // Retain the authoritative DOM's formatting so Iris's byte comparison
        // succeeds. Every namespace, attribute and content value was checked;
        // Iris still controls resource binding and the Active transition.
        descriptor = *observed;
        qCDebug(lcIrisJingleMedia) << "Using equivalent server Jingle descriptor: publication="
                                   << capability.publicationId;
    }
    qCDebug(lcIrisJingleMedia) << "Caching Jingle publication:" << capability.publicationId
                               << "representation=" << int(capability.representation)
                               << "wire-size=" << capability.wireSize;
    const QPointer<IrisJinglePublicationProvider> guard(this);
    const auto                                    cached = cachePublishedSession(
        endpoint, capability.itemId, descriptor, [guard, id = capability.publicationId](const XMPP::Jid &requester) {
            if (!guard || !guard->backend_)
                return static_cast<XMPP::Jingle::Session *>(nullptr);
            const auto it = guard->capabilities_.constFind(id);
            if (it == guard->capabilities_.cend())
                return static_cast<XMPP::Jingle::Session *>(nullptr);
            const auto capability = *it;
            qCDebug(lcIrisJingleMedia) << "Creating published media session: publication=" << id
                                       << "requester=" << requester.full()
                                       << "representation=" << int(capability.representation);
            if (capability.representation != IrisJingleMediaRepresentation::ChunkedAnyKeep)
                return guard->backend_->createPublishedMediaSession(capability, requester);

            if (!requester.compare(XMPP::Jid(guard->config_.jid).withResource({}), false)) {
                qCWarning(lcIrisJingleMedia)
                    << "Published media requester is not the owning account: publication=" << id;
                return static_cast<XMPP::Jingle::Session *>(nullptr);
            }
            auto *publicationManager = guard->manager();
            auto *jingleManager      = publicationManager ? publicationManager->jingleManager() : nullptr;
            auto *session            = jingleManager ? jingleManager->newSession(requester) : nullptr;
            if (!session) {
                qCWarning(lcIrisJingleMedia) << "Could not create published media session: publication=" << id
                                             << "jingle-manager=" << bool(jingleManager);
                return static_cast<XMPP::Jingle::Session *>(nullptr);
            }
            auto *app = static_cast<XMPP::Jingle::FileTransfer::Application *>(
                session->newContent(XMPP::Jingle::FileTransfer::NS, session->role()));
            if (!app) {
                qCWarning(lcIrisJingleMedia) << "Could not create file-transfer content: publication=" << id;
                session->deleteLater();
                return static_cast<XMPP::Jingle::Session *>(nullptr);
            }

            XMPP::Jingle::FileTransfer::File file;
            file.setName(capability.reference.portableName + QStringLiteral(".encrypted"));
            file.setMediaType(QStringLiteral("application/octet-stream"));
            file.setSize(capability.wireSize);
            file.addHash(XMPP::Hash(XMPP::Hash::Sha256, capability.cipherHash));
            app->setFile(file);
            auto configureContent = [capability, session](XMPP::Jingle::FileTransfer::Application *content) {
#ifdef IRIS_FT_DEFERRED_RECEIPTS
                content->setKeepTransportUntilReceipt();
#endif
                QObject::connect(
                    content, &XMPP::Jingle::FileTransfer::Application::deviceRequested, content,
                    [content, capability, sid = session->sid()](quint64 offset, std::optional<quint64> size) {
                        qCDebug(lcIrisJingleMedia)
                            << "Published media device requested: publication=" << capability.publicationId
                            << "sid=" << sid << "offset=" << offset
                            << "length=" << (size ? QString::number(*size) : QStringLiteral("to-end"))
                            << "wire-size=" << capability.wireSize;
                        const bool invalidRange
                            = offset > capability.wireSize || (size && *size > capability.wireSize - offset);
                        if (invalidRange) {
                            qCWarning(lcIrisJingleMedia)
                                << "Published media range is out of bounds: publication=" << capability.publicationId;
                            content->setDevice(nullptr);
                            return;
                        }
                        auto *wire = new MediaChunkWireStream(capability.reference, capability.chunked, content);
                        if (!wire->open(QIODevice::ReadOnly) || !wire->seek(qint64(offset))) {
                            qCWarning(lcIrisJingleMedia) << "Could not open/seek published media stream: publication="
                                                         << capability.publicationId << "offset=" << offset
                                                         << "stream-error=" << wire->errorString();
                            wire->deleteLater();
                            content->setDevice(nullptr);
                            return;
                        }
                        // Iris owns the requested range length through its internal
                        // bytesLeft counter, so the same seekable deterministic wire
                        // object can serve both complete and resumed transfers.
                        content->setDevice(wire);
                    });
            };
            configureContent(app);
            QObject::connect(
                session, &XMPP::Jingle::Session::newContentReceived, session, [session, capability, configureContent] {
                    for (auto *base : session->contentList()) {
                        if (!base || base->state() != XMPP::Jingle::State::Created || !base->isRemote())
                            continue;
                        if (base->pad()->ns() != XMPP::Jingle::FileTransfer::NS || base->senders() != session->role()) {
                            base->remove(XMPP::Jingle::Reason::SecurityError,
                                         QStringLiteral("Unsupported publication request"));
                            continue;
                        }
                        auto         *content = static_cast<XMPP::Jingle::FileTransfer::Application *>(base);
                        const auto    file    = content->file();
                        const auto    range   = file.range();
                        const auto    first = MediaChunkWire::chunkIndexForWireOffset(capability.chunked, range.offset);
                        const quint64 end   = range.offset + range.length;
                        const bool    alignedEnd = end == capability.wireSize
                            || end % (quint64(capability.chunked.chunkSize) + MediaChunkWire::Overhead) == 0;
                        if (!file.size() || *file.size() != capability.wireSize || !range.isValid() || !range.length
                            || range.offset >= capability.wireSize || range.length > capability.wireSize - range.offset
                            || !first || !alignedEnd
                            || range.offset % (quint64(capability.chunked.chunkSize) + MediaChunkWire::Overhead) != 0) {
                            content->remove(XMPP::Jingle::Reason::SecurityError,
                                            QStringLiteral("Invalid publication gap"));
                            continue;
                        }
                        auto accepted = file;
                        accepted.setHashes({ XMPP::Hash(XMPP::Hash::Sha256) });
                        content->setAcceptFile(accepted);
                        configureContent(content);
                        content->prepare();
                    }
                });
            session->addContent(app);
            QObject::connect(app, &XMPP::Jingle::FileTransfer::Application::stateChanged, app,
                             [id, sid = session->sid(), app](XMPP::Jingle::State status) {
                                 qCDebug(lcIrisJingleMedia)
                                     << "Published media transfer state: publication=" << id << "sid=" << sid
                                     << "state=" << int(status) << "reason=" << int(app->lastReason().condition());
                             });
            qCDebug(lcIrisJingleMedia) << "Published media session created: publication=" << id
                                       << "sid=" << session->sid();
            return session;
        });
    return cached.isValid();
}

void IrisJinglePublicationProvider::restoreCachedPublishedSessions()
{
    // The provider is attached before Client::start(). Advertise the complete
    // Jingle surface while caps are still being assembled: base Jingle, FT5,
    // registered transports, XEP-0358 and this provider's PEP +notify feature.
    if (auto *publicationManager = manager()) {
        if (auto *client = publicationManager->client()) {
            auto features = client->features();
            if (auto *jingleManager = publicationManager->jingleManager()) {
                for (const auto &feature : jingleManager->discoFeatures())
                    features.addFeature(feature);
            }
            client->setFeatures(features);
        }
    }

    if (!error_.isEmpty()) {
        qWarning().noquote() << "Could not restore durable Jingle media capabilities:" << error_;
        return;
    }
    const auto records = capabilities_.values();
    qInfo() << "Restoring durable Jingle media capabilities:" << records.size();
    for (const auto &capability : records) {
        if (!cacheCapability(capability))
            qWarning().noquote() << "Could not cache durable Jingle media capability:" << capability.publicationId;
    }
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
    if (const auto reason = capability.invalidReason(); !reason.isEmpty()) {
        qWarning().noquote() << "Invalid durable Jingle media capability:" << reason << "publisher=" << capability.from
                             << "note-id-present=" << !capability.noteId.isEmpty()
                             << "content-revision-present=" << !capability.contentRevision.isEmpty()
                             << "media-id=" << capability.reference.id.toString(QUuid::WithoutBraces)
                             << "representation=" << int(capability.representation)
                             << "plain-size=" << capability.reference.size
                             << "checksum-size=" << capability.reference.checksum.size()
                             << "key-size=" << capability.key.size() << "iv-size=" << capability.iv.size()
                             << "chunk-key-size=" << capability.chunked.rootKey.size()
                             << "nonce-prefix-size=" << capability.chunked.noncePrefix.size()
                             << "cipher-hash-size=" << capability.cipherHash.size()
                             << "wire-size=" << capability.wireSize;
        return { {}, QStringLiteral("Invalid durable Jingle media capability: %1").arg(reason) };
    }

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
    if (cipherHash.size() != 32)
        return result;
    QSet<QString> seen;
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
    qCDebug(lcIrisJingleMedia) << "Removing durable Jingle publication:" << publicationId
                               << "present=" << capabilities_.contains(publicationId);
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
    qCDebug(lcIrisJingleMedia) << "Jingle publication item observed: item=" << itemId
                               << "publication=" << publication.id() << "publisher=" << publication.from().full()
                               << "durable-capability=" << capabilities_.contains(publication.id());
    if (capabilities_.contains(publication.id()) && manager()) {
        const auto                                   capability = capabilities_.value(publication.id());
        const XMPP::Jingle::PublishedSessionEndpoint expected { XMPP::Jid(config_.jid).withResource({}),
                                                                capability.node, true };
        const auto                                   cached = manager()->publishedSession(publication.id());
        if (observedKey(expected, capability.itemId) == observedKey(endpoint, itemId)
            && samePublicationContent(this->publication(capability), publication)
            && publicationFingerprint(cached) != publicationFingerprint(publication)) {
            cacheCapability(capability);
        }
        qInfo().noquote() << "Observed own durable Jingle media authority:"
                          << "publication=" << publication.id() << "item=" << itemId
                          << "publisher=" << publication.from().full()
                          << "state=" << int(manager()->publishedSessionState(publication.id()));
    }
}

void IrisJinglePublicationProvider::publishedSessionRetracted(const XMPP::Jingle::PublishedSessionEndpoint &endpoint,
                                                              const QString                                &itemId)
{
    qCDebug(lcIrisJingleMedia) << "Jingle publication retracted: item=" << itemId;
    observed_.remove(observedKey(endpoint, itemId));
}

void IrisJinglePublicationProvider::publishedSessionNodeInvalidated(
    const XMPP::Jingle::PublishedSessionEndpoint &endpoint, bool deleted)
{
    Q_UNUSED(endpoint)
    qCDebug(lcIrisJingleMedia) << "Jingle publication node invalidated: deleted=" << deleted;
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
    if (magic != Magic || (version != LegacyVersion && version != Version) || count > MaximumSize) {
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
            >> capability.reference.size >> capability.reference.checksum >> capability.reference.remoteData;
        if (version == LegacyVersion) {
            in >> cipher >> capability.key >> capability.iv >> capability.cipherHash >> capability.wireSize;
            capability.representation = IrisJingleMediaRepresentation::LegacyXep0448;
        } else {
            quint8 representation = 0;
            in >> representation >> cipher >> capability.key >> capability.iv >> capability.chunked.rootKey
                >> capability.chunked.noncePrefix >> capability.chunked.plainChecksum >> capability.chunked.plainSize
                >> capability.chunked.chunkSize >> capability.cipherHash >> capability.wireSize;
            capability.representation = IrisJingleMediaRepresentation(representation);
        }
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
            << quint8(capability.representation) << quint8(capability.cipher) << capability.key << capability.iv
            << capability.chunked.rootKey << capability.chunked.noncePrefix << capability.chunked.plainChecksum
            << capability.chunked.plainSize << capability.chunked.chunkSize << capability.cipherHash
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
