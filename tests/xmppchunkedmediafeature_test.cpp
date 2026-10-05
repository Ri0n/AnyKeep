#include "xmppnotecodec.h"

#include <QDomDocument>
#include <QtTest>

#include <functional>

using namespace AnyKeep;

namespace {
XmppRemoteNote note()
{
    XmppRemoteNote value;
    value.id       = QStringLiteral("note-chunks");
    value.revision = QStringLiteral("revision-1");
    value.title    = QStringLiteral("Chunked media");
    value.content  = QStringLiteral("video");
    value.modified = QDateTime::fromString(QStringLiteral("2026-10-05T10:00:00.000Z"), Qt::ISODateWithMs);
    return value;
}

XmppRemoteMedia ordinaryMedia()
{
    XmppRemoteMedia value;
    value.reference.id = QUuid(QStringLiteral("11111111-2222-3333-4444-555555555555"));
    value.fileSharingXml = QByteArrayLiteral(
        "<file-sharing xmlns='urn:xmpp:sfs:0' id='11111111-2222-3333-4444-555555555555' disposition='inline'>"
        "<file xmlns='urn:xmpp:file:metadata:0'><media-type>video/mp4</media-type><name>video.mp4</name>"
        "<size>4</size><hash xmlns='urn:xmpp:hashes:2' algo='sha-256'>AQIDBA==</hash></file>"
        "<sources><url-data xmlns='http://jabber.org/protocol/url-data' target='https://example.test/file'/></sources>"
        "</file-sharing>");
    return value;
}

XmppRemoteMedia chunkedMedia()
{
    auto value = ordinaryMedia();
    value.fileSharingXml = QByteArrayLiteral(
        "<file-sharing xmlns='urn:xmpp:sfs:0' id='11111111-2222-3333-4444-555555555555' disposition='inline'>"
        "<file xmlns='urn:xmpp:file:metadata:0'><media-type>video/mp4</media-type><name>video.mp4</name>"
        "<size>4</size><hash xmlns='urn:xmpp:hashes:2' algo='sha-256'>AQIDBA==</hash></file>"
        "<sources><encrypted-chunks xmlns='urn:xmpp:private-notes:media-chunks:0' chunk-size='1048576'/>"
        "</sources></file-sharing>");
    return value;
}

QByteArray masterKey()
{
    return QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
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

QDomElement directChild(const QDomElement &parent, const QString &name,
                        const QString &nameSpace = XmppNoteCodec::protocolNamespace)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && element.namespaceURI() == nameSpace && element.localName() == name)
            return element;
    }
    return {};
}

QByteArray openedPlaintext(const XmppEncryptedPayload &payload, XmppEncryptedPayload::Kind kind)
{
    const auto domain = kind == XmppEncryptedPayload::Index ? KeyDomain::StorageIndex : KeyDomain::StorageContent;
    const auto opened = SecureEnvelope::decryptAead({ payload.nonce, payload.tag, payload.cipherText }, masterKey(),
                                                    domain, KeyDerivationProfile::PrivateNotes);
    return opened ? opened.value : QByteArray {};
}

XmppEncryptedPayload mutatePlaintext(XmppEncryptedPayload payload, XmppEncryptedPayload::Kind kind,
                                     const std::function<void(QDomDocument &)> &mutator)
{
    auto document = parseXml(openedPlaintext(payload, kind));
    if (document.isNull())
        return {};
    mutator(document);
    const auto domain = kind == XmppEncryptedPayload::Index ? KeyDomain::StorageIndex : KeyDomain::StorageContent;
    const auto encrypted = SecureEnvelope::encryptAead(document.toByteArray(-1), masterKey(), domain,
                                                       KeyDerivationProfile::PrivateNotes);
    if (!encrypted)
        return {};
    payload.nonce      = encrypted.value.nonce;
    payload.tag        = encrypted.value.tag;
    payload.cipherText = encrypted.value.cipherText;
    return payload;
}

bool hasRequiredFeature(const QDomElement &root, const QString &feature)
{
    for (auto node = root.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && element.namespaceURI() == XmppNoteCodec::protocolNamespace
            && element.localName() == QStringLiteral("required")
            && element.attribute(QStringLiteral("feature")) == feature) {
            return true;
        }
    }
    return false;
}

void removeRequiredFeature(QDomElement root, const QString &feature)
{
    for (auto node = root.firstChild(); !node.isNull();) {
        const auto next    = node.nextSibling();
        const auto element = node.toElement();
        if (!element.isNull() && element.namespaceURI() == XmppNoteCodec::protocolNamespace
            && element.localName() == QStringLiteral("required")
            && element.attribute(QStringLiteral("feature")) == feature) {
            root.removeChild(node);
        }
        node = next;
    }
}
}

class XmppChunkedMediaFeatureTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripDeclaresChunkedFeature();
    void rejectsUndeclaredChunkedSource();
    void rejectsRequiredFeatureWithoutChunkedSource();
    void rejectsChunkedFeatureOnIndex();
};

void XmppChunkedMediaFeatureTest::roundTripDeclaresChunkedFeature()
{
    auto source  = note();
    source.media = { chunkedMedia() };

    const auto index   = XmppNoteCodec::encodeIndex(source, masterKey(), QStringLiteral("index"));
    const auto content = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    QVERIFY2(index, qPrintable(index.error.message));
    QVERIFY2(content, qPrintable(content.error.message));

    const auto document = parseXml(openedPlaintext(content.value, XmppEncryptedPayload::Content));
    QVERIFY(!document.isNull());
    const auto root = document.documentElement();
    QVERIFY(hasRequiredFeature(root, XmppNoteCodec::mediaFeature));
    QVERIFY(hasRequiredFeature(root, XmppNoteCodec::chunkedMediaFeature));

    const auto decodedIndex = XmppNoteCodec::decodeIndex(index.value, masterKey(), QStringLiteral("index"));
    QVERIFY2(decodedIndex, qPrintable(decodedIndex.error.message));
    const auto decoded
        = XmppNoteCodec::decodeContent(content.value, masterKey(), QStringLiteral("content"), decodedIndex.value);
    QVERIFY2(decoded, qPrintable(decoded.error.message));
    QCOMPARE(decoded.value.media.size(), 1);
    QVERIFY(decoded.value.media.constFirst().fileSharingXml.contains(XmppNoteCodec::chunkedMediaFeature.toUtf8()));

    const auto rewritten = XmppNoteCodec::encodeContent(decoded.value, masterKey(), QStringLiteral("content"));
    QVERIFY2(rewritten, qPrintable(rewritten.error.message));
    const auto rewrittenDocument = parseXml(openedPlaintext(rewritten.value, XmppEncryptedPayload::Content));
    QVERIFY(hasRequiredFeature(rewrittenDocument.documentElement(), XmppNoteCodec::chunkedMediaFeature));
}

void XmppChunkedMediaFeatureTest::rejectsUndeclaredChunkedSource()
{
    auto source  = note();
    source.media = { chunkedMedia() };
    auto content = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    QVERIFY(content);
    content.value = mutatePlaintext(content.value, XmppEncryptedPayload::Content, [](QDomDocument &document) {
        removeRequiredFeature(document.documentElement(), XmppNoteCodec::chunkedMediaFeature);
    });

    const auto decoded = XmppNoteCodec::decodeContent(content.value, masterKey(), QStringLiteral("content"), note());
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppChunkedMediaFeatureTest::rejectsRequiredFeatureWithoutChunkedSource()
{
    auto source  = note();
    source.media = { ordinaryMedia() };
    auto content = XmppNoteCodec::encodeContent(source, masterKey(), QStringLiteral("content"));
    QVERIFY(content);
    content.value = mutatePlaintext(content.value, XmppEncryptedPayload::Content, [](QDomDocument &document) {
        auto required = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        required.setAttribute(QStringLiteral("feature"), XmppNoteCodec::chunkedMediaFeature);
        auto root = document.documentElement();
        root.insertBefore(required, directChild(root, QStringLiteral("content")));
    });

    const auto decoded = XmppNoteCodec::decodeContent(content.value, masterKey(), QStringLiteral("content"), note());
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

void XmppChunkedMediaFeatureTest::rejectsChunkedFeatureOnIndex()
{
    auto index = XmppNoteCodec::encodeIndex(note(), masterKey(), QStringLiteral("index"));
    QVERIFY(index);
    index.value = mutatePlaintext(index.value, XmppEncryptedPayload::Index, [](QDomDocument &document) {
        auto required = document.createElementNS(XmppNoteCodec::protocolNamespace, QStringLiteral("required"));
        required.setAttribute(QStringLiteral("feature"), XmppNoteCodec::chunkedMediaFeature);
        auto root = document.documentElement();
        root.insertBefore(required, directChild(root, QStringLiteral("content")));
    });

    const auto decoded = XmppNoteCodec::decodeIndex(index.value, masterKey(), QStringLiteral("index"));
    QVERIFY(!decoded);
    QCOMPARE(decoded.error.code, CryptoError::Corrupt);
}

QTEST_GUILESS_MAIN(XmppChunkedMediaFeatureTest)
#include "xmppchunkedmediafeature_test.moc"
