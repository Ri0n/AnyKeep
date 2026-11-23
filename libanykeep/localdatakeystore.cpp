#include "localdatakeystore.h"
#include "localkeyrecoverypolicy.h"
#include "keychainservice.h"

#include "secureenvelope.h"
#include "utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QDebug>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>

#include <utility>


namespace AnyKeep {
namespace {
    const QString LocalKeychainService = QStringLiteral("com.github.ri0n.anykeep");
    const QString KeychainKey     = QStringLiteral("draft-store-master-key-v1");
    QString keychainError(const KeychainService::Result &result, const char *operation = nullptr)
    {
#ifdef Q_OS_ANDROID
        qWarning().noquote() << "Android local keychain" << (operation ? operation : "unknown") << ':' << result.error;
        return QCoreApplication::translate("LocalDataKeyStore",
                                           "Local note saving is unavailable. Previously saved notes have not been changed.");
#else
        Q_UNUSED(operation);
        return QCoreApplication::translate("LocalDataKeyStore", "Failed to access the secure keychain: %1")
            .arg(result.error);
#endif
    }

#ifdef Q_OS_ANDROID
        // Android users should never have to decipher Keystore internals.
        // Keep the operation phase and technical error in logcat for diagnosis.
        qWarning().noquote() << "Android local keychain" << (operation ? operation : "unknown") << ':'
                             << job->errorString();
        return QCoreApplication::translate("LocalDataKeyStore",
                                           "Local note saving is unavailable. Previously saved notes have not been changed.");
#else
        Q_UNUSED(operation);
        return QCoreApplication::translate("LocalDataKeyStore", "Failed to access the secure keychain: %1")
            .arg(job->errorString());
#endif
    }

#ifdef Q_OS_ANDROID
    // A QtKeychain settings entry was restored, but its AndroidKeyStore
    // private key is missing. This is not a transient decryption error.
    bool isMissingAndroidPrivateKey(const KeychainService::Result &result)
    {
        return result.status == KeychainService::Status::AccessDenied
            && result.error.contains(QStringLiteral("retrieve private key from keystore"), Qt::CaseInsensitive);
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
        bool                      inaccessiblePreviousContent = false;
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
    void noteDiscardedLocalContent(const QString &preservedPath)
    {
        if (preservedPath.isEmpty())
            return;
        QDirIterator lostContent(preservedPath,
                                 { QStringLiteral("*.draft"), QStringLiteral("*.blob"), QStringLiteral("*.source") },
                                 QDir::Files, QDirIterator::Subdirectories);
        if (!lostContent.hasNext())
            return;
        auto &state = keyLoadState();
        const QMutexLocker locker(&state.cacheMutex);
        state.inaccessiblePreviousContent = true;
    }
#endif
} // namespace

bool LocalDataKeyStore::takeInaccessiblePreviousContentNotice()
{
#ifdef Q_OS_ANDROID
    auto &state = keyLoadState();
    const QMutexLocker locker(&state.cacheMutex);
    const bool result = state.inaccessiblePreviousContent;
    state.inaccessiblePreviousContent = false;
    return result;
#else
    return false;
#endif
}

QByteArray LocalDataKeyStore::loadOrCreateMasterKey(QString *error)
{
    QString ignored;
    auto &errorText = error ? *error : ignored;
    errorText.clear();
    const auto cached = cachedMasterKey();
    if (cached.size() == MasterKeySize)
        return cached;
    auto *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread()) {
        errorText = QCoreApplication::translate(
            "LocalDataKeyStore", "The local encryption key must be initialized on the application thread.");
        return {};
    }

    // Transitional startup compatibility. Both sync and async consumers join
    // the same in-flight read/create operation; no competing master keys.
    QEventLoop loop;
    QByteArray key;
    loadOrCreateMasterKeyAsync(&loop, [&](QByteArray resolved, QString failure) {
        key = std::move(resolved);
        errorText = std::move(failure);
        loop.quit();
    });
    loop.exec();
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
    auto deliver = [context = QPointer<QObject>(context), callback = std::move(callback)](
                       QByteArray key, QString error) mutable {
        if (!context)
            return;
        QMetaObject::invokeMethod(
            context,
            [context, callback = std::move(callback), key = std::move(key), error = std::move(error)]() mutable {
                if (context)
                    callback(std::move(key), std::move(error));
            },
            Qt::QueuedConnection);
    };
    const auto cached = cachedMasterKey();
    if (cached.size() == MasterKeySize) {
        deliver(cached, {});
        return;
    }
    if (!app) {
        deliver({}, QCoreApplication::translate("LocalDataKeyStore", "No secure system keychain is available."));
        return;
    }

    auto &state = keyLoadState();
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

    const auto createAndStore = [app, finish](const QString &preservedPath) {
        const auto key = SecureEnvelope::generateMasterKey();
        if (key.size() != MasterKeySize) {
            auto message = QCoreApplication::translate(
                "LocalDataKeyStore", "A secure local data key could not be generated.");
#ifdef Q_OS_ANDROID
            message += restoreAfterFailedRecovery(preservedPath);
#endif
            finish({}, message);
            return;
        }
        KeychainService::writeBinary(
            app, LocalKeychainService, KeychainKey, key,
            [key, finish, preservedPath](KeychainService::Result result) {
                auto message = result ? QString() : keychainError(result, "write");
#ifdef Q_OS_ANDROID
                if (message.isEmpty()) {
                    noteDiscardedLocalContent(preservedPath);
                    if (!preservedPath.isEmpty() && !LocalKeyRecoveryPolicy::discardStagedFiles(preservedPath))
                        qWarning() << "Failed to clean up temporary unreadable Android encryption state";
                } else {
                    message += restoreAfterFailedRecovery(preservedPath);
                }
#else
                Q_UNUSED(preservedPath);
#endif
                finish(message.isEmpty() ? key : QByteArray(), std::move(message));
            });
    };

    KeychainService::read(
        app, LocalKeychainService, KeychainKey,
        [app, finish, createAndStore](KeychainService::Result result) {
            if (result.status == KeychainService::Status::Ok) {
                auto key = std::move(result.binary);
                if (key.size() != MasterKeySize) {
                    finish({}, QCoreApplication::translate(
                                   "LocalDataKeyStore", "The local data encryption key is invalid."));
                    return;
                }
                finish(std::move(key), {});
                return;
            }
            if (result.status != KeychainService::Status::NotFound) {
#ifdef Q_OS_ANDROID
                if (isMissingAndroidPrivateKey(result)) {
                    QString preservedPath;
                    QString recoveryError;
                    if (!preserveInaccessibleAndroidData(&preservedPath, &recoveryError)) {
                        finish({}, recoveryError);
                        return;
                    }
                    KeychainService::remove(
                        app, LocalKeychainService, KeychainKey,
                        [finish, createAndStore, preservedPath](KeychainService::Result removed) {
                            if (!removed) {
                                finish({}, keychainError(removed, "repair")
                                               + restoreAfterFailedRecovery(preservedPath));
                                return;
                            }
                            createAndStore(preservedPath);
                        });
                    return;
                }
#endif
                finish({}, keychainError(result, "read"));
                return;
            }
#ifdef Q_OS_ANDROID
            QString category;
            if (LocalKeyRecoveryPolicy::containsProtectedFiles(Utils::anykeepDataDir(), &category)) {
                QString preservedPath, recoveryError;
                if (!preserveInaccessibleAndroidData(&preservedPath, &recoveryError)) {
                    finish({}, recoveryError);
                    return;
                }
                createAndStore(preservedPath);
                return;
            }
#endif
            createAndStore({});
        });
}

} // namespace AnyKeep
