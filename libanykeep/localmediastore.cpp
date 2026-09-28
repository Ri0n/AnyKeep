#include "localmediastore.h"

#include "localdatakeystore.h"
#include "secureenvelope.h"
#include "utils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThread>

#include <limits>
#include <utility>

namespace AnyKeep {
namespace {
    // Consumer schema passed to SecureEnvelope::associatedData(); see AeadContext::schema.
    constexpr quint32 AeadContextSchema    = 1;
    constexpr quint32 ExternalSourceMagic  = 0x414b4553; // AKES
    constexpr quint16 ExternalSourceFormat = 1;

    AeadContext externalSourceContext(const QByteArray &blobId)
    {
        return { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media-source"),
                 QString::fromLatin1(blobId.toHex()), AeadContextSchema, QStringLiteral("external-file") };
    }

    QByteArray serializeExternalSource(const LocalMediaExternalSource &source)
    {
        QByteArray bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_5_10);
        out << ExternalSourceMagic << ExternalSourceFormat << source.fileName << source.size << source.checksum
            << source.modifiedMsecsSinceEpoch << source.chunkSize << source.chunkHashes;
        return bytes;
    }

    bool deserializeExternalSource(const QByteArray &bytes, LocalMediaExternalSource *source)
    {
        if (!source)
            return false;
        quint32 magic = 0;
        quint16 version = 0;
        QDataStream in(bytes);
        in.setVersion(QDataStream::Qt_5_10);
        in >> magic >> version >> source->fileName >> source->size >> source->checksum
            >> source->modifiedMsecsSinceEpoch >> source->chunkSize >> source->chunkHashes;
        if (magic != ExternalSourceMagic || version != ExternalSourceFormat || in.status() != QDataStream::Ok
            || !in.atEnd() || source->fileName.isEmpty() || source->size < 0 || source->checksum.size() != 32
            || source->chunkSize == 0) {
            return false;
        }
        const qint64 expectedChunks
            = source->size == 0 ? 0 : (source->size + qint64(source->chunkSize) - 1) / qint64(source->chunkSize);
        if (source->chunkHashes.size() != expectedChunks)
            return false;
        for (const auto &hash : source->chunkHashes) {
            if (hash.size() != 32)
                return false;
        }
        return true;
    }
}

LocalMediaStore::LocalMediaStore(const QString &rootPath, const QByteArray &masterKey) :
    rootPath_(rootPath), masterKey_(masterKey)
{
}

LocalMediaStore *LocalMediaStore::instance()
{
    static LocalMediaStore store;
    return &store;
}

QString LocalMediaStore::blobPath(const QByteArray &blobId) const
{
    const auto hex  = blobId.toHex();
    const auto root = rootPath_.isEmpty() ? Utils::anykeepDataDir() + QStringLiteral("/media") : rootPath_;
    return root + QLatin1Char('/') + QString::fromLatin1(hex.left(2)) + QLatin1Char('/')
        + QString::fromLatin1(hex.mid(2, 2)) + QLatin1Char('/') + QString::fromLatin1(hex) + QStringLiteral(".blob");
}

QString LocalMediaStore::externalSourcePath(const QByteArray &blobId) const
{
    const auto hex  = blobId.toHex();
    const auto root = rootPath_.isEmpty() ? Utils::anykeepDataDir() + QStringLiteral("/media") : rootPath_;
    return root + QStringLiteral("/external/") + QString::fromLatin1(hex.left(2)) + QLatin1Char('/')
        + QString::fromLatin1(hex.mid(2, 2)) + QLatin1Char('/') + QString::fromLatin1(hex)
        + QStringLiteral(".source");
}

bool LocalMediaStore::initialize(QString *error) const
{
    {
        const QMutexLocker locker(&masterKeyMutex_);
        if (!masterKey_.isEmpty()) {
            if (error)
                error->clear();
            return true;
        }
    }

    auto *application = QCoreApplication::instance();
    if (application && QThread::currentThread() != application->thread()) {
        if (error) {
            *error = QCoreApplication::translate(
                "LocalMediaStore", "The local media store must be initialized on the application thread.");
        }
        return false;
    }

    QString keyError;
    auto    key = LocalDataKeyStore::loadOrCreateMasterKey(&keyError);
    if (key.isEmpty()) {
        if (error)
            *error = keyError;
        return false;
    }

    {
        const QMutexLocker locker(&masterKeyMutex_);
        if (masterKey_.isEmpty())
            masterKey_ = std::move(key);
    }
    if (error)
        error->clear();
    return true;
}

