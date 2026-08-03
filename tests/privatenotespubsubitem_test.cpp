#include "privatenotespubsubitem.h"
#include "xmpppepextension.h"

#include <QDomDocument>
#include <QXmlStreamWriter>
#include <QXmppPubSubEvent.h>
#include <QtTest>

using namespace AnyKeep;

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

QString encryptedXml()
{
    return QStringLiteral("<encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                          "<nonce xmlns='urn:xmpp:private-notes:0'>IiIiIiIiIiIiIiIi</nonce>"
                          "<payload xmlns='urn:xmpp:private-notes:0'>Y2lwaGVydGV4dA==</payload>"
                          "<tag xmlns='urn:xmpp:private-notes:0'>MzMzMzMzMzMzMzMzMzMzMw==</tag>"
                          "</encrypted>")
        .arg(keyIdText());
}
}

class PrivateNotesPubSubItemTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripCurrentXml();
    void acceptsNamespacedPubSubItemAndRedundantNamespaceDeclarations();
    void acceptsLiveDomWithoutInheritedChildNamespaces();
    void parsesHeadlineEventWithPortableXml();
    void pepExtensionPublishesPortableXmlEvent();
    void classifiesLegacyPayloadAsObsolete();
    void protectsFutureMajorNamespace();
    void classifiesMalformedCurrentPayload();
    void rejectsUnknownCoreAttribute();
    void rejectsUnknownCoreElement();
    void rejectsNonCanonicalBase64();
};

void PrivateNotesPubSubItemTest::roundTripCurrentXml()
{
    XmppEncryptedPayload payload;
    payload.id         = QStringLiteral("note-1");
    payload.keyId      = QByteArray(32, '\x11');
    payload.nonce      = QByteArray(12, '\x22');
    payload.tag        = QByteArray(16, '\x33');
    payload.cipherText = QByteArrayLiteral("portable-xml-ciphertext");

    PrivateNotesPubSubItem source(payload);
    QString          xml;
    QXmlStreamWriter writer(&xml);
    source.toXml(&writer);
    QVERIFY(!xml.contains(QStringLiteral("wire=")));
    QVERIFY(!xml.contains(QStringLiteral("schema=")));
    QVERIFY(!xml.contains(QStringLiteral("kind=")));
    QVERIFY(!xml.contains(QStringLiteral("<nonce xmlns=")));
    QVERIFY(!xml.contains(QStringLiteral("<payload xmlns=")));
    QVERIFY(!xml.contains(QStringLiteral("<tag xmlns=")));

    QDomDocument document;
    const auto   element = parseItemElement(xml, &document);
    QVERIFY(!element.isNull());
    PrivateNotesPubSubItem parsed;
    parsed.parse(element);
    QVERIFY2(parsed.isValid(), qPrintable(parsed.parseError()));
    QCOMPARE(parsed.payload().id, payload.id);
    QCOMPARE(parsed.payload().keyId, payload.keyId);
    QCOMPARE(parsed.payload().nonce, payload.nonce);
    QCOMPARE(parsed.payload().tag, payload.tag);
    QCOMPARE(parsed.payload().cipherText, payload.cipherText);
}

void PrivateNotesPubSubItemTest::acceptsNamespacedPubSubItemAndRedundantNamespaceDeclarations()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item xmlns='http://jabber.org/protocol/pubsub' id='note-1'>"
                                          "<q:encrypted xmlns:q='urn:xmpp:private-notes:0' key-id='%1'>"
                                          "<q:nonce xmlns:q='urn:xmpp:private-notes:0'>IiIiIiIiIiIiIiIi</q:nonce>"
                                          "<q:payload>Y2lwaGVydGV4dA==</q:payload>"
                                          "<q:tag>MzMzMzMzMzMzMzMzMzMzMw==</q:tag></q:encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(!element.isNull());
    QVERIFY(PrivateNotesPubSubItem::isItem(element));
    PrivateNotesPubSubItem parsed;
    parsed.parse(element);
    QVERIFY2(parsed.isValid(), qPrintable(parsed.parseError()));
}

void PrivateNotesPubSubItemTest::acceptsLiveDomWithoutInheritedChildNamespaces()
{
    QDomDocument document;
    auto         item = document.createElement(QStringLiteral("item"));
    item.setAttribute(QStringLiteral("id"), QStringLiteral("note-live"));
    document.appendChild(item);

    auto encrypted = document.createElementNS(PrivateNotesPubSubItem::payloadNamespace, QStringLiteral("encrypted"));
    encrypted.setAttribute(QStringLiteral("key-id"), keyIdText());
    item.appendChild(encrypted);

    const auto appendBinary = [&document, &encrypted](const QString &name, const QString &value) {
        auto element = document.createElement(name);
        element.appendChild(document.createTextNode(value));
        encrypted.appendChild(element);
    };
    appendBinary(QStringLiteral("nonce"), QStringLiteral("IiIiIiIiIiIiIiIi"));
    appendBinary(QStringLiteral("payload"), QStringLiteral("Y2lwaGVydGV4dA=="));
    appendBinary(QStringLiteral("tag"), QStringLiteral("MzMzMzMzMzMzMzMzMzMzMw=="));

    PrivateNotesPubSubItem parsed;
    parsed.parse(item);
    QVERIFY2(parsed.isValid(), qPrintable(parsed.parseError()));
    QCOMPARE(parsed.payload().id, QStringLiteral("note-live"));
}

