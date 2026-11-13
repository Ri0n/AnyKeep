#include "localdatakeystore.h"
#include "localkeyrecoverypolicy.h"

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

#include <qt6keychain/keychain.h>

namespace AnyKeep {
namespace {
    const QString KeychainService = QStringLiteral("com.github.ri0n.anykeep");
    const QString KeychainKey     = QStringLiteral("draft-store-master-key-v1");
    QString keychainError(QKeychain::Job *job, const char *operation = nullptr)
    {
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
    auto   &errorText = error ? *error : ignored;
    errorText.clear();
    const auto cached = cachedMasterKey();
    if (cached.size() == MasterKeySize)
        return cached;
    if (!QKeychain::isAvailable()) {
        errorText = QCoreApplication::translate("LocalDataKeyStore", "No secure system keychain is available.");
        return {};
    }

    QKeychain::ReadPasswordJob read(KeychainService);
    read.setAutoDelete(false);
    read.setInsecureFallback(false);
    read.setKey(KeychainKey);
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
        // This entry cannot decrypt the old ciphertext. Remove its stale
        // QtKeychain record before storing a new key under the same alias.
        QKeychain::DeletePasswordJob remove(KeychainService);
        remove.setAutoDelete(false);
        remove.setInsecureFallback(false);
        remove.setKey(KeychainKey);
        QObject::connect(&remove, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        remove.start();
        loop.exec();
        if (remove.error() != QKeychain::NoError) {
            errorText = keychainError(&remove, "repair") + restoreAfterFailedRecovery(preservedPath);
            return {};
        }
#else
        errorText = keychainError(&read, "read");
        return {};
#endif
    }
#ifdef Q_OS_ANDROID
    if (read.error() == QKeychain::EntryNotFound) {
        QString category;
        if (LocalKeyRecoveryPolicy::containsProtectedFiles(Utils::anykeepDataDir(), &category)
            && !preserveInaccessibleAndroidData(&preservedPath, &errorText))
            return {};
    }
#endif

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
    write.setKey(KeychainKey);
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
    cacheMasterKey(key);
#ifdef Q_OS_ANDROID
    noteDiscardedLocalContent(preservedPath);
    if (!preservedPath.isEmpty() && !LocalKeyRecoveryPolicy::discardStagedFiles(preservedPath))
        qWarning() << "Failed to clean up temporary unreadable Android encryption state";
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

    const auto createAndStore = [app, finish](const QString &preservedPath) {
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
        write->setKey(KeychainKey);
        write->setBinaryData(key);
        QObject::connect(write, &QKeychain::Job::finished, write,
                         [write, key, finish, preservedPath] {
            auto message = write->error() == QKeychain::NoError ? QString() : keychainError(write, "write");
            write->deleteLater();
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
            finish(message.isEmpty() ? key : QByteArray(), message);
        });
        write->start();
    };

    auto *read = new QKeychain::ReadPasswordJob(KeychainService, app);
    read->setAutoDelete(false);
    read->setInsecureFallback(false);
    read->setKey(KeychainKey);
    QObject::connect(read, &QKeychain::Job::finished, read, [read, app, finish, createAndStore] {
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
                auto *remove = new QKeychain::DeletePasswordJob(KeychainService, app);
                remove->setAutoDelete(false);
                remove->setInsecureFallback(false);
                remove->setKey(KeychainKey);
                QObject::connect(remove, &QKeychain::Job::finished, remove,
                                 [remove, finish, createAndStore, preservedPath] {
                    auto message = remove->error() == QKeychain::NoError
                        ? QString() : keychainError(remove, "repair");
                    remove->deleteLater();
                    if (!message.isEmpty()) {
                        message += restoreAfterFailedRecovery(preservedPath);
                        finish({}, message);
                        return;
                    }
                    createAndStore(preservedPath);
                });
                remove->start();
                return;
            }
#endif
            const auto message = keychainError(read, "read");
            read->deleteLater();
            finish({}, message);
            return;
        }
        read->deleteLater();
#ifdef Q_OS_ANDROID
        QString category;
        if (LocalKeyRecoveryPolicy::containsProtectedFiles(Utils::anykeepDataDir(), &category)) {
            QString preservedPath, error;
            if (!preserveInaccessibleAndroidData(&preservedPath, &error)) {
                finish({}, error);
                return;
            }
            createAndStore(preservedPath);
            return;
        }
#endif
        createAndStore({});
    });
    read->start();
}

} // namespace AnyKeep
