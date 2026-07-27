#include "xmppbackend.h"
#include "xmppnotecodec.h"

#include <QDomDocument>
#include <QtTest>

#include <functional>
#include <type_traits>

using namespace QtNote;

static_assert(
    std::is_same_v<decltype(&XmppBackend::getNoteAsync), void (XmppBackend::*)(QString, XmppBackend::NoteCallback)>);
static_assert(std::is_same_v<decltype(&XmppBackend::saveNoteAsync),
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
    return value;
}

QByteArray masterKey()
{
    return QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

QByteArray openedPlaintext(const XmppEncryptedPayload &payload, const QByteArray &key)
{
    const auto domain
        = payload.kind == XmppEncryptedPayload::Index ? KeyDomain::StorageIndex : KeyDomain::StorageContent;
    const auto opened = SecureEnvelope::decryptAead({ payload.nonce, payload.tag, payload.cipherText }, key, domain);
    return opened ? opened.value : QByteArray {};
}

XmppEncryptedPayload encryptedPlaintext(XmppEncryptedPayload payload, const QByteArray &plainText,
                                        const QByteArray &key)
{
    const auto domain
        = payload.kind == XmppEncryptedPayload::Index ? KeyDomain::StorageIndex : KeyDomain::StorageContent;
    const auto encrypted = SecureEnvelope::encryptAead(plainText, key, domain);
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
                                     const std::function<void(QDomDocument &)> &mutator)
{
    auto document = parseXml(openedPlaintext(payload, key));
    if (document.isNull())
        return {};
    mutator(document);
    return encryptedPlaintext(payload, document.toByteArray(-1), key);
}

QDomElement directChild(const QDomElement &parent, const QString &nameSpace, const QString &name)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && element.namespaceURI() == nameSpace && element.localName() == name)
            return element;
    }
    return {};
}
}

class XmppNoteCodecTest : public QObject {
    Q_OBJECT

private slots:
    void roundTrip();
    void encryptedPayloadDoesNotExposeText();
    void plaintextUsesPortableXml();
    void interoperabilityVector();
    void preservesUnknownOptionalXmlAndNewerMinor();
    void templatesDoNotDuplicateKnownNoteData();
    void rejectsUnknownRequiredExtension();
    void rejectsUnknownNoteFormat();
    void rejectsEmptyOptionalIdentifiers();
    void reportsWrongStorageKey();
    void rejectsWrongNodeBinding();
    void rejectsWrongItemBinding();
    void rejectsOuterVersionTampering();
    void rejectsMalformedPlaintextXml();
    void acceptsXmlDeclaration();
    void rejectsProcessingInstructions();
    void rejectsDoctypeAndEntities();
    void rejectsMultipleCoreRecords();
    void protectsFutureMajor();
    void rejectsContentRevisionMismatch();
    void acceptsEmptyContent();
    void rekeysMixedIndexAndContentKeys();
};

