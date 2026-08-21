#include "xmppnotecodec.h"

#include <QDateTime>
#include <QDomDocument>
#include <QDomElement>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace AnyKeep {

const QString XmppNoteCodec::protocolNamespace        = QStringLiteral("urn:xmpp:private-notes:0");
const QString XmppNoteCodec::folderNamespace          = QStringLiteral("urn:xmpp:private-notes:folders:0");
const QString XmppNoteCodec::favoriteNamespace        = QStringLiteral("urn:xmpp:private-notes:favorite:0");
const QString XmppNoteCodec::contentRevisionNamespace = QStringLiteral("urn:xmpp:private-notes:content:0");
const QString XmppNoteCodec::mediaFeature             = QStringLiteral("urn:xmpp:private-notes:media:0");

namespace {
    const QString StatelessFileSharingNamespace = QStringLiteral("urn:xmpp:sfs:0");

    constexpr int MaxXmlDepth      = 32;
    constexpr int MaxXmlElements   = 8192;
    constexpr int MaxXmlAttributes = 256;

    struct XmlRecordDocument {
        QDomDocument  document;
        QDomElement   root;
        QDomElement   content;
        QDomElement   record;
        QSet<QString> requiredFeatures;
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
                return corrupt(
                    QStringLiteral("Unsupported XML processing instruction in encrypted private-note record"));
        }
        if (node.isDocumentType() || node.isEntityReference())
            return corrupt(QStringLiteral("Unsupported XML node in encrypted private-note record"));
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

