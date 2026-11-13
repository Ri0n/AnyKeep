#include "localdatakeystore.h"
#include "localkeyrecoverypolicy.h"

#include "secureenvelope.h"
#include "utils.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QEventLoop>
#include <QDebug>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSettings>
#include <QUuid>
#include <QThread>

#include <utility>

#include <qt6keychain/keychain.h>

namespace AnyKeep {
namespace {
    const QString KeychainService = QStringLiteral("com.github.ri0n.anykeep");
    const QString KeychainKey     = QStringLiteral("draft-store-master-key-v1");
#ifdef Q_OS_ANDROID
    const QString ActiveKeychainAliasSetting = QStringLiteral("security.local-data-keychain-alias");
#endif

    QString activeKeychainAlias()
    {
#ifdef Q_OS_ANDROID
        const auto configured = QSettings().value(ActiveKeychainAliasSetting).toString();
        return configured.startsWith(QStringLiteral("draft-store-master-key-v2:")) ? configured : KeychainKey;
#else
        return KeychainKey;
#endif
    }

#ifdef Q_OS_ANDROID
    QString nextAndroidKeychainAlias()
    {
        // Rotate instead of deleting the original alias. Even if Android
        // later recovers the old private key, the old QtKeychain entry remains
        // available for manual recovery of the quarantined ciphertext.
        return QStringLiteral("draft-store-master-key-v2:")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    bool storeActiveAndroidAlias(const QString &alias, QString *error)
    {
        QSettings settings;
        settings.setValue(ActiveKeychainAliasSetting, alias);
        settings.sync();
        if (settings.status() == QSettings::NoError)
            return true;
        if (error)
            *error = QCoreApplication::translate("LocalDataKeyStore",
                                                  "The new local encryption key could not be activated.");
        return false;
    }
#endif

    QString keychainError(QKeychain::Job *job, const char *operation = nullptr)
    {
        const auto message = QCoreApplication::translate("LocalDataKeyStore", "Failed to access the secure keychain: %1")
                                 .arg(job->errorString());
#ifdef Q_OS_ANDROID
        // The old QtKeychain Android implementation reports the same error
        // for a stale read and for failure to fetch a newly generated RSA key.
        // Keep the phase visible to distinguish the two on physical devices.
        if (operation)
            return message + QStringLiteral(" [") + QString::fromLatin1(operation) + QLatin1Char(']');
#else
        Q_UNUSED(operation);
#endif
        return message;
    }

#ifdef Q_OS_ANDROID
    // A QtKeychain settings entry was restored, but its AndroidKeyStore
    // private key is missing. This is not a transient decryption error.
    bool isMissingAndroidPrivateKey(const QKeychain::Job *job)
    {
        return job->error() == QKeychain::AccessDenied
            && job->errorString().contains(QStringLiteral("retrieve private key from keystore"),
                                           Qt::CaseInsensitive);
    }

    bool preserveInaccessibleAndroidData(QString *preservedPath, QString *error)
    {
        return LocalKeyRecoveryPolicy::quarantineProtectedFiles(Utils::anykeepDataDir(), preservedPath, error);
    }

    QString restoreAfterFailedRecovery(const QString &preservedPath)
    {
        if (preservedPath.isEmpty())
            return {};
        if (LocalKeyRecoveryPolicy::restoreQuarantinedFiles(Utils::anykeepDataDir(), preservedPath))
            return {};
        return QCoreApplication::translate("LocalDataKeyStore",
                                           " The backup of previous local data could not be restored.");
    }

#endif

    struct PendingKeyCallback {
        QPointer<QObject> context;
        LocalDataKeyStore::AsyncKeyCallback callback;
    };

    struct KeyLoadState {
        bool                      inFlight = false;
        QMutex                    cacheMutex;
        QByteArray                cachedKey;
        QList<PendingKeyCallback> pending;
        bool                      inaccessiblePreviousDrafts = false;
    };

    KeyLoadState &keyLoadState()
    {
        static KeyLoadState state;
        return state;
    }

    QByteArray cachedMasterKey()
    {
        auto &state = keyLoadState();
        const QMutexLocker locker(&state.cacheMutex);
        return state.cachedKey;
    }

    void cacheMasterKey(const QByteArray &key)
    {
        auto &state = keyLoadState();
        const QMutexLocker locker(&state.cacheMutex);
        state.cachedKey = key;
    }
#ifdef Q_OS_ANDROID
    void notePreservedDrafts(const QString &preservedPath)
    {
        if (preservedPath.isEmpty())
            return;
        QDirIterator drafts(preservedPath, { QStringLiteral("*.draft") }, QDir::Files,
                            QDirIterator::Subdirectories);
        if (!drafts.hasNext())
            return;
        auto &state = keyLoadState();
        const QMutexLocker locker(&state.cacheMutex);
        state.inaccessiblePreviousDrafts = true;
    }
#endif
} // namespace

bool LocalDataKeyStore::takeInaccessiblePreviousDraftsNotice()
{
#ifdef Q_OS_ANDROID
    auto &state = keyLoadState();
    const QMutexLocker locker(&state.cacheMutex);
    const bool result = state.inaccessiblePreviousDrafts;
    state.inaccessiblePreviousDrafts = false;
    return result;
#else
    return false;
#endif
}

QByteArray LocalDataKeyStore::loadOrCreateMasterKey(QString *error)
{
    QString ignored;
    auto   &errorText = error ? *error : ignored;
    errorText.clear();
    const auto cached = cachedMasterKey();
    if (cached.size() == MasterKeySize)
        return cached;
    if (!QKeychain::isAvailable()) {
        errorText = QCoreApplication::translate("LocalDataKeyStore", "No secure system keychain is available.");
        return {};
    }

    const auto oldAlias = activeKeychainAlias();
    QKeychain::ReadPasswordJob read(KeychainService);
    read.setAutoDelete(false);
    read.setInsecureFallback(false);
    read.setKey(oldAlias);
    QEventLoop loop;
    QObject::connect(&read, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    read.start();
    loop.exec();
    if (read.error() == QKeychain::NoError) {
        const auto key = read.binaryData();
        if (key.size() != MasterKeySize)
            errorText = QCoreApplication::translate("LocalDataKeyStore", "The local data encryption key is invalid.");
        if (key.size() == MasterKeySize)
            cacheMasterKey(key);
        return key.size() == MasterKeySize ? key : QByteArray();
    }
    QString preservedPath;
    if (read.error() != QKeychain::EntryNotFound) {
#ifdef Q_OS_ANDROID
        if (!isMissingAndroidPrivateKey(&read)) {
            errorText = keychainError(&read, "read");
            return {};
        }
        if (!preserveInaccessibleAndroidData(&preservedPath, &errorText))
            return {};
        // The old alias and its AndroidKeyStore key are left untouched.
        // Only the new editor state will use the replacement alias.
#else
        errorText = keychainError(&read, "read");
        return {};
#endif
    }

    const auto key = SecureEnvelope::generateMasterKey();
    if (key.size() != MasterKeySize) {
        errorText = QCoreApplication::translate("LocalDataKeyStore", "A secure local data key could not be generated.");
#ifdef Q_OS_ANDROID
        errorText += restoreAfterFailedRecovery(preservedPath);
#endif
        return {};
    }
    QKeychain::WritePasswordJob write(KeychainService);
    write.setAutoDelete(false);
    write.setInsecureFallback(false);
#ifdef Q_OS_ANDROID
    const auto nextAlias = read.error() == QKeychain::EntryNotFound ? oldAlias : nextAndroidKeychainAlias();
#else
    const auto nextAlias = oldAlias;
#endif
    write.setKey(nextAlias);
    write.setBinaryData(key);
    QObject::connect(&write, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    write.start();
    loop.exec();
    if (write.error() != QKeychain::NoError) {
        errorText = keychainError(&write, "write");
#ifdef Q_OS_ANDROID
        errorText += restoreAfterFailedRecovery(preservedPath);
#endif
        return {};
    }
#ifdef Q_OS_ANDROID
    if (nextAlias != oldAlias && !storeActiveAndroidAlias(nextAlias, &errorText)) {
        errorText += restoreAfterFailedRecovery(preservedPath);
        return {};
    }
#endif
    cacheMasterKey(key);
#ifdef Q_OS_ANDROID
    notePreservedDrafts(preservedPath);
#endif
    return key;
}


void LocalDataKeyStore::loadOrCreateMasterKeyAsync(QObject *context, AsyncKeyCallback callback)
{
    if (!context || !callback)
        return;
    auto *app = QCoreApplication::instance();
    if (app && QThread::currentThread() != app->thread()) {
        const QPointer<QObject> guard(context);
        QMetaObject::invokeMethod(app, [guard, callback = std::move(callback)]() mutable {
            if (guard)
                loadOrCreateMasterKeyAsync(guard, std::move(callback));
        }, Qt::QueuedConnection);
        return;
    }

    auto &state = keyLoadState();
    auto deliver = [context = QPointer<QObject>(context), callback = std::move(callback)](
                             QByteArray key, QString error) mutable {
        if (!context)
            return;
        QMetaObject::invokeMethod(context, [callback = std::move(callback), key = std::move(key),
                                            error = std::move(error)]() mutable {
            callback(std::move(key), std::move(error));
        }, Qt::QueuedConnection);
    };
    const auto cached = cachedMasterKey();
    if (cached.size() == MasterKeySize) {
        deliver(cached, {});
        return;
    }
    if (!app || !QKeychain::isAvailable()) {
        deliver({}, QCoreApplication::translate("LocalDataKeyStore", "No secure system keychain is available."));
        return;
    }
    state.pending.append({ context, std::move(deliver) });
    if (state.inFlight)
        return;
    state.inFlight = true;

    const auto finish = [](QByteArray key, QString error) {
        auto &state = keyLoadState();
        if (key.size() == MasterKeySize)
            cacheMasterKey(key);
        auto pending = std::exchange(state.pending, {});
        state.inFlight = false;
        for (auto &waiter : pending) {
            if (waiter.context)
                waiter.callback(key, error);
        }
    };

    const auto oldAlias = activeKeychainAlias();
    const auto createAndStore = [app, finish, oldAlias](const QString &preservedPath, const QString &nextAlias) {
        const auto key = SecureEnvelope::generateMasterKey();
        if (key.size() != MasterKeySize) {
            auto message = QCoreApplication::translate("LocalDataKeyStore", "A secure local data key could not be generated.");
#ifdef Q_OS_ANDROID
            message += restoreAfterFailedRecovery(preservedPath);
#endif
            finish({}, message);
            return;
        }
        auto *write = new QKeychain::WritePasswordJob(KeychainService, app);
        write->setAutoDelete(false);
        write->setInsecureFallback(false);
        write->setKey(nextAlias);
        write->setBinaryData(key);
        QObject::connect(write, &QKeychain::Job::finished, write,
                         [write, key, finish, preservedPath, nextAlias, oldAlias] {
            auto message = write->error() == QKeychain::NoError ? QString() : keychainError(write, "write");
            write->deleteLater();
#ifdef Q_OS_ANDROID
            if (message.isEmpty() && nextAlias != oldAlias)
                storeActiveAndroidAlias(nextAlias, &message);
            if (message.isEmpty())
                notePreservedDrafts(preservedPath);
            else
                message += restoreAfterFailedRecovery(preservedPath);
#else
            Q_UNUSED(preservedPath);
            Q_UNUSED(oldAlias);
#endif
            finish(message.isEmpty() ? key : QByteArray(), message);
        });
        write->start();
    };

    auto *read = new QKeychain::ReadPasswordJob(KeychainService, app);
    read->setAutoDelete(false);
    read->setInsecureFallback(false);
    read->setKey(oldAlias);
    QObject::connect(read, &QKeychain::Job::finished, read, [read, finish, createAndStore, oldAlias] {
        const auto error = read->error();
        if (error == QKeychain::NoError) {
            auto key = read->binaryData();
            read->deleteLater();
            if (key.size() != MasterKeySize) {
                finish({}, QCoreApplication::translate("LocalDataKeyStore", "The local data encryption key is invalid."));
                return;
            }
            finish(std::move(key), {});
            return;
        }
        if (error != QKeychain::EntryNotFound) {
#ifdef Q_OS_ANDROID
            if (isMissingAndroidPrivateKey(read)) {
                QString preservedPath;
                QString recoveryError;
                if (!preserveInaccessibleAndroidData(&preservedPath, &recoveryError)) {
                    read->deleteLater();
                    finish({}, recoveryError);
                    return;
                }
                read->deleteLater();
                createAndStore(preservedPath, nextAndroidKeychainAlias());
                return;
            }
#endif
            const auto message = keychainError(read, "read");
            read->deleteLater();
            finish({}, message);
            return;
        }
        read->deleteLater();
        createAndStore({}, oldAlias);
    });
    read->start();
}

} // namespace AnyKeep
