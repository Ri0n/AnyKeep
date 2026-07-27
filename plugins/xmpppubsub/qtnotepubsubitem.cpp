#include "qtnotepubsubitem.h"

#include "xmppnotecodec.h"

#include <QDomElement>
#include <QXmlStreamWriter>

#include <limits>

namespace QtNote {
namespace {
    QString elementLocalName(const QDomElement &element)
    {
        const auto local = element.localName();
        return local.isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : local;
    }

    QString versionString(const XmppFormatVersion &version)
    {
        return QStringLiteral("%1.%2").arg(version.major).arg(version.minor);
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

    bool parseVersion(const QString &text, XmppFormatVersion *version)
    {
        const auto parts = text.split(QLatin1Char('.'));
        if (parts.size() != 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty())
            return false;
        bool       majorOk = false;
        bool       minorOk = false;
        const auto major   = parts.at(0).toUInt(&majorOk);
        const auto minor   = parts.at(1).toUInt(&minorOk);
        if (!majorOk || !minorOk || major > std::numeric_limits<quint16>::max()
            || minor > std::numeric_limits<quint16>::max()) {
            return false;
        }
        *version = { quint16(major), quint16(minor) };
        return true;
    }

    bool isPayloadChild(const QDomElement &element, const QString &name)
    {
        if (element.isNull() || elementLocalName(element) != name)
            return false;
        if (element.namespaceURI() == QtNotePubSubItem::payloadNamespace)
            return true;

        // QXmpp's live stanza DOM may expose an unprefixed child with an empty
        // namespace URI even though the wire XML inherits the encrypted
        // element's default namespace. Accept only that representation; an
        // explicit xmlns="" reset remains invalid.
        if (!element.namespaceURI().isEmpty() || !element.prefix().isEmpty())
            return false;
        if (element.hasAttribute(QStringLiteral("xmlns")))
            return element.attribute(QStringLiteral("xmlns")) == QtNotePubSubItem::payloadNamespace;
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

    bool decodeCanonicalBase64(const QDomElement &element, QByteArray *output, qsizetype maximumDecodedSize)
    {
        const auto encoded            = compactBase64(element.text());
        const auto maximumEncodedSize = ((maximumDecodedSize + 2) / 3) * 4;
        if (encoded.size() > maximumEncodedSize || !isBase64Alphabet(encoded, false, true))
            return false;
        *output = QByteArray::fromBase64(encoded, QByteArray::Base64Encoding);
        return output->toBase64(QByteArray::Base64Encoding) == encoded;
    }
}

const QString QtNotePubSubItem::payloadNamespace = QStringLiteral("urn:xmpp:qtnote:encrypted:1");

QtNotePubSubItem::QtNotePubSubItem(const XmppEncryptedPayload &payload) :
    QXmppPubSubBaseItem(payload.id), payload_(payload), valid_(true)
{
}

bool QtNotePubSubItem::isItem(const QDomElement &element)
{
    if (!QXmppPubSubBaseItem::isItem(element))
        return false;
    const auto payload = element.firstChildElement();
    return !payload.isNull() && elementLocalName(payload) == QStringLiteral("encrypted")
        && payload.namespaceURI() == payloadNamespace;
}

void QtNotePubSubItem::parsePayload(const QDomElement &element)
{
    valid_ = false;
    parseError_.clear();
    parseFailure_ = ParseFailure::Malformed;
    payload_      = {};
    payload_.id   = id();

    const auto wireText   = element.attribute(QStringLiteral("wire"));
    const auto schemaText = element.attribute(QStringLiteral("schema"));
    if ((wireText.isEmpty() && schemaText == QStringLiteral("1"))
        || (!hasElementChildren(element) && !compactBase64(element.text()).isEmpty())) {
        parseFailure_ = ParseFailure::ObsoleteFormat;
        parseError_   = QStringLiteral("Obsolete pre-XML encrypted QtNote payload");
        return;
    }

    XmppFormatVersion wireVersion;
    XmppFormatVersion schemaVersion;
    const bool        wireOk   = parseVersion(wireText, &wireVersion);
    const bool        schemaOk = parseVersion(schemaText, &schemaVersion);
    if ((wireOk && wireVersion.major != XmppNoteCodec::WireMajor)
        || (schemaOk && schemaVersion.major != XmppNoteCodec::SchemaMajor)) {
        parseFailure_ = ParseFailure::UnsupportedFormat;
        parseError_   = QStringLiteral("Unsupported encrypted QtNote format version");
        return;
    }
    if (!wireOk || !schemaOk) {
        parseError_ = QStringLiteral("Malformed encrypted QtNote format version");
        return;
    }
    payload_.wireVersion   = wireVersion;
    payload_.schemaVersion = schemaVersion;

    const auto kind = element.attribute(QStringLiteral("kind"));
    if (kind == QStringLiteral("index"))
        payload_.kind = XmppEncryptedPayload::Index;
    else if (kind == QStringLiteral("content"))
        payload_.kind = XmppEncryptedPayload::Content;
    else {
        parseFailure_ = ParseFailure::UnsupportedFormat;
        parseError_   = QStringLiteral("Unsupported encrypted QtNote payload kind");
        return;
    }

    const auto encodedKeyId = element.attribute(QStringLiteral("key-id")).toLatin1();
    if (payload_.id.isEmpty() || encodedKeyId.size() != 43 || !isBase64Alphabet(encodedKeyId, true, false)) {
        parseError_ = QStringLiteral("Incomplete or malformed encrypted QtNote payload");
        return;
    }
    payload_.keyId = QByteArray::fromBase64(encodedKeyId, QByteArray::Base64UrlEncoding);
    if (payload_.keyId.size() != 32
        || payload_.keyId.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) != encodedKeyId) {
        parseError_ = QStringLiteral("Non-canonical key ID in encrypted QtNote payload");
        return;
    }

    const auto nonceElements   = directChildren(element, QStringLiteral("nonce"));
    const auto payloadElements = directChildren(element, QStringLiteral("payload"));
    const auto tagElements     = directChildren(element, QStringLiteral("tag"));
    if (nonceElements.size() != 1 || payloadElements.size() != 1 || tagElements.size() != 1
        || hasNonWhitespaceDirectText(element)) {
        parseError_ = QStringLiteral("Incomplete or malformed encrypted QtNote XML envelope");
        return;
    }
    constexpr qsizetype MaximumEncodedPayloadSize = ((XmppNoteCodec::MaximumXmlSize + 2) / 3) * 4;
    if (compactBase64(payloadElements.constFirst().text()).size() > MaximumEncodedPayloadSize) {
        parseFailure_ = ParseFailure::UnsupportedFormat;
        parseError_   = QStringLiteral("Encrypted QtNote payload exceeds the implementation size limit");
        return;
    }
    const auto simpleBinaryElement
        = [](const QDomElement &field) { return !hasProtocolAttributes(field) && !hasElementChildren(field); };
    if (!simpleBinaryElement(nonceElements.constFirst()) || !simpleBinaryElement(payloadElements.constFirst())
        || !simpleBinaryElement(tagElements.constFirst())
        || !decodeCanonicalBase64(nonceElements.constFirst(), &payload_.nonce, 12)
        || !decodeCanonicalBase64(tagElements.constFirst(), &payload_.tag, 16)
        || !decodeCanonicalBase64(payloadElements.constFirst(), &payload_.cipherText, XmppNoteCodec::MaximumXmlSize)) {
        parseError_ = QStringLiteral("Invalid Base64 in encrypted QtNote XML envelope");
        return;
    }
    if (payload_.nonce.size() != 12 || payload_.tag.size() != 16 || payload_.cipherText.isEmpty()) {
        parseError_ = QStringLiteral("Invalid encrypted QtNote XML envelope fields");
        return;
    }

    parseFailure_ = ParseFailure::None;
    valid_        = true;
}

void QtNotePubSubItem::serializePayload(QXmlStreamWriter *writer) const
{
    writer->writeStartElement(QStringLiteral("encrypted"));
    writer->writeDefaultNamespace(payloadNamespace);
    writer->writeAttribute(QStringLiteral("wire"), versionString(payload_.wireVersion));
    writer->writeAttribute(QStringLiteral("schema"), versionString(payload_.schemaVersion));
    writer->writeAttribute(QStringLiteral("kind"),
                           payload_.kind == XmppEncryptedPayload::Index ? QStringLiteral("index")
                                                                        : QStringLiteral("content"));
    writer->writeAttribute(
        QStringLiteral("key-id"),
        QString::fromLatin1(payload_.keyId.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    const auto writeBinaryElement = [writer](const QString &name, const QByteArray &value) {
        writer->writeStartElement(name);
        // Repeat the namespace declaration deliberately. QXmpp builds the
        // live stanza DOM incrementally, and explicit declarations keep these
        // framing fields namespaced on every supported parser path.
        writer->writeDefaultNamespace(QtNotePubSubItem::payloadNamespace);
        writer->writeCharacters(QString::fromLatin1(value.toBase64()));
        writer->writeEndElement();
    };
    writeBinaryElement(QStringLiteral("nonce"), payload_.nonce);
    writeBinaryElement(QStringLiteral("payload"), payload_.cipherText);
    writeBinaryElement(QStringLiteral("tag"), payload_.tag);
    writer->writeEndElement();
}

} // namespace QtNote
