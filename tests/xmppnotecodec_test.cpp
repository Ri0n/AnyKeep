#include "xmppbackend.h"
#include "xmppnotecodec.h"

#include <QDomDocument>
#include <QtTest>

#include <functional>
#include <type_traits>

using namespace AnyKeep;

static_assert(
    std::is_same_v<decltype(&XmppBackend::getNoteAsync), void (XmppBackend::*)(QString, XmppBackend::NoteCallback)>);
static_assert(std::is_same_v<decltype(&XmppBackend::saveNoteAsync),
                             void (XmppBackend::*)(XmppRemoteNote, XmppBackend::NoteCallback)>);
static_assert(std::is_same_v<decltype(&XmppBackend::updateNoteIndexAsync),
                             void (XmppBackend::*)(XmppRemoteNote, XmppBackend::NoteCallback)>);
static_assert(std::is_same_v<decltype(&XmppBackend::deleteNoteAsync),
                             void (XmppBackend::*)(QString, XmppBackend::StatusCallback)>);

namespace {
XmppRemoteNote note()
{
    XmppRemoteNote value;
    value.id             = QStringLiteral("note-1");
    value.revision       = QStringLiteral("revision-1");
    value.parentRevision = QStringLiteral("revision-0");
    value.originId       = QStringLiteral("device-a");
    value.title          = QStringLiteral("Portable note");
    value.content        = QStringLiteral("# Body\n\nHello XML");
    value.modified       = QDateTime::fromString(QStringLiteral("2026-07-27T19:00:00.123Z"), Qt::ISODateWithMs);
    value.tags           = { QStringLiteral("one"), QStringLiteral("two") };
    value.folderPath     = { QStringLiteral("Projects"), QStringLiteral("2026") };
    return value;
}

QByteArray masterKey()
{
    return QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

QByteArray openedPlaintext(const XmppEncryptedPayload &payload, const QByteArray &key, XmppEncryptedPayload::Kind kind)
{
    const auto domain = kind == XmppEncryptedPayload::Index ? KeyDomain::StorageIndex : KeyDomain::StorageContent;
    const auto opened = SecureEnvelope::decryptAead({ payload.nonce, payload.tag, payload.cipherText }, key, domain,
                                                    KeyDerivationProfile::PrivateNotes);
    return opened ? opened.value : QByteArray {};
}

XmppEncryptedPayload encryptedPlaintext(XmppEncryptedPayload payload, const QByteArray &plainText,
                                        const QByteArray &key, XmppEncryptedPayload::Kind kind)
{
    const auto domain    = kind == XmppEncryptedPayload::Index ? KeyDomain::StorageIndex : KeyDomain::StorageContent;
    const auto encrypted = SecureEnvelope::encryptAead(plainText, key, domain, KeyDerivationProfile::PrivateNotes);
    if (!encrypted)
        return {};
    payload.nonce      = encrypted.value.nonce;
    payload.tag        = encrypted.value.tag;
    payload.cipherText = encrypted.value.cipherText;
    return payload;
}

QDomDocument parseXml(const QByteArray &xml)
{
    QDomDocument document;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (!document.setContent(xml, QDomDocument::ParseOption::UseNamespaceProcessing))
#else
    if (!document.setContent(xml, true))
#endif
        return {};
    return document;
}

XmppEncryptedPayload mutatePlaintext(XmppEncryptedPayload payload, const QByteArray &key,
                                     XmppEncryptedPayload::Kind                 kind,
                                     const std::function<void(QDomDocument &)> &mutator)
{
    auto document = parseXml(openedPlaintext(payload, key, kind));
    if (document.isNull())
        return {};
    mutator(document);
    return encryptedPlaintext(payload, document.toByteArray(-1), key, kind);
}

QDomElement directChild(const QDomElement &parent, const QString &name,
                        const QString &nameSpace = XmppNoteCodec::protocolNamespace)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && element.namespaceURI() == nameSpace && element.localName() == name) {
            return element;
        }
    }
    return {};
}
}

