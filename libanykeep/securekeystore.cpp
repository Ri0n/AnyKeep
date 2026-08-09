#include "securekeystore.h"

#include <QEventLoop>
#include <qt6keychain/keychain.h>

namespace AnyKeep {
namespace {
    const QString Service = QStringLiteral("com.github.ri0n.anykeep");

    void waitFor(QKeychain::Job &job)
    {
        QEventLoop loop;
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec();
    }
}

CryptoResult<QString> SecureKeyStore::readPassword(const QString &service, const QString &keyName)
{
    if (!QKeychain::isAvailable())
        return { {}, { CryptoError::Unavailable, QStringLiteral("No secure system keychain is available") } };
    QKeychain::ReadPasswordJob job(service);
    job.setAutoDelete(false);
    job.setInsecureFallback(false);
    job.setKey(keyName);
    waitFor(job);
    if (job.error() == QKeychain::EntryNotFound)
        return { {}, { CryptoError::InvalidArgument, QStringLiteral("Password was not found") } };
    if (job.error() != QKeychain::NoError)
        return { {}, { CryptoError::Unavailable, job.errorString() } };
    return { job.textData(), {} };
}

CryptoError SecureKeyStore::writePassword(const QString &service, const QString &keyName, const QString &password)
{
    if (!QKeychain::isAvailable())
        return { CryptoError::Unavailable, QStringLiteral("No secure system keychain is available") };
    if (service.isEmpty() || keyName.isEmpty())
        return { CryptoError::InvalidArgument, QStringLiteral("Password service and key must not be empty") };
    QKeychain::WritePasswordJob job(service);
    job.setAutoDelete(false);
    job.setInsecureFallback(false);
    job.setKey(keyName);
    job.setTextData(password);
    waitFor(job);
    return job.error() == QKeychain::NoError ? CryptoError {}
                                             : CryptoError { CryptoError::Unavailable, job.errorString() };
}

CryptoResult<QByteArray> SecureKeyStore::read(const QString &keyName)
{
    if (!QKeychain::isAvailable())
        return { {}, { CryptoError::Unavailable, QStringLiteral("No secure system keychain is available") } };
    QKeychain::ReadPasswordJob job(Service);
    job.setAutoDelete(false);
    job.setInsecureFallback(false);
    job.setKey(keyName);
    waitFor(job);
    if (job.error() == QKeychain::EntryNotFound)
        return { {}, { CryptoError::InvalidArgument, QStringLiteral("Encryption key was not found") } };
    if (job.error() != QKeychain::NoError)
        return { {}, { CryptoError::Unavailable, job.errorString() } };
    const auto key = job.binaryData();
    if (key.size() != SecureEnvelope::MasterKeySize)
        return { {}, { CryptoError::Corrupt, QStringLiteral("Stored encryption key has an invalid size") } };
    return { key, {} };
}

CryptoError SecureKeyStore::write(const QString &keyName, const QByteArray &key)
{
    if (key.size() != SecureEnvelope::MasterKeySize)
        return { CryptoError::InvalidArgument, QStringLiteral("Encryption key has an invalid size") };
    QKeychain::WritePasswordJob job(Service);
    job.setAutoDelete(false);
    job.setInsecureFallback(false);
    job.setKey(keyName);
    job.setBinaryData(key);
    waitFor(job);
    return job.error() == QKeychain::NoError ? CryptoError {}
                                             : CryptoError { CryptoError::Unavailable, job.errorString() };
}

CryptoResult<QByteArray> SecureKeyStore::loadOrCreate(const QString &keyName)
{
    auto existing = read(keyName);
    if (existing)
        return existing;
    if (existing.error.code != CryptoError::InvalidArgument)
        return existing;
    const auto key = SecureEnvelope::generateMasterKey();
    if (key.size() != SecureEnvelope::MasterKeySize)
        return { {}, { CryptoError::Unavailable, QStringLiteral("Could not generate a secure encryption key") } };
    if (auto error = write(keyName, key))
        return { {}, error };
    return { key, {} };
}

} // namespace AnyKeep
