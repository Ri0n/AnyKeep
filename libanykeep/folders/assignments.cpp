#include "foldercatalog.h"

#include "private.h"

#include <algorithm>

namespace AnyKeep {

using FolderCatalogPrivate::error;

FolderCatalogError FolderCatalog::assignNote(const QString &storageId, const QString &noteId, const QUuid &folderId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Storage id and note id are required"));
    if (!folder(folderId))
        return error(FolderCatalogError::NotFound, QStringLiteral("Target folder was not found"));

    auto       candidate = snapshot_;
    const auto index     = indexOfAssignment(storageId, noteId);
    if (index < 0) {
        NoteFolderAssignment record;
        record.storageId  = storageId;
        record.noteId     = noteId;
        record.folderId   = folderId;
        record.revision   = 1;
        record.modifiedAt = currentTime();
        candidate.assignments.append(std::move(record));
    } else {
        auto &record            = candidate.assignments[index];
        record.folderId         = folderId;
        record.previousFolderId = {};
        record.recycledAt       = {};
        record.tombstone        = false;
        record.revision         = std::max<quint64>(record.revision + 1, 1);
        record.modifiedAt       = currentTime();
    }
    return replaceSnapshot(std::move(candidate));
}

FolderCatalogError FolderCatalog::clearNoteAssignment(const QString &storageId, const QString &noteId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Storage id and note id are required"));

    auto       candidate = snapshot_;
    const auto index     = indexOfAssignment(storageId, noteId);
    if (index < 0) {
        NoteFolderAssignment record;
        record.storageId  = storageId;
        record.noteId     = noteId;
        record.revision   = 1;
        record.modifiedAt = currentTime();
        record.tombstone  = true;
        candidate.assignments.append(std::move(record));
    } else {
        auto &record            = candidate.assignments[index];
        record.folderId         = {};
        record.previousFolderId = {};
        record.recycledAt       = {};
        record.tombstone        = true;
        record.revision         = std::max<quint64>(record.revision + 1, 1);
        record.modifiedAt       = currentTime();
    }
    return replaceSnapshot(std::move(candidate));
}

FolderCatalogError FolderCatalog::recycleNote(const QString &storageId, const QString &noteId,
                                              const QUuid &previousFolderId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Storage id and note id are required"));

    auto       candidate  = snapshot_;
    const auto trashId    = recycleBinId();
    auto       trashIndex = -1;
    for (int index = 0; index < candidate.folders.size(); ++index) {
        if (candidate.folders.at(index).id == trashId) {
            trashIndex = index;
            break;
        }
    }
    if (trashIndex < 0) {
        FolderRecord trash;
        trash.id         = trashId;
        trash.name       = QStringLiteral("Recycle Bin");
        trash.sortOrder  = std::numeric_limits<qint64>::max() / 4;
        trash.archived   = true;
        trash.revision   = 1;
        trash.modifiedAt = currentTime();
        candidate.folders.append(std::move(trash));
    }

    const auto index = indexOfAssignment(storageId, noteId);
    const auto now   = currentTime();
    if (index < 0) {
        NoteFolderAssignment record;
        record.storageId        = storageId;
        record.noteId           = noteId;
        record.folderId         = trashId;
        record.previousFolderId = previousFolderId;
        record.recycledAt       = now;
        record.revision         = 1;
        record.modifiedAt       = now;
        candidate.assignments.append(std::move(record));
    } else {
        auto &record = candidate.assignments[index];
        if (!record.tombstone && isRecycleBinId(record.folderId))
            return {};
        record.folderId         = trashId;
        record.previousFolderId = previousFolderId;
        record.recycledAt       = now;
        record.tombstone        = false;
        record.revision         = std::max<quint64>(record.revision + 1, 1);
        record.modifiedAt       = now;
    }
    return replaceSnapshot(std::move(candidate));
}

FolderCatalogResult<QUuid> FolderCatalog::restoreRecycledNote(const QString &storageId, const QString &noteId)
{
    const auto index = indexOfAssignment(storageId, noteId);
    if (index < 0 || snapshot_.assignments.at(index).tombstone
        || !isRecycleBinId(snapshot_.assignments.at(index).folderId)) {
        return { {}, error(FolderCatalogError::NotFound, QStringLiteral("Recycled note was not found")) };
    }

    auto        candidate       = snapshot_;
    auto       &record          = candidate.assignments[index];
    const QUuid previous        = record.previousFolderId;
    const bool  restoreToFolder = !previous.isNull() && folder(previous);
    if (restoreToFolder) {
        record.folderId  = previous;
        record.tombstone = false;
    } else {
        record.folderId  = {};
        record.tombstone = true;
    }
    record.previousFolderId = {};
    record.recycledAt       = {};
    record.revision         = std::max<quint64>(record.revision + 1, 1);
    record.modifiedAt       = currentTime();
    if (const auto validation = validate(candidate))
        return { {}, validation };
    snapshot_ = std::move(candidate);
    return { restoreToFolder ? previous : QUuid {}, {} };
}

} // namespace AnyKeep