void XmppNoteCodecTest::roundTrip()
{
    const auto source      = note();
    const auto key         = masterKey();
    const auto indexNode   = QStringLiteral("urn:xmpp:qtnote:notes:0:index:1");
    const auto contentNode = QStringLiteral("urn:xmpp:qtnote:notes:0:content:1");

    const auto encodedIndex = XmppNoteCodec::encodeIndex(source, key, indexNode);
    QVERIFY2(encodedIndex, qPrintable(encodedIndex.error.message));
    const auto decodedIndex = XmppNoteCodec::decodeIndex(encodedIndex.value, key, indexNode);
    QVERIFY2(decodedIndex, qPrintable(decodedIndex.error.message));
    QCOMPARE(decodedIndex.value.id, source.id);
    QCOMPARE(decodedIndex.value.revision, source.revision);
    QCOMPARE(decodedIndex.value.parentRevision, source.parentRevision);
    QCOMPARE(decodedIndex.value.originId, source.originId);
    QCOMPARE(decodedIndex.value.title, source.title);
    QCOMPARE(decodedIndex.value.modified, source.modified);
    QCOMPARE(decodedIndex.value.tags, source.tags);
    QVERIFY(!decodedIndex.value.contentPresent);

    const auto encodedContent = XmppNoteCodec::encodeContent(source, key, contentNode);
    QVERIFY2(encodedContent, qPrintable(encodedContent.error.message));
    const auto decodedContent
        = XmppNoteCodec::decodeContent(encodedContent.value, key, contentNode, decodedIndex.value);
    QVERIFY2(decodedContent, qPrintable(decodedContent.error.message));
    QCOMPARE(decodedContent.value.content, source.content);
    QVERIFY(decodedContent.value.contentPresent);
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

void XmppNoteCodecTest::plaintextUsesPortableXml()
{
    const auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index-node"));
    QVERIFY(encoded);
    const auto document = parseXml(openedPlaintext(encoded.value, masterKey()));
    QVERIFY(!document.isNull());
    const auto root = document.documentElement();
    QCOMPARE(root.localName(), QStringLiteral("envelope"));
    QCOMPARE(root.namespaceURI(), XmppNoteCodec::storageNamespace);
    QCOMPARE(root.attribute(QStringLiteral("wire")), QStringLiteral("1.0"));
    QCOMPARE(root.attribute(QStringLiteral("schema")), QStringLiteral("1.0"));
    QCOMPARE(directChild(root, XmppNoteCodec::storageNamespace, QStringLiteral("node")).text(),
             QStringLiteral("index-node"));
    const auto content = directChild(root, XmppNoteCodec::storageNamespace, QStringLiteral("content"));
    const auto index   = directChild(content, XmppNoteCodec::noteNamespace, QStringLiteral("index"));
    QCOMPARE(index.attribute(QStringLiteral("id")), note().id);
    QCOMPARE(directChild(index, XmppNoteCodec::noteNamespace, QStringLiteral("title")).text(), note().title);
}

void XmppNoteCodecTest::interoperabilityVector()
{
    const auto key = masterKey();
    QCOMPARE(SecureEnvelope::keyId(key),
             QByteArray::fromHex("b53bc2faae79dc9b6acc75ecb629d2776034083cd25cae34c174cae5ad0275d9"));
    QCOMPARE(SecureEnvelope::deriveKey(key, KeyDomain::StorageContent),
             QByteArray::fromHex("8933156b20b6e6fffbdb1378006d04420af34f0286057980c4007f815f4624ee"));

    XmppEncryptedPayload payload;
    payload.id            = QStringLiteral("empty-note");
    payload.kind          = XmppEncryptedPayload::Content;
    payload.wireVersion   = { 1, 0 };
    payload.schemaVersion = { 1, 0 };
    payload.keyId         = SecureEnvelope::keyId(key);
    payload.nonce         = QByteArray::fromHex("303132333435363738393a3b");
    payload.cipherText    = QByteArray::fromHex(
        "6061bf03cef0a8f981c925acb8ab510665e2a2005948afec4d493bfbe59345ce77714a704938c32ae3a943cf6704308f"
           "f6308e54db2775927547e02d11c2a3e52158371c3ebcd3dbdc832760e8e8a6ad047ce8cb109aeea8d2368d7eb0f2d0"
           "58639afc775d285664ec1f99f3f82e6270d9c930f5ac10feff57b36f8aa9890b5fa8e1d4f156ed1fd1684e232bd45e7"
           "4abdf88f995e47a72e0dc386d34d0f84a33bddf53f945155583a1eebbb8d4bad000880d575931fa78a2d275cacad6ed1"
           "f3c35647c4e138ee755a4059712a92da8e23de68465aa3737fa18f47b10ddf72feffe1e38b51fe4dc69290f10ec4d8a"
           "fb17178ca3e3daa5c8ae9cd0b692e8c9a19e22c5");
    payload.tag = QByteArray::fromHex("9ce4fbd392205e92a34267582cfcbff2");

    const auto plaintext = openedPlaintext(payload, key);
    QCOMPARE(plaintext,
             QByteArrayLiteral("<envelope xmlns=\"urn:xmpp:qtnote:storage:1\" "
                               "xmlns:note=\"urn:xmpp:qtnote:note:1\" wire=\"1.0\" schema=\"1.0\">"
                               "<node>urn:xmpp:qtnote:notes:0:content:1</node><content><note:note id=\"empty-note\" "
                               "revision=\"empty-revision\"><note:body /></note:note></content></envelope>"));

    XmppRemoteNote index;
    index.id             = payload.id;
    index.revision       = QStringLiteral("empty-revision");
    index.contentPresent = false;
    const auto decoded
        = XmppNoteCodec::decodeContent(payload, key, QStringLiteral("urn:xmpp:qtnote:notes:0:content:1"), index);
    QVERIFY2(decoded, qPrintable(decoded.error.message));
    QVERIFY(decoded.value.content.isEmpty());
}

void XmppNoteCodecTest::preservesUnknownOptionalXmlAndNewerMinor()
{
    auto source = note();
    source.indexRecordTemplate
        = QByteArrayLiteral("<envelope xmlns='urn:xmpp:qtnote:storage:1' xmlns:x='urn:example:qtnote:extension:1' "
                            "wire='1.3' schema='1.7' x:root='keep-root'>"
                            "<x:root-extension value='42'/><content x:container='keep-content'>"
                            "<index xmlns='urn:xmpp:qtnote:note:1' xmlns:x='urn:example:qtnote:extension:1' "
                            "x:record='keep-record'><x:record-extension>future</x:record-extension></index>"
                            "</content></envelope>");

    const auto encoded = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index-node"));
    QVERIFY2(encoded, qPrintable(encoded.error.message));
    QCOMPARE(encoded.value.wireVersion.major, quint16(1));
    QCOMPARE(encoded.value.wireVersion.minor, quint16(3));
    QCOMPARE(encoded.value.schemaVersion.major, quint16(1));
    QCOMPARE(encoded.value.schemaVersion.minor, quint16(7));

    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index-node"));
    QVERIFY2(decoded, qPrintable(decoded.error.message));
    auto changed         = decoded.value;
    changed.title        = QStringLiteral("Changed title");
    const auto reencoded = XmppNoteCodec::encodeIndex(changed, masterKey(), QStringLiteral("index-node"));
    QVERIFY2(reencoded, qPrintable(reencoded.error.message));

    const auto document = parseXml(openedPlaintext(reencoded.value, masterKey()));
    const auto root     = document.documentElement();
    QCOMPARE(root.attributeNS(QStringLiteral("urn:example:qtnote:extension:1"), QStringLiteral("root")),
             QStringLiteral("keep-root"));
    QVERIFY(!directChild(root, QStringLiteral("urn:example:qtnote:extension:1"), QStringLiteral("root-extension"))
                 .isNull());
    const auto content = directChild(root, XmppNoteCodec::storageNamespace, QStringLiteral("content"));
    QCOMPARE(content.attributeNS(QStringLiteral("urn:example:qtnote:extension:1"), QStringLiteral("container")),
             QStringLiteral("keep-content"));
    const auto index = directChild(content, XmppNoteCodec::noteNamespace, QStringLiteral("index"));
    QCOMPARE(index.attributeNS(QStringLiteral("urn:example:qtnote:extension:1"), QStringLiteral("record")),
             QStringLiteral("keep-record"));
    QCOMPARE(
        directChild(index, QStringLiteral("urn:example:qtnote:extension:1"), QStringLiteral("record-extension")).text(),
        QStringLiteral("future"));
    QCOMPARE(directChild(index, XmppNoteCodec::noteNamespace, QStringLiteral("title")).text(), changed.title);
}

void XmppNoteCodecTest::templatesDoNotDuplicateKnownNoteData()
{
    const auto source      = note();
    const auto indexNode   = QStringLiteral("index-node");
    const auto contentNode = QStringLiteral("content-node");
    const auto index       = XmppNoteCodec::encodeIndex(source, masterKey(), indexNode);
    const auto content     = XmppNoteCodec::encodeContent(source, masterKey(), contentNode);
    QVERIFY(index);
    QVERIFY(content);
    const auto decodedIndex = XmppNoteCodec::decodeIndex(index.value, masterKey(), indexNode);
    QVERIFY(decodedIndex);
    const auto decodedContent
        = XmppNoteCodec::decodeContent(content.value, masterKey(), contentNode, decodedIndex.value);
    QVERIFY(decodedContent);
    const auto &indexTemplate   = decodedContent.value.indexRecordTemplate;
    const auto &contentTemplate = decodedContent.value.contentRecordTemplate;
    QVERIFY(!indexTemplate.contains(source.title.toUtf8()));
    QVERIFY(!indexTemplate.contains(source.id.toUtf8()));
    QVERIFY(!indexTemplate.contains(indexNode.toUtf8()));
    QVERIFY(!contentTemplate.contains(source.content.toUtf8()));
    QVERIFY(!contentTemplate.contains(source.id.toUtf8()));
    QVERIFY(!contentTemplate.contains(contentNode.toUtf8()));
    QVERIFY(!indexTemplate.contains(SecureEnvelope::keyId(masterKey())));
    QVERIFY(!contentTemplate.contains(SecureEnvelope::keyId(masterKey())));
}

void XmppNoteCodecTest::rejectsUnknownRequiredExtension()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, [](QDomDocument &document) {
        auto root     = document.documentElement();
        auto required = document.createElementNS(XmppNoteCodec::storageNamespace, QStringLiteral("required"));
        required.setAttribute(QStringLiteral("feature"), QStringLiteral("urn:example:required:1"));
        const auto content = directChild(root, XmppNoteCodec::storageNamespace, QStringLiteral("content"));
        root.insertBefore(required, content);
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Unsupported);
}

