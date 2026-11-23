#ifndef SECUREKEYSTORE_H
#define SECUREKEYSTORE_H

#include "secureenvelope.h"

#include <functional>

class QObject;

namespace AnyKeep {

class ANYKEEP_EXPORT SecureKeyStore {
public:
    using PasswordCallback = std::function<void(CryptoResult<QString>)>;
    using KeyCallback      = std::function<void(CryptoResult<QByteArray>)>;
    using ErrorCallback    = std::function<void(CryptoError)>;

    // All callbacks are queued onto context's thread and suppressed if it dies.
    // Platform keychain I/O never runs in a caller's nested event loop.
    static void readPasswordAsync(QObject *context, const QString &service, const QString &keyName,
                                  PasswordCallback callback);
    static void writePasswordAsync(QObject *context, const QString &service, const QString &keyName,
                                   const QString &password, ErrorCallback callback);
    static void readAsync(QObject *context, const QString &keyName, KeyCallback callback);
    static void writeAsync(QObject *context, const QString &keyName, const QByteArray &key, ErrorCallback callback);
    static void removeAsync(QObject *context, const QString &keyName, ErrorCallback callback);
    static void loadOrCreateAsync(QObject *context, const QString &keyName, KeyCallback callback);

    // Legacy entry points, retained only until their startup/settings callers
    // have moved to the async API. Never call from SCTP or GUI callbacks.
    static CryptoResult<QString> readPassword(const QString &service, const QString &keyName);
    static CryptoError           writePassword(const QString &service, const QString &keyName, const QString &password);
    static CryptoResult<QByteArray> read(const QString &keyName);
    static CryptoError              write(const QString &keyName, const QByteArray &key);
    static CryptoError              remove(const QString &keyName);
    static CryptoResult<QByteArray> loadOrCreate(const QString &keyName);
};

} // namespace AnyKeep

#endif // SECUREKEYSTORE_H