class XmppNoteCodecTest : public QObject {
    Q_OBJECT

private slots:
    void roundTrip();
    void encryptedPayloadDoesNotExposeText();
    void plaintextUsesOneVersionedNamespace();
    void preservesUnknownOptionalXml();
    void rejectsUnknownCoreAttribute();
    void rejectsUnknownCoreElement();
    void templatesDoNotDuplicateKnownNoteData();
    void rejectsMalformedFolderPath();
    void rejectsInvalidFolderPathOnEncode();
    void metadataOnlyIndexKeepsBodyBinding();
    void rejectsUndeclaredContentRevisionExtension();
    void rejectsContentRevisionRequirementOnContent();
    void rejectsUnknownRequiredExtension();
    void rejectsUnknownNoteFormat();
    void rejectsEmptyOptionalIdentifiers();
    void reportsWrongStorageKey();
    void rejectsWrongNodeBinding();
    void rejectsWrongItemBinding();
    void rejectsMalformedPlaintextXml();
    void acceptsXmlDeclaration();
    void rejectsProcessingInstructions();
    void rejectsDoctypeAndEntities();
    void rejectsMultipleCoreRecords();
    void rejectsContentRevisionMismatch();
    void acceptsEmptyContent();
    void rekeysMixedIndexAndContentKeys();
};

void XmppNoteCodecTest::roundTrip()
{
    const auto source      = note();
    const auto key         = masterKey();
    const auto indexNode   = QStringLiteral("urn:xmpp:private-notes:0:index");
    const auto contentNode = QStringLiteral("urn:xmpp:private-notes:0:content");

    const auto encodedIndex = XmppNoteCodec::encodeIndex(source, key, indexNode);
    QVERIFY2(encodedIndex, qPrintable(encodedIndex.error.message));
    const auto decodedIndex = XmppNoteCodec::decodeIndex(encodedIndex.value, key, indexNode);
    QVERIFY2(decodedIndex, qPrintable(decodedIndex.error.message));
    QCOMPARE(decodedIndex.value.id, source.id);
    QCOMPARE(decodedIndex.value.revision, source.revision);
    QCOMPARE(decodedIndex.value.contentRevision, source.revision);
    QCOMPARE(decodedIndex.value.parentRevision, source.parentRevision);
    QCOMPARE(decodedIndex.value.originId, source.originId);
    QCOMPARE(decodedIndex.value.title, source.title);
    QCOMPARE(decodedIndex.value.modified, source.modified);
    QCOMPARE(decodedIndex.value.tags, source.tags);
    QCOMPARE(decodedIndex.value.folderPath, source.folderPath);
    QVERIFY(!decodedIndex.value.contentPresent);

    const auto encodedContent = XmppNoteCodec::encodeContent(source, key, contentNode);
    QVERIFY2(encodedContent, qPrintable(encodedContent.error.message));
    const auto decodedContent
        = XmppNoteCodec::decodeContent(encodedContent.value, key, contentNode, decodedIndex.value);
    QVERIFY2(decodedContent, qPrintable(decodedContent.error.message));
    QCOMPARE(decodedContent.value.content, source.content);
}

void XmppNoteCodecTest::encryptedPayloadDoesNotExposeText()
{
    const auto source  = note();
    const auto index   = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index"));
    const auto content = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    QVERIFY(index);
    QVERIFY(content);
    QVERIFY(!index.value.cipherText.contains(source.title.toUtf8()));
    QVERIFY(!content.value.cipherText.contains(source.content.toUtf8()));
}

void XmppNoteCodecTest::plaintextUsesOneVersionedNamespace()
{
    const auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index-node"));
    QVERIFY(encoded);
    const auto plaintext = openedPlaintext(encoded.value, masterKey(), XmppEncryptedPayload::Index);
    QVERIFY(!plaintext.contains("wire="));
    QVERIFY(!plaintext.contains("schema="));
    const auto document = parseXml(plaintext);
    const auto root     = document.documentElement();
    QCOMPARE(root.localName(), QStringLiteral("envelope"));
    QCOMPARE(root.namespaceURI(), XmppNoteCodec::protocolNamespace);
    QVERIFY(root.prefix().isEmpty());
    QCOMPARE(directChild(root, QStringLiteral("node")).text(), QStringLiteral("index-node"));
    const auto content = directChild(root, QStringLiteral("content"));
    const auto index   = directChild(content, QStringLiteral("index"));
    QCOMPARE(index.attribute(QStringLiteral("id")), note().id);
    QCOMPARE(directChild(index, QStringLiteral("title")).text(), note().title);
    const auto folder = directChild(index, QStringLiteral("folder"), XmppNoteCodec::folderNamespace);
    QVERIFY(!folder.isNull());
    QCOMPARE(directChild(folder, QStringLiteral("segment"), XmppNoteCodec::folderNamespace).text(),
             note().folderPath.constFirst());
}

