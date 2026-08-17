#ifndef SECUREENVELOPE_H
#define SECUREENVELOPE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QString>

namespace AnyKeep {

enum class KeyDomain : quint8 {
    LocalDraft = 1,
    StorageIndex,
    StorageContent,
    StorageKeyTransport,
    OmemoState,
    LocalRemoteCache,
    LocalMedia,
    LocalFolderCatalog,
    LocalRuleStore
};

/**
 * Key-derivation labels identify an encryption format, not an application.
 * Use AnyKeepLocal for local storage and PrivateNotes for the portable XMPP
 * protocol.
 */
enum class KeyDerivationProfile : quint8 { AnyKeepLocal, PrivateNotes };

struct ANYKEEP_EXPORT AeadContext {
    KeyDomain domain { KeyDomain::LocalDraft };
    QString   container;
    QString   itemId;
    // Consumer-owned context schema serialized by SecureEnvelope::associatedData().
    // Increment when the consumer changes the authenticated meaning or identity
    // mapping of this context and old ciphertext must no longer open. Payload-only
    // format changes belong to the consumer's payload version instead.
    quint32 schema { 1 };
    QString kind;
};

/** Raw AES-GCM output without Qt-specific framing. */
struct ANYKEEP_EXPORT AeadCiphertext {
    QByteArray nonce;
    QByteArray tag;
    QByteArray cipherText;
};

struct ANYKEEP_EXPORT CryptoError {
    enum Code { None, InvalidArgument, Unavailable, AuthenticationFailed, Corrupt, Unsupported };

    Code    code { None };
    QString message;

    explicit operator bool() const { return code != None; }
};

template <typename T> struct CryptoResult {
    T           value;
    CryptoError error;

    explicit operator bool() const { return !error; }
};

/**
 * Authenticated encryption helpers. The process entry point must keep a
 * QcaInitializer alive while these functions are in use.
 */
class ANYKEEP_EXPORT SecureEnvelope {
public:
    static constexpr int MasterKeySize = 32;

    static bool                     isAvailable();
    static QByteArray               generateMasterKey();
    static QByteArray               keyId(const QByteArray &masterKey);
    static QByteArray               keyId(const QByteArray &masterKey, KeyDerivationProfile profile);
    static QString                  encodeRecoveryKey(const QByteArray &masterKey);
    static QString                  encodeRecoveryKey(const QByteArray &masterKey, KeyDerivationProfile profile);
    static CryptoResult<QByteArray> decodeRecoveryKey(const QString &encoded);
    static CryptoResult<QByteArray> decodeRecoveryKey(const QString &encoded, KeyDerivationProfile profile);
    static QByteArray               deriveKey(const QByteArray &masterKey, KeyDomain domain);
    static QByteArray deriveKey(const QByteArray &masterKey, KeyDomain domain, KeyDerivationProfile profile);
    static QByteArray associatedData(const AeadContext &context);

    /**
     * Raw domain-separated AES-256-GCM for portable protocol codecs.
     *
     * These helpers do not add external AAD or framing. A caller that needs
     * context binding must include and validate that context in the encrypted
     * plaintext, as the XMPP XML codec does. Local data should normally use
     * seal() and open() instead.
     */
    static CryptoResult<AeadCiphertext> encryptAead(const QByteArray &plainText, const QByteArray &masterKey,
                                                    KeyDomain domain);
    static CryptoResult<AeadCiphertext> encryptAead(const QByteArray &plainText, const QByteArray &masterKey,
                                                    KeyDomain domain, KeyDerivationProfile profile);
    static CryptoResult<QByteArray>     decryptAead(const AeadCiphertext &encrypted, const QByteArray &masterKey,
                                                    KeyDomain domain);
    static CryptoResult<QByteArray>     decryptAead(const AeadCiphertext &encrypted, const QByteArray &masterKey,
                                                    KeyDomain domain, KeyDerivationProfile profile);

    static CryptoResult<QByteArray> seal(const QByteArray &plainText, const QByteArray &masterKey,
                                         const AeadContext &context);
    static CryptoResult<QByteArray> open(const QByteArray &envelope, const QByteArray &masterKey,
                                         const AeadContext &context);
};

} // namespace AnyKeep

#endif // SECUREENVELOPE_H
