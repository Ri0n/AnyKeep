#include "localmediastore.h"

#include "localdatakeystore.h"
#include "secureenvelope.h"
#include "utils.h"

#include <QBuffer>
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
    constexpr quint32 ManagedChunkMagic    = 0x414b4d43; // AKMC
    constexpr quint16 ManagedChunkFormat   = 1;
    constexpr quint32 MaxEnvelopeOverhead  = 128 * 1024;

    struct ManagedChunkHeader {
        quint32 chunkSize { 0 };
        quint64 plainSize { 0 };
        quint64 chunkCount { 0 };
        quint32 fullEnvelopeSize { 0 };
        qint64  dataOffset { 0 };
    };

    struct Fingerprint {
        QByteArray blobId;
        QByteArray checksum;
        qint64     size { 0 };
    };

    AeadContext legacyManagedContext(const QByteArray &blobId)
    {
        return { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media"), QString::fromLatin1(blobId.toHex()),
                 AeadContextSchema, QStringLiteral("attachment") };
    }

    AeadContext managedHeaderContext(const QByteArray &blobId)
    {
        return { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media-chunks"),
                 QString::fromLatin1(blobId.toHex()), ManagedChunkFormat, QStringLiteral("header") };
    }

    qint64 expectedChunkSize(const ManagedChunkHeader &header, quint64 index)
    {
        if (index >= header.chunkCount)
            return -1;
        const quint64 offset = index * quint64(header.chunkSize);
        const quint64 left   = header.plainSize - offset;
        return qint64(qMin<quint64>(header.chunkSize, left));
    }

    QString chunkItemId(const QByteArray &blobId, quint64 index)
    {
        return QString::fromLatin1(blobId.toHex()) + QLatin1Char(':')
            + QStringLiteral("%1").arg(qulonglong(index), 16, 16, QLatin1Char('0'));
    }

    AeadContext managedChunkContext(const QByteArray &blobId, const ManagedChunkHeader &header, quint64 index)
    {
        const auto expected = expectedChunkSize(header, index);
        return { KeyDomain::LocalMedia,
                 QStringLiteral("anykeep-local-media-chunks"),
                 chunkItemId(blobId, index),
                 ManagedChunkFormat,
                 QStringLiteral("attachment-chunk:%1:%2:%3:%4")
                     .arg(qulonglong(header.plainSize))
                     .arg(header.chunkSize)
                     .arg(qulonglong(header.chunkCount))
                     .arg(expected) };
    }

    AeadContext externalSourceContext(const QByteArray &blobId)
    {
        return { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media-source"),
                 QString::fromLatin1(blobId.toHex()), AeadContextSchema, QStringLiteral("external-file") };
    }

    QByteArray serializeManagedHeaderDescriptor(const ManagedChunkHeader &header)
    {
        QByteArray bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_5_10);
        out << ManagedChunkMagic << ManagedChunkFormat << header.chunkSize << header.plainSize << header.chunkCount
            << header.fullEnvelopeSize;
        return bytes;
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

    bool fingerprintDevice(QIODevice &device, const QByteArray &idKey, Fingerprint *fingerprint, QString *error)
    {
        if (!fingerprint || idKey.isEmpty()) {
            if (error)
                *error = QStringLiteral("Invalid media fingerprint request");
            return false;
        }
        QMessageAuthenticationCode blobHash(QCryptographicHash::Sha256, idKey);
        QCryptographicHash checksumHash(QCryptographicHash::Sha256);
        qint64 total = 0;
        while (!device.atEnd()) {
            const QByteArray chunk = device.read(LocalMediaStore::ManagedChunkSize);
            if (chunk.isEmpty()) {
                if (device.atEnd())
                    break;
                if (error)
                    *error = device.errorString().isEmpty() ? QStringLiteral("Could not read media source")
                                                            : device.errorString();
                return false;
            }
            if (total > std::numeric_limits<qint64>::max() - chunk.size()) {
                if (error)
                    *error = QStringLiteral("Media source is too large");
                return false;
            }
            total += chunk.size();
            blobHash.addData(chunk);
            checksumHash.addData(chunk);
        }
        fingerprint->blobId   = blobHash.result();
        fingerprint->checksum = checksumHash.result();
        fingerprint->size     = total;
        if (error)
            error->clear();
        return true;
    }

    bool writeEnvelope(QDataStream &out, const QByteArray &envelope)
    {
        if (envelope.isEmpty() || envelope.size() > std::numeric_limits<quint32>::max())
            return false;
        out << quint32(envelope.size());
        return out.status() == QDataStream::Ok
            && out.writeRawData(envelope.constData(), envelope.size()) == envelope.size();
    }

    bool readEnvelope(QDataStream &in, quint32 maxSize, QByteArray *envelope)
    {
        if (!envelope)
            return false;
        quint32 size = 0;
        in >> size;
        if (in.status() != QDataStream::Ok || size == 0 || size > maxSize
            || size > quint32(std::numeric_limits<int>::max())) {
            return false;
        }
        envelope->resize(int(size));
        return in.readRawData(envelope->data(), int(size)) == int(size);
    }

    QString writeManagedChunkedBlob(QIODevice &plain, qint64 plainSize, QIODevice &output, const QByteArray &masterKey,
                                    const QByteArray &idKey, const QByteArray &expectedBlobId,
                                    const QByteArray &expectedChecksum)
    {
        if (plainSize < 0 || masterKey.size() != SecureEnvelope::MasterKeySize || idKey.isEmpty()
            || expectedBlobId.size() != 32 || expectedChecksum.size() != 32) {
            return QStringLiteral("Invalid chunked media write request");
        }

        ManagedChunkHeader header;
        header.chunkSize  = quint32(LocalMediaStore::ManagedChunkSize);
        header.plainSize  = quint64(plainSize);
        header.chunkCount = plainSize == 0
            ? 0
            : (quint64(plainSize) + quint64(header.chunkSize) - 1) / quint64(header.chunkSize);

        QMessageAuthenticationCode blobHash(QCryptographicHash::Sha256, idKey);
        QCryptographicHash checksumHash(QCryptographicHash::Sha256);

        QByteArray firstEnvelope;
        if (header.chunkCount > 0) {
            const qint64 expected = expectedChunkSize(header, 0);
            const QByteArray chunk = plain.read(expected);
            if (chunk.size() != expected)
                return plain.errorString().isEmpty() ? QStringLiteral("Could not read media source")
                                                     : plain.errorString();
            blobHash.addData(chunk);
            checksumHash.addData(chunk);
            const auto sealed = SecureEnvelope::seal(chunk, masterKey, managedChunkContext(expectedBlobId, header, 0));
            if (!sealed)
                return sealed.error.message;
            if (sealed.value.size() > std::numeric_limits<quint32>::max())
                return QStringLiteral("Encrypted media chunk is too large");
            firstEnvelope           = sealed.value;
            header.fullEnvelopeSize = quint32(firstEnvelope.size());
        }

        const QByteArray descriptor = serializeManagedHeaderDescriptor(header);
        const auto headerEnvelope   = SecureEnvelope::seal(descriptor, masterKey, managedHeaderContext(expectedBlobId));
        if (!headerEnvelope)
            return headerEnvelope.error.message;
        if (headerEnvelope.value.size() > MaxEnvelopeOverhead)
            return QStringLiteral("Encrypted media header is unexpectedly large");

        QDataStream out(&output);
        out.setVersion(QDataStream::Qt_5_10);
        out << ManagedChunkMagic << ManagedChunkFormat << header.chunkSize << header.plainSize << header.chunkCount
            << header.fullEnvelopeSize;
        if (out.status() != QDataStream::Ok || !writeEnvelope(out, headerEnvelope.value))
            return output.errorString().isEmpty() ? QStringLiteral("Could not write media header")
                                                  : output.errorString();
        if (header.chunkCount > 0 && !writeEnvelope(out, firstEnvelope))
            return output.errorString().isEmpty() ? QStringLiteral("Could not write media chunk")
                                                  : output.errorString();

        for (quint64 index = 1; index < header.chunkCount; ++index) {
            const qint64 expected = expectedChunkSize(header, index);
            const QByteArray chunk = plain.read(expected);
            if (chunk.size() != expected)
                return plain.errorString().isEmpty() ? QStringLiteral("Could not read media source")
                                                     : plain.errorString();
            blobHash.addData(chunk);
            checksumHash.addData(chunk);
            const auto sealed
                = SecureEnvelope::seal(chunk, masterKey, managedChunkContext(expectedBlobId, header, index));
            if (!sealed)
                return sealed.error.message;
            if (index + 1 < header.chunkCount && sealed.value.size() != header.fullEnvelopeSize)
                return QStringLiteral("Encrypted media chunk framing is not stable");
            if (sealed.value.size() > header.fullEnvelopeSize)
                return QStringLiteral("Encrypted media tail chunk is unexpectedly large");
            if (!writeEnvelope(out, sealed.value))
                return output.errorString().isEmpty() ? QStringLiteral("Could not write media chunk")
                                                      : output.errorString();
        }

        const QByteArray extra = plain.read(1);
        if (!extra.isEmpty() || !plain.atEnd())
            return QStringLiteral("Media source changed while it was being imported");
        if (blobHash.result() != expectedBlobId || checksumHash.result() != expectedChecksum)
            return QStringLiteral("Media source changed while it was being imported");
        if (out.status() != QDataStream::Ok)
            return output.errorString().isEmpty() ? QStringLiteral("Could not write encrypted media")
                                                  : output.errorString();
        return {};
    }

    bool readManagedHeader(QFile &file, const QByteArray &masterKey, const QByteArray &blobId,
                           ManagedChunkHeader *header, QString *error)
    {
        if (!header || !file.seek(0)) {
            if (error)
                *error = file.errorString().isEmpty() ? QStringLiteral("Could not seek encrypted media")
                                                      : file.errorString();
            return false;
        }

        quint32 magic = 0;
        quint16 version = 0;
        QByteArray headerEnvelope;
        QDataStream in(&file);
        in.setVersion(QDataStream::Qt_5_10);
        in >> magic >> version >> header->chunkSize >> header->plainSize >> header->chunkCount
            >> header->fullEnvelopeSize;
        if (in.status() != QDataStream::Ok || magic != ManagedChunkMagic || version != ManagedChunkFormat
            || !readEnvelope(in, MaxEnvelopeOverhead, &headerEnvelope)) {
            if (error)
                *error = QStringLiteral("Invalid chunked media header");
            return false;
        }
        header->dataOffset = file.pos();

        if (header->chunkSize != LocalMediaStore::ManagedChunkSize
            || header->plainSize > quint64(std::numeric_limits<qint64>::max())) {
            if (error)
                *error = QStringLiteral("Unsupported chunked media geometry");
            return false;
        }
        const quint64 expectedCount = header->plainSize == 0
            ? 0
            : (header->plainSize + quint64(header->chunkSize) - 1) / quint64(header->chunkSize);
        if (header->chunkCount != expectedCount
            || (header->chunkCount == 0 && header->fullEnvelopeSize != 0)
            || (header->chunkCount > 0
                && (header->fullEnvelopeSize == 0
                    || header->fullEnvelopeSize > header->chunkSize + MaxEnvelopeOverhead))) {
            if (error)
                *error = QStringLiteral("Invalid chunked media geometry");
            return false;
        }

        const QByteArray descriptor = serializeManagedHeaderDescriptor(*header);
        const auto opened = SecureEnvelope::open(headerEnvelope, masterKey, managedHeaderContext(blobId));
        if (!opened || opened.value != descriptor) {
            if (error)
                *error = opened ? QStringLiteral("Chunked media header authentication failed") : opened.error.message;
            return false;
        }

        if (header->chunkCount == 0) {
            if (file.size() != header->dataOffset) {
                if (error)
                    *error = QStringLiteral("Invalid empty chunked media container");
                return false;
            }
        } else {
            const quint64 recordSpan = quint64(sizeof(quint32)) + header->fullEnvelopeSize;
            const quint64 prefixRecords = header->chunkCount - 1;
            if (prefixRecords > quint64(std::numeric_limits<qint64>::max() - header->dataOffset) / recordSpan) {
                if (error)
                    *error = QStringLiteral("Chunked media container is too large");
                return false;
            }
            const qint64 lastOffset = header->dataOffset + qint64(prefixRecords * recordSpan);
            if (!file.seek(lastOffset)) {
                if (error)
                    *error = QStringLiteral("Invalid chunked media tail offset");
                return false;
            }
            quint32 lastSize = 0;
            QDataStream tail(&file);
            tail.setVersion(QDataStream::Qt_5_10);
            tail >> lastSize;
            if (tail.status() != QDataStream::Ok || lastSize == 0 || lastSize > header->fullEnvelopeSize
                || lastOffset > std::numeric_limits<qint64>::max() - qint64(sizeof(quint32)) - lastSize
                || lastOffset + qint64(sizeof(quint32)) + lastSize != file.size()) {
                if (error)
                    *error = QStringLiteral("Invalid chunked media container length");
                return false;
            }
        }

        if (error)
            error->clear();
        return true;
    }

    LocalMediaDataResult readManagedChunk(QFile &file, const QByteArray &masterKey, const QByteArray &blobId,
                                          const ManagedChunkHeader &header, quint64 index)
    {
        if (index >= header.chunkCount)
            return { {}, QStringLiteral("Media chunk is out of range") };
        const quint64 recordSpan = quint64(sizeof(quint32)) + header.fullEnvelopeSize;
        if (index > quint64(std::numeric_limits<qint64>::max() - header.dataOffset) / recordSpan)
            return { {}, QStringLiteral("Media chunk offset overflow") };
        const qint64 offset = header.dataOffset + qint64(index * recordSpan);
        if (!file.seek(offset))
            return { {}, file.errorString().isEmpty() ? QStringLiteral("Could not seek media chunk")
                                                      : file.errorString() };

        QByteArray envelope;
        QDataStream in(&file);
        in.setVersion(QDataStream::Qt_5_10);
        if (!readEnvelope(in, header.fullEnvelopeSize, &envelope))
            return { {}, QStringLiteral("Invalid encrypted media chunk") };
        const qint64 expected = expectedChunkSize(header, index);
        if ((index + 1 < header.chunkCount || expected == header.chunkSize)
            && envelope.size() != header.fullEnvelopeSize) {
            return { {}, QStringLiteral("Invalid encrypted media chunk size") };
        }

        const auto opened = SecureEnvelope::open(envelope, masterKey, managedChunkContext(blobId, header, index));
        if (!opened)
            return { {}, opened.error.message };
        if (opened.value.size() != expected)
            return { {}, QStringLiteral("Invalid decrypted media chunk size") };
        return { opened.value, {} };
    }

    LocalMediaDataResult openLegacyManagedBlob(QFile &file, const QByteArray &masterKey, const QByteArray &blobId)
    {
        if (!file.seek(0))
            return { {}, file.errorString() };
        const auto opened = SecureEnvelope::open(file.readAll(), masterKey, legacyManagedContext(blobId));
        if (!opened)
            return { {}, opened.error.message };
        return { opened.value, {} };
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
    const QFileInfo before(fileName);
    if (!before.isFile())
        return { {}, QStringLiteral("The selected media source is not a regular file") };
    const QString mediaType = QMimeDatabase().mimeTypeForFile(before.absoluteFilePath(), QMimeDatabase::MatchContent).name();

    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };

    QString keyError;
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return { {}, keyError };
    const auto idKey = SecureEnvelope::deriveKey(key, KeyDomain::LocalMedia);

    Fingerprint fingerprint;
    QString fingerprintError;
    if (!fingerprintDevice(file, idKey, &fingerprint, &fingerprintError))
        return { {}, fingerprintError };
    const QFileInfo afterFingerprint(before.absoluteFilePath());
    if (fingerprint.size != before.size() || afterFingerprint.size() != before.size()
        || afterFingerprint.lastModified() != before.lastModified()) {
        return { {}, QStringLiteral("The selected media source changed while it was being imported") };
    }

    const QString path = blobPath(fingerprint.blobId);
    if (!QFileInfo::exists(path)) {
        if (!file.seek(0))
            return { {}, file.errorString() };
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return { {}, QStringLiteral("Failed to create local media directory") };
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly))
            return { {}, output.errorString() };
        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QString writeError = writeManagedChunkedBlob(file, fingerprint.size, output, key, idKey,
                                                           fingerprint.blobId, fingerprint.checksum);
        if (!writeError.isEmpty())
            return { {}, writeError };
        if (!output.commit())
            return { {}, output.errorString() };
    }

    MediaReference reference;
    reference.id           = attachmentId.isNull() ? QUuid::createUuid() : attachmentId;
    reference.blobId       = fingerprint.blobId;
    reference.originalName = before.fileName();
    reference.portableName = Utils::portableFileName(reference.originalName, QStringLiteral("attachment"));
    reference.mediaType    = mediaType;
    reference.size         = fingerprint.size;
    reference.checksum     = fingerprint.checksum;
    return { reference, {} };
}

