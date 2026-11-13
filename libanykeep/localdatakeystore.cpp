#include "localdatakeystore.h"

#include "secureenvelope.h"
#include "utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QEventLoop>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSettings>
#include <QThread>

#include <utility>

#include <qt6keychain/keychain.h>

namespace AnyKeep {
namespace {
    const QString KeychainService = QStringLiteral("com.github.ri0n.anykeep");
    const QString KeychainKey     = QStringLiteral("draft-store-master-key-v1");

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
    // QtKeychain persists the encrypted value in QSettings, but Android does
    // not restore its hardware-backed private key along with a settings backup.
    // Repair only a genuinely empty local installation: an existing draft,
    // catalog, media blob or remote cache must never be orphaned silently.
    bool canReplaceMissingAndroidKey(QKeychain::Job *job)
    {
        if (job->error() != QKeychain::AccessDenied
            || !job->errorString().contains(QStringLiteral("retrieve private key from keystore"),
                                            Qt::CaseInsensitive))
            return false;

        const QFileInfo root(Utils::anykeepDataDir());
        if (!root.isDir() || !root.isReadable())
            return false;
        QDirIterator files(root.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                           QDirIterator::Subdirectories);
        const QDir dataDir(root.absoluteFilePath());
        // Some Qt/Android configurations place QSettings inside AppDataLocation
        // itself. The stale QtKeychain settings file is not encrypted user data
        // that could be recovered by preserving the missing Keystore key.
        const QString keychainSettingsPath = QSettings(KeychainService).fileName();
        const QString appSettingsPath = QSettings().fileName();
        while (files.hasNext()) {
            const auto path = files.next();
            const auto relativePath = dataDir.relativeFilePath(path);
            // Plain PTF notes and their metadata do not depend on the key.
            if (relativePath.startsWith(QStringLiteral("ptf/")) || path == keychainSettingsPath
                || path == appSettingsPath)
                continue;
            return false;
        }
        return true;
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
} // namespace

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
    if (read.error() != QKeychain::EntryNotFound) {
#ifdef Q_OS_ANDROID
        if (!canReplaceMissingAndroidKey(&read)) {
            errorText = keychainError(&read, "read");
            return {};
        }
        // Do not change an existing key on an ordinary access failure. This
        // narrowly handles a backed-up QSettings entry with no Keystore key.
        QKeychain::DeletePasswordJob remove(KeychainService);
        remove.setAutoDelete(false);
        remove.setInsecureFallback(false);
        remove.setKey(KeychainKey);
        QObject::connect(&remove, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        remove.start();
        loop.exec();
        if (remove.error() != QKeychain::NoError) {
            errorText = keychainError(&remove, "repair");
            return {};
        }
#else
        errorText = keychainError(&read, "read");
        return {};
#endif
    }

    const auto key = SecureEnvelope::generateMasterKey();
    if (key.size() != MasterKeySize) {
        errorText = QCoreApplication::translate("LocalDataKeyStore", "A secure local data key could not be generated.");
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
        return {};
    }
    cacheMasterKey(key);
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

    const auto createAndStore = [app, finish] {
        const auto key = SecureEnvelope::generateMasterKey();
        if (key.size() != MasterKeySize) {
            finish({}, QCoreApplication::translate("LocalDataKeyStore", "A secure local data key could not be generated."));
            return;
        }
        auto *write = new QKeychain::WritePasswordJob(KeychainService, app);
        write->setAutoDelete(false);
        write->setInsecureFallback(false);
        write->setKey(KeychainKey);
        write->setBinaryData(key);
        QObject::connect(write, &QKeychain::Job::finished, write, [write, key, finish] {
            const auto message = write->error() == QKeychain::NoError ? QString() : keychainError(write, "write");
            write->deleteLater();
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
            if (canReplaceMissingAndroidKey(read)) {
                read->deleteLater();
                auto *remove = new QKeychain::DeletePasswordJob(KeychainService, app);
                remove->setAutoDelete(false);
                remove->setInsecureFallback(false);
                remove->setKey(KeychainKey);
                QObject::connect(remove, &QKeychain::Job::finished, remove,
                                 [remove, finish, createAndStore] {
                    const auto message = remove->error() == QKeychain::NoError ? QString() : keychainError(remove, "repair");
                    remove->deleteLater();
                    if (!message.isEmpty()) {
                        finish({}, message);
                        return;
                    }
                    createAndStore();
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
        createAndStore();
    });
    read->start();
}

} // namespace AnyKeep
