#include "iris/irischunkedmediasource.h"
#include "iris/irisjinglepublicationprovider.h"
#include "iris/irisxmppbackend.h"
#include "mediapartialchunkcache.h"
#include "secureenvelope.h"

#include <iris/jingle-ft.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_pubsub.h>
#include <iris/xmpp_status.h>

#include <QCryptographicHash>
#include <QDomDocument>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

using namespace AnyKeep;

namespace {

QList<XMPP::Task *> findTasks(XMPP::Task *root, const char *className)
{
    // The concrete PubSub task metaobjects are not exported by Iris's Windows
    // DLL. Only reference the exported Task metaobject and query derived types
    // through QObject's runtime interface.
    QList<XMPP::Task *> result;
    for (auto *task : root->findChildren<XMPP::Task *>()) {
        if (task->inherits(className))
            result.append(task);
    }
    return result;
}

QByteArray fingerprint(const XMPP::Jingle::JinglePub &publication)
{
    // Iris uses these serialized bytes to confirm the local capability against
    // the authoritative PubSub item before accepting a <start/> request.
    QDomDocument document;
    document.appendChild(publication.toXml(&document));
    return document.toByteArray(-1);
}

IrisJingleCapability validCapability(const XmppConfig &config, bool chunked, bool knownHash)
{
    IrisJingleCapability capability;
    capability.from                   = config.jid + QStringLiteral("/source");
    capability.noteId                 = QStringLiteral("note-id");
    capability.contentRevision        = QStringLiteral("revision");
    capability.reference.id           = QUuid::createUuid();
    capability.reference.portableName = QStringLiteral("media.bin");
    capability.reference.size         = 1234;
    capability.reference.checksum
        = QCryptographicHash::hash(QByteArrayLiteral("plaintext"), QCryptographicHash::Sha256);
    capability.reference.blobId = capability.reference.checksum;
    if (knownHash)
        capability.cipherHash = QCryptographicHash::hash(QByteArrayLiteral("ciphertext"), QCryptographicHash::Sha256);
    if (chunked) {
        capability.representation = IrisJingleMediaRepresentation::ChunkedAnyKeep;
        const auto parameters     = MediaChunkWire::generate(capability.reference.size, capability.reference.checksum);
        Q_ASSERT(parameters);
        capability.chunked = *parameters;
        const auto size    = MediaChunkWire::wireSize(capability.chunked);
        Q_ASSERT(size);
        capability.wireSize = *size;
    } else {
        capability.cipher = XMPP::StatelessFileSharing::Cipher::Aes256Gcm;
        capability.key    = QByteArray(32, 'k');
        capability.iv     = QByteArray(12, 'i');
        const auto size   = XMPP::StatelessFileSharing::encryptedSize(capability.cipher, capability.reference.size);
        Q_ASSERT(size);
        capability.wireSize = *size;
    }

    return capability;
}

QDomDocument parseIrisStream(const QByteArray &xml)
{
    // Match Iris Parser::handleStartElement/handleText: resolve namespaces,
    // discard namespace declaration attributes, retain spacing-only text.
    QXmlStreamReader reader(xml);
    QDomDocument     document;
    QDomElement      parent;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const auto ns      = reader.namespaceUri().toString();
            const auto name    = reader.name().toString();
            auto       element = ns.isEmpty() ? document.createElement(name) : document.createElementNS(ns, name);
            for (const auto &attribute : reader.attributes()) {
                if (attribute.namespaceUri().isEmpty())
                    element.setAttribute(attribute.name().toString(), attribute.value().toString());
                else
                    element.setAttributeNS(attribute.namespaceUri().toString(), attribute.qualifiedName().toString(),
                                           attribute.value().toString());
            }
            if (parent.isNull())
                document.appendChild(element);
            else
                parent.appendChild(element);
            parent = element;
        } else if (token == QXmlStreamReader::EndElement) {
            parent = parent.parentNode().toElement();
        } else if (token == QXmlStreamReader::Characters && !parent.isNull()) {
            parent.appendChild(document.createTextNode(reader.text().toString()));
        }
    }
    return reader.hasError() ? QDomDocument() : document;
}

} // namespace

