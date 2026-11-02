#include "qcainitializer.h"
#include "secureenvelope.h"

#include <QCoreApplication>
#include <QtTest>

class QcaStartupTest : public QObject {
    Q_OBJECT
private slots:
    void firstCryptoUseHasSecureRandomAndAes()
    {
        // Check the combined contract before anything can trigger a later
        // provider rescan and conceal an incorrectly cached default RNG.
        QVERIFY(AnyKeep::SecureEnvelope::isAvailable());
        const auto key = AnyKeep::SecureEnvelope::generateMasterKey();
        QCOMPARE(key.size(), AnyKeep::SecureEnvelope::MasterKeySize);
        AnyKeep::AeadContext context;
        context.domain    = AnyKeep::KeyDomain::LocalDraft;
        context.container = QStringLiteral("startup");
        context.itemId    = QStringLiteral("probe");
        context.kind      = QStringLiteral("startup-test");
        const auto sealed = AnyKeep::SecureEnvelope::seal(QByteArrayLiteral("startup probe"), key, context);
        QVERIFY(sealed);
        const auto opened = AnyKeep::SecureEnvelope::open(sealed.value, key, context);
        QVERIFY(opened);
        QCOMPARE(opened.value, QByteArrayLiteral("startup probe"));
    }
};

int main(int argc, char **argv)
{
    AnyKeep::QcaInitializer qca;
    QCoreApplication        app(argc, argv);
    qca.configureProviders();
    QcaStartupTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "qcastartup_test.moc"
