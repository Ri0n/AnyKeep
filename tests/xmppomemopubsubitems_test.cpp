#include "xmppomemopubsubitems.h"

#include <QDomDocument>
#include <QXmlStreamWriter>
#include <QtTest>

using namespace AnyKeep;

class XmppOmemoPubSubItemsTest : public QObject {
    Q_OBJECT

private slots:
    void parsesDeviceList();
    void serializesDeviceList();
    void parsesBundleIdentityKey();
    void repairsIncompleteBundle();
};

void XmppOmemoPubSubItemsTest::parsesDeviceList()
{
    QDomDocument document;
    auto         xmlData = QStringLiteral("<item id='current'><devices xmlns='urn:xmpp:omemo:2'>"
                                                  "<device id='9523' label='AnyKeep-one'/><device id='672' label='AnyKeep-two'/>"
                                                  "</devices></item>");
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QVERIFY(document.setContent(xmlData, QDomDocument::ParseOption::UseNamespaceProcessing));
#else
    QString errorMsg;
    int     errorLine, errorColumn;

    QVERIFY(document.setContent(xmlData, true, &errorMsg, &errorLine, &errorColumn));
#endif
    XmppOmemoDeviceListItem item;
    item.parse(document.documentElement());
    QCOMPARE(item.devices().size(), 2);
    QCOMPARE(item.devices().at(1).id, QStringLiteral("672"));
    QCOMPARE(item.devices().at(1).label, QStringLiteral("AnyKeep-two"));
}

void XmppOmemoPubSubItemsTest::serializesDeviceList()
{
    XmppOmemoDeviceListItem item;
    item.setId(QStringLiteral("current"));
    item.setDevices({ { QStringLiteral("AnyKeep-one"), QStringLiteral("9523"), QStringLiteral("signature") },
                      { QStringLiteral("AnyKeep-two"), QStringLiteral("672"), {} } });
    QString          xml;
    QXmlStreamWriter writer(&xml);
    item.toXml(&writer);
    QVERIFY(xml.contains(QStringLiteral("<device id=\"9523\" label=\"AnyKeep-one\" labelsig=\"signature\"")));
    QVERIFY(xml.contains(QStringLiteral("<device id=\"672\" label=\"AnyKeep-two\"")));
}

void XmppOmemoPubSubItemsTest::parsesBundleIdentityKey()
{
    const auto   identity = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    QDomDocument document;
    auto         xmlData = QStringLiteral("<item id='672'><bundle xmlns='urn:xmpp:omemo:2'><ik>%1</ik></bundle></item>")
                       .arg(QString::fromLatin1(identity.toBase64()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QVERIFY(document.setContent(xmlData, QDomDocument::ParseOption::UseNamespaceProcessing));
#else
    QString errorMsg;
    int     errorLine, errorColumn;

    QVERIFY(document.setContent(xmlData, true, &errorMsg, &errorLine, &errorColumn));
#endif
    XmppOmemoBundleItem item;
    item.parse(document.documentElement());
    QCOMPARE(item.identityKey(), identity);
}

void XmppOmemoPubSubItemsTest::repairsIncompleteBundle()
{
    const auto parse = [](const QString &xml) {
        QDomDocument document;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (!document.setContent(xml, QDomDocument::ParseOption::UseNamespaceProcessing))
#else
        QString errorMsg;
        int     errorLine, errorColumn;
        if (!document.setContent(xml, true, &errorMsg, &errorLine, &errorColumn))
#endif
            return XmppOmemoBundleItem {};
        XmppOmemoBundleItem item;
        item.parse(document.documentElement());
        return item;
    };
    auto valid      = parse(QStringLiteral(
        "<item id='7'><bundle xmlns='urn:xmpp:omemo:2'><ik>aWs=</ik><spk id='4'>c3Br</spk>"
             "<spks>c2ln</spks><prekeys><pk id='10'>b2xk</pk><pk id='11'>a2VlcA==</pk></prekeys></bundle></item>"));
    auto incomplete = parse(QStringLiteral("<item id='7'><bundle xmlns='urn:xmpp:omemo:2'><ik/><spk id='0'/><spks/>"
                                           "<prekeys><pk id='12'>bmV3</pk></prekeys></bundle></item>"));

    const auto repaired = incomplete.repairedFrom(valid, { 10 });
    QCOMPARE(repaired.identityKey(), QByteArrayLiteral("ik"));
    QVERIFY(!repaired.publicPreKeys().contains(10));
    QCOMPARE(repaired.publicPreKeys().value(11), QByteArrayLiteral("keep"));
    QCOMPARE(repaired.publicPreKeys().value(12), QByteArrayLiteral("new"));

    QString          xml;
    QXmlStreamWriter writer(&xml);
    repaired.toXml(&writer);
    QVERIFY(xml.contains(QStringLiteral("<ik>aWs=</ik>")));
    QVERIFY(xml.contains(QStringLiteral("<spk id=\"4\">c3Br</spk>")));
    QVERIFY(!xml.contains(QStringLiteral("id=\"10\"")));
    QVERIFY(xml.contains(QStringLiteral("id=\"11\"")));
    QVERIFY(xml.contains(QStringLiteral("id=\"12\"")));
}

QTEST_GUILESS_MAIN(XmppOmemoPubSubItemsTest)
#include "xmppomemopubsubitems_test.moc"
