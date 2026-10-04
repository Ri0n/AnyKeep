#include "mediachunkwire.h"
#include "secureenvelope.h"

#include <QCryptographicHash>
#include <QtTest>

using namespace AnyKeep;

Q_DECLARE_METATYPE(AnyKeep::AeadContext)

class SecureEnvelopeTest : public QObject {
    Q_OBJECT

private slots:
    void domainsProduceDifferentKeys();
    void contextIsAuthenticated_data();
    void contextIsAuthenticated();
    void rawAeadRoundTrip();
    void explicitNonceIsDeterministic();
    void privateNotesProfileRoundTrip();
    void mediaChunkWireRoundTripAndMapping();
    void mediaChunkWireRejectsTamperingAndWrongIndex();
    void mediaChunkWireRepresentsEmptyFile();
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
    const auto media   = SecureEnvelope::deriveKey(master, KeyDomain::RemoteMediaChunk,
                                                   KeyDerivationProfile::PrivateNotes);
    QCOMPARE(draft.size(), SecureEnvelope::MasterKeySize);
    QCOMPARE(media.size(), SecureEnvelope::MasterKeySize);
    QVERIFY(draft != index);
    QVERIFY(index != content);
    QVERIFY(draft != content);
    QVERIFY(media != SecureEnvelope::deriveKey(master, KeyDomain::StorageContent,
                                               KeyDerivationProfile::PrivateNotes));
}

