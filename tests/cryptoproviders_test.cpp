#include <QtCrypto>
#include <QtTest>
#include <qcaprovider.h>

#ifdef ANYKEEP_TEST_IRIS_JDNS
#include <iris/irisnet/noncore/remotenameprovider.h>
#endif

class CryptoProvidersTest : public QObject {
    Q_OBJECT

private slots:
    void windowsUsesNativeProviders()
    {
#ifdef Q_OS_WIN
        QVERIFY(QCA::isSupported("tls", QStringLiteral("qca-wincrypto")));
        QVERIFY(QCA::isSupported("aes256-gcm", QStringLiteral("qca-wincrypto")));
        QVERIFY(QCA::isSupported("sasl", QStringLiteral("qca-wingss")));
        QVERIFY(QCA::isSupported("sasl", QStringLiteral("qca-cyrus-sasl")));
        QCA::TLS tls;
        QCOMPARE(tls.provider()->name(), QStringLiteral("qca-wincrypto"));
        QCA::Cipher cipher(QStringLiteral("aes256"), QCA::Cipher::GCM);
        QCOMPARE(cipher.provider()->name(), QStringLiteral("qca-wincrypto"));
#else
        QSKIP("Native Windows providers are platform-specific");
#endif
    }

    void windowsSaslStartsScram_data()
    {
        QTest::addColumn<QString>("provider");
        QTest::newRow("default") << QString();
        QTest::newRow("wingss") << QStringLiteral("qca-wingss");
        QTest::newRow("cyrus-sasl") << QStringLiteral("qca-cyrus-sasl");
    }

    void windowsSaslStartsScram()
    {
#ifdef Q_OS_WIN
        QFETCH(QString, provider);
        QCA::SASL client(nullptr, provider);
        client.setUsername(QStringLiteral("user"));
        client.setPassword(QCA::SecureArray("pencil"));
        QSignalSpy started(&client, &QCA::SASL::clientStarted);
        QSignalSpy errors(&client, &QCA::SASL::error);
        client.startClient(QStringLiteral("xmpp"), QStringLiteral("example.test"),
                           QStringList { QStringLiteral("SCRAM-SHA-256") }, QCA::SASL::AllowClientSendFirst);
        QTRY_VERIFY_WITH_TIMEOUT(!started.isEmpty() || !errors.isEmpty(), 5000);
        QVERIFY2(errors.isEmpty(), "Unable to start SCRAM through the Windows SASL provider");
        QCOMPARE(client.mechanism(), QStringLiteral("SCRAM-SHA-256"));
        QVERIFY(started.first().at(0).toBool());
        QVERIFY(started.first().at(1).toByteArray().startsWith("n,,n=user,r="));
#else
        QSKIP("Native Windows providers are platform-specific");
#endif
    }

    void irisHasJdns()
    {
#ifdef ANYKEEP_TEST_IRIS_JDNS
        QVERIFY2(XMPP::RemoteNameProvider::isSupported(), "Iris must be built with IRIS_ENABLE_JDNS=ON");
#else
        QSKIP("Iris is not available in this build");
#endif
    }
};

QTEST_MAIN(CryptoProvidersTest)
#include "cryptoproviders_test.moc"