void PrivateNotesPubSubItemTest::parsesHeadlineEventWithPortableXml()
{
    QDomDocument document;
    const auto   message = parseItemElement(QStringLiteral("<message type='headline' from='romeo@example.net'>"
                                                             "<event xmlns='http://jabber.org/protocol/pubsub#event'>"
                                                             "<items node='urn:xmpp:private-notes:0:index'>"
                                                             "<item id='note-1'>%1</item></items></event></message>")
                                                .arg(encryptedXml()),
                                            &document);
    QVERIFY(!message.isNull());
    QVERIFY(QXmppPubSubEvent<PrivateNotesPubSubItem>::isPubSubEvent(message));

    QXmppPubSubEvent<PrivateNotesPubSubItem> event;
    event.parse(message);
    QCOMPARE(event.items().size(), 1);
    QVERIFY2(event.items().constFirst().isValid(), qPrintable(event.items().constFirst().parseError()));
}

void PrivateNotesPubSubItemTest::pepExtensionPublishesPortableXmlEvent()
{
    qRegisterMetaType<XmppEncryptedPayload>();
    const auto   nodeName = QStringLiteral("urn:xmpp:private-notes:0:index");
    QDomDocument document;
    const auto   message
        = parseItemElement(QStringLiteral("<message type='headline' from='romeo@example.net'>"
                                          "<event xmlns='http://jabber.org/protocol/pubsub#event'>"
                                          "<items node='%1'><item id='note-1'>%2</item></items></event></message>")
                               .arg(nodeName, encryptedXml()),
                           &document);

    XmppPepExtension extension;
    extension.setOwnBareJid(QStringLiteral("romeo@example.net/resource"));
    extension.setNodeName(nodeName);
    QSignalSpy published(&extension, &XmppPepExtension::payloadPublished);
    QVERIFY(extension.handlePubSubEvent(message, QStringLiteral("romeo@example.net"), nodeName));
    QCOMPARE(published.count(), 1);
}

void PrivateNotesPubSubItemTest::classifiesLegacyPayloadAsObsolete()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:private-notes:encrypted:0' wire='1.0' "
                                          "schema='1.0' kind='index' key-id='%1'>Y2Jvcg==</encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(PrivateNotesPubSubItem::isItem(element));
    PrivateNotesPubSubItem parsed;
    parsed.parse(element);
    QCOMPARE(parsed.parseFailure(), PrivateNotesPubSubItem::ParseFailure::ObsoleteFormat);
    QVERIFY(parsed.isObsoleteOrMalformed());
}

void PrivateNotesPubSubItemTest::protectsFutureMajorNamespace()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:private-notes:1' key-id='%1'>"
                                          "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA==</payload>"
                                          "<tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    QVERIFY(!PrivateNotesPubSubItem::isItem(element));
}

void PrivateNotesPubSubItemTest::classifiesMalformedCurrentPayload()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                                          "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA==</payload>"
                                          "</encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    PrivateNotesPubSubItem parsed;
    parsed.parse(element);
    QCOMPARE(parsed.parseFailure(), PrivateNotesPubSubItem::ParseFailure::Malformed);
}

void PrivateNotesPubSubItemTest::rejectsUnknownCoreAttribute()
{
    QDomDocument document;
    const auto   element = parseItemElement(
        QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1' minor='1'>"
                           "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA==</payload>"
                           "<tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted></item>")
            .arg(keyIdText()),
        &document);
    PrivateNotesPubSubItem parsed;
    parsed.parse(element);
    QVERIFY(!parsed.isValid());
}

void PrivateNotesPubSubItemTest::rejectsUnknownCoreElement()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                                          "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA==</payload>"
                                          "<future/><tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    PrivateNotesPubSubItem parsed;
    parsed.parse(element);
    QVERIFY(!parsed.isValid());
}

void PrivateNotesPubSubItemTest::rejectsNonCanonicalBase64()
{
    QDomDocument document;
    const auto   element
        = parseItemElement(QStringLiteral("<item id='note-1'><encrypted xmlns='urn:xmpp:private-notes:0' key-id='%1'>"
                                          "<nonce>IiIiIiIiIiIiIiIi</nonce><payload>Y2lwaGVydGV4dA</payload>"
                                          "<tag>MzMzMzMzMzMzMzMzMzMzMw==</tag></encrypted></item>")
                               .arg(keyIdText()),
                           &document);
    PrivateNotesPubSubItem parsed;
    parsed.parse(element);
    QVERIFY(!parsed.isValid());
}

QTEST_GUILESS_MAIN(PrivateNotesPubSubItemTest)
#include "privatenotespubsubitem_test.moc"