void SecureEnvelopeTest::contextIsAuthenticated_data()
{
    QTest::addColumn<AeadContext>("changed");
    const AeadContext base { KeyDomain::StorageIndex, QStringLiteral("urn:xmpp:private-notes:index:0"),
                             QStringLiteral("note-1"), 1, QStringLiteral("index") };
    auto              context = base;
    context.domain            = KeyDomain::StorageContent;
    QTest::newRow("domain") << context;
    context           = base;
    context.container = QStringLiteral("urn:xmpp:private-notes:content:0");
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
    const AeadContext original { KeyDomain::StorageIndex, QStringLiteral("urn:xmpp:private-notes:index:0"),
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
    QCOMPARE(encrypted.value.nonce.size(), SecureEnvelope::AeadNonceSize);
    QCOMPARE(encrypted.value.tag.size(), SecureEnvelope::AeadTagSize);
    const auto opened = SecureEnvelope::decryptAead(encrypted.value, key, KeyDomain::StorageIndex);
    QVERIFY2(opened, qPrintable(opened.error.message));
    QCOMPARE(opened.value, QByteArrayLiteral("portable plaintext"));
    const auto wrongDomain = SecureEnvelope::decryptAead(encrypted.value, key, KeyDomain::StorageContent);
    QVERIFY(!wrongDomain);
    QCOMPARE(wrongDomain.error.code, CryptoError::AuthenticationFailed);
}

void SecureEnvelopeTest::explicitNonceIsDeterministic()
{
    const auto key = SecureEnvelope::generateMasterKey();
    const QByteArray nonce = QByteArray::fromHex("0102030405060708090a0b0c");
    const QByteArray plain("same immutable representation");
    const auto first = SecureEnvelope::encryptAeadWithNonce(plain, key, KeyDomain::RemoteMediaChunk,
                                                            KeyDerivationProfile::PrivateNotes, nonce);
    const auto second = SecureEnvelope::encryptAeadWithNonce(plain, key, KeyDomain::RemoteMediaChunk,
                                                             KeyDerivationProfile::PrivateNotes, nonce);
    QVERIFY2(first, qPrintable(first.error.message));
    QVERIFY2(second, qPrintable(second.error.message));
    QCOMPARE(first.value.nonce, nonce);
    QCOMPARE(first.value.cipherText, second.value.cipherText);
    QCOMPARE(first.value.tag, second.value.tag);

    auto changedNonce = nonce;
    changedNonce[changedNonce.size() - 1] ^= char(1);
    const auto changed = SecureEnvelope::encryptAeadWithNonce(plain, key, KeyDomain::RemoteMediaChunk,
                                                              KeyDerivationProfile::PrivateNotes, changedNonce);
    QVERIFY2(changed, qPrintable(changed.error.message));
    QVERIFY(first.value.cipherText != changed.value.cipherText || first.value.tag != changed.value.tag);

    const auto opened = SecureEnvelope::decryptAead(first.value, key, KeyDomain::RemoteMediaChunk,
                                                    KeyDerivationProfile::PrivateNotes);
    QVERIFY2(opened, qPrintable(opened.error.message));
    QCOMPARE(opened.value, plain);
}

void SecureEnvelopeTest::privateNotesProfileRoundTrip()
{
    const auto key       = SecureEnvelope::generateMasterKey();
    const auto encrypted = SecureEnvelope::encryptAead(QByteArrayLiteral("portable plaintext"), key,
                                                       KeyDomain::StorageIndex, KeyDerivationProfile::PrivateNotes);
    QVERIFY2(encrypted, qPrintable(encrypted.error.message));
    const auto opened = SecureEnvelope::decryptAead(encrypted.value, key, KeyDomain::StorageIndex,
                                                    KeyDerivationProfile::PrivateNotes);
    QVERIFY2(opened, qPrintable(opened.error.message));
    QCOMPARE(opened.value, QByteArrayLiteral("portable plaintext"));
    QVERIFY(SecureEnvelope::keyId(key, KeyDerivationProfile::PrivateNotes) != SecureEnvelope::keyId(key));

    const auto recovery = SecureEnvelope::encodeRecoveryKey(key, KeyDerivationProfile::PrivateNotes);
    QVERIFY(recovery.startsWith(QStringLiteral("private-notes-key-v1:")));
    QVERIFY(SecureEnvelope::decodeRecoveryKey(recovery, KeyDerivationProfile::PrivateNotes));
    QVERIFY(!SecureEnvelope::decodeRecoveryKey(recovery));
}

void SecureEnvelopeTest::mediaChunkWireRoundTripAndMapping()
{
    QByteArray plain(2500, Qt::Uninitialized);
    for (int i = 0; i < plain.size(); ++i)
        plain[i] = char(i % 251);
    const auto checksum  = QCryptographicHash::hash(plain, QCryptographicHash::Sha256);
    const auto generated = MediaChunkWire::generate(plain.size(), checksum, 1024);
    QVERIFY(generated);
    const auto parameters = *generated;
    QCOMPARE(parameters.chunkCount(), quint64(3));

    const auto totalWireSize = MediaChunkWire::wireSize(parameters);
    QVERIFY(totalWireSize);
    QCOMPARE(*totalWireSize, quint64(2740));
    const auto offset0 = MediaChunkWire::wireChunkOffset(parameters, 0);
    const auto offset1 = MediaChunkWire::wireChunkOffset(parameters, 1);
    const auto offset2 = MediaChunkWire::wireChunkOffset(parameters, 2);
    QVERIFY(offset0);
    QVERIFY(offset1);
    QVERIFY(offset2);
    QCOMPARE(*offset0, quint64(0));
    QCOMPARE(*offset1, quint64(1104));
    QCOMPARE(*offset2, quint64(2208));
    const auto firstWireChunkSize = MediaChunkWire::wireChunkSize(parameters, 0);
    const auto lastWireChunkSize  = MediaChunkWire::wireChunkSize(parameters, 2);
    QVERIFY(firstWireChunkSize);
    QVERIFY(lastWireChunkSize);
    QCOMPARE(*firstWireChunkSize, quint64(1104));
    QCOMPARE(*lastWireChunkSize, quint64(532));

    QByteArray wire;
    QByteArray restored;
    for (quint64 index = 0; index < parameters.chunkCount(); ++index) {
        const auto actual = MediaChunkWire::plainChunkSize(parameters, index);
        QVERIFY(actual);
        const QByteArray plainChunk = plain.mid(qsizetype(index * parameters.chunkSize), *actual);
        const auto encrypted = MediaChunkWire::encryptChunk(parameters, index, plainChunk);
        QVERIFY2(encrypted, qPrintable(encrypted.error));
        const auto repeated = MediaChunkWire::encryptChunk(parameters, index, plainChunk);
        QVERIFY2(repeated, qPrintable(repeated.error));
        QCOMPARE(repeated.value, encrypted.value);
        wire.append(encrypted.value);
        const auto decrypted = MediaChunkWire::decryptChunk(parameters, index, encrypted.value);
        QVERIFY2(decrypted, qPrintable(decrypted.error));
        restored.append(decrypted.value);
    }
    QCOMPARE(restored, plain);
    QCOMPARE(quint64(wire.size()), *totalWireSize);

    const auto index0End = MediaChunkWire::chunkIndexForWireOffset(parameters, 1103);
    const auto index1    = MediaChunkWire::chunkIndexForWireOffset(parameters, 1104);
    const auto indexLast = MediaChunkWire::chunkIndexForWireOffset(parameters, quint64(wire.size() - 1));
    QVERIFY(index0End);
    QVERIFY(index1);
    QVERIFY(indexLast);
    QCOMPARE(*index0End, quint64(0));
    QCOMPARE(*index1, quint64(1));
    QCOMPARE(*indexLast, quint64(2));
    QVERIFY(!MediaChunkWire::chunkIndexForWireOffset(parameters, quint64(wire.size())));
}

void SecureEnvelopeTest::mediaChunkWireRejectsTamperingAndWrongIndex()
{
    const QByteArray plain(2048, 'm');
    const auto generated
        = MediaChunkWire::generate(plain.size(), QCryptographicHash::hash(plain, QCryptographicHash::Sha256), 1024);
    QVERIFY(generated);
    const auto parameters = *generated;
    const auto first      = MediaChunkWire::encryptChunk(parameters, 0, plain.left(1024));
    QVERIFY2(first, qPrintable(first.error));

    auto tampered = first.value;
    tampered[tampered.size() / 2] ^= char(1);
    QVERIFY(!MediaChunkWire::decryptChunk(parameters, 0, tampered));

    // Full records have the same byte length, but a record is bound to its
    // chunk index through both nonce selection and the encrypted context.
    QVERIFY(!MediaChunkWire::decryptChunk(parameters, 1, first.value));

    auto wrongIdentity = parameters;
    wrongIdentity.plainChecksum[0] ^= char(1);
    QVERIFY(!MediaChunkWire::decryptChunk(wrongIdentity, 0, first.value));
}

void SecureEnvelopeTest::mediaChunkWireRepresentsEmptyFile()
{
    const QByteArray checksum  = QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256);
    const auto       generated = MediaChunkWire::generate(0, checksum, 1024);
    QVERIFY(generated);
    const auto parameters = *generated;
    QCOMPARE(parameters.chunkCount(), quint64(1));
    const auto totalWireSize = MediaChunkWire::wireSize(parameters);
    QVERIFY(totalWireSize);
    QCOMPARE(*totalWireSize, quint64(MediaChunkWire::Overhead));
    const auto encrypted = MediaChunkWire::encryptChunk(parameters, 0, {});
    QVERIFY2(encrypted, qPrintable(encrypted.error));
    QCOMPARE(encrypted.value.size(), qsizetype(MediaChunkWire::Overhead));
    const auto decrypted = MediaChunkWire::decryptChunk(parameters, 0, encrypted.value);
    QVERIFY2(decrypted, qPrintable(decrypted.error));
    QVERIFY(decrypted.value.isEmpty());
}

void SecureEnvelopeTest::recoveryKeyRoundTrip()
{
    const auto key     = SecureEnvelope::generateMasterKey();
    const auto encoded = SecureEnvelope::encodeRecoveryKey(key);
    QVERIFY(encoded.startsWith(QStringLiteral("anykeep-key-v1:")));
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