void XmppNoteCodecTest::rejectsUnknownNoteFormat()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, [](QDomDocument &document) {
        const auto root    = document.documentElement();
        const auto content = directChild(root, XmppNoteCodec::storageNamespace, QStringLiteral("content"));
        auto       index   = directChild(content, XmppNoteCodec::noteNamespace, QStringLiteral("index"));
        index.setAttribute(QStringLiteral("format"), QStringLiteral("future-format"));
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
        QVERIFY(payload);
        payload.value      = mutatePlaintext(payload.value, key, [&attribute](QDomDocument &document) {
            const auto root    = document.documentElement();
            const auto content = directChild(root, XmppNoteCodec::storageNamespace, QStringLiteral("content"));
            auto       index   = directChild(content, XmppNoteCodec::noteNamespace, QStringLiteral("index"));
            index.setAttribute(attribute, QString {});
        });
        const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
        QVERIFY(!decoded);
        QCOMPARE(decoded.error.code, CryptoError::Corrupt);
    }
}

void XmppNoteCodecTest::reportsWrongStorageKey()
{
    const auto key     = masterKey();
    const auto payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, QByteArray(32, '\x55'), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);
    QVERIFY(decoded.error.message.contains(QStringLiteral("storage key mismatch")));
}

void XmppNoteCodecTest::rejectsWrongNodeBinding()
{
    const auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index-a"));
    QVERIFY(encoded);
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index-b"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);
}

