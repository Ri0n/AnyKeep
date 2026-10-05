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
    constexpr quint32 AeadContextSchema           = 1;
    constexpr quint32 ExternalSourceMagic         = 0x414b4553; // AKES
    constexpr quint16 LegacyExternalSourceFormat  = 1;
    constexpr quint16 ExternalSourceFormat        = 2;
    constexpr quint32 ManagedChunkMagic           = 0x414b4d43; // AKMC
    constexpr quint16 ManagedChunkFormat          = 1;
    constexpr quint32 MaxEnvelopeOverhead         = 128 * 1024;

    struct ManagedChunkHeader {
        quint32 chunkSize { 0 };
        quint64 plainSize { 0 };
        quint64 chunkCount { 0 };
        quint32 fullEnvelopeSize { 0 };
        qint64  dataOffset { 0 };
    };

    struct Fingerprint {
        QByteArray           blobId;
        QByteArray           checksum;
        qint64               size { 0 };
        QList<QByteArray>     chunkHashes;
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

    AeadContext legacyExternalSourceContext(const QByteArray &blobId)
    {
        return { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media-source"),
                 QString::fromLatin1(blobId.toHex()), AeadContextSchema, QStringLiteral("external-file") };
    }

    AeadContext externalSourceContext(const QUuid &attachmentId)
    {
        return { KeyDomain::LocalMedia, QStringLiteral("anykeep-local-media-source"),
                 attachmentId.toString(QUuid::WithoutBraces), ExternalSourceFormat, QStringLiteral("external-file") };
    }

    bool inlineMediaType(const QString &type)
    {
        return type.startsWith(QLatin1String("image/")) || type.startsWith(QLatin1String("audio/"))
            || type.startsWith(QLatin1String("video/"));
    }

    QString mediaTypeForFile(const QString &fileName)
    {
        QMimeDatabase database;
        const auto content = database.mimeTypeForFile(fileName, QMimeDatabase::MatchContent);
        const auto suffix  = database.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);

        // For user-selected inline media, an explicit media extension is the
        // stable cross-platform hint. Content sniffers can disagree on short or
        // partially downloaded media (for example an AVI being classified as
        // TGA on Linux), which should not silently change the media block kind.
        if (inlineMediaType(suffix.name()))
            return suffix.name();
        if (!content.name().isEmpty())
            return content.name();
        return suffix.name().isEmpty() ? QStringLiteral("application/octet-stream") : suffix.name();
    }

    bool sourceRevisionMatches(const LocalMediaExternalSource &source, const QFileInfo &file)
    {
        return file.isFile() && file.size() == source.size
            && file.lastModified().toMSecsSinceEpoch() == source.modifiedMsecsSinceEpoch;
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
        out << ExternalSourceMagic << ExternalSourceFormat << source.fileName << source.size
            << source.modifiedMsecsSinceEpoch << source.blobId << source.checksum << source.chunkSize
            << source.chunkHashes;
        return bytes;
    }

    bool validateExternalFingerprint(const LocalMediaExternalSource &source)
    {
        if (source.blobId.isEmpty() && source.checksum.isEmpty() && source.chunkHashes.isEmpty())
            return source.chunkSize == LocalMediaStore::ExternalChunkSize;
        if (source.blobId.size() != 32 || source.checksum.size() != 32
            || source.chunkSize != LocalMediaStore::ExternalChunkSize) {
            return false;
        }
        const qint64 expectedChunks
            = source.size == 0 ? 0 : (source.size + qint64(source.chunkSize) - 1) / qint64(source.chunkSize);
        if (source.chunkHashes.size() != expectedChunks)
            return false;
        for (const auto &hash : source.chunkHashes) {
            if (hash.size() != 32)
                return false;
        }
        return true;
    }

    bool deserializeExternalSource(const QByteArray &bytes, LocalMediaExternalSource *source)
    {
        if (!source)
            return false;
        quint32 magic = 0;
        quint16 version = 0;
        QDataStream in(bytes);
        in.setVersion(QDataStream::Qt_5_10);
        in >> magic >> version;
        if (magic != ExternalSourceMagic)
            return false;
        if (version == LegacyExternalSourceFormat) {
            in >> source->fileName >> source->size >> source->checksum >> source->modifiedMsecsSinceEpoch
                >> source->chunkSize >> source->chunkHashes;
        } else if (version == ExternalSourceFormat) {
            in >> source->fileName >> source->size >> source->modifiedMsecsSinceEpoch >> source->blobId
                >> source->checksum >> source->chunkSize >> source->chunkHashes;
        } else {
            return false;
        }
        if (in.status() != QDataStream::Ok || !in.atEnd() || source->fileName.isEmpty() || source->size < 0
            || !validateExternalFingerprint(*source)) {
            return false;
        }
        return true;
    }

    bool fingerprintDevice(QIODevice &device, const QByteArray &idKey, Fingerprint *fingerprint, QString *error,
                           bool keepChunkHashes = false)
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
            if (keepChunkHashes)
                fingerprint->chunkHashes.append(QCryptographicHash::hash(chunk, QCryptographicHash::Sha256));
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

