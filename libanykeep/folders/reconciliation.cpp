#include "foldercatalog.h"

#include "private.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace AnyKeep {

using FolderCatalogPrivate::assignmentKey;
using FolderCatalogPrivate::error;
using FolderCatalogPrivate::folderKey;

FolderCatalogError FolderCatalog::reconcileProviderFolderPaths(const QString                             &storageId,
                                                               const QList<ProviderFolderPathAssignment> &assignments)
{
    if (storageId.trimmed().isEmpty())
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Storage id is required"));

    QList<ProviderFolderPathAssignment> normalizedAssignments;
    normalizedAssignments.reserve(assignments.size());
    QSet<QString> seenNoteIds;
    for (const auto &incoming : assignments) {
        if (incoming.noteId.isEmpty())
            return error(FolderCatalogError::InvalidArgument, QStringLiteral("Provider note id is required"));
        if (seenNoteIds.contains(incoming.noteId))
            return error(FolderCatalogError::AlreadyExists, QStringLiteral("Duplicate provider note path assignment"));
        seenNoteIds.insert(incoming.noteId);

        ProviderFolderPathAssignment normalized = incoming;
        for (auto &segment : normalized.path) {
            segment = segment.trimmed();
            if (segment.isEmpty()) {
                return error(FolderCatalogError::InvalidArgument,
                             QStringLiteral("Provider folder paths cannot contain empty segments"));
            }
        }
        normalizedAssignments.append(std::move(normalized));
    }

    auto       candidate = snapshot_;
    const auto hintIndex = [&candidate](const QString &providerId, const QStringList &path) {
        const auto key = FolderCatalog::pathKey(providerId, path);
        for (int index = 0; index < candidate.pathHints.size(); ++index) {
            const auto &hint = candidate.pathHints.at(index);
            if (FolderCatalog::pathKey(hint.storageId, hint.path) == key)
                return index;
        }
        return -1;
    };
    const auto assignmentIndex = [&candidate](const QString &providerId, const QString &noteId) {
        for (int index = 0; index < candidate.assignments.size(); ++index) {
            const auto &assignment = candidate.assignments.at(index);
            if (assignment.storageId == providerId && assignment.noteId == noteId)
                return index;
        }
        return -1;
    };
    const auto findChild = [&candidate](const QUuid &parentId, const QString &name) {
        const auto normalized = FolderCatalog::normalizedName(name);
        for (const auto &folder : candidate.folders) {
            if (!folder.tombstone && folder.parentId == parentId
                && FolderCatalog::normalizedName(folder.name) == normalized) {
                return folder.id;
            }
        }
        return QUuid {};
    };
    const auto nextSortOrder = [&candidate](const QUuid &parentId) {
        qint64 largest = 0;
        bool   found   = false;
        for (const auto &folder : candidate.folders) {
            if (folder.tombstone || folder.parentId != parentId)
                continue;
            largest = found ? std::max(largest, folder.sortOrder) : folder.sortOrder;
            found   = true;
        }
        if (!found)
            return qint64(0);
        if (largest > std::numeric_limits<qint64>::max() - 1024)
            return largest;
        return largest + 1024;
    };

    for (const auto &incoming : normalizedAssignments) {
        QUuid folderId;
        if (!incoming.path.isEmpty()) {
            const auto existingHint = hintIndex(storageId, incoming.path);
            if (existingHint >= 0) {
                folderId = candidate.pathHints.at(existingHint).folderId;
            } else {
                QUuid parentId;
                for (const auto &segment : incoming.path) {
                    auto childId = findChild(parentId, segment);
                    if (childId.isNull()) {
                        FolderRecord folder;
                        folder.id         = QUuid::createUuid();
                        folder.parentId   = parentId;
                        folder.name       = segment;
                        folder.sortOrder  = nextSortOrder(parentId);
                        folder.revision   = 1;
                        folder.modifiedAt = incoming.modifiedAt.isValid() ? incoming.modifiedAt : currentTime();
                        candidate.folders.append(folder);
                        childId = folder.id;
                    }
                    parentId = childId;
                }
                folderId = parentId;

                ProviderPathHint hint;
                hint.storageId  = storageId;
                hint.path       = incoming.path;
                hint.folderId   = folderId;
                hint.revision   = 1;
                hint.modifiedAt = incoming.modifiedAt.isValid() ? incoming.modifiedAt : currentTime();
                candidate.pathHints.append(std::move(hint));
            }
        }

        const bool tombstone = folderId.isNull();
        const auto index     = assignmentIndex(storageId, incoming.noteId);
        if (index < 0) {
            // An unclassified note has no local record. Creating tombstones
            // for every such remote note would make the catalog grow without
            // preserving any additional user choice.
            if (tombstone)
                continue;
            NoteFolderAssignment assignment;
            assignment.storageId  = storageId;
            assignment.noteId     = incoming.noteId;
            assignment.folderId   = folderId;
            assignment.revision   = 1;
            assignment.modifiedAt = incoming.modifiedAt.isValid() ? incoming.modifiedAt : currentTime();
            candidate.assignments.append(std::move(assignment));
            continue;
        }

        auto &current = candidate.assignments[index];
        if (current.folderId == folderId && current.tombstone == tombstone)
            continue;
        if (!incoming.modifiedAt.isValid())
            continue;
        if (current.modifiedAt.isValid()) {
            if (incoming.modifiedAt < current.modifiedAt)
                continue;
            if (incoming.modifiedAt == current.modifiedAt) {
                return error(FolderCatalogError::Conflict,
                             QStringLiteral("Conflicting provider folder assignment timestamps"));
            }
        }

        current.folderId   = folderId;
        current.tombstone  = tombstone;
        current.revision   = std::max<quint64>(current.revision + 1, 1);
        current.modifiedAt = incoming.modifiedAt;
    }

    return replaceSnapshot(std::move(candidate));
}

