#include "irischunkedmediasource.h"

#include "secureenvelope.h"

#include <iris/xmpp_hash.h>

#include <QDomDocument>
#include <QSet>

#include <limits>

namespace AnyKeep {

const QString IrisChunkedMediaSource::Namespace   = QStringLiteral("urn:xmpp:private-notes:media-chunks:0");
const QString IrisChunkedMediaSource::ElementName = QStringLiteral("encrypted-chunks");

namespace {

const QString HashNamespace = QStringLiteral("urn:xmpp:hashes:2");
const QString SfsNamespace  = QStringLiteral("urn:xmpp:sfs:0");

QString localName(const QDomElement &element)
{
    const auto local = element.localName();
    return local.isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : local;
}

bool isNamespaceDeclaration(const QDomNode &attribute)
{
    return attribute.namespaceURI() == QStringLiteral("http://www.w3.org/2000/xmlns/")
        || attribute.nodeName() == QStringLiteral("xmlns") || attribute.prefix() == QStringLiteral("xmlns");
}

QList<QDomElement> directChildren(const QDomElement &parent, const QString &nameSpace, const QString &name)
{
    QList<QDomElement> result;
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && element.namespaceURI() == nameSpace && localName(element) == name)
            result.append(element);
    }
    return result;
}

bool hasOnlyAttributes(const QDomElement &element, const QSet<QString> &allowed)
{
    const auto attributes = element.attributes();
    for (int index = 0; index < attributes.count(); ++index) {
        const auto attribute = attributes.item(index);
        if (isNamespaceDeclaration(attribute))
            continue;
        if (!attribute.namespaceURI().isEmpty() || !allowed.contains(attribute.nodeName()))
            return false;
    }
    return true;
}

std::optional<QByteArray> strictBase64Text(const QDomElement &element)
{
    QString text;
    for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
        if (node.isText() || node.isCDATASection()) {
            text += node.nodeValue();
        } else if (!node.isComment()) {
            return std::nullopt;
        }
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        text.trimmed().toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    return decoded.decoded;
}

bool isHttpUrl(const QUrl &url)
{
    return url.isValid()
        && (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
            || url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0);
}

} // namespace

QString IrisChunkedMediaSource::invalidReason() const
{
    if (!parameters.isValid())
        return QStringLiteral("chunked media parameters are invalid");
    if (!wireHash.isEmpty() && wireHash.size() != 32)
        return QStringLiteral("chunked media wire checksum is not SHA-256");
    const auto expectedWireSize = MediaChunkWire::wireSize(parameters);
    if (!expectedWireSize || *expectedWireSize > quint64(std::numeric_limits<qint64>::max()))
        return QStringLiteral("chunked media wire size cannot be represented");
    if (!sources.isValid() || sources.isEmpty())
        return QStringLiteral("chunked media has no transport sources");
    for (const auto &source : sources.items()) {
        switch (source.type()) {
        case XMPP::StatelessFileSharing::Source::Type::UrlData:
            if (!isHttpUrl(source.url()))
                return QStringLiteral("chunked media URL source is not HTTP(S)");
            break;
        case XMPP::StatelessFileSharing::Source::Type::JinglePub:
            if (!source.jinglePub().isValid())
                return QStringLiteral("chunked media Jingle publication is invalid");
            break;
        default:
            return QStringLiteral("chunked media contains a non-transport nested source");
        }
    }
    return {};
}

QDomElement IrisChunkedMediaSource::toXml(QDomDocument *document) const
{
    if (!document || !isValid())
        return {};
    const auto wireSize = MediaChunkWire::wireSize(parameters);
    if (!wireSize)
        return {};

    auto element = document->createElementNS(Namespace, ElementName);
    element.setAttribute(QStringLiteral("version"), QString::number(Version));
    element.setAttribute(QStringLiteral("chunk-size"), QString::number(parameters.chunkSize));
    element.setAttribute(QStringLiteral("wire-size"), QString::number(*wireSize));

    auto key = document->createElementNS(Namespace, QStringLiteral("key"));
    key.appendChild(document->createTextNode(QString::fromLatin1(parameters.rootKey.toBase64())));
    element.appendChild(key);

    auto noncePrefix = document->createElementNS(Namespace, QStringLiteral("nonce-prefix"));
    noncePrefix.appendChild(document->createTextNode(QString::fromLatin1(parameters.noncePrefix.toBase64())));
    element.appendChild(noncePrefix);

    if (!wireHash.isEmpty()) {
        const XMPP::Hash hash(XMPP::Hash::Sha256, wireHash);
        const auto       hashElement = hash.toXml(document);
        if (hashElement.isNull())
            return {};
        element.appendChild(hashElement);
    }

    const auto transportSources = sources.toXml(document);
    if (transportSources.isNull())
        return {};
    element.appendChild(transportSources);
    return element;
}

