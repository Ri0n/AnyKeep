#include "xmppnotecodec.h"

#include <QDateTime>
#include <QDomDocument>
#include <QDomElement>
#include <QSet>

#include <algorithm>
#include <utility>

namespace QtNote {

const QString XmppNoteCodec::protocolNamespace = QStringLiteral("urn:xmpp:qtnote:notes:1");

namespace {
    constexpr int MaxXmlDepth      = 32;
    constexpr int MaxXmlElements   = 8192;
    constexpr int MaxXmlAttributes = 256;

    struct XmlRecordDocument {
        QDomDocument document;
        QDomElement  root;
        QDomElement  content;
        QDomElement  record;
    };

    CryptoError cryptoError(CryptoError::Code code, const QString &message) { return { code, message }; }
    CryptoError corrupt(const QString &message) { return cryptoError(CryptoError::Corrupt, message); }
    CryptoError unsupported(const QString &message) { return cryptoError(CryptoError::Unsupported, message); }

    QString localName(const QDomElement &element)
    {
        const auto local = element.localName();
        return local.isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : local;
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

    bool isNamespaceDeclaration(const QDomNode &attribute)
    {
        return attribute.namespaceURI() == QStringLiteral("http://www.w3.org/2000/xmlns/")
            || attribute.nodeName() == QStringLiteral("xmlns") || attribute.prefix() == QStringLiteral("xmlns");
    }

    int protocolAttributeCount(const QDomElement &element)
    {
        int        count      = 0;
        const auto attributes = element.attributes();
        for (int i = 0; i < attributes.count(); ++i) {
            if (!isNamespaceDeclaration(attributes.item(i)))
                ++count;
        }
        return count;
    }

    CryptoError validateAttributes(const QDomElement &element, const QSet<QString> &allowedCore, const QString &context,
                                   bool allowForeign = true)
    {
        const auto attributes = element.attributes();
        for (int i = 0; i < attributes.count(); ++i) {
            const auto attribute = attributes.item(i);
            if (isNamespaceDeclaration(attribute))
                continue;
            const auto nameSpace = attribute.namespaceURI();
            if (nameSpace.isEmpty()) {
                if (!allowedCore.contains(attribute.nodeName()))
                    return corrupt(QStringLiteral("Unknown core attribute in %1").arg(context));
            } else if (nameSpace == XmppNoteCodec::protocolNamespace || !allowForeign) {
                return corrupt(QStringLiteral("Unsupported attribute namespace in %1").arg(context));
            }
        }
        return {};
    }

    CryptoError validateChildren(const QDomElement &element, const QSet<QString> &allowedCore, const QString &context)
    {
        for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const auto child = node.toElement();
            if (child.isNull())
                continue;
            const auto nameSpace = child.namespaceURI();
            if (nameSpace.isEmpty())
                return corrupt(QStringLiteral("Unnamespaced child element in %1").arg(context));
            if (nameSpace == XmppNoteCodec::protocolNamespace && !allowedCore.contains(localName(child)))
                return corrupt(QStringLiteral("Unknown core element in %1").arg(context));
        }
        return {};
    }

    CryptoError validateLeaf(const QDomElement &element, const QString &context)
    {
        if (const auto error = validateAttributes(element, {}, context, false); error)
            return error;
        return {};
    }

    bool hasNonWhitespaceDirectText(const QDomElement &element)
    {
        for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
            if ((node.isText() || node.isCDATASection()) && !node.nodeValue().trimmed().isEmpty())
                return true;
        }
        return false;
    }

    CryptoResult<QString> simpleText(const QDomElement &element, const QString &name)
    {
        QString result;
        for (auto node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
            if (node.isText() || node.isCDATASection()) {
                result += node.nodeValue();
            } else if (node.isComment()) {
                continue;
            } else {
                return { {}, corrupt(QStringLiteral("Invalid %1 text element").arg(name)) };
            }
        }
        return { result, {} };
    }

