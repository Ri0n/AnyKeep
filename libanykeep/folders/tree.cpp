#include "foldercatalog.h"

#include "private.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <limits>

namespace AnyKeep {

using FolderCatalogPrivate::error;

FolderCatalogResult<QUuid> FolderCatalog::addFolder(FolderRecord record)
{
    record.name = record.name.trimmed();
    if (record.id.isNull())
        record.id = QUuid::createUuid();
    if (record.revision == 0)
        record.revision = 1;
    if (!record.modifiedAt.isValid())
        record.modifiedAt = currentTime();
    if (record.tombstone)
        return { {}, error(FolderCatalogError::InvalidArgument, QStringLiteral("Cannot add a deleted folder")) };
    if (indexOfFolder(record.id) >= 0)
        return { {}, error(FolderCatalogError::AlreadyExists, QStringLiteral("Folder id already exists")) };

    auto candidate = snapshot_;
    candidate.folders.append(record);
    if (const auto validation = validate(candidate))
        return { {}, validation };
    snapshot_ = std::move(candidate);
    return { record.id, {} };
}

FolderCatalogError FolderCatalog::updateFolder(FolderRecord record)
{
    record.name      = record.name.trimmed();
    const auto index = indexOfFolder(record.id);
    if (index < 0)
        return error(FolderCatalogError::NotFound, QStringLiteral("Folder was not found"));
    if (record.revision <= snapshot_.folders.at(index).revision)
        record.revision = snapshot_.folders.at(index).revision + 1;
    if (!record.modifiedAt.isValid())
        record.modifiedAt = currentTime();

    auto candidate           = snapshot_;
    candidate.folders[index] = std::move(record);
    return replaceSnapshot(std::move(candidate));
}

FolderCatalogError FolderCatalog::renameFolder(const QUuid &id, const QString &name)
{
    return mutateFolder(id, [&name](FolderRecord &record) { record.name = name.trimmed(); });
}

FolderCatalogError FolderCatalog::moveFolder(const QUuid &id, const QUuid &parentId, qint64 sortOrder)
{
    return mutateFolder(id, [parentId, sortOrder](FolderRecord &record) {
        record.parentId  = parentId;
        record.sortOrder = sortOrder;
    });
}

FolderCatalogError FolderCatalog::moveFolderRelative(const QUuid &id, const QUuid &parentId, const QUuid &beforeId)
{
    const auto sourceIndex = indexOfFolder(id);
    if (sourceIndex < 0 || snapshot_.folders.at(sourceIndex).tombstone)
        return error(FolderCatalogError::NotFound, QStringLiteral("Folder was not found"));
    if (!parentId.isNull() && !folder(parentId))
        return error(FolderCatalogError::NotFound, QStringLiteral("Folder parent was not found"));
    if (beforeId == id)
        return {};

    const auto   source         = snapshot_.folders.at(sourceIndex);
    const auto   targetSiblings = children(parentId);
    QList<QUuid> targetIds;
    targetIds.reserve(targetSiblings.size() + 1);
    for (const auto &sibling : targetSiblings) {
        if (sibling.id != id)
            targetIds.append(sibling.id);
    }

    qsizetype insertionIndex = targetIds.size();
    if (!beforeId.isNull()) {
        const auto beforeIndex = targetIds.indexOf(beforeId);
        if (beforeIndex < 0) {
            return error(FolderCatalogError::InvalidArgument,
                         QStringLiteral("The folder used as the insertion boundary was not found"));
        }
        insertionIndex = beforeIndex;
    }
    targetIds.insert(insertionIndex, id);

    QList<QUuid> currentTargetIds;
    currentTargetIds.reserve(targetSiblings.size());
    for (const auto &sibling : targetSiblings)
        currentTargetIds.append(sibling.id);
    if (source.parentId == parentId && currentTargetIds == targetIds)
        return {};

    auto              candidate = snapshot_;
    QHash<QUuid, int> indexes;
    indexes.reserve(candidate.folders.size());
    for (int index = 0; index < candidate.folders.size(); ++index)
        indexes.insert(candidate.folders.at(index).id, index);

    const auto modifiedAt = currentTime();
    bool       changed    = false;
    const auto normalize
        = [&candidate, &indexes, modifiedAt, &changed](const QList<QUuid> &ids, const QUuid &expectedParent) {
              for (qsizetype order = 0; order < ids.size(); ++order) {
                  const auto index = indexes.constFind(ids.at(order));
                  if (index == indexes.cend())
                      return false;

                  auto      &record   = candidate.folders[index.value()];
                  const auto newOrder = qint64(order) * 1024;
                  if (record.parentId == expectedParent && record.sortOrder == newOrder)
                      continue;
                  record.parentId   = expectedParent;
                  record.sortOrder  = newOrder;
                  record.revision   = std::max<quint64>(record.revision + 1, 1);
                  record.modifiedAt = modifiedAt;
                  changed           = true;
              }
              return true;
          };

    if (source.parentId != parentId) {
        const auto   sourceSiblings = children(source.parentId);
        QList<QUuid> sourceIds;
        sourceIds.reserve(sourceSiblings.size());
        for (const auto &sibling : sourceSiblings) {
            if (sibling.id != id)
                sourceIds.append(sibling.id);
        }
        if (!normalize(sourceIds, source.parentId))
            return error(FolderCatalogError::NotFound, QStringLiteral("Folder was not found"));
    }
    if (!normalize(targetIds, parentId))
        return error(FolderCatalogError::NotFound, QStringLiteral("Folder was not found"));
    return changed ? replaceSnapshot(std::move(candidate)) : FolderCatalogError {};
}

FolderCatalogError FolderCatalog::setFolderCollapsed(const QUuid &id, bool collapsed)
{
    return mutateFolder(id, [collapsed](FolderRecord &record) { record.collapsed = collapsed; });
}

FolderCatalogError FolderCatalog::setAllFoldersCollapsed(bool collapsed)
{
    auto       candidate  = snapshot_;
    bool       changed    = false;
    const auto modifiedAt = currentTime();
    for (auto &record : candidate.folders) {
        if (record.tombstone || record.collapsed == collapsed)
            continue;
        record.collapsed  = collapsed;
        record.revision   = std::max<quint64>(record.revision + 1, 1);
        record.modifiedAt = modifiedAt;
        changed           = true;
    }
    return changed ? replaceSnapshot(std::move(candidate)) : FolderCatalogError {};
}

FolderCatalogError FolderCatalog::setFolderFlags(const QUuid &id, bool favorite, bool archived)
{
    return mutateFolder(id, [favorite, archived](FolderRecord &record) {
        record.favorite = favorite;
        record.archived = archived;
    });
}

FolderCatalogResult<DeletedFolderBranch> FolderCatalog::trashFolderBranch(const QUuid &id)
{
    if (id.isNull() || isRecycleBinId(id))
        return { {}, error(FolderCatalogError::InvalidArgument, QStringLiteral("A deletable folder is required")) };
    if (!folder(id))
        return { {}, error(FolderCatalogError::NotFound, QStringLiteral("Folder was not found")) };

    DeletedFolderBranch branch;
    branch.rootId = id;

    QSet<QUuid>  branchIds;
    QList<QUuid> pending { id };
    while (!pending.isEmpty()) {
        const QUuid currentId = pending.takeFirst();
        if (branchIds.contains(currentId))
            continue;
        const auto *current = folder(currentId);
        if (!current)
            continue;
        branchIds.insert(currentId);
        branch.folders.append(*current);
        for (const auto &child : children(currentId))
            pending.append(child.id);
    }

    for (const auto &assignment : snapshot_.assignments) {
        if (!assignment.tombstone && branchIds.contains(assignment.folderId))
            branch.assignments.append(assignment);
    }
    for (const auto &hint : snapshot_.pathHints) {
        if (branchIds.contains(hint.folderId))
            branch.pathHints.append(hint);
    }

    auto       candidate = snapshot_;
    const auto now       = currentTime();
    const auto recycleId = recycleBinId();
    if (!branch.assignments.isEmpty()) {
        auto recyclePos = -1;
        for (int index = 0; index < candidate.folders.size(); ++index) {
            if (candidate.folders.at(index).id == recycleId) {
                recyclePos = index;
                break;
            }
        }
        if (recyclePos < 0) {
            FolderRecord recycle;
            recycle.id         = recycleId;
            recycle.name       = QStringLiteral("Recycle Bin");
            recycle.sortOrder  = std::numeric_limits<qint64>::max() / 4;
            recycle.archived   = true;
            recycle.revision   = 1;
            recycle.modifiedAt = now;
            candidate.folders.append(std::move(recycle));
        }
    }

    for (auto &assignment : candidate.assignments) {
        if (assignment.tombstone || !branchIds.contains(assignment.folderId))
            continue;
        assignment.folderId = recycleId;
        // Folder deletion keeps the restore mapping only in DeletedFolderBranch.
        assignment.previousFolderId = {};
        assignment.recycledAt       = now;
        assignment.tombstone        = false;
        assignment.revision         = std::max<quint64>(assignment.revision + 1, 1);
        assignment.modifiedAt       = now;
    }

    for (auto &record : candidate.folders) {
        if (!branchIds.contains(record.id))
            continue;
        record.parentId = {};
        record.name.clear();
        record.sortOrder  = 0;
        record.collapsed  = false;
        record.favorite   = false;
        record.archived   = false;
        record.tombstone  = true;
        record.revision   = std::max<quint64>(record.revision + 1, 1);
        record.modifiedAt = now;
    }

    candidate.pathHints.erase(
        std::remove_if(candidate.pathHints.begin(), candidate.pathHints.end(),
                       [&branchIds](const ProviderPathHint &hint) { return branchIds.contains(hint.folderId); }),
        candidate.pathHints.end());

    if (const auto validation = validate(candidate))
        return { {}, validation };
    snapshot_ = std::move(candidate);
    return { branch, {} };
}

FolderCatalogError FolderCatalog::restoreFolderBranch(const DeletedFolderBranch &branch)
{
    if (branch.rootId.isNull() || branch.folders.isEmpty())
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Deleted folder data is required"));

    auto        candidate = snapshot_;
    const auto  now       = currentTime();
    QSet<QUuid> branchIds;
    for (const auto &saved : branch.folders)
        branchIds.insert(saved.id);
    if (!branchIds.contains(branch.rootId))
        return error(FolderCatalogError::InvalidArgument, QStringLiteral("Deleted folder root was not found"));

    for (const auto &saved : branch.folders) {
        const auto index = [&candidate, &saved]() {
            for (int i = 0; i < candidate.folders.size(); ++i) {
                if (candidate.folders.at(i).id == saved.id)
                    return i;
            }
            return -1;
        }();
        if (index < 0)
            return error(FolderCatalogError::NotFound, QStringLiteral("Deleted folder tombstone was not found"));
        if (!candidate.folders.at(index).tombstone)
            return error(FolderCatalogError::Conflict, QStringLiteral("A deleted folder has already been recreated"));

        FolderRecord restored    = saved;
        restored.tombstone       = false;
        restored.revision        = std::max(candidate.folders.at(index).revision + 1, saved.revision + 1);
        restored.modifiedAt      = now;
        candidate.folders[index] = std::move(restored);
    }

    for (const auto &saved : branch.assignments) {
        auto index = -1;
        for (int i = 0; i < candidate.assignments.size(); ++i) {
            if (candidate.assignments.at(i).storageId == saved.storageId
                && candidate.assignments.at(i).noteId == saved.noteId) {
                index = i;
                break;
            }
        }
        if (index < 0)
            continue;
        auto &current = candidate.assignments[index];
        // Do not override a note the user moved or restored after deleting the folder.
        if (current.tombstone || !isRecycleBinId(current.folderId))
            continue;
        current.folderId         = saved.folderId;
        current.previousFolderId = saved.previousFolderId;
        current.recycledAt       = saved.recycledAt;
        current.tombstone        = saved.tombstone;
        current.revision         = std::max(current.revision + 1, saved.revision + 1);
        current.modifiedAt       = now;
    }

    for (const auto &saved : branch.pathHints) {
        const auto key      = pathKey(saved.storageId, saved.path);
        const auto existing = std::find_if(
            candidate.pathHints.cbegin(), candidate.pathHints.cend(),
            [&key](const ProviderPathHint &current) { return pathKey(current.storageId, current.path) == key; });
        // A provider may have reused the same path after the deletion. Undo
        // must not overwrite that newer association.
        if (existing != candidate.pathHints.cend())
            continue;
        ProviderPathHint restored = saved;
        restored.revision         = std::max<quint64>(saved.revision + 1, 1);
        restored.modifiedAt       = now;
        candidate.pathHints.append(std::move(restored));
    }

    if (const auto validation = validate(candidate))
        return validation;
    snapshot_ = std::move(candidate);
    return {};
}

} // namespace AnyKeep
