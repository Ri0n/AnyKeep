#include "localdatakeystore.h"

#include "secureenvelope.h"

#include <QCoreApplication>
#include <QEventLoop>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <qt5keychain/keychain.h>
#else
#include <qt6keychain/keychain.h>
#endif

namespace AnyKeep {
namespace {
    const QString KeychainService = QStringLiteral("com.github.ri0n.anykeep");
    const QString KeychainKey     = QStringLiteral("draft-store-master-key-v1");

    QString keychainError(QKeychain::Job *job)
    {
        return QCoreApplication::translate("LocalDataKeyStore", "Failed to access the secure keychain: %1")
            .arg(job->errorString());
    }
} // namespace

QByteArray LocalDataKeyStore::loadOrCreateMasterKey(QString *error)
{
    QString ignored;
    auto   &errorText = error ? *error : ignored;
    errorText.clear();
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
    return key;
}

} // namespace AnyKeep