namespace AnyKeep {

class IrisJinglePublicationProviderTest : public QObject {
    Q_OBJECT

private slots:
    void pubsubRoundTripPreservesPublicationAuthority_data();
    void pubsubRoundTripPreservesPublicationAuthority();
    void openChecksPublicationAuthority_data();
    void openChecksPublicationAuthority();
    void offlineOpenDoesNotQueuePublication();
    void discoversChunkedReplicas_data();
    void discoversChunkedReplicas();
    void cachedReplicaServesExactAuthenticatedWireRanges();
    void rejectsChangedServerDescriptor_data();
    void rejectsChangedServerDescriptor();
};

void IrisJinglePublicationProviderTest::pubsubRoundTripPreservesPublicationAuthority_data()
{
    QTest::addColumn<bool>("chunked");
    QTest::addColumn<bool>("knownHash");
    QTest::addColumn<bool>("streamParser");
    QTest::newRow("legacy-hash-used") << false << false << false;
    QTest::newRow("legacy-known-hash") << false << true << false;
    QTest::newRow("chunked-hash-used") << true << false << false;
    QTest::newRow("chunked-known-hash") << true << true << false;
    QTest::newRow("stream-legacy-hash-used") << false << false << true;
    QTest::newRow("stream-legacy-known-hash") << false << true << true;
    QTest::newRow("stream-chunked-hash-used") << true << false << true;
    QTest::newRow("stream-chunked-known-hash") << true << true << true;
}

void IrisJinglePublicationProviderTest::pubsubRoundTripPreservesPublicationAuthority()
{
    QFETCH(bool, chunked);
    QFETCH(bool, knownHash);
    QFETCH(bool, streamParser);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    XmppConfig config;
    config.instanceId    = QStringLiteral("publication-test");
    config.jid           = QStringLiteral("owner@example.test");
    const auto storePath = directory.filePath(QStringLiteral("publications.bin"));
    const auto storeKey  = SecureEnvelope::generateMasterKey();

    const auto capability = validCapability(config, chunked, knownHash);

    XMPP::Client            client;
    auto                   *manager = client.jingleManager()->publicationManager();
    XMPP::Jingle::JinglePub published;
    {
        IrisJinglePublicationProvider provider(nullptr, config, storePath, storeKey);
        manager->registerProvider(&provider);
        const auto prepared = provider.prepare(capability);
        QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
        QVERIFY(prepared.publication.isValid());
        published = prepared.publication;
    }

    // PubSub parses incoming XML with namespace processing. Include normal
    // indentation too, as the server is free to reserialize the same payload.
    QDomDocument outgoing;
    outgoing.appendChild(published.toXml(&outgoing));
    QDomDocument incoming;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QVERIFY(incoming.setContent(outgoing.toByteArray(), QDomDocument::ParseOption::UseNamespaceProcessing));
#else
    QVERIFY(incoming.setContent(outgoing.toByteArray(), true));
#endif
    if (streamParser)
        incoming = parseIrisStream(outgoing.toByteArray());
    const XMPP::Jingle::JinglePub observed(incoming.documentElement());
    QVERIFY(observed.isValid());
    if (!streamParser)
        QCOMPARE(fingerprint(published), fingerprint(observed));
    else
        QVERIFY(fingerprint(published) != fingerprint(observed));

    // Startup reconstructs the cached descriptor from the durable capability;
    // it must still match the already published server item without republishing.
    IrisJinglePublicationProvider restored(nullptr, config, storePath, storeKey);
    QVERIFY2(restored.errorString().isEmpty(), qPrintable(restored.errorString()));
    manager->registerProvider(&restored);
    QVERIFY(manager->publishedSession(published.id()).isValid());
    const XMPP::Jingle::PublishedSessionEndpoint endpoint { XMPP::Jid(config.jid), config.jinglePubNodeName(), true };
    restored.publishedSessionObserved(endpoint, published.id(), observed);
    QCOMPARE(fingerprint(manager->publishedSession(published.id())), fingerprint(observed));

    // A later preparation must keep the verified server representation too.
    const auto preparedAgain = restored.prepare(capability);
    QVERIFY2(preparedAgain.error.isEmpty(), qPrintable(preparedAgain.error));
    QCOMPARE(preparedAgain.publication.id(), published.id());
    QCOMPARE(fingerprint(manager->publishedSession(published.id())), fingerprint(observed));
}

void IrisJinglePublicationProviderTest::offlineOpenDoesNotQueuePublication()
{
    QTemporaryDir directory;
    XmppConfig    config;
    config.jid        = QStringLiteral("owner@example.test");
    config.instanceId = QStringLiteral("offline-offer");
    XMPP::Client                  client;
    IrisJinglePublicationProvider provider(nullptr, config, directory.filePath(QStringLiteral("offers")),
                                           SecureEnvelope::generateMasterKey());
    client.jingleManager()->publicationManager()->registerProvider(&provider);
    provider.ensurePublishedAsync(validCapability(config, true, false));
    QVERIFY(provider.capabilities_.isEmpty());
    QVERIFY(findTasks(client.rootTask(), "XMPP::PubSubItemsTask").isEmpty());
    QVERIFY(findTasks(client.rootTask(), "XMPP::PubSubPublishTask").isEmpty());
}

void IrisJinglePublicationProviderTest::openChecksPublicationAuthority_data()
{
    QTest::addColumn<QString>("reply");
    for (const auto *kind : { "matching", "missing", "timeout", "changed", "retracted", "changed-during-check" })
        QTest::newRow(kind) << QString::fromLatin1(kind);
}

void IrisJinglePublicationProviderTest::openChecksPublicationAuthority()
{
    QFETCH(QString, reply);
    QTemporaryDir directory;
    XmppConfig    config;
    config.jid        = QStringLiteral("owner@example.test");
    config.resource   = QStringLiteral("source");
    config.instanceId = QStringLiteral("open-offer");
    XMPP::Client                  client;
    IrisXmppBackend               backend;
    auto                         *manager = client.jingleManager()->publicationManager();
    IrisJinglePublicationProvider provider(&backend, config, directory.filePath(QStringLiteral("offers")),
                                           SecureEnvelope::generateMasterKey());
    manager->registerProvider(&provider);
    const auto answer = [&client, &config](XMPP::Task *task, const XMPP::Jingle::JinglePub &offer = {},
                                           bool timeout = false) {
        QDomDocument doc;
        auto         iq = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("iq"));
        iq.setAttribute(QStringLiteral("type"), timeout ? QStringLiteral("error") : QStringLiteral("result"));
        iq.setAttribute(QStringLiteral("id"), task->id());
        iq.setAttribute(QStringLiteral("from"), config.jid);
        if (timeout) {
            auto error = doc.createElement(QStringLiteral("error"));
            error.setAttribute(QStringLiteral("type"), QStringLiteral("wait"));
            error.appendChild(doc.createElementNS(QStringLiteral("urn:ietf:params:xml:ns:xmpp-stanzas"),
                                                  QStringLiteral("remote-server-timeout")));
            iq.appendChild(error);
        } else {
            auto pubsub
                = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/pubsub"), QStringLiteral("pubsub"));
            auto items
                = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/pubsub"), QStringLiteral("items"));
            items.setAttribute(QStringLiteral("node"), config.jinglePubNodeName());
            if (offer.isValid()) {
                auto item
                    = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/pubsub"), QStringLiteral("item"));
                item.setAttribute(QStringLiteral("id"), offer.id());
                item.appendChild(offer.toXml(&doc));
                items.appendChild(item);
            }
            pubsub.appendChild(items);
            iq.appendChild(pubsub);
        }
        return client.rootTask()->take(iq);
    };
    // Use Iris's real task parser with synchronous fake IQ replies. There is
    // no socket, external account, or actual publication in this regression.
    client.start(QStringLiteral("example.test"), QStringLiteral("owner"), {}, config.resource);
    client.setPresence(XMPP::Status());
    const auto initial = findTasks(client.rootTask(), "XMPP::PubSubItemsTask");
    QVERIFY(!initial.isEmpty());
    for (auto *task : initial)
        QVERIFY(answer(task));
    QCOMPARE(provider.state(), XMPP::Jingle::PublishedSessionProvider::State::Synchronized);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    const auto capability = validCapability(config, true, false);
    provider.ensurePublishedAsync(capability);
    const auto queries = findTasks(client.rootTask(), "XMPP::PubSubItemsTask");
    QCOMPARE(queries.size(), 1);
    QCOMPARE(provider.capabilities_.size(), 1);
    const auto id       = provider.capabilities_.constBegin().key();
    auto       observed = provider.publication(provider.capabilities_.value(id));
    QVERIFY(observed.isValid());
    // Repeated views share the same in-flight authority check.
    provider.ensurePublishedAsync(capability);
    QCOMPARE(findTasks(client.rootTask(), "XMPP::PubSubItemsTask").size(), 1);
    if (reply == QLatin1String("retracted")) {
        const XMPP::Jingle::PublishedSessionEndpoint endpoint { XMPP::Jid(config.jid), config.jinglePubNodeName(),
                                                                true };
        provider.publishedSessionRetracted(endpoint, id);
    }
    if (reply == QLatin1String("changed-during-check")) {
        auto changed = observed;
        changed.setUri(QUrl(QStringLiteral("urn:uuid:another-attachment")));
        const XMPP::Jingle::PublishedSessionEndpoint endpoint { XMPP::Jid(config.jid), config.jinglePubNodeName(),
                                                                true };
        provider.publishedSessionObserved(endpoint, id, changed);
    }
    if (reply == QLatin1String("changed"))
        observed.setUri(QUrl(QStringLiteral("urn:uuid:another-attachment")));
    QVERIFY(answer(queries.first(),
                   (reply == QLatin1String("matching") || reply == QLatin1String("changed"))
                       ? observed
                       : XMPP::Jingle::JinglePub(),
                   reply == QLatin1String("timeout")));
    const auto publishes = findTasks(client.rootTask(), "XMPP::PubSubPublishTask");
    QCOMPARE(publishes.size(), reply == QLatin1String("missing") ? 1 : 0);
    if (!publishes.isEmpty())
        QVERIFY(answer(publishes.first()));
    if (reply == QLatin1String("matching") || reply == QLatin1String("missing"))
        QCOMPARE(manager->publishedSessionState(id), XMPP::Jingle::PublicationManager::SessionState::Active);
    else
        QVERIFY(manager->publishedSessionState(id) != XMPP::Jingle::PublicationManager::SessionState::Active);