    CryptoResult<QSet<QString>> requiredExtensions(const QDomElement &root)
    {
        const auto    required = directChildren(root, XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        QSet<QString> features;
        for (const auto &element : required) {
            const auto feature = element.attribute(QStringLiteral("feature"));
            if (feature.isEmpty() || features.contains(feature) || protocolAttributeCount(element) != 1
                || !element.firstChild().isNull()) {
                return { {}, corrupt(QStringLiteral("Invalid required-extension declaration")) };
            }
            features.insert(feature);
        }
        QSet<QString> unsupportedFeatures = features;
        unsupportedFeatures.remove(XmppNoteCodec::contentRevisionNamespace);
        unsupportedFeatures.remove(XmppNoteCodec::mediaFeature);
        if (!unsupportedFeatures.isEmpty()) {
            auto list = unsupportedFeatures.values();
            std::sort(list.begin(), list.end());
            return { {},
                     unsupported(QStringLiteral("Unsupported required private-note extensions: %1")
                                     .arg(list.join(QStringLiteral(", ")))) };
        }
        return { features, {} };
    }

    CryptoResult<XmlRecordDocument> locateRecord(QDomDocument document, XmppEncryptedPayload::Kind kind,
                                                 bool requireNode)
    {
        XmlRecordDocument result;
        result.document = std::move(document);
        result.root     = result.document.documentElement();
        if (result.root.isNull() || result.root.namespaceURI() != XmppNoteCodec::protocolNamespace
            || localName(result.root) != QStringLiteral("envelope")) {
            return { {}, corrupt(QStringLiteral("Invalid encrypted private-note XML envelope")) };
        }

        if (const auto error = validateAttributes(result.root, {}, QStringLiteral("encrypted private-note envelope"));
            error)
            return { {}, error };
        if (const auto error = validateChildren(
                result.root, { QStringLiteral("node"), QStringLiteral("required"), QStringLiteral("content") },
                QStringLiteral("encrypted private-note envelope"));
            error)
            return { {}, error };
        const auto requiredFeatures = requiredExtensions(result.root);
        if (!requiredFeatures)
            return { {}, requiredFeatures.error };
        result.requiredFeatures = requiredFeatures.value;
        if (kind != XmppEncryptedPayload::Index
            && result.requiredFeatures.contains(XmppNoteCodec::contentRevisionNamespace)) {
            return { {}, corrupt(QStringLiteral("XMPP content-revision extension is valid only for an index record")) };
        }
        if (kind != XmppEncryptedPayload::Content && result.requiredFeatures.contains(XmppNoteCodec::mediaFeature)) {
            return { {}, corrupt(QStringLiteral("XMPP media extension is valid only for a content record")) };
        }
        if (hasNonWhitespaceDirectText(result.root))
            return { {}, corrupt(QStringLiteral("Unexpected text in encrypted private-note XML envelope")) };

        const auto contents = directChildren(result.root, XmppNoteCodec::protocolNamespace, QStringLiteral("content"));
        if (contents.size() != 1)
            return { {}, corrupt(QStringLiteral("Encrypted private-note envelope must contain one content element")) };
        result.content = contents.constFirst();
        if (const auto error = validateAttributes(result.content, {}, QStringLiteral("encrypted private-note content"));
            error)
            return { {}, error };
        if (const auto error = validateChildren(result.content, { QStringLiteral("index"), QStringLiteral("note") },
                                                QStringLiteral("encrypted private-note content"));
            error)
            return { {}, error };
        if (hasNonWhitespaceDirectText(result.content))
            return { {}, corrupt(QStringLiteral("Unexpected text in encrypted private-note content container")) };

        const auto indexRecords
            = directChildren(result.content, XmppNoteCodec::protocolNamespace, QStringLiteral("index"));
        const auto noteRecords
            = directChildren(result.content, XmppNoteCodec::protocolNamespace, QStringLiteral("note"));
        const auto &records = kind == XmppEncryptedPayload::Index ? indexRecords : noteRecords;
        if (records.size() != 1 || indexRecords.size() + noteRecords.size() != 1) {
            return { {},
                     corrupt(QStringLiteral("Encrypted private-note content must contain exactly one %1 record")
                                 .arg(recordName(kind))) };
        }
        result.record = records.constFirst();

        if (requireNode) {
            const auto nodes = directChildren(result.root, XmppNoteCodec::protocolNamespace, QStringLiteral("node"));
            if (nodes.size() != 1)
                return { {}, corrupt(QStringLiteral("Encrypted private-note envelope must contain one node binding")) };
            if (const auto error
                = validateLeaf(nodes.constFirst(), QStringLiteral("encrypted private-note node binding"));
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

    CryptoResult<QStringList> folderPath(const QDomElement &record)
    {
        const auto folders = directChildren(record, XmppNoteCodec::folderNamespace, QStringLiteral("folder"));
        if (folders.size() > 1)
            return { {}, corrupt(QStringLiteral("Encrypted private-note index contains more than one folder path")) };
        if (folders.isEmpty())
            return { {}, {} };

        const auto folder = folders.constFirst();
        if (const auto error = validateAttributes(folder, {}, QStringLiteral("encrypted private-note folder"), false);
            error)
            return { {}, error };
        if (hasNonWhitespaceDirectText(folder))
            return { {}, corrupt(QStringLiteral("Unexpected text in encrypted private-note folder")) };

        QList<QDomElement> segments;
        for (auto node = folder.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const auto element = node.toElement();
            if (element.isNull())
                continue;
            if (element.namespaceURI() != XmppNoteCodec::folderNamespace
                || localName(element) != QStringLiteral("segment")) {
                return { {}, corrupt(QStringLiteral("Invalid child in encrypted private-note folder")) };
            }
            segments.append(element);
        }
        if (segments.isEmpty())
            return { {}, corrupt(QStringLiteral("Encrypted private-note folder must contain one or more segments")) };

        QStringList result;
        result.reserve(segments.size());
        for (const auto &segmentElement : segments) {
            if (const auto error
                = validateLeaf(segmentElement, QStringLiteral("encrypted private-note folder segment"));
                error)
                return { {}, error };
            const auto segment = simpleText(segmentElement, QStringLiteral("folder segment"));
            if (!segment)
                return { {}, segment.error };
            if (segment.value.isEmpty() || segment.value != segment.value.trimmed()) {
                return {
                    {}, corrupt(QStringLiteral("Encrypted private-note folder segments must be non-empty and trimmed"))
                };
            }
            result.append(segment.value);
        }
        return { result, {} };
    }

    CryptoResult<QStringList> validatedFolderPath(const QStringList &path)
    {
        QStringList result;
        result.reserve(path.size());
        for (const auto &segment : path) {
            if (segment.isEmpty() || segment != segment.trimmed()) {
                return { {},
                         cryptoError(CryptoError::InvalidArgument,
                                     QStringLiteral("XMPP folder path segments must be non-empty and trimmed")) };
            }
            result.append(segment);
        }
        return { result, {} };
    }

    CryptoResult<QString> contentRevision(const XmlRecordDocument &opened, const QString &indexRevision)
    {
        const auto values   = directChildren(opened.record, XmppNoteCodec::contentRevisionNamespace,
                                             QStringLiteral("content-revision"));
        const bool required = opened.requiredFeatures.contains(XmppNoteCodec::contentRevisionNamespace);
        if (values.isEmpty()) {
            if (required) {
                return {
                    {}, corrupt(QStringLiteral("Required XMPP content-revision extension is missing from the index"))
                };
            }
            return { indexRevision, {} };
        }
        if (values.size() != 1) {
            return { {},
                     corrupt(QStringLiteral("Encrypted private-note index contains more than one content revision")) };
        }
        if (!required) {
            return { {}, corrupt(QStringLiteral("XMPP content-revision extension must be declared as required")) };
        }
        const auto value = values.constFirst();
        if (const auto error = validateLeaf(value, QStringLiteral("encrypted private-note content revision")); error)
            return { {}, error };
        const auto revision = simpleText(value, QStringLiteral("content revision"));
        if (!revision || revision.value.isEmpty()) {
            return { {}, corrupt(QStringLiteral("Invalid encrypted private-note content revision")) };
        }
        if (revision.value == indexRevision) {
            return { {},
                     corrupt(QStringLiteral("XMPP content-revision extension must differ from the index revision")) };
        }
        return revision;
    }

    void removeRequiredFeature(QDomElement root, const QString &feature)
    {
        const auto required = directChildren(root, XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        for (const auto &element : required) {
            if (element.attribute(QStringLiteral("feature")) == feature)
                root.removeChild(element);
        }
    }

    void setRequiredFeature(QDomDocument &document, QDomElement root, const QString &feature, bool required)
    {
        removeRequiredFeature(root, feature);
        if (!required)
            return;
        auto declaration = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        declaration.setAttribute(QStringLiteral("feature"), feature);
        insertBeforeOrAppend(
            root, declaration,
            directChildren(root, XmppNoteCodec::protocolNamespace, QStringLiteral("content")).constFirst());
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
        auto parsed = parseXml(recordTemplate, QStringLiteral("preserved private-note record"));
        if (!parsed)
            return { {}, parsed.error };
        return locateRecord(parsed.value, kind, false);
    }

    CryptoResult<XmppRemoteMedia> parseMediaElement(const QDomElement &element)
    {
        if (element.localName() != QStringLiteral("file-sharing")
            || element.namespaceURI() != StatelessFileSharingNamespace) {
            return { {}, corrupt(QStringLiteral("Invalid XMPP media descriptor")) };
        }
        const auto idText = element.attribute(QStringLiteral("id"));
        const auto id     = QUuid(idText);
        if (id.isNull())
            return { {}, corrupt(QStringLiteral("XMPP media descriptor requires a UUID id")) };

        QDomDocument document;
        document.appendChild(document.importNode(element, true));
        XmppRemoteMedia media;
        media.reference.id   = id;
        media.fileSharingXml = serializeXml(document);
        return { media, {} };
    }

    CryptoResult<QList<XmppRemoteMedia>> mediaDescriptors(const XmlRecordDocument &opened)
    {
        const auto elements
            = directChildren(opened.record, StatelessFileSharingNamespace, QStringLiteral("file-sharing"));
        const bool required = opened.requiredFeatures.contains(XmppNoteCodec::mediaFeature);
        if (elements.isEmpty()) {
            if (required)
                return { {}, corrupt(QStringLiteral("Required XMPP media extension has no file-sharing descriptors")) };
            return { {}, {} };
        }
        if (!required)
            return { {}, corrupt(QStringLiteral("XMPP media descriptors must be declared as a required extension")) };

        QList<XmppRemoteMedia> result;
        QSet<QUuid>            ids;
        result.reserve(elements.size());
        for (const auto &element : elements) {
            const auto parsed = parseMediaElement(element);
            if (!parsed)
                return { {}, parsed.error };
            if (ids.contains(parsed.value.reference.id))
                return { {}, corrupt(QStringLiteral("Duplicate XMPP media attachment id")) };
            ids.insert(parsed.value.reference.id);
            result.append(parsed.value);
        }
        return { result, {} };
    }

    CryptoResult<QDomElement> importMediaElement(QDomDocument &document, const XmppRemoteMedia &media)
    {
        if (media.reference.id.isNull() || media.fileSharingXml.isEmpty()) {
            return { {},
                     cryptoError(CryptoError::InvalidArgument, QStringLiteral("Incomplete XMPP media descriptor")) };
        }
        const auto parsed = parseXml(media.fileSharingXml, QStringLiteral("XMPP media descriptor"));
        if (!parsed)
            return { {}, parsed.error };
        const auto root    = parsed.value.documentElement();
        const auto decoded = parseMediaElement(root);
        if (!decoded)
            return { {}, decoded.error };
        if (decoded.value.reference.id != media.reference.id) {
            return { {},
                     cryptoError(CryptoError::InvalidArgument,
                                 QStringLiteral("XMPP media descriptor id does not match attachment id")) };
        }
        return { document.importNode(root, true).toElement(), {} };
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
            removeDirectChildren(record, XmppNoteCodec::folderNamespace, QStringLiteral("folder"));
            removeDirectChildren(record, XmppNoteCodec::favoriteNamespace, QStringLiteral("favorite"));
            removeDirectChildren(record, XmppNoteCodec::contentRevisionNamespace, QStringLiteral("content-revision"));
            removeRequiredFeature(root, XmppNoteCodec::contentRevisionNamespace);
        } else {
            record.removeAttribute(QStringLiteral("id"));
            record.removeAttribute(QStringLiteral("revision"));
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("body"));
            removeDirectChildren(record, StatelessFileSharingNamespace, QStringLiteral("file-sharing"));
            removeRequiredFeature(root, XmppNoteCodec::mediaFeature);
        }
        return serializeXml(document);
    }

    CryptoResult<XmppEncryptedPayload> encodeRecord(const XmppRemoteNote &note, XmppEncryptedPayload::Kind kind,
                                                    const QByteArray &masterKey, const QString &nodeName)
    {
        XmppEncryptedPayload payload;
        payload.id    = note.id;
        payload.keyId = SecureEnvelope::keyId(masterKey, KeyDerivationProfile::PrivateNotes);
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
            const auto noteContentRevision = note.contentRevision.isEmpty() ? note.revision : note.contentRevision;
            if (noteContentRevision.isEmpty()) {
                return { {},
                         cryptoError(CryptoError::InvalidArgument, QStringLiteral("Invalid XMPP content revision")) };
            }
            const auto noteFolderPath = validatedFolderPath(note.folderPath);
            if (!noteFolderPath)
                return { {}, noteFolderPath.error };
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
            removeDirectChildren(record, XmppNoteCodec::folderNamespace, QStringLiteral("folder"));
            removeDirectChildren(record, XmppNoteCodec::favoriteNamespace, QStringLiteral("favorite"));
            removeDirectChildren(record, XmppNoteCodec::contentRevisionNamespace, QStringLiteral("content-revision"));
            setRequiredFeature(document, root, XmppNoteCodec::contentRevisionNamespace,
                               noteContentRevision != note.revision);
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
            if (note.favorite) {
                insertBeforeOrAppend(
                    record, document.createElementNS(XmppNoteCodec::favoriteNamespace, QStringLiteral("favorite")),
                    anchor);
            }
            if (!noteFolderPath.value.isEmpty()) {
                auto folder = document.createElementNS(XmppNoteCodec::folderNamespace, QStringLiteral("folder"));
                for (const auto &segment : noteFolderPath.value) {
                    folder.appendChild(createTextElement(document, XmppNoteCodec::folderNamespace,
                                                         QStringLiteral("segment"), segment));
                }
                insertBeforeOrAppend(record, folder, anchor);
            }
            if (noteContentRevision != note.revision) {
                insertBeforeOrAppend(record,
                                     createTextElement(document, XmppNoteCodec::contentRevisionNamespace,
                                                       QStringLiteral("content-revision"), noteContentRevision),
                                     anchor);
            }
        } else {
            const auto noteContentRevision = note.contentRevision.isEmpty() ? note.revision : note.contentRevision;
            if (noteContentRevision.isEmpty())
                return { {}, cryptoError(CryptoError::InvalidArgument, QStringLiteral("Invalid XMPP note content")) };
            record.setAttribute(QStringLiteral("id"), note.id);
            record.setAttribute(QStringLiteral("revision"), noteContentRevision);
            removeDirectChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("body"));
            removeDirectChildren(record, StatelessFileSharingNamespace, QStringLiteral("file-sharing"));
            setRequiredFeature(document, root, XmppNoteCodec::mediaFeature, !note.media.isEmpty());
            const auto anchor = record.firstChild();
            insertBeforeOrAppend(
                record,
                createTextElement(document, XmppNoteCodec::protocolNamespace, QStringLiteral("body"), note.content),
                anchor);
            QSet<QUuid> mediaIds;
            for (const auto &media : note.media) {
                if (mediaIds.contains(media.reference.id)) {
                    return { {},
                             cryptoError(CryptoError::InvalidArgument,
                                         QStringLiteral("Duplicate XMPP media attachment id")) };
                }
                const auto imported = importMediaElement(document, media);
                if (!imported)
                    return { {}, imported.error };
                mediaIds.insert(media.reference.id);
                insertBeforeOrAppend(record, imported.value, anchor);
            }
        }

        const auto plaintext = serializeXml(document);
        if (plaintext.isEmpty() || plaintext.size() > XmppNoteCodec::MaximumXmlSize) {
            return { {},
                     cryptoError(CryptoError::InvalidArgument,
                                 QStringLiteral("Encrypted private-note XML record exceeds the size limit")) };
        }
        const auto encrypted
            = SecureEnvelope::encryptAead(plaintext, masterKey, domainFor(kind), KeyDerivationProfile::PrivateNotes);
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
        const auto expectedKeyId = SecureEnvelope::keyId(masterKey, KeyDerivationProfile::PrivateNotes);
        if (payload.keyId != expectedKeyId) {
            return { {},
                     cryptoError(CryptoError::AuthenticationFailed,
                                 QStringLiteral("Encrypted private-note storage key mismatch (item %1, configured %2)")
                                     .arg(QString::fromLatin1(payload.keyId.left(8).toHex()),
                                          QString::fromLatin1(expectedKeyId.left(8).toHex()))) };
        }
        AeadCiphertext encrypted { payload.nonce, payload.tag, payload.cipherText };
        const auto     opened = SecureEnvelope::decryptAead(encrypted, masterKey, domainFor(expected),
                                                            KeyDerivationProfile::PrivateNotes);
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
                                 QStringLiteral("Encrypted private-note node binding mismatch")) };
        return xml;
    }

    CryptoResult<QDateTime> parseModified(const QString &text)
    {
        if (!text.endsWith(QLatin1Char('Z')))
            return { {}, corrupt(QStringLiteral("Encrypted private-note modified time must be UTC")) };
        auto value = QDateTime::fromString(text, Qt::ISODateWithMs);
        if (!value.isValid())
            value = QDateTime::fromString(text, Qt::ISODate);
        if (!value.isValid())
            return { {}, corrupt(QStringLiteral("Invalid encrypted private-note modified time")) };
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
                                 QStringLiteral("encrypted private-note index record"));
            error)
            return error;
        if (const auto error = validateChildren(record, { QStringLiteral("title"), QStringLiteral("tag") },
                                                QStringLiteral("encrypted private-note index record"));
            error)
            return error;
        if (hasNonWhitespaceDirectText(record))
            return corrupt(QStringLiteral("Unexpected text in encrypted private-note index record"));
        const auto id       = record.attribute(QStringLiteral("id"));
        const auto revision = record.attribute(QStringLiteral("revision"));
        const auto format   = record.attribute(QStringLiteral("format"));
        if (id.isEmpty() || revision.isEmpty() || record.attribute(QStringLiteral("modified")).isEmpty())
            return corrupt(QStringLiteral("Invalid encrypted private-note index attributes"));
        if ((record.hasAttribute(QStringLiteral("parent-revision"))
             && record.attribute(QStringLiteral("parent-revision")).isEmpty())
            || (record.hasAttribute(QStringLiteral("origin-id"))
                && record.attribute(QStringLiteral("origin-id")).isEmpty())) {
            return corrupt(QStringLiteral("Optional encrypted private-note index identifiers must not be empty"));
        }
        if (id != payload.id)
            return cryptoError(CryptoError::AuthenticationFailed,
                               QStringLiteral("Encrypted private-note item binding mismatch"));
        if (format != QStringLiteral("markdown"))
            return unsupported(QStringLiteral("Unsupported encrypted private-note note format %1").arg(format));
        const auto modified = parseModified(record.attribute(QStringLiteral("modified")));
        if (!modified)
            return modified.error;

        const auto titles = directChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("title"));
        if (titles.size() != 1)
            return corrupt(QStringLiteral("Encrypted private-note index must contain one title"));
        if (const auto error = validateLeaf(titles.constFirst(), QStringLiteral("encrypted private-note title")); error)
            return error;
        const auto title = simpleText(titles.constFirst(), QStringLiteral("title"));
        if (!title)
            return title.error;
        const auto decodedFolderPath = folderPath(record);
        if (!decodedFolderPath)
            return decodedFolderPath.error;
        const auto favoriteElements
            = directChildren(record, XmppNoteCodec::favoriteNamespace, QStringLiteral("favorite"));
        if (favoriteElements.size() > 1)
            return corrupt(QStringLiteral("Encrypted private-note index contains more than one favorite marker"));
        if (!favoriteElements.isEmpty()) {
            const auto &favorite = favoriteElements.constFirst();
            if (const auto error = validateLeaf(favorite, QStringLiteral("encrypted private-note favorite")); error)
                return error;
            if (!favorite.firstChild().isNull())
                return corrupt(QStringLiteral("Encrypted private-note favorite marker must be empty"));
        }
        const auto decodedContentRevision = contentRevision(opened, revision);
        if (!decodedContentRevision)
            return decodedContentRevision.error;

        XmppRemoteNote decoded;
        decoded.id              = id;
        decoded.revision        = revision;
        decoded.contentRevision = decodedContentRevision.value;
        decoded.parentRevision  = record.attribute(QStringLiteral("parent-revision"));
        decoded.originId        = record.attribute(QStringLiteral("origin-id"));
        decoded.title           = title.value;
        decoded.modified        = modified.value;
        decoded.format          = QStringLiteral("markdown");
        decoded.folderPath      = decodedFolderPath.value;
        decoded.favorite        = !favoriteElements.isEmpty();
        for (const auto &tagElement : directChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("tag"))) {
            if (const auto error = validateLeaf(tagElement, QStringLiteral("encrypted private-note tag")); error)
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
                                      QString *revision, QString *content, QList<XmppRemoteMedia> *media = nullptr)
    {
        const auto &record = opened.record;
        if (const auto error = validateAttributes(record, { QStringLiteral("id"), QStringLiteral("revision") },
                                                  QStringLiteral("encrypted private-note content record"));
            error)
            return error;
        if (const auto error = validateChildren(record, { QStringLiteral("body") },
                                                QStringLiteral("encrypted private-note content record"));
            error)
            return error;
        if (hasNonWhitespaceDirectText(record))
            return corrupt(QStringLiteral("Unexpected text in encrypted private-note content record"));
        const auto id             = record.attribute(QStringLiteral("id"));
        const auto recordRevision = record.attribute(QStringLiteral("revision"));
        if (id.isEmpty() || recordRevision.isEmpty())
            return corrupt(QStringLiteral("Invalid encrypted private-note content attributes"));
        if (id != payload.id)
            return cryptoError(CryptoError::AuthenticationFailed,
                               QStringLiteral("Encrypted private-note item binding mismatch"));
        const auto bodies = directChildren(record, XmppNoteCodec::protocolNamespace, QStringLiteral("body"));
        if (bodies.size() != 1)
            return corrupt(QStringLiteral("Encrypted private-note content must contain one body"));
        if (const auto error = validateLeaf(bodies.constFirst(), QStringLiteral("encrypted private-note body")); error)
            return error;
        const auto body = simpleText(bodies.constFirst(), QStringLiteral("body"));
        if (!body)
            return body.error;
        const auto decodedMedia = mediaDescriptors(opened);
        if (!decodedMedia)
            return decodedMedia.error;
        if (media)
            *media = decodedMedia.value;
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
    QString                revision;
    QString                content;
    QList<XmppRemoteMedia> media;
    if (const auto error = validateContentRecord(opened.value, payload, &revision, &content, &media); error)
        return { {}, error };
    const auto expectedContentRevision = index.contentRevision.isEmpty() ? index.revision : index.contentRevision;
    if (payload.id != index.id || revision != expectedContentRevision)
        return { {}, corrupt(QStringLiteral("private-note content does not match its index revision")) };
    auto note                  = index;
    note.content               = std::move(content);
    note.media                 = std::move(media);
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

} // namespace AnyKeep
