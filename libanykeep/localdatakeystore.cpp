#include "localdatakeystore.h"

#include "secureenvelope.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>
#include <QThread>

#include <utility>

#include <qt6keychain/keychain.h>

namespace AnyKeep {
namespace {
    const QString KeychainService = QStringLiteral("com.github.ri0n.anykeep");
    const QString KeychainKey     = QStringLiteral("draft-store-master-key-v1");

    QString keychainError(QKeychain::Job *job)
    {
        return QCoreApplication::translate("LocalDataKeyStore", "Failed to access the secure keychain: %1")
            .arg(job->errorString());
    }

    struct PendingKeyCallback {
        QPointer<QObject> context;
        LocalDataKeyStore::AsyncKeyCallback callback;
    };

    struct KeyLoadState {
        bool                      inFlight = false;
        QByteArray                cachedKey;
        QList<PendingKeyCallback> pending;
    };

    KeyLoadState &keyLoadState()
    {
        static KeyLoadState state;
        return state;
    }
} // namespace

QByteArray LocalDataKeyStore::loadOrCreateMasterKey(QString *error)
{
    QString ignored;
    auto   &errorText = error ? *error : ignored;
    errorText.clear();
    if (keyLoadState().cachedKey.size() == MasterKeySize)
        return keyLoadState().cachedKey;
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
            keyLoadState().cachedKey = key;
        return key.size() == MasterKeySize ? key : QByteArray();
    }
    if (read.error() != QKeychain::EntryNotFound) {
        errorText = keychainError(&read);
        return {};
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
        errorText = keychainError(&write);
        return {};
    }
    keyLoadState().cachedKey = key;
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
    const auto deliver = [context = QPointer<QObject>(context), callback = std::move(callback)](
                             QByteArray key, QString error) mutable {
        if (!context)
            return;
        QMetaObject::invokeMethod(context, [callback = std::move(callback), key = std::move(key),
                                            error = std::move(error)]() mutable {
            callback(std::move(key), std::move(error));
        }, Qt::QueuedConnection);
    };
    if (state.cachedKey.size() == MasterKeySize) {
        deliver(state.cachedKey, {});
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
            state.cachedKey = key;
        auto pending = std::exchange(state.pending, {});
        state.inFlight = false;
        for (auto &waiter : pending) {
            if (waiter.context)
                waiter.callback(key, error);
        }
    };

    auto *read = new QKeychain::ReadPasswordJob(KeychainService, app);
    read->setAutoDelete(false);
    read->setInsecureFallback(false);
    read->setKey(KeychainKey);
    QObject::connect(read, &QKeychain::Job::finished, read, [read, app, finish] {
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
            const auto message = keychainError(read);
            read->deleteLater();
            finish({}, message);
            return;
        }
        read->deleteLater();
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
            const auto message = write->error() == QKeychain::NoError ? QString() : keychainError(write);
            write->deleteLater();
            finish(message.isEmpty() ? key : QByteArray(), message);
        });
        write->start();
    });
    read->start();
}

} // namespace AnyKeep
