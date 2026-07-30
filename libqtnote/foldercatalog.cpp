#include "foldercatalog.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <functional>
#include <limits>

namespace QtNote {
namespace {

    FolderCatalogError error(FolderCatalogError::Code code, const QString &message) { return { code, message }; }

    QString folderKey(const QUuid &parentId, const QString &name)
    {
        return parentId.toString(QUuid::WithoutBraces) + QChar(0x1f) + name;
    }

    QString assignmentKey(const QString &storageId, const QString &noteId)
    {
        return QString::number(storageId.size()) + QChar(0x1f) + storageId + noteId;
    }

    int siblingPlacement(const FolderRecord &record)
    {
        if (FolderCatalog::isRecycleBinId(record.id))
            return 2;
        return record.archived ? 1 : 0;
    }

    bool sameFolder(const FolderRecord &left, const FolderRecord &right)
    {
        return left.id == right.id && left.parentId == right.parentId && left.name == right.name
            && left.sortOrder == right.sortOrder && left.collapsed == right.collapsed && left.favorite == right.favorite
            && left.archived == right.archived && left.revision == right.revision && left.modifiedAt == right.modifiedAt
            && left.tombstone == right.tombstone;
    }

    bool sameAssignment(const NoteFolderAssignment &left, const NoteFolderAssignment &right)
    {
        return left.storageId == right.storageId && left.noteId == right.noteId && left.folderId == right.folderId
            && left.previousFolderId == right.previousFolderId && left.recycledAt == right.recycledAt
            && left.revision == right.revision && left.modifiedAt == right.modifiedAt
            && left.tombstone == right.tombstone;
    }

