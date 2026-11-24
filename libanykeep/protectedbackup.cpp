#include "protectedbackup.h"

#include "secureenvelope.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPasswordDigestor>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

namespace AnyKeep {
namespace {
    constexpr quint32 FileMagic = 0x414b5042; // AKPB
    constexpr quint16 Version = 1;
    constexpr qint64 ChunkSize = 1024 * 1024;
    constexpr int Iterations = 600000;
    constexpr qint64 MaxHeaderBytes = 4096;
    const QString HeaderName = QStringLiteral("header.json");
    const QString ManifestName = QStringLiteral("manifest.enc");
    const QString PlainManifestName = QStringLiteral("manifest.json");

    PortableBackupInspection failed(const QString &message)
    {
        PortableBackupInspection result;
        result.error = message;
        return result;
    }

    bool running(const PortableBackup::Continue &keepGoing) { return !keepGoing || keepGoing(); }

    bool regularFile(const QFileInfo &info)
    {
        if (!info.isFile() || info.isSymLink() || !info.isReadable())
            return false;
#ifdef Q_OS_WIN
        return info.canonicalFilePath().compare(info.absoluteFilePath(), Qt::CaseInsensitive) == 0;
#else
        return info.canonicalFilePath() == info.absoluteFilePath();
#endif
    }

    QString opaqueFileName(qsizetype index)
    {
        return QStringLiteral("file-%1.enc").arg(index, 8, 10, QLatin1Char('0'));
    }

    QByteArray passwordKey(const QString &password, const QByteArray &salt)
    {
        if (password.isEmpty() || salt.size() != SecureEnvelope::MasterKeySize)
            return {};
        // Qt implements RFC 8018; no additional crypto provider is required.
        // Version 1 fixes the cost, so hostile input cannot request an
        // unbounded derivation or downgrade the password protection.
        return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(), salt,
                                                  Iterations, SecureEnvelope::MasterKeySize);
    }

    QByteArray makeHeader(const QByteArray &salt)
    {
        return QJsonDocument(QJsonObject {
            { QStringLiteral("format"), QStringLiteral("anykeep-protected-ptf") },
            { QStringLiteral("version"), Version },
            { QStringLiteral("kdf"), QStringLiteral("pbkdf2-hmac-sha256") },
            { QStringLiteral("iterations"), Iterations },
            { QStringLiteral("salt"), QString::fromLatin1(salt.toHex()) }
        }).toJson(QJsonDocument::Compact);
    }

    QByteArray headerSalt(const QByteArray &bytes)
    {
        const auto doc = QJsonDocument::fromJson(bytes);
        if (!doc.isObject())
            return {};
        const auto header = doc.object();
        if (header.value(QStringLiteral("format")).toString() != QStringLiteral("anykeep-protected-ptf")
            || header.value(QStringLiteral("version")).toInt(-1) != Version
            || header.value(QStringLiteral("kdf")).toString() != QStringLiteral("pbkdf2-hmac-sha256")
            || header.value(QStringLiteral("iterations")).toInt(-1) != Iterations)
            return {};
        const auto hex = header.value(QStringLiteral("salt")).toString().toLatin1();
        const auto salt = QByteArray::fromHex(hex);
        if (salt.size() != SecureEnvelope::MasterKeySize || salt.toHex() != hex)
            return {};
        return salt;
    }

    QString headerIdentity(const QByteArray &header)
    {
        return QString::fromLatin1(QCryptographicHash::hash(header, QCryptographicHash::Sha256).toHex());
    }

    QByteArray chunkContext(const QString &container, const QString &fileId, qint64 size, quint64 index)
    {
        return SecureEnvelope::associatedData({ KeyDomain::PortableBackup, container, fileId, Version,
                                               QStringLiteral("%1:%2").arg(size).arg(index) });
    }

    bool writeBytes(QIODevice &device, const QByteArray &bytes)
    {
        return device.write(bytes) == bytes.size();
    }

    bool writePrivateFile(const QString &path, const QByteArray &bytes)
    {
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        return output.open(QIODevice::WriteOnly)
            && output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            && writeBytes(output, bytes) && output.commit();
    }

    QByteArray readBounded(const QString &path, qint64 maximum)
    {
        const QFileInfo info(path);
        if (!regularFile(info) || info.size() > maximum)
            return {};
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const auto bytes = file.read(maximum + 1);
        if (bytes.size() > maximum || !file.atEnd() || file.error() != QFileDevice::NoError)
            return {};
        return bytes;
    }

