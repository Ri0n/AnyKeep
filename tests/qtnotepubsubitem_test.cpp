#include "qtnotepubsubitem.h"
#include "xmpppepextension.h"

#include <QDomDocument>
#include <QXmlStreamWriter>
#include <QXmppPubSubEvent.h>
#include <QtTest>

using namespace QtNote;

namespace {
QDomElement parseItemElement(const QString &xml, QDomDocument *document)
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

class QtNotePubSubItemTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripCurrentXml();
    void acceptsNamespacedPubSubItemAndRedundantNamespaceDeclarations();
    void acceptsLiveDomWithoutInheritedChildNamespaces();
    void parsesHeadlineEventWithPortableXml();
    void pepExtensionPublishesPortableXmlEvent();
    void classifiesPreXmlPayloadAsObsolete();
    void protectsFutureMajor();
    void classifiesMalformedCurrentPayload();
    void rejectsNonCanonicalBase64();
};

void QtNotePubSubItemTest::roundTripCurrentXml()
{
    XmppEncryptedPayload payload;
    payload.id            = QStringLiteral("note-1");
    payload.kind          = XmppEncryptedPayload::Index;
    payload.wireVersion   = { 1, 0 };
    payload.schemaVersion = { 1, 0 };
    payload.keyId         = QByteArray(32, '\x11');
    payload.nonce         = QByteArray(12, '\x22');
    payload.tag           = QByteArray(16, '\x33');
    payload.cipherText    = QByteArrayLiteral("portable-xml-ciphertext");

    QtNotePubSubItem source(payload);
    QString          xml;
    QXmlStreamWriter writer(&xml);
    source.toXml(&writer);

    QDomDocument document;
    const auto   element = parseItemElement(xml, &document);
    QVERIFY(!element.isNull());
    QtNotePubSubItem parsed;
    parsed.parse(element);
    QVERIFY2(parsed.isValid(), qPrintable(parsed.parseError()));
    QCOMPARE(parsed.payload().id, payload.id);
    QCOMPARE(int(parsed.payload().kind), int(payload.kind));
    QCOMPARE(parsed.payload().wireVersion.major, payload.wireVersion.major);
    QCOMPARE(parsed.payload().wireVersion.minor, payload.wireVersion.minor);
    QCOMPARE(parsed.payload().schemaVersion.major, payload.schemaVersion.major);
    QCOMPARE(parsed.payload().schemaVersion.minor, payload.schemaVersion.minor);
    QCOMPARE(parsed.payload().keyId, payload.keyId);
    QCOMPARE(parsed.payload().nonce, payload.nonce);
    QCOMPARE(parsed.payload().tag, payload.tag);
    QCOMPARE(parsed.payload().cipherText, payload.cipherText);
}

void QtNotePubSubItemTest::acceptsNamespacedPubSubItemAndRedundantNamespaceDeclarations()
{
    QDomDocument document;
    const auto   element = parseItemElement(
        QStringLiteral("<item xmlns='http://jabber.org/protocol/pubsub' id='note-1'>"
                           "<enc:encrypted xmlns:enc='urn:xmpp:qtnote:encrypted:1' wire='1.0' schema='1.0' "
                           "kind='index' key-id='%1'><enc:nonce xmlns:enc='urn:xmpp:qtnote:encrypted:1'>"
                           "IiIiIiIiIiIiIiIi</enc:nonce><enc:payload>Y2lwaGVydGV4dA==</enc:payload>"
                           "<enc:tag>MzMzMzMzMzMzMzMzMzMzMw==</enc:tag></enc:encrypted></item>")
            .arg(keyIdText()),
        &document);
    QVERIFY(!element.isNull());
    QVERIFY(QtNotePubSubItem::isItem(element));
    QtNotePubSubItem parsed;
    parsed.parse(element);
    QVERIFY2(parsed.isValid(), qPrintable(parsed.parseError()));
    QCOMPARE(parsed.payload().id, QStringLiteral("note-1"));
}

