#include "filedraftstore.h"
#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace AnyKeep {
namespace {
    constexpr quint32 PayloadMagic   = 0x514e4450; // QNDP
    constexpr quint16 PayloadVersion = 8;
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
        media.reserve(int(count));
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

    DraftStoreError error(DraftStoreError::Code code, const QString &message) { return { code, message }; }

    QByteArray serialize(const DraftRecord &record)
    {
        QByteArray  bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_5_10);
        out << PayloadMagic << PayloadVersion << record.id << quint8(record.state) << record.storageId
            << record.remoteNoteId << record.title << record.body << quint8(record.format) << record.tags
            << record.folderId << record.folderUserOverride << record.removeSourceStorageId << record.removeSourceNoteId
            << record.revision << record.updatedAt << record.lastError << record.retryAt << quint8(record.operation)
            << record.backendData;
        writeMedia(out, record.media);
        return bytes;
    }

    DraftStoreResult<DraftRecord> deserialize(const QByteArray &bytes)
    {
        DraftRecord record;
        quint32     magic;
        quint16     version;
        quint8      state;
        quint8      format;
        quint8      operation;
        QDataStream in(bytes);
        in.setVersion(QDataStream::Qt_5_10);
        in >> magic >> version;
        if (magic != PayloadMagic
            || (version != 4 && version != 5 && version != 6 && version != 7 && version != PayloadVersion))
            return { {}, error(DraftStoreError::Corrupt, QStringLiteral("Unsupported draft payload")) };
        in >> record.id >> state >> record.storageId >> record.remoteNoteId >> record.title >> record.body >> format
            >> record.tags;
        if (version >= 6)
            in >> record.folderId;
        if (version >= 8)
            in >> record.folderUserOverride;
        if (version >= 7)
            in >> record.removeSourceStorageId >> record.removeSourceNoteId;
        if (version >= 5)
            in >> record.revision;
        in >> record.updatedAt >> record.lastError >> record.retryAt >> operation >> record.backendData;
        if (version == 4)
            record.revision = 1;
        if (!readMedia(in, record.media))
            return { {}, error(DraftStoreError::Corrupt, QStringLiteral("Invalid draft media manifest")) };
        if (in.status() != QDataStream::Ok || record.id.isNull() || state > DraftRecord::NeedsRouting
            || format > Note::Html || operation > DraftRecord::Delete) {
            return { {}, error(DraftStoreError::Corrupt, QStringLiteral("Invalid draft payload")) };
        }
        record.state     = DraftRecord::State(state);
        record.format    = Note::Format(format);
        record.operation = DraftRecord::Operation(operation);
        return { record, {} };
    }
} // namespace

FileDraftStore::FileDraftStore(QString rootPath, QByteArray masterKey) :
    rootPath_(std::move(rootPath)), masterKey_(std::move(masterKey))
{
}

QByteArray FileDraftStore::generateMasterKey() { return SecureEnvelope::generateMasterKey(); }

bool FileDraftStore::cryptoAvailable() { return SecureEnvelope::isAvailable(); }

QString FileDraftStore::pathFor(const QUuid &id, DraftRecord::State) const
{
    return QDir(rootPath_).filePath(id.toString(QUuid::WithoutBraces) + QStringLiteral(".draft"));
}

QString FileDraftStore::findPath(const QUuid &id, DraftRecord::State *state) const
{
    if (state)
        *state = DraftRecord::Editing;
    const auto path = pathFor(id, DraftRecord::Editing);
    return QFileInfo::exists(path) ? path : QString();
}