    // The outer file header contains only the public length. Every chunk
    // authenticates the header identity, file identity, total size and index.
    // Even an empty file has one authenticated chunk. The fixed record sizes
    // avoid deserializing attacker-controlled allocation lengths.
    QString encryptFile(QIODevice &input, const PortableBackupFileInfo &entry, const QString &target,
                        const QByteArray &key, const QString &container, const QString &fileId,
                        const PortableBackup::Continue &keepGoing)
    {
        QSaveFile output(target);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly))
            return output.errorString();
        if (!output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            return output.errorString();
        QDataStream stream(&output);
        stream.setVersion(QDataStream::Qt_5_10);
        stream << FileMagic << Version << entry.size;
        if (stream.status() != QDataStream::Ok)
            return QStringLiteral("Could not write protected file header");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 offset = 0;
        quint64 index = 0;
        do {
            if (!running(keepGoing))
                return QStringLiteral("Backup protection cancelled");
            const auto count = qMin(ChunkSize, entry.size - offset);
            const auto plain = input.read(count);
            if (plain.size() != count)
                return QStringLiteral("The portable file changed while protecting it");
            hash.addData(plain);
            const auto context = chunkContext(container, fileId, entry.size, index++);
            const auto sealed = SecureEnvelope::encryptAead(context + plain, key, KeyDomain::PortableBackup);
            if (!sealed)
                return sealed.error.message;
            if (!writeBytes(output, sealed.value.nonce) || !writeBytes(output, sealed.value.tag)
                || !writeBytes(output, sealed.value.cipherText))
                return QStringLiteral("Could not write protected file");
            offset += count;
        } while (offset < entry.size);
        if (!input.atEnd() || hash.result() != entry.sha256)
            return QStringLiteral("The portable file changed while protecting it");
        return output.commit() ? QString() : output.errorString();
    }

    // Authenticate every chunk before writing it. The sink is either a private
    // QSaveFile, a bounded manifest buffer, or nullptr for readback verification.
    QString decryptFile(const QString &path, QIODevice *sink, qint64 expectedSize, qint64 maximumSize,
                        const QByteArray &expectedHash, const QByteArray &key, const QString &container,
                        const QString &fileId, const PortableBackup::Continue &keepGoing)
    {
        if (!regularFile(QFileInfo(path)))
            return QStringLiteral("A protected file is missing or unsafe");
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly))
            return input.errorString();
        quint32 magic = 0;
        quint16 version = 0;
        qint64 size = -1;
        QDataStream stream(&input);
        stream.setVersion(QDataStream::Qt_5_10);
        stream >> magic >> version >> size;
        if (stream.status() != QDataStream::Ok || magic != FileMagic || version != Version
            || size < 0 || size > maximumSize || (expectedSize >= 0 && size != expectedSize))
            return QStringLiteral("Invalid protected file header");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 offset = 0;
        quint64 index = 0;
        do {
            if (!running(keepGoing))
                return QStringLiteral("Backup protection cancelled");
            const auto count = qMin(ChunkSize, size - offset);
            const auto context = chunkContext(container, fileId, size, index++);
            AeadCiphertext encrypted;
            encrypted.nonce = input.read(SecureEnvelope::AeadNonceSize);
            encrypted.tag = input.read(SecureEnvelope::AeadTagSize);
            encrypted.cipherText = input.read(count + context.size());
            if (encrypted.nonce.size() != SecureEnvelope::AeadNonceSize
                || encrypted.tag.size() != SecureEnvelope::AeadTagSize
                || encrypted.cipherText.size() != count + context.size())
                return QStringLiteral("Truncated protected file");
            const auto opened = SecureEnvelope::decryptAead(encrypted, key, KeyDomain::PortableBackup);
            if (!opened)
                return opened.error.message;
            if (opened.value.size() != count + context.size() || !opened.value.startsWith(context))
                return QStringLiteral("Protected file identity mismatch");
            const auto plain = opened.value.mid(context.size());
            hash.addData(plain);
            if (sink && !writeBytes(*sink, plain))
                return QStringLiteral("Could not write authenticated portable file");
            offset += count;
        } while (offset < size);
        if (!input.atEnd() || input.error() != QFileDevice::NoError
            || (!expectedHash.isEmpty() && hash.result() != expectedHash))
            return QStringLiteral("Protected file integrity mismatch");
        return {};
    }

    bool newDirectory(const QFileInfo &target)
    {
        return !target.exists() && !target.isSymLink() && !target.fileName().isEmpty() && target.dir().exists();
    }

    bool publish(QTemporaryDir &staging, const QFileInfo &target, const PortableBackup::Continue &keepGoing,
                 const PortableBackup::Continue &beforePublish)
    {
        if (!running(keepGoing) || (beforePublish && !beforePublish())
            || !newDirectory(QFileInfo(target.absoluteFilePath()))
            || !target.dir().rename(staging.path(), target.absoluteFilePath()))
            return false;
        staging.setAutoRemove(false);
        return true;
    }

    bool exactProtectedTree(const QDir &directory, qsizetype fileCount)
    {
        const auto entries = directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        if (entries.size() != fileCount + 2)
            return false;
        QSet<QString> expected { HeaderName, ManifestName };
        for (qsizetype i = 0; i < fileCount; ++i)
            expected.insert(opaqueFileName(i));
        for (const auto &entry : entries) {
            if (!regularFile(entry) || !expected.remove(entry.fileName()))
                return false;
        }
        return expected.isEmpty();
    }
} // namespace