void XmppNoteCodecTest::rejectsWrongItemBinding()
{
    auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(encoded);
    encoded.value.id   = QStringLiteral("moved-item");
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);
}

void XmppNoteCodecTest::rejectsOuterVersionTampering()
{
    auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(encoded);
    encoded.value.wireVersion.minor = 9;
    auto decoded                    = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);

    encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(encoded);
    encoded.value.schemaVersion.minor = 9;
    decoded                           = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::AuthenticationFailed);
}

void XmppNoteCodecTest::rejectsMalformedPlaintextXml()
{
    auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(encoded);
    encoded.value      = encryptedPlaintext(encoded.value, QByteArrayLiteral("not XML"), masterKey());
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::acceptsXmlDeclaration()
{
    const auto key     = masterKey();
    const auto node    = QStringLiteral("urn:xmpp:qtnote:notes:0:index:1");
    auto       encoded = XmppNoteCodec::encodeIndex(note(), key, node);
    QVERIFY(encoded);

    const auto plaintext
        = QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") + openedPlaintext(encoded.value, key);
    encoded.value = encryptedPlaintext(encoded.value, plaintext, key);

    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, key, node);
    QVERIFY2(decoded, qPrintable(decoded.error.message));
    QCOMPARE(decoded.value.id, note().id);
}

