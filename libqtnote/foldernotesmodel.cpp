/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "foldernotesmodel.h"

#include "foldercatalogmanager.h"
#include "notemanager.h"
#include "notesindex.h"

#include <algorithm>

namespace QtNote {

FolderNotesModel::FolderNotesModel(FolderCatalogManager *catalogManager, QObject *parent) : QAbstractListModel(parent)
{
    catalogManager_   = catalogManager ? catalogManager : FolderCatalogManager::instance();
    catalogAvailable_ = catalogManager_->isAvailable();

    connect(catalogManager_, &FolderCatalogManager::catalogChanged, this, &FolderNotesModel::rebuild);
    connect(catalogManager_, &FolderCatalogManager::availabilityChanged, this, [this](bool available) {
        if (catalogAvailable_ != available) {
            catalogAvailable_ = available;
            emit catalogAvailableChanged();
        }
        rebuild();
    });

    auto *notes = NoteManager::instance();
    connect(notes->notesIndex(), &NotesIndex::storageNotesChanged, this, [this](const QString &) { rebuild(); });
    connect(notes, &NoteManager::storageAdded, this, [this](const NoteStorage::Ptr &) { rebuild(); });
    connect(notes, &NoteManager::storageRemoved, this, [this](const NoteStorage::Ptr &) { rebuild(); });
    connect(notes, &NoteManager::storageReady, this, [this](const NoteStorage::Ptr &) { rebuild(); });

    rebuild();
}

int FolderNotesModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : rows_.size(); }

QVariant FolderNotesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size())
        return {};
    const auto &row = rows_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return row.title;
    case RowKindRole:
        return int(row.kind);
    case FolderIdRole:
        return row.folderId.toString(QUuid::WithoutBraces);
    case ParentFolderIdRole:
        return row.parentFolderId.toString(QUuid::WithoutBraces);
    case StorageIdRole:
        return row.storageId;
    case NoteIdRole:
        return row.noteId;
    case DepthRole:
        return row.depth;
    case CollapsedRole:
        return row.collapsed;
    case FavoriteRole:
        return row.favorite;
    case ArchivedRole:
        return row.archived;
    case ChildFolderCountRole:
        return row.childFolderCount;
    case NoteCountRole:
        return row.noteCount;
    }
    return {};
}

QHash<int, QByteArray> FolderNotesModel::roleNames() const
{
    return {
        { RowKindRole, "rowKind" },
        { FolderIdRole, "folderId" },
        { ParentFolderIdRole, "parentFolderId" },
        { StorageIdRole, "storageId" },
        { NoteIdRole, "noteId" },
        { TitleRole, "title" },
        { DepthRole, "depth" },
        { CollapsedRole, "collapsed" },
        { FavoriteRole, "favorite" },
        { ArchivedRole, "archived" },
        { ChildFolderCountRole, "childFolderCount" },
        { NoteCountRole, "noteCount" },
    };
}

bool FolderNotesModel::catalogAvailable() const { return catalogAvailable_; }

QVariantMap FolderNotesModel::itemAt(int row) const
{
    if (row < 0 || row >= rows_.size())
        return {};
    const auto &item = rows_.at(row);
    return {
        { QStringLiteral("rowKind"), int(item.kind) },
        { QStringLiteral("folderId"), item.folderId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("parentFolderId"), item.parentFolderId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("storageId"), item.storageId },
        { QStringLiteral("noteId"), item.noteId },
        { QStringLiteral("title"), item.title },
        { QStringLiteral("depth"), item.depth },
        { QStringLiteral("collapsed"), item.collapsed },
        { QStringLiteral("favorite"), item.favorite },
        { QStringLiteral("archived"), item.archived },
        { QStringLiteral("childFolderCount"), item.childFolderCount },
        { QStringLiteral("noteCount"), item.noteCount },
    };
}

int FolderNotesModel::rowForFolder(const QString &folderId) const
{
    const QUuid id(folderId);
    for (qsizetype row = 0; row < rows_.size(); ++row) {
        if (rows_.at(row).kind == FolderRow && rows_.at(row).folderId == id)
            return int(row);
    }
    return -1;
}

QVariantList FolderNotesModel::folderPickerItems(bool includeArchived) const
{
    QVariantList items;
    if (!catalogManager_)
        return items;
    for (const auto &folder : catalogManager_->catalog().children())
        appendFolderPickerItems(folder.id, 0, includeArchived, &items);
    return items;
}