    if (reply == QLatin1String("matching")) {
        // An authenticated own device can request media before presence has
        // established its capabilities. The acknowledged session needs an offer.
        QDomDocument document;
        auto         iq = document.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("iq"));
        iq.setAttribute(QStringLiteral("type"), QStringLiteral("get"));
        iq.setAttribute(QStringLiteral("id"), QStringLiteral("media-start"));
        iq.setAttribute(QStringLiteral("from"), config.jid + QStringLiteral("/requester"));
        auto start = document.createElementNS(QStringLiteral("urn:xmpp:jinglepub:1"), QStringLiteral("start"));
        start.setAttribute(QStringLiteral("id"), id);
        iq.appendChild(start);
        QVERIFY(client.rootTask()->take(iq));
        const auto sessions = client.jingleManager()->findChildren<XMPP::Jingle::Session *>();
        QCOMPARE(sessions.size(), 1);
        const auto contents = sessions.first()->contentList();
        QCOMPARE(contents.size(), 1);
        QVERIFY(contents.first()->transport());
    }
}

void IrisJinglePublicationProviderTest::discoversChunkedReplicas_data()
{
    QTest::addColumn<bool>("knownHash");
    QTest::newRow("known-ciphertext-hash") << true;
    QTest::newRow("hash-used-candidate") << false;
}

