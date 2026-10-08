#include "iris/irischunkedmediasource.h"

#include <QCryptographicHash>
#include <QDomDocument>
#include <QtTest>

#include <functional>

using namespace AnyKeep;

namespace {

QByteArray plainChecksum()
{
    return QCryptographicHash::hash(QByteArrayLiteral("plain media payload"), QCryptographicHash::Sha256);
}

IrisChunkedMediaSource validDescriptor()
{
    const auto parameters = MediaChunkWire::generate(2 * MediaChunkWireParameters::DefaultChunkSize + 17,
                                                     plainChecksum());
    Q_ASSERT(parameters);

    XMPP::StatelessFileSharing::Sources transports;
    transports.add(XMPP::StatelessFileSharing::Source::fromUrl(QUrl(QStringLiteral("https://example.test/media.bin"))));

    IrisChunkedMediaSource descriptor;
    descriptor.parameters = *parameters;
    descriptor.wireHash   = QCryptographicHash::hash(QByteArrayLiteral("complete wire object"),
                                                     QCryptographicHash::Sha256);
    descriptor.sources    = transports;
    return descriptor;
}

XMPP::StatelessFileSharing::Source mutatedSource(
    const XMPP::StatelessFileSharing::Source &source,
    const std::function<void(QDomElement &)> &mutator)
{
    QDomDocument document;
    auto element = document.importNode(source.rawElement(), true).toElement();
    document.appendChild(element);
    mutator(element);
    return XMPP::StatelessFileSharing::Source::fromElement(document.documentElement());
}

QDomElement directChild(QDomElement parent, const QString &nameSpace, const QString &localName)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto child = node.toElement();
        if (!child.isNull() && child.namespaceURI() == nameSpace && child.localName() == localName)
            return child;
    }
    return {};
}

} // namespace

class IrisChunkedMediaSourceTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripPreservesSecretsGeometryAndTransports();
    void rejectsTamperedWireSize();
    void rejectsMalformedSecretEncoding();
    void rejectsNonHttpNestedTransport();
};

void IrisChunkedMediaSourceTest::roundTripPreservesSecretsGeometryAndTransports()
{
    const auto descriptor = validDescriptor();
    QVERIFY2(descriptor.isValid(), qPrintable(descriptor.invalidReason()));

    const auto source = descriptor.toSource();
    QCOMPARE(source.type(), XMPP::StatelessFileSharing::Source::Type::Other);

    const auto parsed = IrisChunkedMediaSource::fromSource(
        source, descriptor.parameters.plainSize, descriptor.parameters.plainChecksum);
    QVERIFY2(parsed, qPrintable(parsed.error));
    QCOMPARE(parsed.value.parameters.rootKey, descriptor.parameters.rootKey);
    QCOMPARE(parsed.value.parameters.noncePrefix, descriptor.parameters.noncePrefix);
    QCOMPARE(parsed.value.parameters.plainChecksum, descriptor.parameters.plainChecksum);
    QCOMPARE(parsed.value.parameters.plainSize, descriptor.parameters.plainSize);
    QCOMPARE(parsed.value.parameters.chunkSize, descriptor.parameters.chunkSize);
    QCOMPARE(parsed.value.wireHash, descriptor.wireHash);
    QCOMPARE(parsed.value.sources.items().size(), 1);
    QCOMPARE(parsed.value.sources.items().constFirst().type(), XMPP::StatelessFileSharing::Source::Type::UrlData);
    QCOMPARE(parsed.value.sources.items().constFirst().url(), QUrl(QStringLiteral("https://example.test/media.bin")));
}

void IrisChunkedMediaSourceTest::rejectsTamperedWireSize()
{
    const auto descriptor = validDescriptor();
    const auto source     = descriptor.toSource();
    const auto expected   = MediaChunkWire::wireSize(descriptor.parameters);
    QVERIFY(expected);

    const auto tampered = mutatedSource(source, [expected](QDomElement &element) {
        element.setAttribute(QStringLiteral("wire-size"), QString::number(*expected + 1));
    });
    const auto parsed = IrisChunkedMediaSource::fromSource(
        tampered, descriptor.parameters.plainSize, descriptor.parameters.plainChecksum);
    QVERIFY(!parsed);
    QVERIFY(parsed.error.contains(QStringLiteral("wire size")));
}

void IrisChunkedMediaSourceTest::rejectsMalformedSecretEncoding()
{
    const auto descriptor = validDescriptor();
    const auto source     = descriptor.toSource();
    const auto tampered   = mutatedSource(source, [](QDomElement &element) {
        auto key = directChild(element, IrisChunkedMediaSource::Namespace, QStringLiteral("key"));
        QVERIFY(!key.isNull());
        while (!key.firstChild().isNull())
            key.removeChild(key.firstChild());
        key.appendChild(key.ownerDocument().createTextNode(QStringLiteral("not base64 !!!")));
    });
    const auto parsed = IrisChunkedMediaSource::fromSource(
        tampered, descriptor.parameters.plainSize, descriptor.parameters.plainChecksum);
    QVERIFY(!parsed);
    QVERIFY(parsed.error.contains(QStringLiteral("key or nonce prefix")));
}

void IrisChunkedMediaSourceTest::rejectsNonHttpNestedTransport()
{
    const auto descriptor = validDescriptor();
    const auto source     = descriptor.toSource();
    const auto tampered   = mutatedSource(source, [](QDomElement &element) {
        auto sources = directChild(element, XMPP::StatelessFileSharing::NS, QStringLiteral("sources"));
        QVERIFY(!sources.isNull());
        auto transport = sources.firstChildElement();
        QVERIFY(!transport.isNull());
        transport.setAttribute(QStringLiteral("target"), QStringLiteral("ftp://example.test/media.bin"));
    });
    const auto parsed = IrisChunkedMediaSource::fromSource(
        tampered, descriptor.parameters.plainSize, descriptor.parameters.plainChecksum);
    QVERIFY(!parsed);
    QVERIFY(parsed.error.contains(QStringLiteral("HTTP(S)")));
}

QTEST_GUILESS_MAIN(IrisChunkedMediaSourceTest)
#include "irischunkedmediasource_test.moc"