    CryptoError validateXmlTree(const QDomNode &node, int depth, int *elementCount)
    {
        if (depth > MaxXmlDepth)
            return unsupported(QStringLiteral("XML nesting exceeds the implementation limit"));
        if (node.isProcessingInstruction()) {
            const auto declaration      = node.toProcessingInstruction();
            const auto parent           = node.parentNode();
            const bool isXmlDeclaration = !parent.isNull() && parent.isDocument() && node.previousSibling().isNull()
                && declaration.target() == QStringLiteral("xml");
            if (!isXmlDeclaration)
                return corrupt(QStringLiteral("Unsupported XML processing instruction in encrypted QtNote record"));
        }
        if (node.isDocumentType() || node.isEntityReference())
            return corrupt(QStringLiteral("Unsupported XML node in encrypted QtNote record"));
        if (node.isElement()) {
            if (++*elementCount > MaxXmlElements)
                return unsupported(QStringLiteral("XML element count exceeds the implementation limit"));
            if (node.attributes().count() > MaxXmlAttributes)
                return unsupported(QStringLiteral("XML attribute count exceeds the implementation limit"));
        }
        for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (const auto error = validateXmlTree(child, depth + 1, elementCount); error)
                return error;
        }
        return {};
    }

    CryptoResult<QDomDocument> parseXml(const QByteArray &bytes, const QString &name)
    {
        if (bytes.isEmpty())
            return { {}, corrupt(QStringLiteral("Invalid %1 size").arg(name)) };
        if (bytes.size() > XmppNoteCodec::MaximumXmlSize)
            return { {}, unsupported(QStringLiteral("%1 exceeds the implementation size limit").arg(name)) };
        if (bytes.contains(QByteArrayLiteral("<!DOCTYPE")) || bytes.contains(QByteArrayLiteral("<!ENTITY")))
            return { {},
                     corrupt(QStringLiteral("Document types and entity declarations are forbidden in %1").arg(name)) };
        QDomDocument document;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (!document.setContent(bytes, QDomDocument::ParseOption::UseNamespaceProcessing))
#else
        QString errorMessage;
        int     errorLine   = 0;
        int     errorColumn = 0;
        if (!document.setContent(bytes, true, &errorMessage, &errorLine, &errorColumn))
#endif
            return { {}, corrupt(QStringLiteral("Invalid %1 XML").arg(name)) };
        // QDomDocument always owns an internal, empty QDomDocumentType object in Qt 6.
        // Only a non-empty document-type name means that the input contained <!DOCTYPE ...>.
        if (!document.doctype().name().isEmpty())
            return { {}, corrupt(QStringLiteral("XML document types are forbidden in %1").arg(name)) };
        int elementCount = 0;
        if (const auto error = validateXmlTree(document, 0, &elementCount); error)
            return { {}, { error.code, QStringLiteral("Invalid %1 XML: %2").arg(name, error.message) } };
        return { document, {} };
    }

    QByteArray serializeXml(const QDomDocument &document) { return document.toByteArray(-1); }

    KeyDomain domainFor(XmppEncryptedPayload::Kind kind)
    {
        return kind == XmppEncryptedPayload::Index ? KeyDomain::StorageIndex : KeyDomain::StorageContent;
    }

    QString recordName(XmppEncryptedPayload::Kind kind)
    {
        return kind == XmppEncryptedPayload::Index ? QStringLiteral("index") : QStringLiteral("note");
    }

    CryptoError validateRequiredExtensions(const QDomElement &root)
    {
        const auto    required = directChildren(root, XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        QSet<QString> features;
        for (const auto &element : required) {
            const auto feature = element.attribute(QStringLiteral("feature"));
            if (feature.isEmpty() || features.contains(feature) || protocolAttributeCount(element) != 1
                || !element.firstChild().isNull()) {
                return corrupt(QStringLiteral("Invalid required-extension declaration"));
            }
            features.insert(feature);
        }
        if (!features.isEmpty()) {
            auto list = features.values();
            std::sort(list.begin(), list.end());
            return unsupported(
                QStringLiteral("Unsupported required QtNote extensions: %1").arg(list.join(QStringLiteral(", "))));
        }
        return {};
    }

    CryptoResult<XmlRecordDocument> locateRecord(QDomDocument document, XmppEncryptedPayload::Kind kind,
                                                 bool requireNode)
    {
        XmlRecordDocument result;
        result.document = std::move(document);
        result.root     = result.document.documentElement();
        if (result.root.isNull() || result.root.namespaceURI() != XmppNoteCodec::protocolNamespace
            || localName(result.root) != QStringLiteral("envelope")) {
            return { {}, corrupt(QStringLiteral("Invalid encrypted QtNote XML envelope")) };
        }

        if (const auto error = validateAttributes(result.root, {}, QStringLiteral("encrypted QtNote envelope")); error)
            return { {}, error };
        if (const auto error = validateChildren(
                result.root, { QStringLiteral("node"), QStringLiteral("required"), QStringLiteral("content") },
                QStringLiteral("encrypted QtNote envelope"));
            error)
            return { {}, error };
        if (const auto error = validateRequiredExtensions(result.root); error)
            return { {}, error };
        if (hasNonWhitespaceDirectText(result.root))
            return { {}, corrupt(QStringLiteral("Unexpected text in encrypted QtNote XML envelope")) };

        const auto contents = directChildren(result.root, XmppNoteCodec::protocolNamespace, QStringLiteral("content"));
        if (contents.size() != 1)
            return { {}, corrupt(QStringLiteral("Encrypted QtNote envelope must contain one content element")) };
        result.content = contents.constFirst();
        if (const auto error = validateAttributes(result.content, {}, QStringLiteral("encrypted QtNote content"));
            error)
            return { {}, error };
        if (const auto error = validateChildren(result.content, { QStringLiteral("index"), QStringLiteral("note") },
                                                QStringLiteral("encrypted QtNote content"));
            error)
            return { {}, error };
        if (hasNonWhitespaceDirectText(result.content))
            return { {}, corrupt(QStringLiteral("Unexpected text in encrypted QtNote content container")) };

        const auto indexRecords
            = directChildren(result.content, XmppNoteCodec::protocolNamespace, QStringLiteral("index"));
        const auto noteRecords
            = directChildren(result.content, XmppNoteCodec::protocolNamespace, QStringLiteral("note"));
        const auto &records = kind == XmppEncryptedPayload::Index ? indexRecords : noteRecords;
        if (records.size() != 1 || indexRecords.size() + noteRecords.size() != 1) {
            return {
                {},
                corrupt(
                    QStringLiteral("Encrypted QtNote content must contain exactly one %1 record").arg(recordName(kind)))
            };
        }
        result.record = records.constFirst();

        if (requireNode) {
            const auto nodes = directChildren(result.root, XmppNoteCodec::protocolNamespace, QStringLiteral("node"));
            if (nodes.size() != 1)
                return { {}, corrupt(QStringLiteral("Encrypted QtNote envelope must contain one node binding")) };
            if (const auto error = validateLeaf(nodes.constFirst(), QStringLiteral("encrypted QtNote node binding"));
                error)
                return { {}, error };
            const auto text = simpleText(nodes.constFirst(), QStringLiteral("node"));
            if (!text)
                return { {}, text.error };
        }
        return { result, {} };
    }

    QDomElement createTextElement(QDomDocument &document, const QString &nameSpace, const QString &name,
                                  const QString &text)
    {
        auto element = document.createElementNS(nameSpace, name);
        element.appendChild(document.createTextNode(text));
        return element;
    }

    void removeDirectChildren(QDomElement parent, const QString &nameSpace, const QString &name)
    {
        const auto elements = directChildren(parent, nameSpace, name);
        for (const auto &element : elements)
            parent.removeChild(element);
    }

    void insertBeforeOrAppend(QDomElement parent, const QDomNode &newChild, const QDomNode &anchor)
    {
        if (anchor.isNull())
            parent.appendChild(newChild);
        else
            parent.insertBefore(newChild, anchor);
    }

    CryptoResult<XmlRecordDocument> templateDocument(const QByteArray &recordTemplate, XmppEncryptedPayload::Kind kind)
    {
        if (recordTemplate.isEmpty()) {
            QDomDocument document;
            auto         root = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("envelope"));
            document.appendChild(root);
            auto content = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("content"));
            root.appendChild(content);
            content.appendChild(document.createElementNS(XmppNoteCodec::protocolNamespace, recordName(kind)));
            return locateRecord(document, kind, false);
        }
        auto parsed = parseXml(recordTemplate, QStringLiteral("preserved QtNote record"));
        if (!parsed)
            return { {}, parsed.error };
        return locateRecord(parsed.value, kind, false);
    }

    QByteArray preservationTemplate(const XmlRecordDocument &opened, XmppEncryptedPayload::Kind kind)
    {
        QDomDocument document;
        auto         root = document.importNode(opened.root, true).toElement();
        document.appendChild(root);

        removeDirectChildren(root, XmppNoteCodec::protocolNamespace, QStringLiteral("node"));
        auto content = directChildren(root, XmppNoteCodec::protocolNamespace, QStringLiteral("content")).constFirst();
        auto record  = directChildren(content, XmppNoteCodec::protocolNamespace, recordName(kind)).constFirst();
        if (kind == XmppEncryptedPayload::Index) {
            for (const auto &name :
                 { QStringLiteral("id"), QStringLiteral("revision"), QStringLiteral("parent-revision"),
                   QStringLiteral("origin-id"), QStringLiteral("modified"), QStringLiteral("format") }) {
                record.removeAttribute(name);
            }
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("title"));
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("tag"));
        } else {
            record.removeAttribute(QStringLiteral("id"));
            record.removeAttribute(QStringLiteral("revision"));
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("body"));
        }
        return serializeXml(document);
    }

    CryptoResult<XmppEncryptedPayload> encodeRecord(const XmppRemoteNote &note, XmppEncryptedPayload::Kind kind,
                                                    const QByteArray &masterKey, const QString &nodeName)
    {
        XmppEncryptedPayload payload;
        payload.id    = note.id;
        payload.keyId = SecureEnvelope::keyId(masterKey);
        if (payload.id.isEmpty() || payload.keyId.isEmpty() || nodeName.isEmpty())
            return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Missing note ID, node or key")) };

        const auto &recordTemplate
            = kind == XmppEncryptedPayload::Index ? note.indexRecordTemplate : note.contentRecordTemplate;
        auto xml = templateDocument(recordTemplate, kind);
        if (!xml)
            return { {}, xml.error };

        auto &document = xml.value.document;
        auto  root     = xml.value.root;
        auto  content  = xml.value.content;
        auto  record   = xml.value.record;

        removeDirectChildren(root, XmppNoteCodec::protocolNamespace, QStringLiteral("node"));
        auto nodeElement
            = createTextElement(document, XmppNoteCodec::protocolNamespace, QStringLiteral("node"), nodeName);
        root.insertBefore(nodeElement, root.firstChild());

        if (kind == XmppEncryptedPayload::Index) {
            if (note.revision.isEmpty() || !note.modified.isValid() || note.format != QStringLiteral("markdown"))
                return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Invalid XMPP note index")) };
            record.setAttribute(QStringLiteral("id"), note.id);
            record.setAttribute(QStringLiteral("revision"), note.revision);
            if (note.parentRevision.isEmpty())
                record.removeAttribute(QStringLiteral("parent-revision"));
            else
                record.setAttribute(QStringLiteral("parent-revision"), note.parentRevision);
            if (note.originId.isEmpty())
                record.removeAttribute(QStringLiteral("origin-id"));
            else
                record.setAttribute(QStringLiteral("origin-id"), note.originId);
            record.setAttribute(QStringLiteral("modified"), note.modified.toUTC().toString(Qt::ISODateWithMs));
            record.setAttribute(QStringLiteral("format"), QStringLiteral("markdown"));
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("title"));
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("tag"));
            const auto anchor = record.firstChild();
            insertBeforeOrAppend(
                record,
                createTextElement(document, XmppNoteCodec::protocolNamespace, QStringLiteral("title"), note.title),
                anchor);
            for (const auto &tag : note.tags) {
                insertBeforeOrAppend(
                    record, createTextElement(document, XmppNoteCodec::protocolNamespace, QStringLiteral("tag"), tag),
                    anchor);
            }
        } else {
            if (note.revision.isEmpty())
                return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Invalid XMPP note content")) };
            record.setAttribute(QStringLiteral("id"), note.id);
            record.setAttribute(QStringLiteral("revision"), note.revision);
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("body"));
            insertBeforeOrAppend(
                record,
                createTextElement(document, XmppNoteCodec::protocolNamespace, QStringLiteral("body"), note.content),
                record.firstChild());
        }

        const auto plaintext = serializeXml(document);
        if (plaintext.isEmpty() || plaintext.size() > XmppNoteCodec::MaximumXmlSize) {
            return { {},
                     cryptoError(CryptoError::InvalidArgument,
                                 QStringLiteral("Encrypted QtNote XML record exceeds the size limit")) };
        }
        const auto encrypted = SecureEnvelope::encryptAead(plaintext, masterKey, domainFor(kind));
        if (!encrypted)
            return { {}, encrypted.error };
        payload.nonce      = encrypted.value.nonce;
        payload.tag        = encrypted.value.tag;
        payload.cipherText = encrypted.value.cipherText;
        return { payload, {} };
    }

    CryptoResult<XmlRecordDocument> openPayload(const XmppEncryptedPayload &payload,
                                                XmppEncryptedPayload::Kind expected, const QByteArray &masterKey,
                                                const QString &nodeName)
    {
        if (payload.id.isEmpty() || nodeName.isEmpty())
            return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Missing note ID or node name")) };
        const auto expectedKeyId = SecureEnvelope::keyId(masterKey);
        if (payload.keyId != expectedKeyId) {
            return { {},
                     cryptoError(CryptoError::AuthenticationFailed,
                                 QStringLiteral("Encrypted QtNote storage key mismatch (item %1, configured %2)")
                                     .arg(QString::fromLatin1(payload.keyId.left(8).toHex()),
                                          QString::fromLatin1(expectedKeyId.left(8).toHex()))) };
        }
        AeadCiphertext encrypted { payload.nonce, payload.tag, payload.cipherText };
        const auto     opened = SecureEnvelope::decryptAead(encrypted, masterKey, domainFor(expected));
        if (!opened)
            return { {}, opened.error };
        auto parsed = parseXml(opened.value, QStringLiteral("authenticated plaintext"));
        if (!parsed)
            return { {}, parsed.error };
        auto xml = locateRecord(parsed.value, expected, true);
        if (!xml)
            return { {}, xml.error };
        const auto nodes     = directChildren(xml.value.root, XmppNoteCodec::protocolNamespace, QStringLiteral("node"));
        const auto boundNode = simpleText(nodes.constFirst(), QStringLiteral("node"));
        if (!boundNode)
            return { {}, boundNode.error };
        if (boundNode.value != nodeName)
            return { {},
                     cryptoError(CryptoError::AuthenticationFailed,
                                 QStringLiteral("Encrypted QtNote node binding mismatch")) };
        return xml;
    }

    CryptoResult<QDateTime> parseModified(const QString &text)
    {
        if (!text.endsWith(QLatin1Char('Z')))
            return { {}, corrupt(QStringLiteral("Encrypted QtNote modified time must be UTC")) };
        auto value = QDateTime::fromString(text, Qt::ISODateWithMs);
        if (!value.isValid())
            value = QDateTime::fromString(text, Qt::ISODate);
        if (!value.isValid())
            return { {}, corrupt(QStringLiteral("Invalid encrypted QtNote modified time")) };
        return { value.toUTC(), {} };
    }

    CryptoError validateIndexRecord(const XmlRecordDocument &opened, const XmppEncryptedPayload &payload,
                                    XmppRemoteNote *note)
    {
        const auto &record = opened.record;
        if (const auto error
            = validateAttributes(record,
                                 { QStringLiteral("id"), QStringLiteral("revision"), QStringLiteral("parent-revision"),
                                   QStringLiteral("origin-id"), QStringLiteral("modified"), QStringLiteral("format") },
                                 QStringLiteral("encrypted QtNote index record"));
            error)
            return error;
        if (const auto error = validateChildren(record, { QStringLiteral("title"), QStringLiteral("tag") },
                                                QStringLiteral("encrypted QtNote index record"));
            error)
            return error;
        if (hasNonWhitespaceDirectText(record))
            return corrupt(QStringLiteral("Unexpected text in encrypted QtNote index record"));
        const auto id       = record.attribute(QStringLiteral("id"));
        const auto revision = record.attribute(QStringLiteral("revision"));
        const auto format   = record.attribute(QStringLiteral("format"));
        if (id.isEmpty() || revision.isEmpty() || record.attribute(QStringLiteral("modified")).isEmpty())
            return corrupt(QStringLiteral("Invalid encrypted QtNote index attributes"));
        if ((record.hasAttribute(QStringLiteral("parent-revision"))
             && record.attribute(QStringLiteral("parent-revision")).isEmpty())
            || (record.hasAttribute(QStringLiteral("origin-id"))
                && record.attribute(QStringLiteral("origin-id")).isEmpty())) {
            return corrupt(QStringLiteral("Optional encrypted QtNote index identifiers must not be empty"));
        }
        if (id != payload.id)
            return cryptoError(CryptoError::AuthenticationFailed,
                               QStringLiteral("Encrypted QtNote item binding mismatch"));
        if (format != QStringLiteral("markdown"))
            return unsupported(QStringLiteral("Unsupported encrypted QtNote note format %1").arg(format));
        const auto modified = parseModified(record.attribute(QStringLiteral("modified")));
        if (!modified)
            return modified.error;

        const auto titles = directChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("title"));
        if (titles.size() != 1)
            return corrupt(QStringLiteral("Encrypted QtNote index must contain one title"));
        if (const auto error = validateLeaf(titles.constFirst(), QStringLiteral("encrypted QtNote title")); error)
            return error;
        const auto title = simpleText(titles.constFirst(), QStringLiteral("title"));
        if (!title)
            return title.error;

        XmppRemoteNote decoded;
        decoded.id             = id;
        decoded.revision       = revision;
        decoded.parentRevision = record.attribute(QStringLiteral("parent-revision"));
        decoded.originId       = record.attribute(QStringLiteral("origin-id"));
        decoded.title          = title.value;
        decoded.modified       = modified.value;
        decoded.format         = QStringLiteral("markdown");
        for (const auto &tagElement : directChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("tag"))) {
            if (const auto error = validateLeaf(tagElement, QStringLiteral("encrypted QtNote tag")); error)
                return error;
            const auto tag = simpleText(tagElement, QStringLiteral("tag"));
            if (!tag)
                return tag.error;
            decoded.tags.append(tag.value);
        }
        decoded.contentPresent      = false;
        decoded.indexRecordTemplate = preservationTemplate(opened, XmppEncryptedPayload::Index);
        if (note)
            *note = std::move(decoded);
        return {};
    }

    CryptoError validateContentRecord(const XmlRecordDocument &opened, const XmppEncryptedPayload &payload,
                                      QString *revision, QString *content)
    {
        const auto &record = opened.record;
        if (const auto error = validateAttributes(record, { QStringLiteral("id"), QStringLiteral("revision") },
                                                  QStringLiteral("encrypted QtNote content record"));
            error)
            return error;
        if (const auto error
            = validateChildren(record, { QStringLiteral("body") }, QStringLiteral("encrypted QtNote content record"));
            error)
            return error;
        if (hasNonWhitespaceDirectText(record))
            return corrupt(QStringLiteral("Unexpected text in encrypted QtNote content record"));
        const auto id             = record.attribute(QStringLiteral("id"));
        const auto recordRevision = record.attribute(QStringLiteral("revision"));
        if (id.isEmpty() || recordRevision.isEmpty())
            return corrupt(QStringLiteral("Invalid encrypted QtNote content attributes"));
        if (id != payload.id)
            return cryptoError(CryptoError::AuthenticationFailed,
                               QStringLiteral("Encrypted QtNote item binding mismatch"));
        const auto bodies = directChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("body"));
        if (bodies.size() != 1)
            return corrupt(QStringLiteral("Encrypted QtNote content must contain one body"));
        if (const auto error = validateLeaf(bodies.constFirst(), QStringLiteral("encrypted QtNote body")); error)
            return error;
        const auto body = simpleText(bodies.constFirst(), QStringLiteral("body"));
        if (!body)
            return body.error;
        if (revision)
            *revision = recordRevision;
        if (content)
            *content = body.value;
        return {};
    }
}