PortableBackupInspection ProtectedBackup::protectDirectory(
    const QString &privatePlainDirectory, const QString &destination, const QString &password,
    const PortableBackup::Progress &progress, const PortableBackup::Continue &keepGoing,
    const PortableBackup::Continue &beforePublish)
{
    if (!QDir::isAbsolutePath(destination) || password.isEmpty())
        return failed(QStringLiteral("A new destination and nonempty backup password are required"));
    const QFileInfo target(QDir::cleanPath(destination));
    if (!newDirectory(target))
        return failed(QStringLiteral("The protected backup destination must be a new directory"));
    const auto inspected = PortableBackup::inspectDirectory(privatePlainDirectory, {}, keepGoing);
    if (!inspected)
        return inspected;
    const QDir source(QFileInfo(privatePlainDirectory).canonicalFilePath());
    const auto manifest = readBounded(source.filePath(PlainManifestName), PortableBackup::MaximumManifestBytes);
    if (manifest.isEmpty() || QCryptographicHash::hash(manifest, QCryptographicHash::Sha256) != inspected.manifestSha256)
        return failed(QStringLiteral("The backup manifest changed before protection"));

    const auto salt = SecureEnvelope::generateMasterKey();
    const auto key = passwordKey(password, salt);
    if (key.size() != SecureEnvelope::MasterKeySize)
        return failed(QStringLiteral("Could not derive a backup encryption key"));
    if (!running(keepGoing))
        return failed(QStringLiteral("Backup protection cancelled"));
    const auto header = makeHeader(salt);
    const auto container = headerIdentity(header);
    QTemporaryDir staging(target.dir().filePath(QStringLiteral(".anykeep-protected-XXXXXX")));
    if (!staging.isValid())
        return failed(staging.errorString());
    if (!writePrivateFile(staging.filePath(HeaderName), header))
        return failed(QStringLiteral("Could not write protected backup header"));

    QBuffer manifestInput;
    manifestInput.setData(manifest);
    manifestInput.open(QIODevice::ReadOnly);
    const PortableBackupFileInfo manifestInfo { PlainManifestName, manifest.size(), inspected.manifestSha256 };
    if (const auto error = encryptFile(manifestInput, manifestInfo, staging.filePath(ManifestName), key, container,
                                      PlainManifestName, keepGoing); !error.isEmpty())
        return failed(error);
    if (const auto error = decryptFile(staging.filePath(ManifestName), nullptr, manifest.size(),
                                      PortableBackup::MaximumManifestBytes, inspected.manifestSha256, key,
                                      container, PlainManifestName, keepGoing); !error.isEmpty())
        return failed(error);

    for (qsizetype i = 0; i < inspected.files.size(); ++i) {
        if (!running(keepGoing) || (progress && !progress(int(i), int(inspected.files.size()))))
            return failed(QStringLiteral("Backup protection cancelled"));
        const auto &entry = inspected.files.at(i);
        const auto path = source.filePath(entry.path);
        if (!regularFile(QFileInfo(path)))
            return failed(QStringLiteral("A portable file is missing or unsafe"));
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly) || input.size() != entry.size)
            return failed(QStringLiteral("A portable file changed before protection"));
        const auto protectedPath = staging.filePath(opaqueFileName(i));
        if (const auto error = encryptFile(input, entry, protectedPath, key, container, entry.path, keepGoing);
            !error.isEmpty())
            return failed(error);
        if (const auto error = decryptFile(protectedPath, nullptr, entry.size, entry.size, entry.sha256, key,
                                          container, entry.path, keepGoing); !error.isEmpty())
            return failed(error);
    }
    if (!exactProtectedTree(QDir(staging.path()), inspected.files.size())
        || readBounded(staging.filePath(HeaderName), MaxHeaderBytes) != header)
        return failed(QStringLiteral("Could not verify the protected backup tree"));
    if ((progress && !progress(int(inspected.files.size()), int(inspected.files.size())))
        || !publish(staging, target, keepGoing, beforePublish))
        return failed(QStringLiteral("Could not publish the protected backup"));
    return inspected;
}