    bool samePathHint(const ProviderPathHint &left, const ProviderPathHint &right)
    {
        return left.storageId == right.storageId && left.path == right.path && left.folderId == right.folderId
            && left.revision == right.revision && left.modifiedAt == right.modifiedAt;
    }

} // namespace

FolderCatalogError FolderCatalog::replaceSnapshot(FolderCatalogSnapshot snapshot)
{
    if (const auto validation = validate(snapshot))
        return validation;
    snapshot_ = std::move(snapshot);
    return {};
}

FolderCatalogError FolderCatalog::merge(const FolderCatalogSnapshot &incoming)
{
    if (const auto validation = validate(incoming))
        return validation;

    auto              candidate = snapshot_;
    QHash<QUuid, int> folders;
    folders.reserve(candidate.folders.size());
    for (int index = 0; index < candidate.folders.size(); ++index)
        folders.insert(candidate.folders.at(index).id, index);

    for (const auto &record : incoming.folders) {
        const auto found = folders.constFind(record.id);
        if (found == folders.cend()) {
            folders.insert(record.id, candidate.folders.size());
            candidate.folders.append(record);
        } else if (incomingWins(record.revision, record.modifiedAt, candidate.folders.at(found.value()).revision,
                                candidate.folders.at(found.value()).modifiedAt)) {
            candidate.folders[found.value()] = record;
        } else if (record.revision == candidate.folders.at(found.value()).revision
                   && record.modifiedAt == candidate.folders.at(found.value()).modifiedAt
                   && !sameFolder(record, candidate.folders.at(found.value()))) {
            return error(FolderCatalogError::Conflict, QStringLiteral("Conflicting folder revisions"));
        }
    }

    QHash<QString, int> assignments;
    assignments.reserve(candidate.assignments.size());
    for (int index = 0; index < candidate.assignments.size(); ++index) {
        const auto &record = candidate.assignments.at(index);
        assignments.insert(assignmentKey(record.storageId, record.noteId), index);
    }
    for (const auto &record : incoming.assignments) {
        const auto key   = assignmentKey(record.storageId, record.noteId);
        const auto found = assignments.constFind(key);
        if (found == assignments.cend()) {
            assignments.insert(key, candidate.assignments.size());
            candidate.assignments.append(record);
        } else if (incomingWins(record.revision, record.modifiedAt, candidate.assignments.at(found.value()).revision,
                                candidate.assignments.at(found.value()).modifiedAt)) {
            candidate.assignments[found.value()] = record;
        } else if (record.revision == candidate.assignments.at(found.value()).revision
                   && record.modifiedAt == candidate.assignments.at(found.value()).modifiedAt
                   && !sameAssignment(record, candidate.assignments.at(found.value()))) {
            return error(FolderCatalogError::Conflict, QStringLiteral("Conflicting note assignment revisions"));
        }
    }

    QHash<QString, int> pathHints;
    pathHints.reserve(candidate.pathHints.size());
    for (int index = 0; index < candidate.pathHints.size(); ++index) {
        const auto &record = candidate.pathHints.at(index);
        pathHints.insert(pathKey(record.storageId, record.path), index);
    }
    for (const auto &record : incoming.pathHints) {
        const auto key   = pathKey(record.storageId, record.path);
        const auto found = pathHints.constFind(key);
        if (found == pathHints.cend()) {
            pathHints.insert(key, candidate.pathHints.size());
            candidate.pathHints.append(record);
        } else if (incomingWins(record.revision, record.modifiedAt, candidate.pathHints.at(found.value()).revision,
                                candidate.pathHints.at(found.value()).modifiedAt)) {
            candidate.pathHints[found.value()] = record;
        } else if (record.revision == candidate.pathHints.at(found.value()).revision
                   && record.modifiedAt == candidate.pathHints.at(found.value()).modifiedAt
                   && !samePathHint(record, candidate.pathHints.at(found.value()))) {
            return error(FolderCatalogError::Conflict, QStringLiteral("Conflicting provider path revisions"));
        }
    }

    return replaceSnapshot(std::move(candidate));
}

const FolderRecord *FolderCatalog::folder(const QUuid &id) const
{
    const auto index = indexOfFolder(id);
    if (index < 0 || snapshot_.folders.at(index).tombstone)
        return nullptr;
    return &snapshot_.folders.at(index);
}

QList<FolderRecord> FolderCatalog::children(const QUuid &parentId) const
{
    QList<FolderRecord> result;
    for (const auto &record : snapshot_.folders) {
        if (!record.tombstone && record.parentId == parentId)
            result.append(record);
    }
    std::sort(result.begin(), result.end(), [](const FolderRecord &left, const FolderRecord &right) {
        const int leftPlacement  = siblingPlacement(left);
        const int rightPlacement = siblingPlacement(right);
        if (leftPlacement != rightPlacement)
            return leftPlacement < rightPlacement;
        if (left.favorite != right.favorite)
            return left.favorite;
        if (left.sortOrder != right.sortOrder)
            return left.sortOrder < right.sortOrder;
        const auto leftName  = FolderCatalog::normalizedName(left.name);
        const auto rightName = FolderCatalog::normalizedName(right.name);
        if (leftName != rightName)
            return leftName < rightName;
        return left.id.toString(QUuid::WithoutBraces) < right.id.toString(QUuid::WithoutBraces);
    });
    return result;
}

QStringList FolderCatalog::pathForFolder(const QUuid &id) const
{
    QStringList path;
    QSet<QUuid> visited;
    QUuid       currentId = id;
    while (!currentId.isNull()) {
        if (visited.contains(currentId))
            return {};
        visited.insert(currentId);
        const auto *current = folder(currentId);
        if (!current)
            return {};
        path.prepend(current->name);
        currentId = current->parentId;
    }
    return path;
}

const NoteFolderAssignment *FolderCatalog::assignment(const QString &storageId, const QString &noteId) const
{
    const auto index = indexOfAssignment(storageId, noteId);
    if (index < 0)
        return nullptr;
    return &snapshot_.assignments.at(index);
}

QUuid FolderCatalog::folderForNote(const QString &storageId, const QString &noteId) const
{
    const auto *record = assignment(storageId, noteId);
    return record && !record->tombstone ? record->folderId : QUuid {};
}

const ProviderPathHint *FolderCatalog::pathHint(const QString &storageId, const QStringList &path) const
{
    const auto index = indexOfPathHint(storageId, path);
    return index < 0 ? nullptr : &snapshot_.pathHints.at(index);
}

QUuid FolderCatalog::recycleBinId()
{
    // A fixed application-owned UUID prevents a translated display name from
    // becoming part of the data contract and cannot collide with a normal
    // folder created through addFolder().
    static const QUuid id(QStringLiteral("7cc71968-8b71-4bbf-a0ec-61f0d0e6cd1b"));
    return id;
}

bool FolderCatalog::isRecycleBinId(const QUuid &id) { return id == recycleBinId(); }

bool FolderCatalog::isRecycled(const QString &storageId, const QString &noteId) const
{
    const auto *record = assignment(storageId, noteId);
    return record && !record->tombstone && isRecycleBinId(record->folderId);
}

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

} // namespace QtNote