CryptoResult<XmppEncryptedPayload> XmppNoteCodec::encodeIndex(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                              const QString &nodeName)
{
    return encodeRecord(note, XmppEncryptedPayload::Index, masterKey, nodeName);
}

CryptoResult<XmppEncryptedPayload> XmppNoteCodec::encodeContent(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                                const QString &nodeName)
{
    return encodeRecord(note, XmppEncryptedPayload::Content, masterKey, nodeName);
}

CryptoResult<XmppRemoteNote> XmppNoteCodec::decodeIndex(const XmppEncryptedPayload &payload,
                                                        const QByteArray &masterKey, const QString &nodeName)
{
    auto opened = openPayload(payload, XmppEncryptedPayload::Index, masterKey, nodeName);
    if (!opened)
        return { {}, opened.error };
    XmppRemoteNote note;
    if (const auto error = validateIndexRecord(opened.value, payload, &note); error)
        return { {}, error };
    return { note, {} };
}

CryptoResult<XmppRemoteNote> XmppNoteCodec::decodeContent(const XmppEncryptedPayload &payload,
                                                          const QByteArray &masterKey, const QString &nodeName,
                                                          const XmppRemoteNote &index)
{
    auto opened = openPayload(payload, XmppEncryptedPayload::Content, masterKey, nodeName);
    if (!opened)
        return { {}, opened.error };
    QString revision;
    QString content;
    if (const auto error = validateContentRecord(opened.value, payload, &revision, &content); error)
        return { {}, error };
    if (payload.id != index.id || revision != index.revision)
        return { {}, corrupt(QStringLiteral("QtNote content does not match its index revision")) };
    auto note                  = index;
    note.content               = std::move(content);
    note.contentPresent        = true;
    note.contentRecordTemplate = preservationTemplate(opened.value, XmppEncryptedPayload::Content);
    return { note, {} };
}

CryptoError XmppNoteCodec::validatePayload(const XmppEncryptedPayload &payload, XmppEncryptedPayload::Kind expectedKind,
                                           const QByteArray &masterKey, const QString &nodeName)
{
    auto opened = openPayload(payload, expectedKind, masterKey, nodeName);
    if (!opened)
        return opened.error;
    if (expectedKind == XmppEncryptedPayload::Index)
        return validateIndexRecord(opened.value, payload, nullptr);
    return validateContentRecord(opened.value, payload, nullptr, nullptr);
}

} // namespace QtNote