QByteArray LocalMediaStore::masterKey(QString *error) const
{
    {
        const QMutexLocker locker(&masterKeyMutex_);
        if (!masterKey_.isEmpty()) {
            if (error)
                error->clear();
            return masterKey_;
        }
    }

    if (!initialize(error))
        return {};

    const QMutexLocker locker(&masterKeyMutex_);
    return masterKey_;
}

LocalMediaResult LocalMediaStore::importFile(const QString &fileName, const QUuid &attachmentId)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };
    const auto mediaType = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchContent).name();
    return importData(file.readAll(), QFileInfo(fileName).fileName(), mediaType, attachmentId);
}

LocalMediaResult LocalMediaStore::referenceFile(const QString &fileName, const QUuid &attachmentId)
{
    const QFileInfo before(fileName);
    if (!before.isFile())
        return { {}, QStringLiteral("The selected media source is not a regular file") };

    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };

    QString keyError;
    const auto masterKey = this->masterKey(&keyError);
    if (masterKey.isEmpty())
        return { {}, keyError };
    const auto idKey = SecureEnvelope::deriveKey(masterKey, KeyDomain::LocalMedia);

    QMessageAuthenticationCode blobHash(QCryptographicHash::Sha256, idKey);
    QCryptographicHash checksumHash(QCryptographicHash::Sha256);
    QList<QByteArray> chunkHashes;
    qint64 total = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(ExternalChunkSize);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return { {}, file.errorString() };
        if (chunk.isEmpty())
            break;
        total += chunk.size();
        blobHash.addData(chunk);
        checksumHash.addData(chunk);
        chunkHashes.append(QCryptographicHash::hash(chunk, QCryptographicHash::Sha256));
    }

    const QFileInfo after(before.absoluteFilePath());
    if (total != before.size() || after.size() != before.size()
        || after.lastModified() != before.lastModified()) {
        return { {}, QStringLiteral("The selected media source changed while it was being linked") };
    }

    const QByteArray blobId = blobHash.result();
    LocalMediaExternalSource source;
    source.fileName                    = after.canonicalFilePath().isEmpty() ? after.absoluteFilePath()
                                                                            : after.canonicalFilePath();
    source.size                        = total;
    source.checksum                    = checksumHash.result();
    source.modifiedMsecsSinceEpoch     = after.lastModified().toMSecsSinceEpoch();
    source.chunkSize                   = quint32(ExternalChunkSize);
    source.chunkHashes                 = std::move(chunkHashes);
    if (const QString sourceError = writeExternalSource(blobId, source); !sourceError.isEmpty())
        return { {}, sourceError };

    MediaReference reference;
    reference.id           = attachmentId.isNull() ? QUuid::createUuid() : attachmentId;
    reference.blobId       = blobId;
    reference.originalName = after.fileName();
    reference.portableName = Utils::portableFileName(reference.originalName, QStringLiteral("attachment"));
    reference.mediaType    = QMimeDatabase().mimeTypeForFile(source.fileName, QMimeDatabase::MatchContent).name();
    reference.size         = source.size;
    reference.checksum     = source.checksum;
    return { reference, {} };
}

LocalMediaResult LocalMediaStore::importData(const QByteArray &plain, const QString &originalName,
                                             const QString &mediaType, const QUuid &attachmentId)
{
    QString    keyError;
    const auto masterKey = this->masterKey(&keyError);
    if (masterKey.isEmpty())
        return { {}, keyError };
    const auto idKey  = SecureEnvelope::deriveKey(masterKey, KeyDomain::LocalMedia);
    const auto blobId = QMessageAuthenticationCode::hash(plain, idKey, QCryptographicHash::Sha256);
    const auto path   = blobPath(blobId);
    if (!QFileInfo::exists(path)) {
        const AeadContext context { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media"),
                                    QString::fromLatin1(blobId.toHex()), AeadContextSchema,
                                    QStringLiteral("attachment") };
        const auto        sealed = SecureEnvelope::seal(plain, masterKey, context);
        if (!sealed)
            return { {}, sealed.error.message };
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return { {}, QStringLiteral("Failed to create local media directory") };
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return { {}, file.errorString() };
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(sealed.value) != sealed.value.size() || !file.commit())
            return { {}, file.errorString() };
    }

    MediaReference reference;
    reference.id           = attachmentId.isNull() ? QUuid::createUuid() : attachmentId;
    reference.blobId       = blobId;
    reference.originalName = QFileInfo(originalName).fileName();
    reference.portableName = Utils::portableFileName(reference.originalName, QStringLiteral("attachment"));
    reference.mediaType    = mediaType;
    reference.size         = plain.size();
    reference.checksum     = QCryptographicHash::hash(plain, QCryptographicHash::Sha256);
    return { reference, {} };
}