void IrisJinglePublicationProviderTest::discoversChunkedReplicas()
{
    QFETCH(bool, knownHash);
    QTemporaryDir directory;
    XmppConfig    config;
    config.jid        = QStringLiteral("owner@example.test");
    config.instanceId = QStringLiteral("replicas");
    IrisJinglePublicationProvider provider(nullptr, config, directory.filePath(QStringLiteral("offers")),
                                           SecureEnvelope::generateMasterKey());
    auto                          capability = validCapability(config, true, knownHash);
    capability.publicationId = capability.itemId = QStringLiteral("replica");
    capability.node                              = config.jinglePubNodeName();
    const auto replica                           = provider.publication(capability);
    QVERIFY(replica.isValid());
    const XMPP::Jingle::PublishedSessionEndpoint endpoint { XMPP::Jid(config.jid), config.jinglePubNodeName(), true };
    QSignalSpy changes(&provider, &IrisJinglePublicationProvider::publicationsChanged);
    provider.publishedSessionObserved(endpoint, capability.itemId, replica);
    QCOMPARE(changes.count(), 1);
    IrisChunkedMediaSource descriptor;
    descriptor.parameters = capability.chunked;
    descriptor.wireHash   = capability.cipherHash;
    auto found            = provider.matchingPublications(descriptor, capability.reference.id);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().id(), replica.id());
    // A same-sized object from a different account is never a candidate.
    auto foreign = replica;
    foreign.setFrom(XMPP::Jid(QStringLiteral("other@example.test/device")));
    provider.publishedSessionObserved(endpoint, QStringLiteral("foreign"), foreign);
    QCOMPARE(provider.matchingPublications(descriptor, capability.reference.id).size(), 1);
    if (knownHash) {
        auto changed     = descriptor;
        changed.wireHash = QByteArray(32, 'x');
        QVERIFY(provider.matchingPublications(changed, capability.reference.id).isEmpty());
    } else {
        QVERIFY(provider.matchingPublications(descriptor, QUuid::createUuid()).isEmpty());
    }
    provider.publishedSessionRetracted(endpoint, capability.itemId);
    QVERIFY(provider.matchingPublications(descriptor, capability.reference.id).isEmpty());
}

