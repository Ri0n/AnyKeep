#include "xmpppayloadxml.h"

#include "xmppnotecodec.h"

#include <QDomDocument>
#include <QDomNode>

namespace AnyKeep {
namespace {
    QString elementLocalName(const QDomElement &element)
    {
        const auto local = element.localName();
        return local.isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : local;
    }

    QByteArray compactBase64(const QString &text)
    {
        QByteArray compact;
        const auto encoded = text.toLatin1();
        compact.reserve(encoded.size());
        for (const char ch : encoded) {
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
                compact.append(ch);
        }
        return compact;
    }

    bool isBase64Alphabet(const QByteArray &encoded, bool urlSafe, bool allowPadding)
    {
        if (encoded.isEmpty() || encoded.size() % 4 == 1)
            return false;
        qsizetype padding = 0;
        while (padding < encoded.size() && encoded.at(encoded.size() - 1 - padding) == '=')
            ++padding;
        if ((!allowPadding && padding) || padding > 2 || (padding && encoded.size() % 4 != 0))
            return false;
        for (qsizetype i = 0; i < encoded.size() - padding; ++i) {
            const char ch           = encoded.at(i);
            const bool alphaNumeric = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
            if (!alphaNumeric && !(urlSafe ? (ch == '-' || ch == '_') : (ch == '+' || ch == '/')))
                return false;
        }
        return true;
    }

    bool isPayloadChild(const QDomElement &element, const QString &name)
    {
        if (element.isNull() || elementLocalName(element) != name)
            return false;
        if (element.namespaceURI() == XmppPayloadXml::payloadNamespace)
            return true;

        // Some live stanza DOM implementations leave inherited default namespaces
        // empty on an unprefixed child. Accept that representation as long as it
        // does not explicitly declare another namespace.
        if (!element.namespaceURI().isEmpty() || !element.prefix().isEmpty())
            return false;
        if (element.hasAttribute(QStringLiteral("xmlns")))
            return element.attribute(QStringLiteral("xmlns")) == XmppPayloadXml::payloadNamespace;
        return true;
    }

    QList<QDomElement> directChildren(const QDomElement &parent, const QString &name)
    {
        QList<QDomElement> result;
        for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const auto element = node.toElement();
            if (isPayloadChild(element, name))
                result.append(element);
        }
        return result;
    }

    bool isNamespaceDeclaration(const QDomNode &attribute)
    {
        return attribute.namespaceURI() == QStringLiteral("http://www.w3.org/2000/xmlns/")
            || attribute.nodeName() == QStringLiteral("xmlns") || attribute.prefix() == QStringLiteral("xmlns");
    }

    bool hasUnknownCoreAttributes(const QDomElement &element)
    {
        const auto attributes = element.attributes();
        for (int i = 0; i < attributes.count(); ++i) {
            const auto attribute = attributes.item(i);
            if (isNamespaceDeclaration(attribute))
                continue;
            if (attribute.namespaceURI().isEmpty() && attribute.nodeName() != QStringLiteral("key-id"))
                return true;
        }
        return false;
    }

    bool hasProtocolAttributes(const QDomElement &element)
    {
        const auto attributes = element.attributes();
        for (int i = 0; i < attributes.count(); ++i) {
            if (!isNamespaceDeclaration(attributes.item(i)))
                return true;
        }
        return false;
    }

    bool hasElementChildren(const QDomElement &element)
    {
        for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
            if (node.isElement())
                return true;
        }
        return false;
    }

    bool hasNonWhitespaceDirectText(const QDomElement &element)
    {
        for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
            if ((node.isText() || node.isCDATASection()) && !node.nodeValue().trimmed().isEmpty())
                return true;
        }
        return false;
    }

    bool hasUnknownCoreChildren(const QDomElement &element)
    {
        for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const auto child = node.toElement();
            if (child.isNull())
                continue;
            if (isPayloadChild(child, QStringLiteral("nonce")) || isPayloadChild(child, QStringLiteral("payload"))
                || isPayloadChild(child, QStringLiteral("tag"))) {
                continue;
            }
            if (child.namespaceURI().isEmpty() || child.namespaceURI() == XmppPayloadXml::payloadNamespace)
                return true;
        }
        return false;
    }

    bool decodeCanonicalBase64(const QDomElement &element, QByteArray *output, qsizetype maximumDecodedSize)
    {
        const auto encoded            = compactBase64(element.text());
        const auto maximumEncodedSize = ((maximumDecodedSize + 2) / 3) * 4;
        if (encoded.size() > maximumEncodedSize || !isBase64Alphabet(encoded, false, true))
            return false;
        *output = QByteArray::fromBase64(encoded, QByteArray::Base64Encoding);
        return output->toBase64(QByteArray::Base64Encoding) == encoded;
    }

    QDomElement appendBinary(QDomDocument &document, QDomElement &parent, const QString &name, const QByteArray &value)
    {
        auto child = document.createElementNS(XmppPayloadXml::payloadNamespace, name);
        child.appendChild(document.createTextNode(QString::fromLatin1(value.toBase64())));
        parent.appendChild(child);
        return child;
    }
} // namespace

const QString XmppPayloadXml::payloadNamespace       = QStringLiteral("urn:xmpp:private-notes:0");
const QString XmppPayloadXml::legacyPayloadNamespace = QStringLiteral("urn:xmpp:private-notes:encrypted:0");

