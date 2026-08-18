#include "xmpppayloadxml.h"

#include <QDomDocument>
#include <QtTest>

using namespace AnyKeep;

namespace {
QDomElement parseElement(const QString &xml, QDomDocument *document)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (!document->setContent(xml, QDomDocument::ParseOption::UseNamespaceProcessing))
#else
    QString errorMessage;
    int     errorLine   = 0;
    int     errorColumn = 0;
    if (!document->setContent(xml, true, &errorMessage, &errorLine, &errorColumn))
#endif
        return {};
    return document->documentElement();
}

QString keyIdText()
{
    return QString::fromLatin1(
        QByteArray(32, '\x11').toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
}

class XmppPayloadXmlTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripCurrentXml();
    void acceptsInheritedChildNamespaces();
    void classifiesLegacyPayloadAsObsolete();
    void protectsFutureMajorNamespace();
    void classifiesMalformedCurrentPayload();
    void rejectsUnknownCoreField();
    void rejectsNonCanonicalBase64();
};

void XmppPayloadXmlTest::roundTripCurrentXml()
{
    XmppEncryptedPayload payload;
    payload.id         = QStringLiteral("note-1");
    payload.keyId      = QByteArray(32, '\x11');
    payload.nonce      = QByteArray(12, '\x22');
    payload.tag        = QByteArray(16, '\x33');
    payload.cipherText = QByteArrayLiteral("portable-xml-ciphertext");

    QDomDocument document;
    const auto   element = XmppPayloadXml::serialize(document, payload);
    QVERIFY(!element.isNull());
    QCOMPARE(element.namespaceURI(), XmppPayloadXml::payloadNamespace);
    QVERIFY(!element.hasAttribute(QStringLiteral("wire")));
    QVERIFY(!element.hasAttribute(QStringLiteral("schema")));
    QVERIFY(!element.hasAttribute(QStringLiteral("kind")));

    const auto parsed = XmppPayloadXml::parse(payload.id, element);
    QVERIFY2(parsed.valid, qPrintable(parsed.error));
    QCOMPARE(parsed.payload.id, payload.id);
    QCOMPARE(parsed.payload.keyId, payload.keyId);
    QCOMPARE(parsed.payload.nonce, payload.nonce);
    QCOMPARE(parsed.payload.tag, payload.tag);
    QCOMPARE(parsed.payload.cipherText, payload.cipherText);
}

void XmppPayloadXmlTest::acceptsInheritedChildNamespaces()
{
    QDomDocument document;
    const auto   element = parseElement(QStringLiteral("<encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                                                       "<nonce>IiIiIiIiIiIiIiIi</nonce>"
                                                       "<payload>Y2lwaGVydGV4dA==</payload>"
                                                       "<tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted>")
                                            .arg(keyIdText()),
                                        &document);
    const auto   parsed  = XmppPayloadXml::parse(QStringLiteral("note-1"), element);
    QVERIFY2(parsed.valid, qPrintable(parsed.error));
}

void XmppPayloadXmlTest::classifiesLegacyPayloadAsObsolete()
{
    QDomDocument document;
    const auto   element = parseElement(
        QStringLiteral("<encrypted xmlns='urn:xmpp:private-notes:encrypted:0' wire='1.0' schema='1.0' kind='index' "
                       "key-id='%1'>Y2Jvcg==</encrypted>")
            .arg(keyIdText()),
        &document);
    const auto parsed = XmppPayloadXml::parse(QStringLiteral("note-1"), element);
    QCOMPARE(parsed.failure, XmppPayloadParseFailure::ObsoleteFormat);
    QVERIFY(parsed.isObsoleteOrMalformed());
}

void XmppPayloadXmlTest::protectsFutureMajorNamespace()
{
    QDomDocument document;
    const auto   element
        = parseElement(QStringLiteral("<encrypted xmlns='urn:xmpp:private-notes:1' key-id='%1'>"
                                      "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA==</payload>"
                                      "<tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted>")
                           .arg(keyIdText()),
                       &document);
    QVERIFY(!XmppPayloadXml::isEncryptedPayload(element));
    const auto parsed = XmppPayloadXml::parse(QStringLiteral("note-1"), element);
    QVERIFY(!parsed.valid);
    QCOMPARE(parsed.failure, XmppPayloadParseFailure::UnsupportedFormat);
    QVERIFY(!parsed.isObsoleteOrMalformed());
}

void XmppPayloadXmlTest::classifiesMalformedCurrentPayload()
{
    QDomDocument document;
    const auto   element
        = parseElement(QStringLiteral("<encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                                      "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA==</payload></encrypted>")
                           .arg(keyIdText()),
                       &document);
    const auto parsed = XmppPayloadXml::parse(QStringLiteral("note-1"), element);
    QCOMPARE(parsed.failure, XmppPayloadParseFailure::Malformed);
}

void XmppPayloadXmlTest::rejectsUnknownCoreField()
{
    QDomDocument document;
    const auto   element
        = parseElement(QStringLiteral("<encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                                      "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA==</payload>"
                                      "<future/><tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted>")
                           .arg(keyIdText()),
                       &document);
    const auto parsed = XmppPayloadXml::parse(QStringLiteral("note-1"), element);
    QVERIFY(!parsed.valid);
    QCOMPARE(parsed.failure, XmppPayloadParseFailure::Malformed);
}

void XmppPayloadXmlTest::rejectsNonCanonicalBase64()
{
    QDomDocument document;
    const auto element = parseElement(QStringLiteral("<encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                                                     "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA</payload>"
                                                     "<tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted>")
                                          .arg(keyIdText()),
                                      &document);
    const auto parsed  = XmppPayloadXml::parse(QStringLiteral("note-1"), element);
    QVERIFY(!parsed.valid);
}

QTEST_GUILESS_MAIN(XmppPayloadXmlTest)
#include "xmpppayloadxml_test.moc"