XMPP::StatelessFileSharing::Source IrisChunkedMediaSource::toSource() const
{
    QDomDocument document;
    const auto   element = toXml(&document);
    if (element.isNull())
        return {};
    document.appendChild(element);
    return XMPP::StatelessFileSharing::Source::fromElement(document.documentElement());
}

IrisChunkedMediaSourceResult
IrisChunkedMediaSource::fromSource(const XMPP::StatelessFileSharing::Source &source, quint64 plainSize,
                                   const QByteArray &plainChecksum)
{
    if (source.type() != XMPP::StatelessFileSharing::Source::Type::Other)
        return { {}, QStringLiteral("XEP-0447 source is not an AnyKeep chunked media source") };
    const auto element = source.rawElement();
    if (element.isNull() || element.namespaceURI() != Namespace || localName(element) != ElementName)
        return { {}, QStringLiteral("XEP-0447 source has the wrong chunked media namespace or element") };
    if (!hasOnlyAttributes(element,
                           { QStringLiteral("version"), QStringLiteral("chunk-size"), QStringLiteral("wire-size") })) {
        return { {}, QStringLiteral("Chunked media source contains unsupported attributes") };
    }

    bool okVersion   = false;
    bool okChunkSize = false;
    bool okWireSize  = false;
    const auto version = element.attribute(QStringLiteral("version")).toUInt(&okVersion);
    const auto chunkSize64 = element.attribute(QStringLiteral("chunk-size")).toULongLong(&okChunkSize);
    const auto declaredWireSize = element.attribute(QStringLiteral("wire-size")).toULongLong(&okWireSize);
    if (!okVersion || version != Version || !okChunkSize || chunkSize64 == 0
        || chunkSize64 > MediaChunkWireParameters::MaxChunkSize || !okWireSize) {
        return { {}, QStringLiteral("Chunked media source has invalid geometry") };
    }

    const auto keys          = directChildren(element, Namespace, QStringLiteral("key"));
    const auto noncePrefixes = directChildren(element, Namespace, QStringLiteral("nonce-prefix"));
    const auto hashes        = directChildren(element, HashNamespace, QStringLiteral("hash"));
    const auto sourceLists   = directChildren(element, SfsNamespace, QStringLiteral("sources"));
    if (keys.size() != 1 || noncePrefixes.size() != 1 || hashes.size() > 1 || sourceLists.size() != 1)
        return { {}, QStringLiteral("Chunked media source has invalid child cardinality") };

    for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
        if (node.isText() || node.isCDATASection()) {
            if (!node.nodeValue().trimmed().isEmpty())
                return { {}, QStringLiteral("Chunked media source contains unexpected text") };
            continue;
        }
        if (node.isComment())
            continue;
        const auto child = node.toElement();
        if (child.isNull())
            return { {}, QStringLiteral("Chunked media source contains an unsupported XML node") };
        const bool known = (child.namespaceURI() == Namespace
                            && (localName(child) == QStringLiteral("key")
                                || localName(child) == QStringLiteral("nonce-prefix")))
            || (child.namespaceURI() == HashNamespace && localName(child) == QStringLiteral("hash"))
            || (child.namespaceURI() == SfsNamespace && localName(child) == QStringLiteral("sources"));
        if (!known)
            return { {}, QStringLiteral("Chunked media source contains an unsupported child element") };
    }

    if (!hasOnlyAttributes(keys.constFirst(), {}) || !hasOnlyAttributes(noncePrefixes.constFirst(), {}))
        return { {}, QStringLiteral("Chunked media secret element contains unsupported attributes") };
    const auto key         = strictBase64Text(keys.constFirst());
    const auto noncePrefix = strictBase64Text(noncePrefixes.constFirst());
    if (!key || key->size() != SecureEnvelope::MasterKeySize || !noncePrefix || noncePrefix->size() != 4)
        return { {}, QStringLiteral("Chunked media key or nonce prefix is invalid") };

    IrisChunkedMediaSource result;
    result.parameters.rootKey       = *key;
    result.parameters.noncePrefix   = *noncePrefix;
    result.parameters.plainChecksum = plainChecksum;
    result.parameters.plainSize     = plainSize;
    result.parameters.chunkSize     = quint32(chunkSize64);
    if (!hashes.isEmpty()) {
        const XMPP::Hash hash(hashes.constFirst());
        if (hash.type() != XMPP::Hash::Sha256 || hash.data().size() != 32)
            return { {}, QStringLiteral("Chunked media wire hash is not SHA-256") };
        result.wireHash = hash.data();
    }
    result.sources = XMPP::StatelessFileSharing::Sources(sourceLists.constFirst());

    const auto expectedWireSize = MediaChunkWire::wireSize(result.parameters);
    if (!expectedWireSize || declaredWireSize != *expectedWireSize)
        return { {}, QStringLiteral("Chunked media declared wire size does not match its geometry") };
    if (const auto reason = result.invalidReason(); !reason.isEmpty())
        return { {}, reason };
    return { result, {} };
}

} // namespace AnyKeep