bool XmppPayloadXml::isEncryptedPayload(const QDomElement &element)
{
    return !element.isNull() && elementLocalName(element) == QStringLiteral("encrypted")
        && (element.namespaceURI() == payloadNamespace || element.namespaceURI() == legacyPayloadNamespace);
}

XmppPayloadParseResult XmppPayloadXml::parse(const QString &itemId, const QDomElement &element)
{
    XmppPayloadParseResult result;
    result.failure    = XmppPayloadParseFailure::Malformed;
    result.payload.id = itemId;

    if (!isEncryptedPayload(element)) {
        // A payload with the expected element name but an unknown namespace may
        // belong to a future incompatible protocol major.  It is unreadable by
        // this implementation, but it must never be classified as malformed: the
        // maintenance path is allowed to remove malformed current-major data.
        if (!element.isNull() && elementLocalName(element) == QStringLiteral("encrypted")
            && !element.namespaceURI().isEmpty()) {
            result.failure = XmppPayloadParseFailure::UnsupportedFormat;
            result.error   = QStringLiteral("Unsupported encrypted private-note payload namespace");
        } else {
            result.error = QStringLiteral("Missing encrypted private-note payload");
        }
        return result;
    }
    if (element.namespaceURI() != payloadNamespace || element.hasAttribute(QStringLiteral("wire"))
        || element.hasAttribute(QStringLiteral("schema")) || element.hasAttribute(QStringLiteral("kind"))
        || (!hasElementChildren(element) && !compactBase64(element.text()).isEmpty())) {
        result.failure = XmppPayloadParseFailure::ObsoleteFormat;
        result.error   = QStringLiteral("Obsolete pre-unified private-note encrypted payload");
        return result;
    }
    if (hasUnknownCoreAttributes(element) || hasUnknownCoreChildren(element)) {
        result.error = QStringLiteral("Unknown core field in encrypted private-note payload");
        return result;
    }

    const auto encodedKeyId = element.attribute(QStringLiteral("key-id")).toLatin1();
    if (result.payload.id.isEmpty() || encodedKeyId.size() != 43 || !isBase64Alphabet(encodedKeyId, true, false)) {
        result.error = QStringLiteral("Incomplete or malformed encrypted private-note payload");
        return result;
    }
    result.payload.keyId = QByteArray::fromBase64(encodedKeyId, QByteArray::Base64UrlEncoding);
    if (result.payload.keyId.size() != 32
        || result.payload.keyId.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
            != encodedKeyId) {
        result.error = QStringLiteral("Non-canonical key ID in encrypted private-note payload");
        return result;
    }

    const auto nonceElements   = directChildren(element, QStringLiteral("nonce"));
    const auto payloadElements = directChildren(element, QStringLiteral("payload"));
    const auto tagElements     = directChildren(element, QStringLiteral("tag"));
    if (nonceElements.size() != 1 || payloadElements.size() != 1 || tagElements.size() != 1
        || hasNonWhitespaceDirectText(element)) {
        result.error = QStringLiteral("Incomplete or malformed encrypted private-note XML envelope");
        return result;
    }
    constexpr qsizetype MaximumEncodedPayloadSize = ((XmppNoteCodec::MaximumXmlSize + 2) / 3) * 4;
    if (compactBase64(payloadElements.constFirst().text()).size() > MaximumEncodedPayloadSize) {
        result.failure = XmppPayloadParseFailure::UnsupportedFormat;
        result.error   = QStringLiteral("Encrypted private-note payload exceeds the implementation size limit");
        return result;
    }
    const auto simpleBinaryElement
        = [](const QDomElement &field) { return !hasProtocolAttributes(field) && !hasElementChildren(field); };
    if (!simpleBinaryElement(nonceElements.constFirst()) || !simpleBinaryElement(payloadElements.constFirst())
        || !simpleBinaryElement(tagElements.constFirst())
        || !decodeCanonicalBase64(nonceElements.constFirst(), &result.payload.nonce, 12)
        || !decodeCanonicalBase64(tagElements.constFirst(), &result.payload.tag, 16)
        || !decodeCanonicalBase64(payloadElements.constFirst(), &result.payload.cipherText,
                                  XmppNoteCodec::MaximumXmlSize)) {
        result.error = QStringLiteral("Invalid Base64 in encrypted private-note XML envelope");
        return result;
    }
    if (result.payload.nonce.size() != 12 || result.payload.tag.size() != 16 || result.payload.cipherText.isEmpty()) {
        result.error = QStringLiteral("Invalid encrypted private-note XML envelope fields");
        return result;
    }

    result.failure = XmppPayloadParseFailure::None;
    result.valid   = true;
    return result;
}

QDomElement XmppPayloadXml::serialize(QDomDocument &document, const XmppEncryptedPayload &payload)
{
    auto encrypted = document.createElementNS(payloadNamespace, QStringLiteral("encrypted"));
    encrypted.setAttribute(
        QStringLiteral("key-id"),
        QString::fromLatin1(payload.keyId.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    appendBinary(document, encrypted, QStringLiteral("nonce"), payload.nonce);
    appendBinary(document, encrypted, QStringLiteral("payload"), payload.cipherText);
    appendBinary(document, encrypted, QStringLiteral("tag"), payload.tag);
    return encrypted;
}

} // namespace AnyKeep
