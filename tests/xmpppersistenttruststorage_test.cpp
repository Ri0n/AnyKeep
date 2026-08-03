#include "xmpppersistenttruststorage.h"

#include "secureenvelope.h"

#include <QFile>
#include <QTemporaryDir>
#include <QXmppTask.h>
#include <QtTest>

#include <optional>

using namespace AnyKeep;

class XmppPersistentTrustStorageTest : public QObject {
    Q_OBJECT

private slots:
    void persistsEncryptedManualTrust();
};

void XmppPersistentTrustStorageTest::persistsEncryptedManualTrust()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path          = directory.filePath(QStringLiteral("trust.bin"));
    const auto encryptionKey = SecureEnvelope::generateMasterKey();
    const auto deviceKey     = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const auto protocol      = QStringLiteral("urn:xmpp:omemo:2");
    const auto jid           = QStringLiteral("me@example.org");
    {
        XmppPersistentTrustStorage storage(path, encryptionKey, jid);
        QVERIFY(storage.isValid());
        QMultiHash<QString, QByteArray> keys;
        keys.insert(jid, deviceKey);
        storage.addKeys(protocol, jid, { deviceKey }, QXmpp::TrustLevel::AutomaticallyDistrusted);
        storage.setTrustLevel(protocol, keys, QXmpp::TrustLevel::ManuallyTrusted);
        QVERIFY(storage.isValid());
    }
    QFile persisted(path);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QVERIFY(!persisted.readAll().contains(deviceKey));

    XmppPersistentTrustStorage restored(path, encryptionKey, jid);
    QVERIFY(restored.isValid());
    std::optional<QXmpp::TrustLevel> level;
    restored.trustLevel(protocol, jid, deviceKey).then(this, [&](QXmpp::TrustLevel value) { level = value; });
    QVERIFY(level.has_value());
    QCOMPARE(*level, QXmpp::TrustLevel::ManuallyTrusted);
}

QTEST_GUILESS_MAIN(XmppPersistentTrustStorageTest)
#include "xmpppersistenttruststorage_test.moc"