QString LocalMediaStore::externalSourcePath(const QUuid &attachmentId) const
{
    const auto id   = attachmentId.toString(QUuid::WithoutBraces);
    const auto root = rootPath_.isEmpty() ? Utils::anykeepDataDir() + QStringLiteral("/media") : rootPath_;
    return root + QStringLiteral("/external/by-id/") + id.left(2) + QLatin1Char('/') + id.mid(2, 2) + QLatin1Char('/')
        + id + QStringLiteral(".source");
}

QString LocalMediaStore::legacyExternalSourcePath(const QByteArray &blobId) const
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
    const QString mediaType = mediaTypeForFile(before.absoluteFilePath());

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
    const QFileInfo file(fileName);
    if (!file.isFile())
        return { {}, QStringLiteral("The selected media source is not a regular file") };

    MediaReference reference;
    reference.id           = attachmentId.isNull() ? QUuid::createUuid() : attachmentId;
    reference.originalName = file.fileName();
    reference.portableName = Utils::portableFileName(reference.originalName, QStringLiteral("attachment"));
    reference.mediaType    = mediaTypeForFile(file.absoluteFilePath());
    reference.size         = file.size();

    LocalMediaExternalSource source;
    source.fileName                = file.canonicalFilePath().isEmpty() ? file.absoluteFilePath() : file.canonicalFilePath();
    source.size                    = file.size();
    source.modifiedMsecsSinceEpoch = file.lastModified().toMSecsSinceEpoch();
    source.chunkSize               = quint32(ExternalChunkSize);
    if (const QString sourceError = writeExternalSource(reference.id, source); !sourceError.isEmpty())
        return { {}, sourceError };
    return { reference, {} };
}