void IrisJinglePublicationProviderTest::cachedReplicaServesExactAuthenticatedWireRanges()
{
    QTemporaryDir    directory;
    const QByteArray plain("A video replica whose source computer is offline.");
    const auto       parameters
        = *MediaChunkWire::generate(plain.size(), QCryptographicHash::hash(plain, QCryptographicHash::Sha256), 8);
    auto cache
        = std::make_shared<MediaPartialChunkCache>(MediaPartialChunkCache::Format::VerifiedOpaque, directory.path());
    QByteArray wire;
    for (quint64 i = 0; i < parameters.chunkCount(); ++i) {
        const auto record = MediaChunkWire::encryptChunk(parameters, i, plain.mid(i * 8, 8)).value;
        QVERIFY(cache->write(i, record, record.size(), [parameters](quint64 index, const QByteArray &bytes) {
            return bool(MediaChunkWire::decryptChunk(parameters, index, bytes));
        }));
        wire += record;
    }
    IrisCachedMediaStream stream(parameters, cache);
    QVERIFY(stream.open(QIODevice::ReadOnly));
    QCOMPARE(stream.size(), qint64(wire.size()));
    QVERIFY(stream.seek(3));
    QCOMPARE(stream.read(91), wire.mid(3, 91));
    QVERIFY(stream.seek(wire.size() - 5));
    QCOMPARE(stream.readAll(), wire.right(5));
    QVERIFY(stream.seek(0));
    QCOMPARE(stream.readAll(), wire);
    // A cached file is never trusted merely because it was verified earlier.
    QFile corrupt(directory.filePath(QStringLiteral("0")));
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    auto first = wire.left(*MediaChunkWire::wireChunkSize(parameters, 0));
    first[first.size() - 1] ^= 1;
    QCOMPARE(corrupt.write(first), first.size());
    corrupt.close();
    IrisCachedMediaStream damaged(parameters, cache);
    QVERIFY(damaged.open(QIODevice::ReadOnly));
    char byte;
    QCOMPARE(damaged.read(&byte, 1), qint64(-1));
    QVERIFY(!damaged.errorString().isEmpty());
}

void IrisJinglePublicationProviderTest::rejectsChangedServerDescriptor_data()
{
    QTest::addColumn<QString>("field");
    for (const auto &field : { "size", "name", "name-whitespace", "media-type", "hash-value", "hash-algorithm",
                               "attribute", "extension", "text", "uri", "publisher", "item", "node" })
        QTest::newRow(field) << QString::fromLatin1(field);
}