LocalMediaResult LocalMediaStore::referenceFile(const QString &fileName, const QUuid &attachmentId)
{
    const QFileInfo before(fileName);
    if (!before.isFile())
        return { {}, QStringLiteral("The selected media source is not a regular file") };

    const QString mediaType = QMimeDatabase().mimeTypeForFile(before.absoluteFilePath(), QMimeDatabase::MatchContent).name();

    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };

    QString keyError;
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return { {}, keyError };
    const auto idKey = SecureEnvelope::deriveKey(key, KeyDomain::LocalMedia);

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
    source.fileName                = after.canonicalFilePath().isEmpty() ? after.absoluteFilePath()
                                                                        : after.canonicalFilePath();
    source.size                    = total;
    source.checksum                = checksumHash.result();
    source.modifiedMsecsSinceEpoch = after.lastModified().toMSecsSinceEpoch();
    source.chunkSize               = quint32(ExternalChunkSize);
    source.chunkHashes             = std::move(chunkHashes);
    if (const QString sourceError = writeExternalSource(blobId, source); !sourceError.isEmpty())
        return { {}, sourceError };

    MediaReference reference;
    reference.id           = attachmentId.isNull() ? QUuid::createUuid() : attachmentId;
    reference.blobId       = blobId;
    reference.originalName = after.fileName();
    reference.portableName = Utils::portableFileName(reference.originalName, QStringLiteral("attachment"));
    reference.mediaType    = mediaType;
    reference.size         = source.size;
    reference.checksum     = source.checksum;
    return { reference, {} };
}