FolderCatalogError FolderCatalog::validate(const FolderCatalogSnapshot &snapshot)
{
    QHash<QUuid, const FolderRecord *> folders;
    folders.reserve(snapshot.folders.size());
    for (const auto &record : snapshot.folders) {
        if (record.id.isNull())
            return error(FolderCatalogError::InvalidArgument, QStringLiteral("Folder id is required"));
        if (folders.contains(record.id))
            return error(FolderCatalogError::AlreadyExists, QStringLiteral("Duplicate folder id"));
        if (!record.tombstone && record.name.trimmed().isEmpty())
            return error(FolderCatalogError::InvalidArgument, QStringLiteral("Folder name is required"));
        folders.insert(record.id, &record);
    }

    QSet<QString> siblingNames;
    for (const auto &record : snapshot.folders) {
        if (record.tombstone)
            continue;
        if (!record.parentId.isNull()) {
            const auto parent = folders.constFind(record.parentId);
            if (parent == folders.cend() || parent.value()->tombstone)
                return error(FolderCatalogError::NotFound, QStringLiteral("Folder parent was not found"));
        }
        const auto key = folderKey(record.parentId, normalizedName(record.name));
        if (siblingNames.contains(key))
            return error(FolderCatalogError::Conflict, QStringLiteral("Folder names must be unique among siblings"));
        siblingNames.insert(key);

        QSet<QUuid>         ancestors;
        const FolderRecord *current = &record;
        while (!current->parentId.isNull()) {
            if (ancestors.contains(current->id))
                return error(FolderCatalogError::Cycle, QStringLiteral("Folder hierarchy contains a cycle"));
            ancestors.insert(current->id);
            const auto parent = folders.constFind(current->parentId);
            if (parent == folders.cend() || parent.value()->tombstone)
                return error(FolderCatalogError::NotFound, QStringLiteral("Folder parent was not found"));
            current = parent.value();
        }
        if (ancestors.contains(current->id))
            return error(FolderCatalogError::Cycle, QStringLiteral("Folder hierarchy contains a cycle"));
    }

    QSet<QString> assignments;
    for (const auto &record : snapshot.assignments) {
        if (record.storageId.isEmpty() || record.noteId.isEmpty())
            return error(FolderCatalogError::InvalidArgument, QStringLiteral("Assignment identity is required"));
        const auto key = assignmentKey(record.storageId, record.noteId);
        if (assignments.contains(key))
            return error(FolderCatalogError::AlreadyExists, QStringLiteral("Duplicate note assignment"));
        assignments.insert(key);
        if (record.tombstone) {
            if (!record.folderId.isNull() || !record.previousFolderId.isNull() || record.recycledAt.isValid())
                return error(FolderCatalogError::InvalidArgument, QStringLiteral("Deleted assignment has a folder"));
            continue;
        }
        const auto folder = folders.constFind(record.folderId);
        if (record.folderId.isNull() || folder == folders.cend() || folder.value()->tombstone)
            return error(FolderCatalogError::NotFound, QStringLiteral("Assignment folder was not found"));
        if (!isRecycleBinId(record.folderId) && (!record.previousFolderId.isNull() || record.recycledAt.isValid())) {
            return error(FolderCatalogError::InvalidArgument,
                         QStringLiteral("Only recycled assignments may have restore metadata"));
        }
    }

    QSet<QString> pathHints;
    for (const auto &record : snapshot.pathHints) {
        if (record.storageId.trimmed().isEmpty() || record.path.isEmpty()) {
            return error(FolderCatalogError::InvalidArgument,
                         QStringLiteral("Provider path hint identity is required"));
        }
        for (const auto &segment : record.path) {
            if (segment.trimmed().isEmpty()) {
                return error(FolderCatalogError::InvalidArgument,
                             QStringLiteral("Provider path hints cannot contain empty segments"));
            }
        }
        const auto key = pathKey(record.storageId, record.path);
        if (pathHints.contains(key))
            return error(FolderCatalogError::AlreadyExists, QStringLiteral("Duplicate provider path hint"));
        pathHints.insert(key);
        const auto folder = folders.constFind(record.folderId);
        if (record.folderId.isNull() || folder == folders.cend() || folder.value()->tombstone)
            return error(FolderCatalogError::NotFound, QStringLiteral("Provider path hint folder was not found"));
    }
    return {};
}

