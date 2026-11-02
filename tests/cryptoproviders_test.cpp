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
