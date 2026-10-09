#include "iris/irisjinglepublicationprovider.h"
#include "secureenvelope.h"

#include <iris/jingle-ft.h>
#include <iris/xmpp_client.h>

#include <QCryptographicHash>
#include <QDomDocument>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

using namespace AnyKeep;

namespace {

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
