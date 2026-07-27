#include "secureenvelope.h"

#include <QtTest>

using namespace QtNote;

Q_DECLARE_METATYPE(QtNote::AeadContext)

class SecureEnvelopeTest : public QObject {
    Q_OBJECT

private slots:
    void domainsProduceDifferentKeys();
    void contextIsAuthenticated_data();
    void contextIsAuthenticated();
    void rawAeadRoundTrip();
    void recoveryKeyRoundTrip();
    void rejectsRecoveryKeyTypo();
    void opensLargeEnvelope();
};

void SecureEnvelopeTest::domainsProduceDifferentKeys()
{
    const auto master = SecureEnvelope::generateMasterKey();
    QCOMPARE(master.size(), SecureEnvelope::MasterKeySize);
    const auto draft   = SecureEnvelope::deriveKey(master, KeyDomain::LocalDraft);
    const auto index   = SecureEnvelope::deriveKey(master, KeyDomain::StorageIndex);
    const auto content = SecureEnvelope::deriveKey(master, KeyDomain::StorageContent);
    QCOMPARE(draft.size(), SecureEnvelope::MasterKeySize);
    QVERIFY(draft != index);
    QVERIFY(index != content);
    QVERIFY(draft != content);
}

void SecureEnvelopeTest::contextIsAuthenticated_data()
{
    QTest::addColumn<AeadContext>("changed");
    const AeadContext base { KeyDomain::StorageIndex, QStringLiteral("urn:xmpp:qtnote:index:1"),
                             QStringLiteral("note-1"), 1, QStringLiteral("index") };
    auto              context = base;
    context.domain            = KeyDomain::StorageContent;
    QTest::newRow("domain") << context;
    context           = base;
    context.container = QStringLiteral("urn:xmpp:qtnote:content:1");
    QTest::newRow("container") << context;
    context        = base;
    context.itemId = QStringLiteral("note-2");
    QTest::newRow("item-id") << context;
    context        = base;
    context.schema = 2;
    QTest::newRow("schema") << context;
    context      = base;
    context.kind = QStringLiteral("content");
    QTest::newRow("kind") << context;
}

void SecureEnvelopeTest::contextIsAuthenticated()
{
    QFETCH(AeadContext, changed);
    const auto        key = SecureEnvelope::generateMasterKey();
    const AeadContext original { KeyDomain::StorageIndex, QStringLiteral("urn:xmpp:qtnote:index:1"),
                                 QStringLiteral("note-1"), 1, QStringLiteral("index") };
    auto              encrypted = SecureEnvelope::seal(QByteArrayLiteral("secret"), key, original);
    QVERIFY(encrypted);
    auto opened = SecureEnvelope::open(encrypted.value, key, changed);
    QVERIFY(!opened);
    QCOMPARE(opened.error.code, CryptoError::AuthenticationFailed);
}

void SecureEnvelopeTest::rawAeadRoundTrip()
{
    const auto key = SecureEnvelope::generateMasterKey();
    const auto encrypted
        = SecureEnvelope::encryptAead(QByteArrayLiteral("portable plaintext"), key, KeyDomain::StorageIndex);
    QVERIFY2(encrypted, qPrintable(encrypted.error.message));
    QCOMPARE(encrypted.value.nonce.size(), 12);
    QCOMPARE(encrypted.value.tag.size(), 16);
    const auto opened = SecureEnvelope::decryptAead(encrypted.value, key, KeyDomain::StorageIndex);
    QVERIFY2(opened, qPrintable(opened.error.message));
    QCOMPARE(opened.value, QByteArrayLiteral("portable plaintext"));
    const auto wrongDomain = SecureEnvelope::decryptAead(encrypted.value, key, KeyDomain::StorageContent);
    QVERIFY(!wrongDomain);
    QCOMPARE(wrongDomain.error.code, CryptoError::AuthenticationFailed);
}

void SecureEnvelopeTest::recoveryKeyRoundTrip()
{
    const auto key     = SecureEnvelope::generateMasterKey();
    const auto encoded = SecureEnvelope::encodeRecoveryKey(key);
    QVERIFY(encoded.startsWith(QStringLiteral("qtnote-key-v1:")));
    auto decoded = SecureEnvelope::decodeRecoveryKey(encoded);
    QVERIFY(decoded);
    QCOMPARE(decoded.value, key);
}

void SecureEnvelopeTest::rejectsRecoveryKeyTypo()
{
    auto encoded = SecureEnvelope::encodeRecoveryKey(SecureEnvelope::generateMasterKey());
    QVERIFY(!encoded.isEmpty());
    encoded[encoded.size() - 1] = encoded.back() == QLatin1Char('0') ? QLatin1Char('1') : QLatin1Char('0');
    QVERIFY(!SecureEnvelope::decodeRecoveryKey(encoded));
}

void SecureEnvelopeTest::opensLargeEnvelope()
{
    const auto        key = SecureEnvelope::generateMasterKey();
    const AeadContext context { KeyDomain::OmemoState, QStringLiteral("xmpp-omemo"),
                                QStringLiteral("account@example.org"), 1, QStringLiteral("state") };
    QByteArray        plainText(16467, '\0');
    for (int i = 0; i < plainText.size(); ++i)
        plainText[i] = char(i % 251);

    const auto sealed = SecureEnvelope::seal(plainText, key, context);
    QVERIFY2(sealed, qPrintable(sealed.error.message));
    const auto opened = SecureEnvelope::open(sealed.value, key, context);
    QVERIFY2(opened, qPrintable(opened.error.message));
    QCOMPARE(opened.value, plainText);
}

QTEST_GUILESS_MAIN(SecureEnvelopeTest)
#include "secureenvelope_test.moc"