QString FolderCatalog::normalizedName(const QString &name) { return name.trimmed().toCaseFolded(); }

QString FolderCatalog::pathKey(const QString &storageId, const QStringList &path)
{
    QString key = QString::number(storageId.size()) + QChar(0x1f) + storageId;
    for (const auto &segment : path) {
        const auto normalized = normalizedName(segment);
        key += QChar(0x1f) + QString::number(normalized.size()) + QChar(0x1e) + normalized;
    }
    return key;
}

bool FolderCatalog::incomingWins(quint64 incomingRevision, const QDateTime &incomingModifiedAt, quint64 currentRevision,
                                 const QDateTime &currentModifiedAt)
{
    if (incomingRevision != currentRevision)
        return incomingRevision > currentRevision;
    if (incomingModifiedAt.isValid() != currentModifiedAt.isValid())
        return incomingModifiedAt.isValid();
    return incomingModifiedAt > currentModifiedAt;
}

QDateTime FolderCatalog::currentTime() { return QDateTime::currentDateTimeUtc(); }

int FolderCatalog::indexOfFolder(const QUuid &id) const
{
    if (id.isNull())
        return -1;
    for (int index = 0; index < snapshot_.folders.size(); ++index) {
        if (snapshot_.folders.at(index).id == id)
            return index;
    }
    return -1;
}

int FolderCatalog::indexOfAssignment(const QString &storageId, const QString &noteId) const
{
    for (int index = 0; index < snapshot_.assignments.size(); ++index) {
        const auto &record = snapshot_.assignments.at(index);
        if (record.storageId == storageId && record.noteId == noteId)
            return index;
    }
    return -1;
}

int FolderCatalog::indexOfPathHint(const QString &storageId, const QStringList &path) const
{
    const auto key = pathKey(storageId, path);
    for (int index = 0; index < snapshot_.pathHints.size(); ++index) {
        const auto &hint = snapshot_.pathHints.at(index);
        if (pathKey(hint.storageId, hint.path) == key)
            return index;
    }
    return -1;
}

FolderCatalogError FolderCatalog::mutateFolder(const QUuid &id, const std::function<void(FolderRecord &)> &mutation)
{
    const auto index = indexOfFolder(id);
    if (index < 0 || snapshot_.folders.at(index).tombstone)
        return error(FolderCatalogError::NotFound, QStringLiteral("Folder was not found"));

    auto  candidate = snapshot_;
    auto &record    = candidate.folders[index];
    mutation(record);
    record.name       = record.name.trimmed();
    record.revision   = std::max<quint64>(record.revision + 1, 1);
    record.modifiedAt = currentTime();
    return replaceSnapshot(std::move(candidate));
}

} // namespace AnyKeep
