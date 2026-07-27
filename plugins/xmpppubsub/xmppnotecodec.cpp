#include "xmppnotecodec.h"

#include <QDateTime>
#include <QDomDocument>
#include <QDomElement>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace QtNote {

const QString XmppNoteCodec::storageNamespace = QStringLiteral("urn:xmpp:qtnote:storage:1");
const QString XmppNoteCodec::noteNamespace    = QStringLiteral("urn:xmpp:qtnote:note:1");

namespace {
    constexpr int MaxXmlDepth      = 32;
    constexpr int MaxXmlElements   = 8192;
    constexpr int MaxXmlAttributes = 256;

    struct XmlRecordDocument {
        QDomDocument      document;
        QDomElement       root;
        QDomElement       content;
        QDomElement       record;
        XmppFormatVersion wireVersion;
        XmppFormatVersion schemaVersion;
    };

    CryptoError cryptoError(CryptoError::Code code, const QString &message) { return { code, message }; }
    CryptoError corrupt(const QString &message) { return cryptoError(CryptoError::Corrupt, message); }
    CryptoError unsupported(const QString &message) { return cryptoError(CryptoError::Unsupported, message); }

    QString localName(const QDomElement &element)
    {
        const auto local = element.localName();
        return local.isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : local;
    }

    QString versionString(const XmppFormatVersion &version)
    {
        return QStringLiteral("%1.%2").arg(version.major).arg(version.minor);
    }

    CryptoResult<XmppFormatVersion> parseVersion(const QString &text, const QString &name)
    {
        const auto parts = text.split(QLatin1Char('.'));
        if (parts.size() != 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty())
            return { {}, corrupt(QStringLiteral("Invalid %1 version").arg(name)) };
        bool       majorOk = false;
        bool       minorOk = false;
        const auto major   = parts.at(0).toUInt(&majorOk);
        const auto minor   = parts.at(1).toUInt(&minorOk);
        if (!majorOk || !minorOk || major > std::numeric_limits<quint16>::max()
            || minor > std::numeric_limits<quint16>::max()) {
            return { {}, corrupt(QStringLiteral("Invalid %1 version").arg(name)) };
        }
        return { { quint16(major), quint16(minor) }, {} };
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
        const auto    required = directChildren(root, XmppNoteCodec::storageNamespace, QStringLiteral("required"));
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
        if (result.root.isNull() || result.root.namespaceURI() != XmppNoteCodec::storageNamespace
            || localName(result.root) != QStringLiteral("envelope")) {
            return { {}, corrupt(QStringLiteral("Invalid encrypted QtNote XML envelope")) };
        }
        auto wire = parseVersion(result.root.attribute(QStringLiteral("wire")), QStringLiteral("wire"));
        if (!wire)
            return { {}, wire.error };
        auto schema = parseVersion(result.root.attribute(QStringLiteral("schema")), QStringLiteral("schema"));
        if (!schema)
            return { {}, schema.error };
        if (wire.value.major != XmppNoteCodec::WireMajor)
            return { {},
                     unsupported(
                         QStringLiteral("Unsupported encrypted QtNote wire major version %1").arg(wire.value.major)) };
        if (schema.value.major != XmppNoteCodec::SchemaMajor)
            return {
                {},
                unsupported(
                    QStringLiteral("Unsupported encrypted QtNote schema major version %1").arg(schema.value.major))
            };
        result.wireVersion   = wire.value;
        result.schemaVersion = schema.value;

        if (const auto error = validateRequiredExtensions(result.root); error)
            return { {}, error };
        if (hasNonWhitespaceDirectText(result.root))
            return { {}, corrupt(QStringLiteral("Unexpected text in encrypted QtNote XML envelope")) };

        const auto contents = directChildren(result.root, XmppNoteCodec::storageNamespace, QStringLiteral("content"));
        if (contents.size() != 1)
            return { {}, corrupt(QStringLiteral("Encrypted QtNote envelope must contain one content element")) };
        result.content = contents.constFirst();
        if (hasNonWhitespaceDirectText(result.content))
            return { {}, corrupt(QStringLiteral("Unexpected text in encrypted QtNote content container")) };

        const auto indexRecords = directChildren(result.content, XmppNoteCodec::noteNamespace, QStringLiteral("index"));
        const auto noteRecords  = directChildren(result.content, XmppNoteCodec::noteNamespace, QStringLiteral("note"));
        const auto &records     = kind == XmppEncryptedPayload::Index ? indexRecords : noteRecords;
        if (records.size() != 1 || indexRecords.size() + noteRecords.size() != 1) {
            return {
                {},
                corrupt(
                    QStringLiteral("Encrypted QtNote content must contain exactly one %1 record").arg(recordName(kind)))
            };
        }
        result.record = records.constFirst();

        if (requireNode) {
            const auto nodes = directChildren(result.root, XmppNoteCodec::storageNamespace, QStringLiteral("node"));
            if (nodes.size() != 1)
                return { {}, corrupt(QStringLiteral("Encrypted QtNote envelope must contain one node binding")) };
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

    CryptoResult<XmlRecordDocument> templateDocument(const QByteArray &recordTemplate, XmppEncryptedPayload::Kind kind)
    {
        if (recordTemplate.isEmpty()) {
            QDomDocument document;
            auto         root = document.createElementNS(XmppNoteCodec::storageNamespace, QStringLiteral("envelope"));
            root.setAttribute(QStringLiteral("wire"),
                              versionString({ XmppNoteCodec::WireMajor, XmppNoteCodec::WireMinor }));
            root.setAttribute(QStringLiteral("schema"),
                              versionString({ XmppNoteCodec::SchemaMajor, XmppNoteCodec::SchemaMinor }));
            document.appendChild(root);
            auto content = document.createElementNS(XmppNoteCodec::storageNamespace, QStringLiteral("content"));
            root.appendChild(content);
            content.appendChild(document.createElementNS(XmppNoteCodec::noteNamespace, recordName(kind)));
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

        removeDirectChildren(root, XmppNoteCodec::storageNamespace, QStringLiteral("node"));
        auto content = directChildren(root, XmppNoteCodec::storageNamespace, QStringLiteral("content")).constFirst();
        auto record  = directChildren(content, XmppNoteCodec::noteNamespace, recordName(kind)).constFirst();
        if (kind == XmppEncryptedPayload::Index) {
            for (const auto &name :
                 { QStringLiteral("id"), QStringLiteral("revision"), QStringLiteral("parent-revision"),
                   QStringLiteral("origin-id"), QStringLiteral("modified"), QStringLiteral("format") }) {
                record.removeAttribute(name);
            }
            removeDirectChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("title"));
            removeDirectChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("tag"));
        } else {
            record.removeAttribute(QStringLiteral("id"));
            record.removeAttribute(QStringLiteral("revision"));
            removeDirectChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("body"));
        }
        return serializeXml(document);
    }