void IrisJinglePublicationProviderTest::rejectsChangedServerDescriptor()
{
    QFETCH(QString, field);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    XmppConfig config;
    config.instanceId = QStringLiteral("publication-test");
    config.jid        = QStringLiteral("owner@example.test");
    XMPP::Client                  client;
    auto                         *manager = client.jingleManager()->publicationManager();
    IrisJinglePublicationProvider provider(nullptr, config, directory.filePath(QStringLiteral("publications.bin")),
                                           SecureEnvelope::generateMasterKey());
    manager->registerProvider(&provider);
    const auto prepared = provider.prepare(validCapability(config, true, true));
    QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
    QVERIFY(prepared.publication.isValid());
    const auto   original = fingerprint(manager->publishedSession(prepared.publication.id()));
    QDomDocument outgoing;
    outgoing.appendChild(prepared.publication.toXml(&outgoing));
    auto description = outgoing.documentElement().firstChildElement(QStringLiteral("description"));
    auto file        = description.firstChildElement(QStringLiteral("file"));
    QVERIFY(!file.isNull());
    if (field == QLatin1String("size") || field == QLatin1String("name") || field == QLatin1String("media-type")) {
        auto text = file.firstChildElement(field).firstChild();
        QVERIFY(!text.isNull());
        text.setNodeValue(text.nodeValue() + QLatin1Char('1'));
    } else if (field == QLatin1String("name-whitespace")) {
        auto text = file.firstChildElement(QStringLiteral("name")).firstChild();
        text.setNodeValue(QLatin1Char(' ') + text.nodeValue() + QLatin1Char(' '));
    } else if (field == QLatin1String("hash-value")) {
        file.firstChildElement(QStringLiteral("hash"))
            .firstChild()
            .setNodeValue(QString::fromLatin1(QByteArray(32, 'z').toBase64()));
    } else if (field == QLatin1String("hash-algorithm")) {
        file.firstChildElement(QStringLiteral("hash")).setAttribute(QStringLiteral("algo"), QStringLiteral("sha-512"));
    } else if (field == QLatin1String("attribute")) {
        file.setAttribute(QStringLiteral("extra"), QStringLiteral("changed"));
    } else if (field == QLatin1String("extension")) {
        file.appendChild(outgoing.createElementNS(QStringLiteral("urn:test:extra"), QStringLiteral("extra")));
    } else if (field == QLatin1String("text")) {
        file.appendChild(outgoing.createTextNode(QStringLiteral("changed")));
    } else if (field == QLatin1String("uri")) {
        outgoing.documentElement()
            .firstChildElement(QStringLiteral("uri"))
            .firstChild()
            .setNodeValue(QStringLiteral("urn:uuid:changed"));
    } else if (field == QLatin1String("publisher")) {
        outgoing.documentElement().setAttribute(QStringLiteral("from"), config.jid + QStringLiteral("/other-resource"));
    }
    const auto                    incoming = parseIrisStream(outgoing.toByteArray());
    const XMPP::Jingle::JinglePub changed(incoming.documentElement());
    QVERIFY(changed.isValid());
    XMPP::Jingle::PublishedSessionEndpoint endpoint { XMPP::Jid(config.jid), config.jinglePubNodeName(), true };
    auto                                   itemId = prepared.publication.id();
    if (field == QLatin1String("item"))
        itemId += QStringLiteral("-other");
    if (field == QLatin1String("node"))
        endpoint.node += QStringLiteral("-other");
    provider.publishedSessionObserved(endpoint, itemId, changed);
    QCOMPARE(fingerprint(manager->publishedSession(prepared.publication.id())), original);
    QCOMPARE(manager->publishedSessionState(prepared.publication.id()),
             XMPP::Jingle::PublicationManager::SessionState::Unverified);
}

} // namespace AnyKeep

QTEST_GUILESS_MAIN(AnyKeep::IrisJinglePublicationProviderTest)
#include "irisjinglepublicationprovider_test.moc"
