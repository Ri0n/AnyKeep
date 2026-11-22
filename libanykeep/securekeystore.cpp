#include "securekeystore.h"

#include "keychainservice.h"

#include <QEventLoop>
#include <QMetaObject>
#include <QPointer>

#include <utility>

namespace AnyKeep {
namespace {
    const QString Service = QStringLiteral("com.github.ri0n.anykeep");

    CryptoError errorFor(const KeychainService::Result &result, const QString &notFound)
    {
        switch (result.status) {
        case KeychainService::Status::Ok:
            return {};
        case KeychainService::Status::NotFound:
            return { CryptoError::InvalidArgument, notFound };
        case KeychainService::Status::AccessDenied:
        case KeychainService::Status::Unavailable:
        case KeychainService::Status::Failed:
            return { CryptoError::Unavailable, result.error };
        }
        return { CryptoError::Unavailable, QStringLiteral("Unknown keychain error") };
    }

    template <typename Result, typename Callback>
    void deliver(QObject *context, Callback callback, Result result)
    {
        if (!context || !callback)
            return;
        const QPointer<QObject> guard(context);
        QMetaObject::invokeMethod(
            context,
            [guard, callback = std::move(callback), result = std::move(result)]() mutable {
                if (guard)
                    callback(std::move(result));
            },
            Qt::QueuedConnection);
    }

    template <typename Result, typename Start>
    Result waitForLegacy(Start start)
    {
        // Transitional compatibility only. Every platform job is asynchronous
        // in KeychainService; the remaining synchronous call sites are audited
        // separately in issue #191 and must be removed before this helper.
        QEventLoop loop;
        Result result {};
        start(&loop, [&](Result value) {
            result = std::move(value);
            loop.quit();
        });
        loop.exec();
        return result;
    }
} // namespace

void SecureKeyStore::readPasswordAsync(QObject *context, const QString &service, const QString &keyName,
                                       PasswordCallback callback)
{
    if (!context || !callback)
        return;
    KeychainService::read(context, service, keyName, [callback = std::move(callback)](KeychainService::Result result) mutable {
        const auto error = errorFor(result, QStringLiteral("Password was not found"));
        callback({ error ? QString() : std::move(result.text), error });
    });
}

void SecureKeyStore::writePasswordAsync(QObject *context, const QString &service, const QString &keyName,
                                        const QString &password, ErrorCallback callback)
{
    if (!context || !callback)
        return;
    if (service.isEmpty() || keyName.isEmpty()) {
        deliver(context, std::move(callback),
                CryptoError { CryptoError::InvalidArgument, QStringLiteral("Password service and key must not be empty") });
        return;
    }
    KeychainService::writeText(context, service, keyName, password,
                              [callback = std::move(callback)](KeychainService::Result result) mutable {
        callback(errorFor(result, QStringLiteral("Password was not found")));
    });
}

void SecureKeyStore::readAsync(QObject *context, const QString &keyName, KeyCallback callback)
{
    if (!context || !callback)
        return;
    KeychainService::read(context, Service, keyName, [callback = std::move(callback)](KeychainService::Result result) mutable {
        auto error = errorFor(result, QStringLiteral("Encryption key was not found"));
        if (!error && result.binary.size() != SecureEnvelope::MasterKeySize)
            error = { CryptoError::Corrupt, QStringLiteral("Stored encryption key has an invalid size") };
        callback({ error ? QByteArray() : std::move(result.binary), error });
    });
}

void SecureKeyStore::writeAsync(QObject *context, const QString &keyName, const QByteArray &key, ErrorCallback callback)
{
    if (!context || !callback)
        return;
    if (key.size() != SecureEnvelope::MasterKeySize) {
        deliver(context, std::move(callback),
                CryptoError { CryptoError::InvalidArgument, QStringLiteral("Encryption key has an invalid size") });
        return;
    }
    KeychainService::writeBinary(context, Service, keyName, key,
                                [callback = std::move(callback)](KeychainService::Result result) mutable {
        callback(errorFor(result, QStringLiteral("Encryption key was not found")));
    });
}

void SecureKeyStore::removeAsync(QObject *context, const QString &keyName, ErrorCallback callback)
{
    if (!context || !callback)
        return;
    KeychainService::remove(context, Service, keyName, [callback = std::move(callback)](KeychainService::Result result) mutable {
        callback(errorFor(result, QStringLiteral("Encryption key was not found")));
    });
}

void SecureKeyStore::loadOrCreateAsync(QObject *context, const QString &keyName, KeyCallback callback)
{
    if (!context || !callback)
        return;
    const QPointer<QObject> guard(context);
    readAsync(context, keyName,
              [guard, keyName, callback = std::move(callback)](CryptoResult<QByteArray> existing) mutable {
        if (existing || existing.error.code != CryptoError::InvalidArgument) {
            callback(std::move(existing));
            return;
        }
        const auto key = SecureEnvelope::generateMasterKey();
        if (key.size() != SecureEnvelope::MasterKeySize) {
            callback({ {}, { CryptoError::Unavailable, QStringLiteral("Could not generate a secure encryption key") } });
            return;
        }
        if (!guard)
            return;
        writeAsync(guard, keyName, key,
                   [callback = std::move(callback), key](CryptoError error) mutable {
            callback({ error ? QByteArray() : key, std::move(error) });
        });
    });
}

CryptoResult<QString> SecureKeyStore::readPassword(const QString &service, const QString &keyName)
{
    return waitForLegacy<CryptoResult<QString>>([&](QObject *context, PasswordCallback callback) {
        readPasswordAsync(context, service, keyName, std::move(callback));
    });
}

CryptoError SecureKeyStore::writePassword(const QString &service, const QString &keyName, const QString &password)
{
    return waitForLegacy<CryptoError>([&](QObject *context, ErrorCallback callback) {
        writePasswordAsync(context, service, keyName, password, std::move(callback));
    });
}

CryptoResult<QByteArray> SecureKeyStore::read(const QString &keyName)
{
    return waitForLegacy<CryptoResult<QByteArray>>([&](QObject *context, KeyCallback callback) {
        readAsync(context, keyName, std::move(callback));
    });
}

CryptoError SecureKeyStore::write(const QString &keyName, const QByteArray &key)
{
    return waitForLegacy<CryptoError>([&](QObject *context, ErrorCallback callback) {
        writeAsync(context, keyName, key, std::move(callback));
    });
}

CryptoError SecureKeyStore::remove(const QString &keyName)
{
    return waitForLegacy<CryptoError>([&](QObject *context, ErrorCallback callback) {
        removeAsync(context, keyName, std::move(callback));
    });
}

CryptoResult<QByteArray> SecureKeyStore::loadOrCreate(const QString &keyName)
{
    return waitForLegacy<CryptoResult<QByteArray>>([&](QObject *context, KeyCallback callback) {
        loadOrCreateAsync(context, keyName, std::move(callback));
    });
}

} // namespace AnyKeep