LocalMediaResult LocalMediaStore::importData(const QByteArray &plain, const QString &originalName,
                                             const QString &mediaType, const QUuid &attachmentId)
{
    QString keyError;
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return { {}, keyError };
    const auto idKey    = SecureEnvelope::deriveKey(key, KeyDomain::LocalMedia);
    const auto blobId   = QMessageAuthenticationCode::hash(plain, idKey, QCryptographicHash::Sha256);
    const auto checksum = QCryptographicHash::hash(plain, QCryptographicHash::Sha256);
    const auto path     = blobPath(blobId);
    if (!QFileInfo::exists(path)) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return { {}, QStringLiteral("Failed to create local media directory") };
        QBuffer input;
        input.setData(plain);
        if (!input.open(QIODevice::ReadOnly))
            return { {}, input.errorString() };
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly))
            return { {}, output.errorString() };
        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QString writeError = writeManagedChunkedBlob(input, plain.size(), output, key, idKey, blobId, checksum);
        if (!writeError.isEmpty())
            return { {}, writeError };
        if (!output.commit())
            return { {}, output.errorString() };
    }

    MediaReference reference;
    reference.id           = attachmentId.isNull() ? QUuid::createUuid() : attachmentId;
    reference.blobId       = blobId;
    reference.originalName = QFileInfo(originalName).fileName();
    reference.portableName = Utils::portableFileName(reference.originalName, QStringLiteral("attachment"));
    reference.mediaType    = mediaType;
    reference.size         = plain.size();
    reference.checksum     = checksum;
    return { reference, {} };
}

