#include "foldercatalog.h"

#include "private.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <functional>
#include <limits>

namespace AnyKeep {
namespace {

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

using FolderCatalogPrivate::assignmentKey;
using FolderCatalogPrivate::error;
using FolderCatalogPrivate::folderKey;

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

bool FolderCatalog::isInArchivedBranch(const QUuid &id) const
{
    QSet<QUuid> visited;
    QUuid       currentId = id;
    while (!currentId.isNull()) {
        if (visited.contains(currentId))
            return false;
        visited.insert(currentId);
        const auto *current = folder(currentId);
        if (!current)
            return false;
        if (current->archived || isRecycleBinId(current->id))
            return true;
        currentId = current->parentId;
    }
    return false;
}

bool FolderCatalog::isEffectivelyFavorite(const QUuid &id) const
{
    QSet<QUuid> visited;
    QUuid       currentId = id;
    bool        favorite  = false;
    while (!currentId.isNull()) {
        if (visited.contains(currentId))
            return false;
        visited.insert(currentId);
        const auto *current = folder(currentId);
        if (!current)
            return false;
        if (current->archived || isRecycleBinId(current->id))
            return false;
        favorite  = favorite || current->favorite;
        currentId = current->parentId;
    }
    return favorite;
}

} // namespace AnyKeep