QString LocalMediaStore::writeExternalSource(const QByteArray &blobId, const LocalMediaExternalSource &source) const
{
    QString keyError;
    const auto masterKey = this->masterKey(&keyError);
    if (masterKey.isEmpty())
        return keyError;
    const auto sealed = SecureEnvelope::seal(serializeExternalSource(source), masterKey, externalSourceContext(blobId));
    if (!sealed)
        return sealed.error.message;

    const QString path = externalSourcePath(blobId);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return QStringLiteral("Failed to create local media source directory");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(sealed.value) != sealed.value.size() || !file.commit())
        return file.errorString();
    return {};
}

LocalMediaExternalSourceResult LocalMediaStore::loadExternalSource(const QByteArray &blobId) const
{
    QString keyError;
    const auto masterKey = this->masterKey(&keyError);
    if (masterKey.isEmpty())
        return { {}, keyError };

    QFile file(externalSourcePath(blobId));
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };
    const auto opened = SecureEnvelope::open(file.readAll(), masterKey, externalSourceContext(blobId));
    if (!opened)
        return { {}, opened.error.message };

    LocalMediaExternalSource source;
    if (!deserializeExternalSource(opened.value, &source))
        return { {}, QStringLiteral("Invalid external media source metadata") };
    return { source, {} };
}

LocalMediaExternalSourceResult LocalMediaStore::externalSource(const MediaReference &reference) const
{
    if (!reference.isValid() || reference.size < 0 || reference.checksum.size() != 32)
        return { {}, QStringLiteral("Invalid media reference") };
    auto source = loadExternalSource(reference.blobId);
    if (!source)
        return source;
    if (source.value.size != reference.size || source.value.checksum != reference.checksum)
        return { {}, QStringLiteral("External media source does not match its reference") };

    const QFileInfo file(source.value.fileName);
    if (!file.isFile())
        return { {}, QStringLiteral("The external media source is no longer available") };
    if (file.size() != source.value.size)
        return { {}, QStringLiteral("The external media source changed size") };
    return source;
}

LocalMediaDataResult LocalMediaStore::data(const QByteArray &blobId) const
{
    QString    keyError;
    const auto masterKey = this->masterKey(&keyError);
    if (masterKey.isEmpty())
        return { {}, keyError };

    QFile managed(blobPath(blobId));
    if (managed.exists()) {
        if (!managed.open(QIODevice::ReadOnly))
            return { {}, managed.errorString() };
        const AeadContext context { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media"),
                                    QString::fromLatin1(blobId.toHex()), AeadContextSchema,
                                    QStringLiteral("attachment") };
        const auto opened = SecureEnvelope::open(managed.readAll(), masterKey, context);
        if (!opened)
            return { {}, opened.error.message };
        return { opened.value, {} };
    }

    const auto source = loadExternalSource(blobId);
    if (!source)
        return { {}, source.error };
    QFile file(source.value.fileName);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };
    if (file.size() != source.value.size)
        return { {}, QStringLiteral("The external media source changed size") };
    if (source.value.size > qint64(std::numeric_limits<qsizetype>::max()))
        return { {}, QStringLiteral("The external media source is too large to materialize in memory") };

    QByteArray result;
    result.reserve(qsizetype(source.value.size));
    QCryptographicHash checksum(QCryptographicHash::Sha256);
    for (qsizetype index = 0; index < source.value.chunkHashes.size(); ++index) {
        const qint64 remaining = source.value.size - qint64(index) * qint64(source.value.chunkSize);
        const qint64 expected  = qMin<qint64>(source.value.chunkSize, remaining);
        const QByteArray chunk = file.read(expected);
        if (chunk.size() != expected)
            return { {}, file.errorString().isEmpty() ? QStringLiteral("Could not read external media source")
                                                      : file.errorString() };
        if (QCryptographicHash::hash(chunk, QCryptographicHash::Sha256) != source.value.chunkHashes.at(index))
            return { {}, QStringLiteral("The external media source changed since it was linked") };
        checksum.addData(chunk);
        result.append(chunk);
    }
    if (result.size() != source.value.size || checksum.result() != source.value.checksum)
        return { {}, QStringLiteral("The external media source failed its integrity check") };
    return { result, {} };
}

bool LocalMediaStore::containsManagedBlob(const QByteArray &blobId) const { return QFileInfo::exists(blobPath(blobId)); }

bool LocalMediaStore::contains(const QByteArray &blobId) const
{
    if (containsManagedBlob(blobId))
        return true;
    const auto source = loadExternalSource(blobId);
    return source && QFileInfo(source.value.fileName).isFile() && QFileInfo(source.value.fileName).size() == source.value.size;
}

} // namespace AnyKeep