QString LocalMediaStore::writeExternalSource(const QByteArray &blobId, const LocalMediaExternalSource &source) const
{
    QString keyError;
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return keyError;
    const auto sealed = SecureEnvelope::seal(serializeExternalSource(source), key, externalSourceContext(blobId));
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
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return { {}, keyError };

    QFile file(externalSourcePath(blobId));
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };
    const auto opened = SecureEnvelope::open(file.readAll(), key, externalSourceContext(blobId));
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

bool LocalMediaStore::isChunkedManagedBlob(const QByteArray &blobId) const
{
    QFile file(blobPath(blobId));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    quint32 magic = 0;
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_5_10);
    in >> magic;
    return in.status() == QDataStream::Ok && magic == ManagedChunkMagic;
}

LocalMediaRangeResult LocalMediaStore::readManagedRange(const QByteArray &blobId, qint64 offset, qint64 maxSize) const
{
    if (blobId.isEmpty() || offset < 0 || maxSize < 0)
        return { {}, -1, QStringLiteral("Invalid managed media range") };

    QString keyError;
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return { {}, -1, keyError };

    QFile file(blobPath(blobId));
    if (!file.open(QIODevice::ReadOnly))
        return { {}, -1, file.errorString() };

    quint32 magic = 0;
    QDataStream probe(&file);
    probe.setVersion(QDataStream::Qt_5_10);
    probe >> magic;
    if (probe.status() != QDataStream::Ok)
        return { {}, -1, QStringLiteral("Invalid encrypted media blob") };

    if (magic != ManagedChunkMagic) {
        const auto legacy = openLegacyManagedBlob(file, key, blobId);
        if (!legacy)
            return { {}, -1, legacy.error };
        if (offset > legacy.value.size())
            return { {}, legacy.value.size(), QStringLiteral("Managed media range is out of bounds") };
        const qint64 count = qMin(maxSize, qint64(legacy.value.size()) - offset);
        return { legacy.value.mid(qsizetype(offset), qsizetype(count)), legacy.value.size(), {} };
    }

    ManagedChunkHeader header;
    QString headerError;
    if (!readManagedHeader(file, key, blobId, &header, &headerError))
        return { {}, -1, headerError };
    const qint64 totalSize = qint64(header.plainSize);
    if (offset > totalSize)
        return { {}, totalSize, QStringLiteral("Managed media range is out of bounds") };
    const qint64 count = qMin(maxSize, totalSize - offset);
    if (count == 0)
        return { {}, totalSize, {} };
    if (count > qint64(std::numeric_limits<qsizetype>::max()))
        return { {}, totalSize, QStringLiteral("Managed media range is too large to materialize") };

    QByteArray result;
    result.reserve(qsizetype(count));
    qint64 cursor = offset;
    qint64 remaining = count;
    while (remaining > 0) {
        const quint64 chunkIndex = quint64(cursor) / header.chunkSize;
        const auto chunk = readManagedChunk(file, key, blobId, header, chunkIndex);
        if (!chunk)
            return { {}, totalSize, chunk.error };
        const qint64 chunkStart = qint64(chunkIndex * quint64(header.chunkSize));
        const qint64 chunkOffset = cursor - chunkStart;
        const qint64 copySize = qMin(remaining, qint64(chunk.value.size()) - chunkOffset);
        if (copySize <= 0)
            return { {}, totalSize, QStringLiteral("Invalid decrypted media range") };
        result.append(chunk.value.constData() + chunkOffset, qsizetype(copySize));
        cursor += copySize;
        remaining -= copySize;
    }
    return { result, totalSize, {} };
}

LocalMediaDataResult LocalMediaStore::data(const QByteArray &blobId) const
{
    QFile managed(blobPath(blobId));
    if (managed.exists()) {
        if (isChunkedManagedBlob(blobId)) {
            const auto probe = readManagedRange(blobId, 0, 0);
            if (!probe)
                return { {}, probe.error };
            if (probe.totalSize > qint64(std::numeric_limits<qsizetype>::max()))
                return { {}, QStringLiteral("The local media source is too large to materialize in memory") };
            const auto all = readManagedRange(blobId, 0, probe.totalSize);
            return all ? LocalMediaDataResult { all.value, {} } : LocalMediaDataResult { {}, all.error };
        }

        QString keyError;
        const auto key = masterKey(&keyError);
        if (key.isEmpty())
            return { {}, keyError };
        if (!managed.open(QIODevice::ReadOnly))
            return { {}, managed.errorString() };
        return openLegacyManagedBlob(managed, key, blobId);
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
    return source && QFileInfo(source.value.fileName).isFile()
        && QFileInfo(source.value.fileName).size() == source.value.size;
}

} // namespace AnyKeep
