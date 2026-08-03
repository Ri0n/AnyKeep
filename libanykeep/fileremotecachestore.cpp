#include "fileremotecachestore.h"

#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

namespace AnyKeep {
namespace {
    constexpr quint32 PayloadMagic   = 0x514e5243; // QNRC
    constexpr quint16 PayloadVersion = 3;
    // Consumer schema passed to SecureEnvelope::associatedData(); see AeadContext::schema.
    constexpr quint32 AeadContextSchema = 1;

    void writeMedia(QDataStream &out, const QList<MediaReference> &media)
    {
        out << quint32(media.size());
        for (const auto &item : media)
            out << item.id << item.blobId << item.originalName << item.portableName << item.mediaType << item.size
                << item.checksum << item.remoteData;
    }

    bool readMedia(QDataStream &in, QList<MediaReference> &media)
    {
        quint32 count = 0;
        in >> count;
        if (count > 10000)
            return false;
        for (quint32 i = 0; i < count; ++i) {
            MediaReference item;
            in >> item.id >> item.blobId >> item.originalName >> item.portableName >> item.mediaType >> item.size
                >> item.checksum >> item.remoteData;
            if (!item.isValid() || item.size < 0)
                return false;
            media.append(std::move(item));
        }
        return true;
    }

    RemoteCacheError error(RemoteCacheError::Code code, const QString &message) { return { code, message }; }

    QByteArray serialize(const QList<RemoteCacheRecord> &records)
    {
        QByteArray  bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_5_10);
        out << PayloadMagic << PayloadVersion << quint32(records.size());
        for (const auto &record : records) {
            out << record.id << record.title << record.tags << record.modified << quint8(record.format) << record.body
                << record.bodyPresent << record.backendData << quint8(record.syncState) << record.lastOpenedAt
                << record.cachedAt << record.folderId;
            writeMedia(out, record.media);
        }
        return bytes;
    }

    RemoteCacheResult<QList<RemoteCacheRecord>> deserialize(const QByteArray &bytes)
    {
        quint32     magic   = 0;
        quint16     version = 0;
        quint32     count   = 0;
        QDataStream in(bytes);
        in.setVersion(QDataStream::Qt_5_10);
        in >> magic >> version >> count;
        if (magic != PayloadMagic || (version != 2 && version != PayloadVersion) || count > 1000000)
            return { {}, error(RemoteCacheError::Corrupt, QStringLiteral("Unsupported remote cache payload")) };

        QList<RemoteCacheRecord> records;
        records.reserve(int(count));
        QSet<QString> ids;
        for (quint32 i = 0; i < count; ++i) {
            RemoteCacheRecord record;
            quint8            format = 0;
            quint8            state  = 0;
            in >> record.id >> record.title >> record.tags >> record.modified >> format >> record.body
                >> record.bodyPresent >> record.backendData >> state >> record.lastOpenedAt >> record.cachedAt;
            if (version >= 3)
                in >> record.folderId;
            if (!readMedia(in, record.media))
                return { {}, error(RemoteCacheError::Corrupt, QStringLiteral("Invalid remote cache media manifest")) };
            if (record.id.isEmpty() || ids.contains(record.id) || format > Note::Html
                || state > RemoteCacheRecord::Conflict) {
                return { {}, error(RemoteCacheError::Corrupt, QStringLiteral("Invalid remote cache record")) };
            }
            record.format    = Note::Format(format);
            record.syncState = RemoteCacheRecord::SyncState(state);
            if (!record.bodyPresent)
                record.body.clear();
            ids.insert(record.id);
            records.append(std::move(record));
        }
        if (in.status() != QDataStream::Ok || !in.atEnd())
            return { {}, error(RemoteCacheError::Corrupt, QStringLiteral("Invalid remote cache payload")) };
        return { records, {} };
    }

    AeadContext context(const QString &instanceId)
    {
        return { KeyDomain::LocalRemoteCache, QStringLiteral("anykeep-remote-cache"), instanceId, AeadContextSchema,
                 QStringLiteral("records") };
    }
} // namespace

FileRemoteCacheStore::FileRemoteCacheStore(QString filePath, QString instanceId, QByteArray masterKey) :
    filePath_(std::move(filePath)), instanceId_(std::move(instanceId)), masterKey_(std::move(masterKey))
{
}

RemoteCacheResult<QList<RemoteCacheRecord>> FileRemoteCacheStore::records() const
{
    if (instanceId_.isEmpty() || masterKey_.size() != SecureEnvelope::MasterKeySize)
        return { {}, error(RemoteCacheError::Locked, QStringLiteral("Remote cache key is unavailable")) };
    QFile file(filePath_);
    if (!file.exists())
        return { {}, {} };
    if (!file.open(QIODevice::ReadOnly))
        return { {}, error(RemoteCacheError::Io, file.errorString()) };
    const auto opened = SecureEnvelope::open(file.readAll(), masterKey_, context(instanceId_));
    if (!opened)
        return { {}, error(RemoteCacheError::Corrupt, opened.error.message) };
    return deserialize(opened.value);
}

RemoteCacheError FileRemoteCacheStore::replaceRecords(const QList<RemoteCacheRecord> &source)
{
    if (instanceId_.isEmpty() || masterKey_.size() != SecureEnvelope::MasterKeySize)
        return error(RemoteCacheError::Locked, QStringLiteral("Remote cache key is unavailable"));
    QSet<QString> ids;
    auto          records = source;
    const auto    now     = QDateTime::currentDateTimeUtc();
    for (auto &record : records) {
        if (record.id.isEmpty() || ids.contains(record.id))
            return error(RemoteCacheError::InvalidArgument, QStringLiteral("Remote cache record id is invalid"));
        ids.insert(record.id);
        if (!record.cachedAt.isValid())
            record.cachedAt = now;
        if (!record.bodyPresent)
            record.body.clear();
    }
    const auto sealed = SecureEnvelope::seal(serialize(records), masterKey_, context(instanceId_));
    if (!sealed)
        return error(RemoteCacheError::CryptoUnavailable, sealed.error.message);
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return error(RemoteCacheError::Io, QStringLiteral("Failed to create remote cache directory"));
    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return error(RemoteCacheError::Io, file.errorString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(sealed.value) != sealed.value.size() || !file.commit())
        return error(RemoteCacheError::Io, file.errorString());
    return {};
}

RemoteCacheError FileRemoteCacheStore::put(const RemoteCacheRecord &record)
{
    if (record.id.isEmpty())
        return error(RemoteCacheError::InvalidArgument, QStringLiteral("Remote cache record id is empty"));
    auto loaded = records();
    if (!loaded)
        return loaded.error;
    bool replaced = false;
    for (auto &current : loaded.value) {
        if (current.id == record.id) {
            current  = record;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        loaded.value.append(record);
    return replaceRecords(loaded.value);
}

RemoteCacheError FileRemoteCacheStore::remove(const QString &id)
{
    if (id.isEmpty())
        return error(RemoteCacheError::InvalidArgument, QStringLiteral("Remote cache record id is empty"));
    auto loaded = records();
    if (!loaded)
        return loaded.error;
    for (int i = 0; i < loaded.value.size(); ++i) {
        if (loaded.value.at(i).id == id) {
            loaded.value.removeAt(i);
            return replaceRecords(loaded.value);
        }
    }
    return {};
}

} // namespace AnyKeep