void XmppNoteCodecTest::preservesUnknownOptionalXml()
{
    const auto key             = masterKey();
    auto       source          = note();
    source.indexRecordTemplate = QByteArrayLiteral(
        "<envelope xmlns='urn:xmpp:private-notes:0' xmlns:x='urn:example:private-notes:extension:1' x:root='keep'>"
        "<x:envelope value='42'/><content x:box='yes'><index x:record='keep'>"
        "<x:record>future</x:record></index></content></envelope>");
    auto encoded = XmppNoteCodec::encodeIndex(source, key, QStringLiteral("index"));
    QVERIFY2(encoded, qPrintable(encoded.error.message));
    auto decoded = XmppNoteCodec::decodeIndex(encoded.value, key, QStringLiteral("index"));
    QVERIFY2(decoded, qPrintable(decoded.error.message));
    decoded.value.title = QStringLiteral("Changed");
    encoded             = XmppNoteCodec::encodeIndex(decoded.value, key, QStringLiteral("index"));
    QVERIFY(encoded);
    const auto plaintext = openedPlaintext(encoded.value, key, XmppEncryptedPayload::Index);
    QVERIFY(plaintext.contains("urn:example:private-notes:extension:1"));
    QVERIFY(plaintext.contains("future"));
    QVERIFY(plaintext.contains("Changed"));
}

void XmppNoteCodecTest::rejectsUnknownCoreAttribute()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        auto content = directChild(document.documentElement(), QStringLiteral("content"));
        directChild(content, QStringLiteral("index")).setAttribute(QStringLiteral("minor"), QStringLiteral("1"));
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsUnknownCoreElement()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        auto content = directChild(document.documentElement(), QStringLiteral("content"));
        auto index   = directChild(content, QStringLiteral("index"));
        index.appendChild(document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("future")));
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::templatesDoNotDuplicateKnownNoteData()
{
    const auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(encoded);
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(decoded);
    QVERIFY(!decoded.value.indexRecordTemplate.contains(note().id.toUtf8()));
    QVERIFY(!decoded.value.indexRecordTemplate.contains(note().title.toUtf8()));
    QVERIFY(!decoded.value.indexRecordTemplate.contains(note().folderPath.constFirst().toUtf8()));
}

void XmppNoteCodecTest::rejectsMalformedFolderPath()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);

    payload.value = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        const auto content = directChild(document.documentElement(), QStringLiteral("content"));
        auto       index   = directChild(content, QStringLiteral("index"));
        const auto folder  = directChild(index, QStringLiteral("folder"), XmppNoteCodec::folderNamespace);
        index.appendChild(folder.cloneNode(true));
    });
    auto decoded  = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);

    payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        const auto content = directChild(document.documentElement(), QStringLiteral("content"));
        const auto index   = directChild(content, QStringLiteral("index"));
        const auto folder  = directChild(index, QStringLiteral("folder"), XmppNoteCodec::folderNamespace);
        const auto segment = directChild(folder, QStringLiteral("segment"), XmppNoteCodec::folderNamespace);
        segment.firstChild().setNodeValue(QStringLiteral(" "));
    });
    decoded       = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsInvalidFolderPathOnEncode()
{
    auto source        = note();
    source.folderPath  = { QStringLiteral(" Projects") };
    const auto encoded = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index"));
    QVERIFY(!encoded);
    QCOMPARE(encoded.error.code, CryptoError::InvalidArgument);
}

