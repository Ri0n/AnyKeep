#include "filefoldercatalogstore.h"

#include "secureenvelope.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

namespace QtNote {
namespace {

    constexpr quint32 PayloadMagic           = 0x514e4643; // QNFC
    constexpr quint16 PayloadVersion         = 3;
    constexpr quint32 AeadContextSchema      = 1;
    constexpr quint32 MaximumFolderCount     = 100000;
    constexpr quint32 MaximumAssignmentCount = 1000000;
    constexpr quint32 MaximumPathHintCount   = 1000000;

    FolderCatalogError error(FolderCatalogError::Code code, const QString &message) { return { code, message }; }

    AeadContext context()
    {
        return { KeyDomain::LocalFolderCatalog, QStringLiteral("qtnote-folder-catalog"), QStringLiteral("global"),
                 AeadContextSchema, QStringLiteral("catalog") };
    }

    QByteArray serialize(const FolderCatalogSnapshot &snapshot)
    {
        QByteArray  bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_5_10);
        out << PayloadMagic << PayloadVersion << quint32(snapshot.folders.size())
            << quint32(snapshot.assignments.size()) << quint32(snapshot.pathHints.size());
        for (const auto &record : snapshot.folders) {
            out << record.id << record.parentId << record.name << record.sortOrder << record.collapsed
                << record.favorite << record.archived << record.revision << record.modifiedAt << record.tombstone;
        }
        for (const auto &record : snapshot.assignments) {
            out << record.storageId << record.noteId << record.folderId << record.revision << record.modifiedAt
                << record.tombstone << record.previousFolderId << record.recycledAt;
        }
        for (const auto &record : snapshot.pathHints) {
            out << record.storageId << record.path << record.folderId << record.revision << record.modifiedAt;
        }
        return bytes;
    }

    FolderCatalogResult<FolderCatalogSnapshot> deserialize(const QByteArray &bytes)
    {
        FolderCatalogSnapshot snapshot;
        quint32               magic           = 0;
        quint16               version         = 0;
        quint32               folderCount     = 0;
        quint32               assignmentCount = 0;
        quint32               pathHintCount   = 0;
        QDataStream           in(bytes);
        in.setVersion(QDataStream::Qt_5_10);
        in >> magic >> version >> folderCount >> assignmentCount;
        if (version >= 2)
            in >> pathHintCount;
        if (magic != PayloadMagic || (version != 1 && version != 2 && version != PayloadVersion) || folderCount > MaximumFolderCount
            || assignmentCount > MaximumAssignmentCount || pathHintCount > MaximumPathHintCount) {
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Unsupported folder catalog payload")) };
        }

        snapshot.folders.reserve(int(folderCount));
        for (quint32 index = 0; index < folderCount; ++index) {
            FolderRecord record;
            in >> record.id >> record.parentId >> record.name >> record.sortOrder >> record.collapsed >> record.favorite
                >> record.archived >> record.revision >> record.modifiedAt >> record.tombstone;
            snapshot.folders.append(std::move(record));
        }

        snapshot.assignments.reserve(int(assignmentCount));
        for (quint32 index = 0; index < assignmentCount; ++index) {
            NoteFolderAssignment record;
            in >> record.storageId >> record.noteId >> record.folderId >> record.revision >> record.modifiedAt
                >> record.tombstone;
            if (version >= 3)
                in >> record.previousFolderId >> record.recycledAt;
            snapshot.assignments.append(std::move(record));
        }
        snapshot.pathHints.reserve(int(pathHintCount));
        for (quint32 index = 0; index < pathHintCount; ++index) {
            ProviderPathHint record;
            in >> record.storageId >> record.path >> record.folderId >> record.revision >> record.modifiedAt;
            snapshot.pathHints.append(std::move(record));
        }
        if (in.status() != QDataStream::Ok || !in.atEnd())
            return { {}, error(FolderCatalogError::Corrupt, QStringLiteral("Invalid folder catalog payload")) };

        FolderCatalog catalog;
        if (const auto validation = catalog.replaceSnapshot(snapshot))
            return { {}, { FolderCatalogError::Corrupt, validation.message } };
        return { snapshot, {} };
    }