void QtNotePubSubItemTest::acceptsLiveDomWithoutInheritedChildNamespaces()
{
    QDomDocument document;
    auto         item = document.createElement(QStringLiteral("item"));
    item.setAttribute(QStringLiteral("id"), QStringLiteral("note-live"));
    document.appendChild(item);

    auto encrypted = document.createElementNS(QtNotePubSubItem::payloadNamespace, QStringLiteral("encrypted"));
    encrypted.setAttribute(QStringLiteral("wire"), QStringLiteral("1.0"));
    encrypted.setAttribute(QStringLiteral("schema"), QStringLiteral("1.0"));
    encrypted.setAttribute(QStringLiteral("kind"), QStringLiteral("index"));
    encrypted.setAttribute(QStringLiteral("key-id"), keyIdText());
    item.appendChild(encrypted);

    const auto appendBinary = [&document, &encrypted](const QString &name, const QString &value) {
        // Model the live QXmpp stanza DOM: the wire XML inherits the parent's
        // default namespace, but these QDomElement objects expose no namespace.
        auto element = document.createElement(name);
        element.appendChild(document.createTextNode(value));
        encrypted.appendChild(element);
    };
    appendBinary(QStringLiteral("nonce"), QStringLiteral("IiIiIiIiIiIiIiIi"));
    appendBinary(QStringLiteral("payload"), QStringLiteral("Y2lwaGVydGV4dA=="));
    appendBinary(QStringLiteral("tag"), QStringLiteral("MzMzMzMzMzMzMzMzMzMzMw=="));

    QtNotePubSubItem parsed;
    parsed.parse(item);
    QVERIFY2(parsed.isValid(), qPrintable(parsed.parseError()));
    QCOMPARE(parsed.payload().id, QStringLiteral("note-live"));
    QCOMPARE(parsed.payload().nonce, QByteArray(12, '\x22'));
    QCOMPARE(parsed.payload().tag, QByteArray(16, '\x33'));
    QCOMPARE(parsed.payload().cipherText, QByteArrayLiteral("ciphertext"));
}