DraftStoreError FileDraftStore::ensureDirectories() const
{
    if (masterKey_.size() != MasterKeySize)
        return error(DraftStoreError::Locked, QStringLiteral("Draft store master key is unavailable"));
    if (!cryptoAvailable())
        return error(DraftStoreError::CryptoUnavailable, QStringLiteral("AES-256-GCM is unavailable"));
    QDir dir;
    if (!dir.mkpath(rootPath_))
        return error(DraftStoreError::Io, QStringLiteral("Failed to create draft directory"));
    QFile::setPermissions(rootPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return {};
}

DraftStoreResult<QByteArray> FileDraftStore::encrypt(const QUuid &id, const QByteArray &plainText) const
{
    if (auto e = ensureDirectories())
        return { {}, e };
    const AeadContext context { KeyDomain::LocalDraft, QStringLiteral("anykeep-local-drafts"),
                                id.toString(QUuid::WithoutBraces), AeadContextSchema, QStringLiteral("draft") };
    auto              result = SecureEnvelope::seal(plainText, masterKey_, context);
    if (!result)
        return { {}, error(DraftStoreError::CryptoUnavailable, result.error.message) };
    return { result.value, {} };
}

DraftStoreResult<QByteArray> FileDraftStore::decrypt(const QUuid &id, const QByteArray &envelope) const
{
    if (masterKey_.size() != MasterKeySize)
        return { {}, error(DraftStoreError::Locked, QStringLiteral("Draft store master key is unavailable")) };
    const AeadContext context { KeyDomain::LocalDraft, QStringLiteral("anykeep-local-drafts"),
                                id.toString(QUuid::WithoutBraces), AeadContextSchema, QStringLiteral("draft") };
    auto              result = SecureEnvelope::open(envelope, masterKey_, context);
    if (!result)
        return { {}, error(DraftStoreError::Corrupt, result.error.message) };
    return { result.value, {} };
}

DraftStoreError FileDraftStore::write(const DraftRecord &source)
{
    if (source.id.isNull())
        return error(DraftStoreError::InvalidArgument, QStringLiteral("Draft id is empty"));
    auto record = source;
    if (!record.updatedAt.isValid())
        record.updatedAt = QDateTime::currentDateTimeUtc();
    auto encrypted = encrypt(record.id, serialize(record));
    if (!encrypted)
        return encrypted.error;
    QSaveFile file(pathFor(record.id, record.state));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return error(DraftStoreError::Io, file.errorString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(encrypted.value) != encrypted.value.size() || !file.commit())
        return error(DraftStoreError::Io, file.errorString());
    return {};
}

DraftStoreResult<DraftRecord> FileDraftStore::load(const QUuid &id) const
{
    const auto path = findPath(id);
    if (path.isEmpty())
        return { {}, error(DraftStoreError::NotFound, QStringLiteral("Draft was not found")) };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, error(DraftStoreError::Io, file.errorString()) };
    auto plainText = decrypt(id, file.readAll());
    if (!plainText)
        return { {}, plainText.error };
    auto record = deserialize(plainText.value);
    if (record && record.value.id != id)
        return { {}, error(DraftStoreError::Corrupt, QStringLiteral("Draft identity mismatch")) };
    return record;
}

DraftStoreResult<QList<DraftRecord>> FileDraftStore::records() const
{
    QList<DraftRecord> result;
    QDir               dir(rootPath_);
    if (!dir.exists())
        return { result, {} };
    for (const auto &name : dir.entryList({ QStringLiteral("*.draft") }, QDir::Files)) {
        const QUuid id(QFileInfo(name).completeBaseName());
        if (id.isNull())
            return { {}, error(DraftStoreError::Corrupt, QStringLiteral("Invalid draft filename")) };
        auto record = load(id);
        if (!record)
            return { {}, record.error };
        result.push_back(record.value);
    }
    return { result, {} };
}

DraftStoreError FileDraftStore::transition(const QUuid &id, DraftRecord::State state)
{
    auto record = load(id);
    if (!record)
        return record.error;
    record.value.state     = state;
    record.value.updatedAt = QDateTime::currentDateTimeUtc();
    return write(record.value);
}

DraftStoreError FileDraftStore::remove(const QUuid &id)
{
    const auto path = findPath(id);
    if (path.isEmpty())
        return error(DraftStoreError::NotFound, QStringLiteral("Draft was not found"));
    if (!QFile::remove(path))
        return error(DraftStoreError::Io, QStringLiteral("Failed to remove draft"));
    return {};
}

} // namespace AnyKeep