    QString recoverySuffix()
    {
        return QStringLiteral(".unrecoverable-%1-%2")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz")),
                 QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

} // namespace

FileFolderCatalogStore::FileFolderCatalogStore(QString filePath, QByteArray masterKey) :
    filePath_(std::move(filePath)), masterKey_(std::move(masterKey))
{
}

bool FileFolderCatalogStore::cryptoAvailable() { return SecureEnvelope::isAvailable(); }

FolderCatalogResult<FolderCatalogSnapshot> FileFolderCatalogStore::load() const { return loadPath(filePath_, true); }

FolderCatalogResult<FolderCatalogSnapshot> FileFolderCatalogStore::loadBackup() const
{
    return loadPath(backupFilePath(), false);
}

FolderCatalogError FileFolderCatalogStore::save(const FolderCatalogSnapshot &snapshot)
{
    if (const auto keyError = validateKey())
        return keyError;
    FolderCatalog catalog;
    if (const auto validation = catalog.replaceSnapshot(snapshot))
        return validation;

    if (QFile::exists(filePath_)) {
        const auto existing = readRaw(filePath_);
        if (!existing)
            return existing.error;
        const auto previous = load();
        if (!previous)
            return previous.error;
        if (const auto backupError = writeRaw(backupFilePath(), existing.value))
            return backupError;
    }

    const auto sealed = SecureEnvelope::seal(serialize(snapshot), masterKey_, context());
    if (!sealed)
        return error(FolderCatalogError::CryptoUnavailable, sealed.error.message);
    return writeRaw(filePath_, sealed.value);
}

bool FileFolderCatalogStore::hasBackup() const { return QFile::exists(backupFilePath()); }

QString FileFolderCatalogStore::backupFilePath() const { return filePath_ + QStringLiteral(".bak"); }

FolderCatalogError FileFolderCatalogStore::restoreBackup(QString *preservedPath)
{
    if (const auto keyError = validateKey())
        return keyError;
    const auto backup = loadBackup();
    if (!backup)
        return backup.error;
    const auto rawBackup = readRaw(backupFilePath());
    if (!rawBackup)
        return rawBackup.error;

    if (const auto quarantineError = quarantineExisting(preservedPath))
        return quarantineError;
    return writeRaw(filePath_, rawBackup.value);
}

FolderCatalogError FileFolderCatalogStore::recreate(QString *preservedPath)
{
    if (const auto keyError = validateKey())
        return keyError;
    if (const auto quarantineError = quarantineExisting(preservedPath))
        return quarantineError;
    return save({});
}

FolderCatalogResult<FolderCatalogSnapshot> FileFolderCatalogStore::loadPath(const QString &path,
                                                                            bool           absentIsEmpty) const
{
    if (const auto keyError = validateKey())
        return { {}, keyError };
    if (!QFile::exists(path)) {
        if (absentIsEmpty)
            return { {}, {} };
        return { {}, error(FolderCatalogError::NotFound, QStringLiteral("Folder catalog backup was not found")) };
    }
    const auto raw = readRaw(path);
    if (!raw)
        return { {}, raw.error };
    const auto opened = SecureEnvelope::open(raw.value, masterKey_, context());
    if (!opened)
        return { {}, error(FolderCatalogError::Corrupt, opened.error.message) };
    return deserialize(opened.value);
}

FolderCatalogResult<QByteArray> FileFolderCatalogStore::readRaw(const QString &path) const
{
    if (path.isEmpty())
        return { {}, error(FolderCatalogError::InvalidArgument, QStringLiteral("Folder catalog path is empty")) };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, error(FolderCatalogError::Io, file.errorString()) };
    return { file.readAll(), {} };
}

FolderCatalogError FileFolderCatalogStore::writeRaw(const QString &path, const QByteArray &bytes) const
{
    if (path.isEmpty())
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Folder catalog path is empty"));
    const auto directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory))
        return error(FolderCatalogError::Io, QStringLiteral("Failed to create folder catalog directory"));

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return error(FolderCatalogError::Io, file.errorString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return error(FolderCatalogError::Io, file.errorString());
    return {};
}

FolderCatalogError FileFolderCatalogStore::validateKey() const
{
    if (filePath_.isEmpty())
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Folder catalog path is empty"));
    if (masterKey_.size() != SecureEnvelope::MasterKeySize)
        return error(FolderCatalogError::Locked, QStringLiteral("Folder catalog key is unavailable"));
    if (!cryptoAvailable())
        return error(FolderCatalogError::CryptoUnavailable, QStringLiteral("AES-256-GCM is unavailable"));
    return {};
}

FolderCatalogError FileFolderCatalogStore::quarantineExisting(QString *preservedPath) const
{
    if (preservedPath)
        preservedPath->clear();
    if (!QFile::exists(filePath_) && !QFile::exists(backupFilePath()))
        return {};

    const auto suffix = recoverySuffix();
    QString    preserved;
    if (QFile::exists(filePath_)) {
        preserved = filePath_ + suffix;
        if (!QFile::rename(filePath_, preserved))
            return error(FolderCatalogError::Io, QStringLiteral("Failed to preserve the existing folder catalog"));
    }
    if (QFile::exists(backupFilePath())) {
        const auto backupPreserved = filePath_ + suffix + QStringLiteral(".bak");
        if (!QFile::rename(backupFilePath(), backupPreserved))
            return error(FolderCatalogError::Io, QStringLiteral("Failed to preserve the folder catalog backup"));
        if (preserved.isEmpty())
            preserved = backupPreserved;
    }
    if (preservedPath)
        *preservedPath = preserved;
    return {};
}

} // namespace QtNote