void QtNotePubSubItemTest::parsesHeadlineEventWithPortableXml()
{
    QDomDocument document;
    const auto   message
        = parseItemElement(QStringLiteral("<message type='headline' from='romeo@example.net'>"
                                          "<event xmlns='http://jabber.org/protocol/pubsub#event'>"
                                          "<items node='urn:xmpp:qtnote:notes:0:index:1'>"
                                          "<item id='note-1'><encrypted xmlns='urn:xmpp:qtnote:encrypted:1' wire='1.0' "
                                          "schema='1.0' kind='index' key-id='%1'>"
                                          "<nonce xmlns='urn:xmpp:qtnote:encrypted:1'>IiIiIiIiIiIiIiIi</nonce>"
                                          "<payload xmlns='urn:xmpp:qtnote:encrypted:1'>Y2lwaGVydGV4dA==</payload>"
                                          "<tag xmlns='urn:xmpp:qtnote:encrypted:1'>MzMzMzMzMzMzMzMzMzMzMw==</tag>"
                                          "</encrypted></item></items></event></message>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(!message.isNull());
    QVERIFY(QXmppPubSubEvent<QtNotePubSubItem>::isPubSubEvent(message));

    QXmppPubSubEvent<QtNotePubSubItem> event;
    event.parse(message);
    QCOMPARE(event.eventType(), QXmppPubSubEventBase::Items);
    QCOMPARE(event.items().size(), 1);
    QVERIFY2(event.items().constFirst().isValid(), qPrintable(event.items().constFirst().parseError()));
    QCOMPARE(event.items().constFirst().payload().id, QStringLiteral("note-1"));
}

void QtNotePubSubItemTest::pepExtensionPublishesPortableXmlEvent()
{
    qRegisterMetaType<XmppEncryptedPayload>();
    const auto   nodeName = QStringLiteral("urn:xmpp:qtnote:notes:0:index:1");
    QDomDocument document;
    const auto   message
        = parseItemElement(QStringLiteral("<message type='headline' from='romeo@example.net'>"
                                          "<event xmlns='http://jabber.org/protocol/pubsub#event'>"
                                          "<items node='%1'><item id='note-1'>"
                                          "<encrypted xmlns='urn:xmpp:qtnote:encrypted:1' wire='1.0' schema='1.0' "
                                          "kind='index' key-id='%2'>"
                                          "<nonce xmlns='urn:xmpp:qtnote:encrypted:1'>IiIiIiIiIiIiIiIi</nonce>"
                                          "<payload xmlns='urn:xmpp:qtnote:encrypted:1'>Y2lwaGVydGV4dA==</payload>"
                                          "<tag xmlns='urn:xmpp:qtnote:encrypted:1'>MzMzMzMzMzMzMzMzMzMzMw==</tag>"
                                          "</encrypted></item></items></event></message>")
                               .arg(nodeName, keyIdText()),
                           &document);
    QVERIFY(!message.isNull());

    XmppPepExtension extension;
    extension.setOwnBareJid(QStringLiteral("romeo@example.net/resource"));
    extension.setNodeName(nodeName);
    QSignalSpy published(&extension, &XmppPepExtension::payloadPublished);
    QSignalSpy invalidated(&extension, &XmppPepExtension::nodeInvalidated);
    QSignalSpy malformed(&extension, &XmppPepExtension::malformedItem);

    QVERIFY(extension.handlePubSubEvent(message, QStringLiteral("romeo@example.net"), nodeName));
    QCOMPARE(published.count(), 1);
    QCOMPARE(invalidated.count(), 0);
    QCOMPARE(malformed.count(), 0);
    const auto payload = qvariant_cast<XmppEncryptedPayload>(published.constFirst().constFirst());
    QCOMPARE(payload.id, QStringLiteral("note-1"));
}

void QtNotePubSubItemTest::classifiesPreXmlPayloadAsObsolete()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:qtnote:encrypted:1' wire='1.0' "
                                          "schema='1.0' kind='index' key-id='%1'>Y2Jvcg==</encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(!element.isNull());
    QtNotePubSubItem parsed;
    parsed.parse(element);
    QVERIFY(!parsed.isValid());
    QCOMPARE(parsed.parseFailure(), QtNotePubSubItem::ParseFailure::ObsoleteFormat);
    QVERIFY(parsed.isObsoleteOrMalformed());
}

void QtNotePubSubItemTest::protectsFutureMajor()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:qtnote:encrypted:1' wire='2.0' "
                                          "schema='1.0' kind='index' key-id='%1'><nonce>IiIiIiIiIiIiIiIi</nonce>"
                                          "<payload>Y2lwaGVydGV4dA==</payload><tag>MzMzMzMzMzMzMzMzMzMzMw==</tag>"
                                          "</encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(!element.isNull());
    QtNotePubSubItem parsed;
    parsed.parse(element);
    QVERIFY(!parsed.isValid());
    QCOMPARE(parsed.parseFailure(), QtNotePubSubItem::ParseFailure::UnsupportedFormat);
    QVERIFY(!parsed.isObsoleteOrMalformed());
}

void QtNotePubSubItemTest::classifiesMalformedCurrentPayload()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:qtnote:encrypted:1' wire='1.0' "
                                          "schema='1.0' kind='index' key-id='%1'><nonce>IiIiIiIiIiIiIiIi</nonce>"
                                          "<payload>Y2lwaGVydGV4dA==</payload></encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(!element.isNull());
    QtNotePubSubItem parsed;
    parsed.parse(element);
    QVERIFY(!parsed.isValid());
    QCOMPARE(parsed.parseFailure(), QtNotePubSubItem::ParseFailure::Malformed);
    QVERIFY(parsed.isObsoleteOrMalformed());
}

void QtNotePubSubItemTest::rejectsNonCanonicalBase64()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:qtnote:encrypted:1' wire='1.0' "
                                          "schema='1.0' kind='index' key-id='%1'><nonce>IiIiIiIiIiIiIiIi</nonce>"
                                          "<payload>Y2lwaGVydGV4dA</payload><tag>MzMzMzMzMzMzMzMzMzMzMw==</tag>"
                                          "</encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(!element.isNull());
    QtNotePubSubItem parsed;
    parsed.parse(element);
    QVERIFY(!parsed.isValid());
    QCOMPARE(parsed.parseFailure(), QtNotePubSubItem::ParseFailure::Malformed);
}

QTEST_GUILESS_MAIN(QtNotePubSubItemTest)
#include "qtnotepubsubitem_test.moc"