PortableBackupInspection ProtectedBackup::unlockDirectory(
    const QString &sourcePath, const QString &privateDestination, const QString &password,
    const PortableBackup::Progress &progress, const PortableBackup::Continue &keepGoing,
    const PortableBackup::Continue &beforePublish)
{
    if (!QDir::isAbsolutePath(sourcePath) || !QDir::isAbsolutePath(privateDestination) || password.isEmpty())
        return failed(QStringLiteral("A protected backup, private destination and password are required"));
    const QFileInfo sourceInfo(QDir::cleanPath(sourcePath));
    const QFileInfo target(QDir::cleanPath(privateDestination));
    if (sourceInfo.isSymLink() || !sourceInfo.isDir() || sourceInfo.canonicalFilePath().isEmpty() || !newDirectory(target))
        return failed(QStringLiteral("The backup source or private destination is unavailable"));
    if (!running(keepGoing))
        return failed(QStringLiteral("Backup unlocking cancelled"));
    const QDir source(sourceInfo.canonicalFilePath());
    const auto header = readBounded(source.filePath(HeaderName), MaxHeaderBytes);
    const auto salt = headerSalt(header);
    if (salt.isEmpty())
        return failed(QStringLiteral("Unsupported protected backup header"));
    const auto key = passwordKey(password, salt);
    if (key.size() != SecureEnvelope::MasterKeySize)
        return failed(QStringLiteral("Could not derive a backup encryption key"));
    const auto container = headerIdentity(header);
    QBuffer manifest;
    manifest.open(QIODevice::WriteOnly);
    if (const auto error = decryptFile(source.filePath(ManifestName), &manifest, -1,
                                      PortableBackup::MaximumManifestBytes, {}, key, container,
                                      PlainManifestName, keepGoing); !error.isEmpty())
        return failed(error);
    const auto inspected = PortableBackup::inspectManifest(manifest.data(), keepGoing);
    if (!inspected)
        return inspected;
    // All paths are validated by the existing manifest reader before any
    // plaintext file is created. Opaque input names must exactly match its
    // authenticated inventory.
    if (!exactProtectedTree(source, inspected.files.size()))
        return failed(QStringLiteral("Unexpected or missing protected backup files"));

    QTemporaryDir staging(target.dir().filePath(QStringLiteral(".anykeep-unlock-XXXXXX")));
    if (!staging.isValid())
        return failed(staging.errorString());
    const QDir stage(staging.path());
    if (!writePrivateFile(stage.filePath(PlainManifestName), manifest.data()))
        return failed(QStringLiteral("Could not write the private manifest"));
    for (qsizetype i = 0; i < inspected.files.size(); ++i) {
        if (!running(keepGoing) || (progress && !progress(int(i), int(inspected.files.size()))))
            return failed(QStringLiteral("Backup unlocking cancelled"));
        const auto &entry = inspected.files.at(i);
        const auto path = stage.filePath(entry.path);
        if (!stage.mkpath(QFileInfo(path).absolutePath()))
            return failed(QStringLiteral("Could not create a private PTF directory"));
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)
            || !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            return failed(output.errorString());
        if (const auto error = decryptFile(source.filePath(opaqueFileName(i)), &output, entry.size, entry.size,
                                          entry.sha256, key, container, entry.path, keepGoing); !error.isEmpty())
            return failed(error);
        if (!output.commit())
            return failed(output.errorString());
    }
    const auto verified = PortableBackup::inspectDirectory(stage.absolutePath(), {}, keepGoing);
    if (!verified)
        return verified;
    if (verified.manifestSha256 != inspected.manifestSha256
        || !exactProtectedTree(source, inspected.files.size())
        || readBounded(source.filePath(HeaderName), MaxHeaderBytes) != header)
        return failed(QStringLiteral("The protected backup changed while unlocking it"));
    if ((progress && !progress(int(inspected.files.size()), int(inspected.files.size())))
        || !publish(staging, target, keepGoing, beforePublish))
        return failed(QStringLiteral("Could not publish the private unlocked backup"));
    return verified;
}

} // namespace AnyKeep