void FolderNotesModel::rebuild()
{
    QHash<QUuid, QList<Note>> notesByFolder;
    QList<Note>               unsorted;

    auto *noteManager = NoteManager::instance();
    for (const auto &storage : noteManager->storages(true)) {
        if (!storage)
            continue;
        for (const auto &note : noteManager->notesIndex()->notes(storage->systemName())) {
            if (note.isNull() || note.id().isEmpty())
                continue;
            const auto folderId = effectiveFolderId(note);
            if (folderId.isNull())
                unsorted.append(note);
            else
                notesByFolder[folderId].append(note);
        }
    }
    const auto orderNotes
        = [](QList<Note> &notes) { std::sort(notes.begin(), notes.end(), noteListItemModifyComparer); };
    orderNotes(unsorted);
    for (auto it = notesByFolder.begin(); it != notesByFolder.end(); ++it)
        orderNotes(it.value());

    beginResetModel();
    rows_.clear();
    if (catalogManager_) {
        for (const auto &folder : catalogManager_->catalog().children())
            appendFolder(folder.id, 0, notesByFolder);
    }

    Row unsortedRow;
    unsortedRow.kind      = UnsortedRow;
    unsortedRow.title     = tr("Unsorted");
    unsortedRow.noteCount = unsorted.size();
    rows_.append(std::move(unsortedRow));
    appendNotes(unsorted, {}, 1);
    endResetModel();
    emit countChanged();
}

QUuid FolderNotesModel::effectiveFolderId(const Note &note) const
{
    if (!catalogManager_ || note.storageId().isEmpty() || note.id().isEmpty())
        return {};
    const auto &catalog    = catalogManager_->catalog();
    const auto *assignment = catalog.assignment(note.storageId(), note.id());
    if (assignment)
        return !assignment->tombstone && catalog.folder(assignment->folderId) ? assignment->folderId : QUuid {};
    return catalog.folder(note.folderId()) ? note.folderId() : QUuid {};
}

void FolderNotesModel::appendFolder(const QUuid &folderId, int depth, const QHash<QUuid, QList<Note>> &notesByFolder)
{
    if (!catalogManager_)
        return;
    const auto &catalog = catalogManager_->catalog();
    const auto *folder  = catalog.folder(folderId);
    if (!folder)
        return;

    Row row;
    row.kind             = FolderRow;
    row.folderId         = folder->id;
    row.parentFolderId   = folder->parentId;
    row.title            = folder->name;
    row.depth            = depth;
    row.collapsed        = folder->collapsed;
    row.favorite         = folder->favorite;
    row.archived         = folder->archived;
    row.childFolderCount = catalog.children(folder->id).size();
    row.noteCount        = notesByFolder.value(folder->id).size();
    rows_.append(std::move(row));

    if (folder->collapsed)
        return;
    for (const auto &child : catalog.children(folder->id))
        appendFolder(child.id, depth + 1, notesByFolder);
    appendNotes(notesByFolder.value(folder->id), folder->id, depth + 1);
}

void FolderNotesModel::appendNotes(const QList<Note> &notes, const QUuid &folderId, int depth)
{
    for (const auto &note : notes) {
        Row row;
        row.kind      = NoteRow;
        row.folderId  = folderId;
        row.storageId = note.storageId();
        row.noteId    = note.id();
        row.title     = note.title();
        row.depth     = depth;
        row.noteCount = 1;
        rows_.append(std::move(row));
    }
}

void FolderNotesModel::appendFolderPickerItems(const QUuid &folderId, int depth, bool includeArchived,
                                               QVariantList *items) const
{
    if (!catalogManager_ || !items)
        return;
    const auto &catalog = catalogManager_->catalog();
    const auto *folder  = catalog.folder(folderId);
    if (!folder || (!includeArchived && folder->archived))
        return;

    const QVariantMap item {
        { QStringLiteral("folderId"), folder->id.toString(QUuid::WithoutBraces) },
        { QStringLiteral("parentFolderId"), folder->parentId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("title"), folder->name },
        { QStringLiteral("depth"), depth },
        { QStringLiteral("favorite"), folder->favorite },
        { QStringLiteral("archived"), folder->archived },
    };
    items->append(item);
    for (const auto &child : catalog.children(folder->id))
        appendFolderPickerItems(child.id, depth + 1, includeArchived, items);
}

} // namespace QtNote