    CryptoResult<XmppEncryptedPayload> encodeRecord(const XmppRemoteNote &note, XmppEncryptedPayload::Kind kind,
                                                    const QByteArray &masterKey, const QString &nodeName)
    {
        XmppEncryptedPayload payload;
        payload.id    = note.id;
        payload.kind  = kind;
        payload.keyId = SecureEnvelope::keyId(masterKey);
        if (payload.id.isEmpty() || payload.keyId.isEmpty() || nodeName.isEmpty())
            return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Missing note ID, node or key")) };

        const auto &recordTemplate
            = kind == XmppEncryptedPayload::Index ? note.indexRecordTemplate : note.contentRecordTemplate;
        auto xml = templateDocument(recordTemplate, kind);
        if (!xml)
            return { {}, xml.error };
        payload.wireVersion   = xml.value.wireVersion;
        payload.schemaVersion = xml.value.schemaVersion;

        auto &document = xml.value.document;
        auto  root     = xml.value.root;
        auto  content  = xml.value.content;
        auto  record   = xml.value.record;

        root.setAttribute(QStringLiteral("wire"), versionString(payload.wireVersion));
        root.setAttribute(QStringLiteral("schema"), versionString(payload.schemaVersion));
        removeDirectChildren(root, XmppNoteCodec::storageNamespace, QStringLiteral("node"));
        auto nodeElement
            = createTextElement(document, XmppNoteCodec::storageNamespace, QStringLiteral("node"), nodeName);
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
            removeDirectChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("title"));
            removeDirectChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("tag"));
            const auto anchor = record.firstChild();
            record.insertBefore(
                createTextElement(document, XmppNoteCodec::noteNamespace, QStringLiteral("title"), note.title), anchor);
            for (const auto &tag : note.tags) {
                record.insertBefore(
                    createTextElement(document, XmppNoteCodec::noteNamespace, QStringLiteral("tag"), tag), anchor);
            }
        } else {
            if (note.revision.isEmpty())
                return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Invalid XMPP note content")) };
            record.setAttribute(QStringLiteral("id"), note.id);
            record.setAttribute(QStringLiteral("revision"), note.revision);
            removeDirectChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("body"));
            record.insertBefore(
                createTextElement(document, XmppNoteCodec::noteNamespace, QStringLiteral("body"), note.content),
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
        if (payload.kind != expected)
            return { {}, corrupt(QStringLiteral("Encrypted QtNote payload kind mismatch")) };
        if (payload.id.isEmpty() || nodeName.isEmpty())
            return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Missing note ID or node name")) };
        if (payload.wireVersion.major != XmppNoteCodec::WireMajor)
            return {
                {},
                unsupported(
                    QStringLiteral("Unsupported encrypted QtNote wire major version %1").arg(payload.wireVersion.major))
            };
        if (payload.schemaVersion.major != XmppNoteCodec::SchemaMajor)
            return { {},
                     unsupported(QStringLiteral("Unsupported encrypted QtNote schema major version %1")
                                     .arg(payload.schemaVersion.major)) };
        const auto expectedKeyId = SecureEnvelope::keyId(masterKey);
        if (payload.keyId != expectedKeyId) {
            return { {},
                     cryptoError(CryptoError::AuthenticationFailed,
                                 QStringLiteral("Encrypted QtNote storage key mismatch (item %1, configured %2)")
                                     .arg(QString::fromLatin1(payload.keyId.left(8).toHex()),
                                          QString::fromLatin1(expectedKeyId.left(8).toHex()))) };
        }
        AeadCiphertext encrypted { payload.nonce, payload.tag, payload.cipherText };
        const auto     opened = SecureEnvelope::decryptAead(encrypted, masterKey, domainFor(payload.kind));
        if (!opened)
            return { {}, opened.error };
        auto parsed = parseXml(opened.value, QStringLiteral("authenticated plaintext"));
        if (!parsed)
            return { {}, parsed.error };
        auto xml = locateRecord(parsed.value, expected, true);
        if (!xml)
            return { {}, xml.error };
        if (!(xml.value.wireVersion == payload.wireVersion))
            return { {},
                     cryptoError(CryptoError::AuthenticationFailed,
                                 QStringLiteral("Encrypted QtNote authenticated wire version mismatch")) };
        if (!(xml.value.schemaVersion == payload.schemaVersion))
            return { {},
                     cryptoError(CryptoError::AuthenticationFailed,
                                 QStringLiteral("Encrypted QtNote schema version mismatch")) };

        const auto nodes     = directChildren(xml.value.root, XmppNoteCodec::storageNamespace, QStringLiteral("node"));
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

        const auto titles = directChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("title"));
        if (titles.size() != 1)
            return corrupt(QStringLiteral("Encrypted QtNote index must contain one title"));
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
        for (const auto &tagElement : directChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("tag"))) {
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
        if (hasNonWhitespaceDirectText(record))
            return corrupt(QStringLiteral("Unexpected text in encrypted QtNote content record"));
        const auto id             = record.attribute(QStringLiteral("id"));
        const auto recordRevision = record.attribute(QStringLiteral("revision"));
        if (id.isEmpty() || recordRevision.isEmpty())
            return corrupt(QStringLiteral("Invalid encrypted QtNote content attributes"));
        if (id != payload.id)
            return cryptoError(CryptoError::AuthenticationFailed,
                               QStringLiteral("Encrypted QtNote item binding mismatch"));
        const auto bodies = directChildren(record, XmppNoteCodec::noteNamespace, QStringLiteral("body"));
        if (bodies.size() != 1)
            return corrupt(QStringLiteral("Encrypted QtNote content must contain one body"));
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

CryptoError XmppNoteCodec::validatePayload(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                           const QString &nodeName)
{
    auto opened = openPayload(payload, payload.kind, masterKey, nodeName);
    if (!opened)
        return opened.error;
    if (payload.kind == XmppEncryptedPayload::Index)
        return validateIndexRecord(opened.value, payload, nullptr);
    return validateContentRecord(opened.value, payload, nullptr, nullptr);
}

} // namespace QtNote