LocalMediaResult LocalMediaStore::fingerprintExternalFile(const MediaReference &reference)
{
    if (!reference.isValid())
        return { {}, QStringLiteral("Invalid media reference") };
    auto loaded = loadExternalSource(reference);
    if (!loaded)
        return { {}, loaded.error };

    auto source = loaded.value;
    QFileInfo before(source.fileName);
    if (!before.isFile())
        return { {}, QStringLiteral("The external media source is no longer available") };

    // A changed source revision invalidates cached hashes. Re-fingerprint the
    // current file rather than trusting hashes computed for an older revision.
    if (sourceRevisionMatches(source, before) && source.hasFingerprint()) {
        auto enriched     = reference;
        enriched.size     = source.size;
        enriched.blobId   = source.blobId;
        enriched.checksum = source.checksum;
        return { enriched, {} };
    }

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
    if (!fingerprintDevice(file, idKey, &fingerprint, &fingerprintError, true))
        return { {}, fingerprintError };
    const QFileInfo after(before.absoluteFilePath());
    if (fingerprint.size != before.size() || after.size() != before.size()
        || after.lastModified() != before.lastModified()) {
        return { {}, QStringLiteral("The external media source changed while it was being fingerprinted") };
    }

    source.fileName                = after.canonicalFilePath().isEmpty() ? after.absoluteFilePath()
                                                                       : after.canonicalFilePath();
    source.size                    = fingerprint.size;
    source.modifiedMsecsSinceEpoch = after.lastModified().toMSecsSinceEpoch();
    source.blobId                  = fingerprint.blobId;
    source.checksum                = fingerprint.checksum;
    source.chunkSize               = quint32(ExternalChunkSize);
    source.chunkHashes             = std::move(fingerprint.chunkHashes);
    if (const QString sourceError = writeExternalSource(reference.id, source); !sourceError.isEmpty())
        return { {}, sourceError };

    auto enriched     = reference;
    enriched.size     = source.size;
    enriched.blobId   = source.blobId;
    enriched.checksum = source.checksum;
    return { enriched, {} };
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

QString LocalMediaStore::writeExternalSource(const QUuid &attachmentId, const LocalMediaExternalSource &source) const
{
    if (attachmentId.isNull())
        return QStringLiteral("Invalid external media attachment id");
    QString keyError;
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return keyError;
    const auto sealed = SecureEnvelope::seal(serializeExternalSource(source), key, externalSourceContext(attachmentId));
    if (!sealed)
        return sealed.error.message;

    const QString path = externalSourcePath(attachmentId);
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

LocalMediaExternalSourceResult LocalMediaStore::loadExternalSource(const MediaReference &reference) const
{
    QString keyError;
    const auto key = masterKey(&keyError);
    if (key.isEmpty())
        return { {}, keyError };

    if (!reference.id.isNull()) {
        QFile file(externalSourcePath(reference.id));
        if (file.open(QIODevice::ReadOnly)) {
            const auto opened = SecureEnvelope::open(file.readAll(), key, externalSourceContext(reference.id));
            if (!opened)
                return { {}, opened.error.message };
            LocalMediaExternalSource source;
            if (!deserializeExternalSource(opened.value, &source))
                return { {}, QStringLiteral("Invalid external media source metadata") };
            return { source, {} };
        }
    }

    // Compatibility with the pre-lazy external-source format, which keyed the
    // encrypted locator by the already-computed content blob id.
    if (reference.blobId.isEmpty())
        return { {}, QStringLiteral("External media source metadata is unavailable") };
    QFile legacy(legacyExternalSourcePath(reference.blobId));
    if (!legacy.open(QIODevice::ReadOnly))
        return { {}, legacy.errorString() };
    const auto opened = SecureEnvelope::open(legacy.readAll(), key, legacyExternalSourceContext(reference.blobId));
    if (!opened)
        return { {}, opened.error.message };
    LocalMediaExternalSource source;
    if (!deserializeExternalSource(opened.value, &source))
        return { {}, QStringLiteral("Invalid external media source metadata") };
    if (source.blobId.isEmpty())
        source.blobId = reference.blobId;
    return { source, {} };
}

LocalMediaExternalSourceResult LocalMediaStore::externalSource(const MediaReference &reference) const
{
    if (!reference.isValid() || reference.size < 0)
        return { {}, QStringLiteral("Invalid media reference") };
    auto source = loadExternalSource(reference);
    if (!source)
        return source;
    if (source.value.size != reference.size)
        return { {}, QStringLiteral("External media source does not match its reference") };
    if (!reference.blobId.isEmpty() && !source.value.blobId.isEmpty() && source.value.blobId != reference.blobId)
        return { {}, QStringLiteral("External media source identity does not match its reference") };
    if (!reference.checksum.isEmpty() && !source.value.checksum.isEmpty()
        && source.value.checksum != reference.checksum) {
        return { {}, QStringLiteral("External media source checksum does not match its reference") };
    }

    const QFileInfo file(source.value.fileName);
    if (!file.isFile())
        return { {}, QStringLiteral("The external media source is no longer available") };
    if (!sourceRevisionMatches(source.value, file))
        return { {}, QStringLiteral("The external media source changed since it was linked") };
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

LocalMediaDataResult LocalMediaStore::data(const MediaReference &reference) const
{
    if (!reference.blobId.isEmpty() && containsManagedBlob(reference.blobId))
        return data(reference.blobId);

    const auto source = externalSource(reference);
    if (!source)
        return { {}, source.error };
    if (source.value.size > qint64(std::numeric_limits<qsizetype>::max()))
        return { {}, QStringLiteral("The external media source is too large to materialize in memory") };

    QFile file(source.value.fileName);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };
    QByteArray result;
    result.reserve(qsizetype(source.value.size));
    QCryptographicHash checksum(QCryptographicHash::Sha256);
    qsizetype index = 0;
    qint64 total = 0;
    while (total < source.value.size) {
        const qint64 expected = qMin<qint64>(ExternalChunkSize, source.value.size - total);
        const QByteArray chunk = file.read(expected);
        if (chunk.size() != expected)
            return { {}, file.errorString().isEmpty() ? QStringLiteral("Could not read external media source")
                                                      : file.errorString() };
        if (source.value.hasFingerprint()
            && QCryptographicHash::hash(chunk, QCryptographicHash::Sha256) != source.value.chunkHashes.at(index)) {
            return { {}, QStringLiteral("The external media source changed since it was fingerprinted") };
        }
        checksum.addData(chunk);
        result.append(chunk);
        total += chunk.size();
        ++index;
    }
    const QFileInfo after(source.value.fileName);
    if (!sourceRevisionMatches(source.value, after))
        return { {}, QStringLiteral("The external media source changed while it was being read") };
    if (source.value.hasFingerprint() && checksum.result() != source.value.checksum)
        return { {}, QStringLiteral("The external media source failed its integrity check") };
    return { result, {} };
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

    // Legacy external sources were content-addressed. New lazy external sources
    // require the MediaReference overload because their attachment id is the
    // local locator key before a blob id exists.
    MediaReference legacyReference;
    legacyReference.id           = QUuid::createUuid();
    legacyReference.blobId       = blobId;
    legacyReference.portableName = QStringLiteral("legacy-external");
    auto source = loadExternalSource(legacyReference);
    if (!source)
        return { {}, source.error };
    legacyReference.size     = source.value.size;
    legacyReference.checksum = source.value.checksum;
    return data(legacyReference);
}

bool LocalMediaStore::containsManagedBlob(const QByteArray &blobId) const
{
    return !blobId.isEmpty() && QFileInfo::exists(blobPath(blobId));
}

bool LocalMediaStore::contains(const MediaReference &reference) const
{
    if (!reference.blobId.isEmpty() && containsManagedBlob(reference.blobId))
        return true;
    return bool(externalSource(reference));
}

bool LocalMediaStore::contains(const QByteArray &blobId) const
{
    if (containsManagedBlob(blobId))
        return true;
    if (blobId.isEmpty())
        return false;
    MediaReference legacyReference;
    legacyReference.id           = QUuid::createUuid();
    legacyReference.blobId       = blobId;
    legacyReference.portableName = QStringLiteral("legacy-external");
    const auto source = loadExternalSource(legacyReference);
    return source && QFileInfo(source.value.fileName).isFile()
        && QFileInfo(source.value.fileName).size() == source.value.size;
}

} // namespace AnyKeep