void XmppNoteCodecTest::metadataOnlyIndexKeepsBodyBinding()
{
    const auto key         = masterKey();
    const auto indexNode   = QStringLiteral("urn:xmpp:private-notes:0:index");
    const auto contentNode = QStringLiteral("urn:xmpp:private-notes:0:content");

    auto bodyRevision     = note();
    bodyRevision.revision = QStringLiteral("body-revision");
    const auto content    = XmppNoteCodec::encodeContent(bodyRevision, key, contentNode);
    QVERIFY2(content, qPrintable(content.error.message));

    auto metadataRevision            = bodyRevision;
    metadataRevision.revision        = QStringLiteral("folder-revision");
    metadataRevision.contentRevision = bodyRevision.revision;
    metadataRevision.parentRevision  = bodyRevision.revision;
    metadataRevision.folderPath      = { QStringLiteral("Archive") };
    const auto index                 = XmppNoteCodec::encodeIndex(metadataRevision, key, indexNode);
    QVERIFY2(index, qPrintable(index.error.message));

    const auto plaintext = openedPlaintext(index.value, key, XmppEncryptedPayload::Index);
    QVERIFY(plaintext.contains(XmppNoteCodec::contentRevisionNamespace.toUtf8()));
    const auto decodedIndex = XmppNoteCodec::decodeIndex(index.value, key, indexNode);
    QVERIFY2(decodedIndex, qPrintable(decodedIndex.error.message));
    QCOMPARE(decodedIndex.value.revision, metadataRevision.revision);
    QCOMPARE(decodedIndex.value.contentRevision, bodyRevision.revision);
    QCOMPARE(decodedIndex.value.folderPath, metadataRevision.folderPath);

    const auto decoded = XmppNoteCodec::decodeContent(content.value, key, contentNode, decodedIndex.value);
    QVERIFY2(decoded, qPrintable(decoded.error.message));
    QCOMPARE(decoded.value.content, bodyRevision.content);
}

void XmppNoteCodecTest::rejectsUndeclaredContentRevisionExtension()
{
    const auto key         = masterKey();
    auto       source      = note();
    source.revision        = QStringLiteral("metadata-revision");
    source.contentRevision = QStringLiteral("body-revision");
    auto payload           = XmppNoteCodec::encodeIndex(source, key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        auto root = document.documentElement();
        for (auto node = root.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const auto element = node.toElement();
            if (!element.isNull() && element.namespaceURI() == XmppNoteCodec::protocolNamespace
                && element.localName() == QStringLiteral("required")) {
                root.removeChild(element);
                break;
            }
        }
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsContentRevisionRequirementOnContent()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeContent(note(), key, QStringLiteral("content"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Content, [](QDomDocument &document) {
        auto required = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        required.setAttribute(QStringLiteral("feature"), XmppNoteCodec::contentRevisionNamespace);
        auto root = document.documentElement();
        root.insertBefore(required, directChild(root, QStringLiteral("content")));
    });
    const auto decoded = XmppNoteCodec::decodeContent(payload.value, key, QStringLiteral("content"), note());
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsUnknownRequiredExtension()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        auto required = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        required.setAttribute(QStringLiteral("feature"), QStringLiteral("urn:example:required:1"));
        document.documentElement().insertBefore(required,
                                                     directChild(document.documentElement(), QStringLiteral("content")));
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Unsupported);
}

void XmppNoteCodecTest::rejectsUnknownNoteFormat()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    payload.value      = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        auto content = directChild(document.documentElement(), QStringLiteral("content"));
        directChild(content, QStringLiteral("index")).setAttribute(QStringLiteral("format"), QStringLiteral("html"));
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Unsupported);
}

void XmppNoteCodecTest::rejectsEmptyOptionalIdentifiers()
{
    const auto key = masterKey();
    for (const auto &attribute : { QStringLiteral("parent-revision"), QStringLiteral("origin-id") }) {
        auto payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
        payload.value
            = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [&attribute](QDomDocument &document) {
                  auto content = directChild(document.documentElement(), QStringLiteral("content"));
                  directChild(content, QStringLiteral("index")).setAttribute(attribute, QString {});
              });
        const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
        QVERIFY(!decoded);
        QCOMPARE(decoded.error.code, CryptoError::Corrupt);
    }
}

void XmppNoteCodecTest::reportsWrongStorageKey()
{
    const auto payload = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, QByteArray(32, '\x55'), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);
}

void XmppNoteCodecTest::rejectsWrongNodeBinding()
{
    const auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index-a"));
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index-b"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);
}

void XmppNoteCodecTest::rejectsWrongItemBinding()
{
    auto encoded       = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    encoded.value.id   = QStringLiteral("moved-item");
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);
}

void XmppNoteCodecTest::rejectsMalformedPlaintextXml()
{
    auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    encoded.value
        = encryptedPlaintext(encoded.value, QByteArrayLiteral("not XML"), masterKey(), XmppEncryptedPayload::Index);
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::acceptsXmlDeclaration()
{
    const auto key       = masterKey();
    const auto node      = QStringLiteral("urn:xmpp:private-notes:0:index");
    auto       encoded   = XmppNoteCodec::encodeIndex(note(), key, node);
    const auto plaintext = QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")
        + openedPlaintext(encoded.value, key, XmppEncryptedPayload::Index);
    encoded.value      = encryptedPlaintext(encoded.value, plaintext, key, XmppEncryptedPayload::Index);
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, key, node);
    QVERIFY2(decoded, qPrintable(decoded.error.message));
}

