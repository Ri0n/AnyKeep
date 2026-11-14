#include "xmppkeysyncextension.h"

#include <QDomDocument>
#include <QXmlStreamWriter>
#include <QtTest>

using namespace AnyKeep;

class XmppKeySyncExtensionTest : public QObject {
    Q_OBJECT

private slots:
    void advertisesProtocol();
    void serializesResourceAddressedRequest();
    void serializesTrustBootstrapWithoutRecoveryKey();
    void parsesMatchingResponseOnly();
    void parsesTrustBootstrapAcknowledgement();
    void parsesExplicitTrustRefusal();
    void duplicateRequestKeepsOriginalApproval();
};

void XmppKeySyncExtensionTest::advertisesProtocol()
{
    XmppKeySyncExtension extension;
    QCOMPARE(extension.discoveryFeatures(), QStringList { XmppKeySyncExtension::feature });
}

void XmppKeySyncExtensionTest::serializesTrustBootstrapWithoutRecoveryKey()
{
    XmppKeySyncExtension extension;
    const auto           iq = extension.makeTrustRequest(QStringLiteral("me@example.org/AnyKeep-laptop"),
                                                         QStringLiteral("trust-1"), QByteArrayLiteral("public-key"));
    QString              xml;
    QXmlStreamWriter     writer(&xml);
    iq.toXml(&writer);
    QVERIFY(xml.contains(QStringLiteral("trust-request")));
    QVERIFY(xml.contains(QString::fromLatin1(QByteArrayLiteral("public-key").toBase64())));
    QVERIFY(!xml.contains(QStringLiteral("recoveryKey")));
}

void XmppKeySyncExtensionTest::serializesResourceAddressedRequest()
{
    XmppKeySyncExtension extension;
    const auto iq = extension.makeRequest(QStringLiteral("me@example.org/AnyKeep-laptop"), QStringLiteral("request-1"));
    QString    xml;
    QXmlStreamWriter writer(&xml);
    iq.toXml(&writer);
    QVERIFY(xml.contains(QStringLiteral("to=\"me@example.org/AnyKeep-laptop\"")));
    QVERIFY(xml.contains(QStringLiteral("type=\"set\"")));
    QVERIFY(xml.contains(QStringLiteral("urn:xmpp:private-notes:key-sync:0")));
    QVERIFY(xml.contains(QStringLiteral("request-1")));
}

void XmppKeySyncExtensionTest::parsesTrustBootstrapAcknowledgement()
{
    QDomDocument document;
    QVERIFY(document.setContent(
        QStringLiteral("<iq type='result' id='trust-1'><key-sync xmlns='urn:xmpp:private-notes:key-sync:0'>"
                       "{&quot;type&quot;:&quot;trust-approved&quot;,&quot;requestId&quot;:&quot;trust-1&quot;}"
                       "</key-sync></iq>")));
    QVERIFY(XmppKeySyncExtension::isTrustApproved(document.documentElement(), QStringLiteral("trust-1")));
    QVERIFY(!XmppKeySyncExtension::isTrustApproved(document.documentElement(), QStringLiteral("other")));
}

void XmppKeySyncExtensionTest::parsesExplicitTrustRefusal()
{
    QDomDocument document;
    QVERIFY(document.setContent(
        QStringLiteral("<iq type='result' id='trust-1'><key-sync xmlns='urn:xmpp:private-notes:key-sync:0'>"
                       "{&quot;type&quot;:&quot;trust-rejected&quot;,&quot;requestId&quot;:&quot;trust-1&quot;}"
                       "</key-sync></iq>")));
    QVERIFY(XmppKeySyncExtension::isTrustRejected(document.documentElement(), QStringLiteral("trust-1")));
    QVERIFY(!XmppKeySyncExtension::isTrustApproved(document.documentElement(), QStringLiteral("trust-1")));
    QVERIFY(!XmppKeySyncExtension::isTrustRejected(document.documentElement(), QStringLiteral("other")));
    document.documentElement().setAttribute(QStringLiteral("type"), QStringLiteral("set"));
    QVERIFY(!XmppKeySyncExtension::isTrustRejected(document.documentElement(), QStringLiteral("trust-1")));
}

void XmppKeySyncExtensionTest::parsesMatchingResponseOnly()
{
    QDomDocument document;
    QVERIFY(document.setContent(
        QStringLiteral("<iq type='result' id='request-1'><key-sync xmlns='urn:xmpp:private-notes:key-sync:0'>"
                       "{&quot;type&quot;:&quot;response&quot;,&quot;requestId&quot;:&quot;request-1&quot;,"
                       "&quot;recoveryKey&quot;:&quot;private-notes-key-v1:test&quot;}</key-sync></iq>")));
    QCOMPARE(XmppKeySyncExtension::responseRecoveryKey(document.documentElement(), QStringLiteral("request-1")),
             QStringLiteral("private-notes-key-v1:test"));
    QVERIFY(XmppKeySyncExtension::responseRecoveryKey(document.documentElement(), QStringLiteral("other")).isEmpty());
    document.documentElement().setAttribute(QStringLiteral("id"), QStringLiteral("different-iq"));
    QVERIFY(
        XmppKeySyncExtension::responseRecoveryKey(document.documentElement(), QStringLiteral("request-1")).isEmpty());
    document.documentElement().setAttribute(QStringLiteral("id"), QStringLiteral("request-1"));
    document.documentElement().setAttribute(QStringLiteral("type"), QStringLiteral("set"));
    QVERIFY(
        XmppKeySyncExtension::responseRecoveryKey(document.documentElement(), QStringLiteral("request-1")).isEmpty());
}

void XmppKeySyncExtensionTest::duplicateRequestKeepsOriginalApproval()
{
    XmppKeySyncExtension extension;
    QSignalSpy           prompts(&extension, &XmppKeySyncExtension::trustRequestReceived);
    QDomDocument         document;
    QVERIFY(document.setContent(
        QStringLiteral("<iq type='set' from='me@example.test/first' id='request-1'>"
                       "<key-sync xmlns='urn:xmpp:private-notes:key-sync:0'>"
                       "{&quot;type&quot;:&quot;trust-request&quot;,&quot;requestId&quot;:&quot;request-1&quot;,"
                       "&quot;senderKey&quot;:&quot;Zmlyc3Q=&quot;}</key-sync></iq>")));
    QVERIFY(extension.handleStanza(document.documentElement(), {}));
    document.documentElement().setAttribute(QStringLiteral("from"), QStringLiteral("me@example.test/second"));
    QVERIFY(extension.handleStanza(document.documentElement(), {}));
    QCOMPARE(prompts.size(), 1);
    QCOMPARE(prompts.first().at(1).toString(), QStringLiteral("me@example.test/first"));
    extension.reject(QStringLiteral("request-1"));
    QVERIFY(extension.handleStanza(document.documentElement(), {}));
    QCOMPARE(prompts.size(), 2);
}

QTEST_GUILESS_MAIN(XmppKeySyncExtensionTest)
#include "xmppkeysyncextension_test.moc"