void XmppNoteCodecTest::rejectsProcessingInstructions()
{
    const auto key     = masterKey();
    const auto node    = QStringLiteral("urn:xmpp:qtnote:notes:0:index:1");
    auto       encoded = XmppNoteCodec::encodeIndex(note(), key, node);
    QVERIFY(encoded);

    const auto plaintext = QByteArrayLiteral("<?qtnote unsupported?>") + openedPlaintext(encoded.value, key);
    encoded.value        = encryptedPlaintext(encoded.value, plaintext, key);

    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, key, node);
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsDoctypeAndEntities()
{
    auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(encoded);
    const auto malicious
        = QByteArrayLiteral("<!DOCTYPE envelope [<!ENTITY x 'boom'>]>") + openedPlaintext(encoded.value, masterKey());
    encoded.value      = encryptedPlaintext(encoded.value, malicious, masterKey());
    const auto decoded = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::rejectsMultipleCoreRecords()
{
    const auto key     = masterKey();
    auto       payload = XmppNoteCodec::encodeIndex(note(), key, QStringLiteral("index"));
    QVERIFY(payload);
    payload.value      = mutatePlaintext(payload.value, key, [](QDomDocument &document) {
        const auto root      = document.documentElement();
        auto       content   = directChild(root, XmppNoteCodec::storageNamespace, QStringLiteral("content"));
        auto       duplicate = document.createElementNS(XmppNoteCodec::noteNamespace, QStringLiteral("note"));
        duplicate.setAttribute(QStringLiteral("id"), QStringLiteral("note-1"));
        duplicate.setAttribute(QStringLiteral("revision"), QStringLiteral("revision-1"));
        duplicate.appendChild(document.createElementNS(XmppNoteCodec::noteNamespace, QStringLiteral("body")));
        content.appendChild(duplicate);
    });
    const auto decoded = XmppNoteCodec::decodeIndex(payload.value, key, QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::protectsFutureMajor()
{
    auto encoded = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(encoded);
    encoded.value.wireVersion.major = 2;
    const auto decoded              = XmppNoteCodec::decodeIndex(encoded.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Unsupported);
}

void XmppNoteCodecTest::rejectsContentRevisionMismatch()
{
    const auto source  = note();
    const auto index   = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index"));
    const auto content = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    QVERIFY(index);
    QVERIFY(content);
    auto decodedIndex = XmppNoteCodec::decodeIndex(index.value, masterKey(), QStringLiteral("index"));
    QVERIFY(decodedIndex);
    decodedIndex.value.revision = QStringLiteral("other-revision");
    const auto decoded
        = XmppNoteCodec::decodeContent(content.value, masterKey(), QStringLiteral("content"), decodedIndex.value);
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppNoteCodecTest::acceptsEmptyContent()
{
    auto source = note();
    source.content.clear();
    const auto index   = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index"));
    const auto content = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    QVERIFY(index);
    QVERIFY(content);
    const auto decodedIndex = XmppNoteCodec::decodeIndex(index.value, masterKey(), QStringLiteral("index"));
    QVERIFY(decodedIndex);
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
    const auto indexNode    = QStringLiteral("index");
    const auto contentNode  = QStringLiteral("content");
    const auto oldIndex     = XmppNoteCodec::encodeIndex(source, indexKey, indexNode);
    const auto oldContent   = XmppNoteCodec::encodeContent(source, contentKey, contentNode);
    QVERIFY(oldIndex);
    QVERIFY(oldContent);
    QVERIFY(oldIndex.value.keyId != oldContent.value.keyId);

    const auto decodedIndex = XmppNoteCodec::decodeIndex(oldIndex.value, indexKey, indexNode);
    QVERIFY(decodedIndex);
    const auto decodedContent
        = XmppNoteCodec::decodeContent(oldContent.value, contentKey, contentNode, decodedIndex.value);
    QVERIFY(decodedContent);

    const auto newContent = XmppNoteCodec::encodeContent(decodedContent.value, canonicalKey, contentNode);
    const auto newIndex   = XmppNoteCodec::encodeIndex(decodedContent.value, canonicalKey, indexNode);
    QVERIFY(newContent);
    QVERIFY(newIndex);
    QCOMPARE(newContent.value.keyId, newIndex.value.keyId);
    QCOMPARE(newIndex.value.keyId, SecureEnvelope::keyId(canonicalKey));
    const auto canonicalIndex = XmppNoteCodec::decodeIndex(newIndex.value, canonicalKey, indexNode);
    QVERIFY(canonicalIndex);
    const auto canonicalContent
        = XmppNoteCodec::decodeContent(newContent.value, canonicalKey, contentNode, canonicalIndex.value);
    QVERIFY(canonicalContent);
    QCOMPARE(canonicalContent.value.content, source.content);
}

QTEST_GUILESS_MAIN(XmppNoteCodecTest)
#include "xmppnotecodec_test.moc"