void XmppNoteCodecTest::rejectsProcessingInstructions()
{
    const auto key       = masterKey();
    auto       encoded   = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    const auto plaintext = QByteArrayLiteral("<?anykeep unsupported?>")
        + openedPlaintext(encoded.value, key, XmppEncryptedPayload::Index);
    encoded.value      = encryptedPlaintext(encoded.value, plaintext, key, XmppEncryptedPayload::Index);
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsDoctypeAndEntities()
{
    auto       encoded   = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    const auto malicious = QByteArrayLiteral("<!DOCTYPE envelope [<!ENTITY x 'boom'>]>")
        + openedPlaintext(encoded.value, masterKey(), XmppEncryptedPayload::Index);
    encoded.value      = encryptedPlaintext(encoded.value, malicious, masterKey(), XmppEncryptedPayload::Index);
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsMultipleCoreRecords()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    payload.value      = mutatePlaintext(payload.value, key, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        auto content   = directChild(document.documentElement(), QStringLiteral("content"));
        auto duplicate = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("note"));
        duplicate.setAttribute(QStringLiteral("id"), QStringLiteral("note-1"));
        duplicate.setAttribute(QStringLiteral("revision"), QStringLiteral("revision-1"));
        duplicate.appendChild(document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("body")));
        content.appendChild(duplicate);
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsContentRevisionMismatch()
{
    const auto source                  = note();
    const auto index                   = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index"));
    const auto content                 = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    auto       decodedIndex            = XmppNoteCodec::decodeIndex(index.value, masterKey(), QStringLiteral("index"));
    decodedIndex.value.contentRevision = QStringLiteral("other-revision");
    const auto decoded
        = XmppNoteCodec::decodeContent(content.value, masterKey(), QStringLiteral("content"), decodedIndex.value);
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::acceptsEmptyContent()
{
    auto source = note();
    source.content.clear();
    const auto index        = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index"));
    const auto content      = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    const auto decodedIndex = XmppNoteCodec::decodeIndex(index.value, masterKey(), QStringLiteral("index"));
    const auto decoded
        = XmppNoteCodec::decodeContent(content.value, masterKey(), QStringLiteral("content"), decodedIndex.value);
    QVERIFY(decoded);
    QVERIFY(decoded.value.content.isEmpty());
}

void XmppNoteCodecTest::rekeysMixedIndexAndContentKeys()
{
    const auto indexKey     = SecureEnvelope::generateMasterKey();
    const auto contentKey   = SecureEnvelope::generateMasterKey();
    const auto canonicalKey = SecureEnvelope::generateMasterKey();
    const auto source       = note();
    const auto oldIndex     = XmppNoteCodec::encodeIndex(source, indexKey, QStringLiteral("index"));
    const auto oldContent   = XmppNoteCodec::encodeContent(source, contentKey, QStringLiteral("content"));
    const auto decodedIndex = XmppNoteCodec::decodeIndex(oldIndex.value, indexKey, QStringLiteral("index"));
    const auto decodedContent
        = XmppNoteCodec::decodeContent(oldContent.value, contentKey, QStringLiteral("content"), decodedIndex.value);
    const auto newContent = XmppNoteCodec::encodeContent(decodedContent.value, canonicalKey, QStringLiteral("content"));
    const auto newIndex   = XmppNoteCodec::encodeIndex(decodedContent.value, canonicalKey, QStringLiteral("index"));
    QCOMPARE(newContent.value.keyId, newIndex.value.keyId);
    const auto canonicalIndex = XmppNoteCodec::decodeIndex(newIndex.value, canonicalKey, QStringLiteral("index"));
    const auto canonicalContent
        = XmppNoteCodec::decodeContent(newContent.value, canonicalKey, QStringLiteral("content"), canonicalIndex.value);
    QVERIFY(canonicalContent);
    QCOMPARE(canonicalContent.value.content, source.content);
}

QTEST_GUILESS_MAIN(XmppNoteCodecTest)
#include "xmppnotecodec_test.moc"
