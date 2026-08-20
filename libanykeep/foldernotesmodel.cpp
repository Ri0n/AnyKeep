/*
AnyKeep - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "foldernotesmodel.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notessearchmodel.h"
#include "notetitleresolver.h"
#include "storageiconimageprovider.h"

#include <QSet>
#include <algorithm>

namespace AnyKeep {

namespace {
    QString notePreview(const Note &note)
    {
        const auto indexedPreview = note.backendValue(QStringLiteral("anykeep.index.preview")).toString();
        if (!indexedPreview.isEmpty() || !note.isLoaded())
            return indexedPreview;

        QString preview = note.text().simplified();
        if (preview.size() > 180)
            preview = preview.left(177) + QStringLiteral("...");
        return preview;
    }

    QString draftPreview(const DraftRecord &draft)
    {
        QString preview = draft.body.simplified();
        if (preview.size() > 180)
            preview = preview.left(177) + QStringLiteral("...");
        return preview;
    }

    QString draftStateName(DraftRecord::State state)
    {
        switch (state) {
        case DraftRecord::Editing:
            return QStringLiteral("editing");
        case DraftRecord::Ready:
            return QStringLiteral("ready");
        case DraftRecord::Publishing:
            return QStringLiteral("publishing");
        case DraftRecord::Retry:
            return QStringLiteral("retry");
        case DraftRecord::NeedsRouting:
            return QStringLiteral("needs-routing");
        }
        return {};
    }
}

FolderNotesModel::FolderNotesModel(FolderCatalogManager *catalogManager, QObject *parent) :
    FolderNotesModel(catalogManager, DraftManager::instance(), parent)
{
}

FolderNotesModel::FolderNotesModel(FolderCatalogManager *catalogManager, DraftManager *draftManager, QObject *parent) :
    QAbstractListModel(parent)
{
    catalogManager_   = catalogManager ? catalogManager : FolderCatalogManager::instance();
    draftManager_     = draftManager ? draftManager : DraftManager::instance();
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
    connect(draftManager_, &DraftManager::draftsChanged, this, &FolderNotesModel::rebuild);

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
    case ItemTypeRole:
        return row.kind == NoteRow ? 1 : 0;
    case GroupKindRole:
        return row.kind == FolderRow
            ? QStringLiteral("folder")
            : (row.kind == UnsortedRow ? QStringLiteral("unsorted")
                                       : (row.kind == DraftsRow ? QStringLiteral("drafts") : QString()));
    case GroupIdRole:
        return row.kind == FolderRow
            ? row.folderId.toString(QUuid::WithoutBraces)
            : (row.kind == UnsortedRow ? QStringLiteral("unsorted")
                                       : (row.kind == DraftsRow ? QStringLiteral("drafts") : QString()));
    case PreviewRole:
        return row.preview;
    case StorageNameRole:
        return row.storageName;
    case AccessibleRole:
        return row.accessible;
    case LoadingRole:
        return false;
    case ErrorStringRole:
        return QString();
    case HasMoreRole:
        return false;
    case IconSourceRole:
        if (row.kind == DraftsRow || (row.kind == NoteRow && row.storageId == DraftManager::draftsStorageId()))
            return QStringLiteral("qrc:/icons/document-open-recent-symbolic.svg");
        return row.kind == NoteRow ? storageIconSource(row.storageId, true) : QString();
    case SystemFolderRole:
        return row.systemFolder;
    case PendingDraftRole:
        return row.pendingDraft;
    case DraftStateRole:
        return row.draftState;
    case DraftErrorRole:
        return row.draftError;
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
        { ItemTypeRole, "itemType" },
        { GroupKindRole, "groupKind" },
        { GroupIdRole, "groupId" },
        { PreviewRole, "preview" },
        { StorageNameRole, "storageName" },
        { AccessibleRole, "accessible" },
        { LoadingRole, "loading" },
        { ErrorStringRole, "errorString" },
        { HasMoreRole, "hasMore" },
        { IconSourceRole, "iconSource" },
        { SystemFolderRole, "systemFolder" },
        { PendingDraftRole, "pendingDraft" },
        { DraftStateRole, "draftState" },
        { DraftErrorRole, "draftError" },
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
        { QStringLiteral("itemType"), item.kind == NoteRow ? 1 : 0 },
        { QStringLiteral("groupKind"),
          item.kind == FolderRow
              ? QStringLiteral("folder")
              : (item.kind == UnsortedRow ? QStringLiteral("unsorted")
                                          : (item.kind == DraftsRow ? QStringLiteral("drafts") : QString())) },
        { QStringLiteral("groupId"),
          item.kind == FolderRow
              ? item.folderId.toString(QUuid::WithoutBraces)
              : (item.kind == UnsortedRow ? QStringLiteral("unsorted")
                                          : (item.kind == DraftsRow ? QStringLiteral("drafts") : QString())) },
        { QStringLiteral("preview"), item.preview },
        { QStringLiteral("storageName"), item.storageName },
        { QStringLiteral("accessible"), item.accessible },
        { QStringLiteral("loading"), false },
        { QStringLiteral("errorString"), QString() },
        { QStringLiteral("hasMore"), false },
        { QStringLiteral("iconSource"),
          item.kind == DraftsRow || (item.kind == NoteRow && item.storageId == DraftManager::draftsStorageId())
              ? QStringLiteral("qrc:/icons/document-open-recent-symbolic.svg")
              : (item.kind == NoteRow ? storageIconSource(item.storageId, true) : QString()) },
        { QStringLiteral("systemFolder"), item.systemFolder },
        { QStringLiteral("pendingDraft"), item.pendingDraft },
        { QStringLiteral("draftState"), item.draftState },
        { QStringLiteral("draftError"), item.draftError },
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

bool FolderNotesModel::setUnsortedCollapsed(bool collapsed)
{
    if (unsortedCollapsed_ == collapsed)
        return true;
    unsortedCollapsed_ = collapsed;
    rebuild();
    return true;
}

bool FolderNotesModel::setDraftsCollapsed(bool collapsed)
{
    if (draftsCollapsed_ == collapsed)
        return true;
    draftsCollapsed_ = collapsed;
    rebuild();
    return true;
}

void FolderNotesModel::setSearchModel(NotesSearchModel *model)
{
    if (searchModel_ == model)
        return;
    if (searchModel_)
        disconnect(searchModel_, nullptr, this, nullptr);
    searchModel_ = model;
    if (searchModel_) {
        connect(searchModel_, &NotesSearchModel::searchTextChanged, this, &FolderNotesModel::rebuild);
        connect(searchModel_, &NotesSearchModel::searchInBodyChanged, this, &FolderNotesModel::rebuild);
        connect(searchModel_, &NotesSearchModel::searchingChanged, this, &FolderNotesModel::rebuild);
    }
    rebuild();
}

void FolderNotesModel::rebuild() { replaceRows(buildRows()); }

QList<FolderNotesModel::Row> FolderNotesModel::buildRows() const
{
    QHash<QUuid, QList<Note>> notesByFolder;
    QList<Note>               unsorted;
    QList<Row>                rows;

    auto pending = draftManager_ ? draftManager_->pendingDrafts() : QList<DraftRecord> {};
    std::sort(pending.begin(), pending.end(),
              [](const DraftRecord &left, const DraftRecord &right) { return left.updatedAt > right.updatedAt; });
    QSet<QString> pendingRemoteNotes;
    QList<Row>    draftRows;
    const QString searchText = searchModel_ ? searchModel_->searchText().trimmed() : QString();
    const bool    searchBody = searchModel_ && searchModel_->searchInBody();
    for (const auto &draft : pending) {
        if (!draft.remoteNoteId.isEmpty())
            pendingRemoteNotes.insert(draft.storageId + QChar(0x1f) + draft.remoteNoteId);

        QString title = NoteTitleResolver::displayTitle(draft.title, draft.body, draft.format);
        if (title.isEmpty())
            title = tr("Untitled note");
        if (!searchText.isEmpty()) {
            bool matches = title.contains(searchText, Qt::CaseInsensitive);
            if (!matches) {
                QString tagText = searchText;
                if (tagText.startsWith(QLatin1Char('#')))
                    tagText.remove(0, 1);
                for (const auto &tag : draft.tags) {
                    if (!tagText.isEmpty() && tag.contains(tagText, Qt::CaseInsensitive)) {
                        matches = true;
                        break;
                    }
                }
            }
            if (!matches && searchBody)
                matches = draft.body.contains(searchText, Qt::CaseInsensitive);
            if (!matches)
                continue;
        }

        Row row;
        row.kind         = NoteRow;
        row.storageId    = draft.remoteNoteId.isEmpty() ? DraftManager::draftsStorageId() : draft.storageId;
        row.noteId       = draft.remoteNoteId.isEmpty() ? draft.id.toString(QUuid::WithoutBraces) : draft.remoteNoteId;
        row.title        = title;
        row.preview      = draftPreview(draft);
        row.depth        = 1;
        row.pendingDraft = true;
        row.draftState   = draftStateName(draft.state);
        row.draftError   = draft.lastError;
        row.accessible   = draft.state != DraftRecord::Publishing;
        if (row.storageId == DraftManager::draftsStorageId()) {
            row.storageName = tr("Drafts");
        } else if (const auto storage = NoteManager::instance()->storage(row.storageId)) {
            row.storageName = storage->name();
        } else {
            row.storageName = row.storageId;
        }
        draftRows.append(std::move(row));
    }

    if (!draftRows.isEmpty()) {
        Row draftsRow;
        draftsRow.kind      = DraftsRow;
        draftsRow.title     = tr("Drafts");
        draftsRow.noteCount = draftRows.size();
        draftsRow.collapsed = draftsCollapsed_;
        rows.append(std::move(draftsRow));
        if (!draftsCollapsed_)
            rows.append(draftRows);
    }

    auto *noteManager = NoteManager::instance();
    for (const auto &storage : noteManager->storages(true)) {
        if (!storage)
            continue;
        for (const auto &note : noteManager->notesIndex()->notes(storage->systemName())) {
            if (note.isNull() || note.id().isEmpty())
                continue;
            if (pendingRemoteNotes.contains(storage->systemName() + QChar(0x1f) + note.id()))
                continue;
            if (!matchesSearch(note))
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

    if (catalogManager_) {
        const auto rootFolders = catalogManager_->catalog().children();
        for (const auto &folder : rootFolders) {
            if (!folder.archived && !FolderCatalog::isRecycleBinId(folder.id))
                appendFolder(&rows, folder.id, 0, notesByFolder);
        }
    }

    Row unsortedRow;
    unsortedRow.kind      = UnsortedRow;
    unsortedRow.title     = tr("Unsorted");
    unsortedRow.noteCount = unsorted.size();
    unsortedRow.collapsed = unsortedCollapsed_;
    rows.append(std::move(unsortedRow));
    if (!unsortedCollapsed_)
        appendNotes(&rows, unsorted, {}, 1);
    if (catalogManager_) {
        const auto rootFolders = catalogManager_->catalog().children();
        for (const auto &folder : rootFolders) {
            if (folder.archived && !FolderCatalog::isRecycleBinId(folder.id))
                appendFolder(&rows, folder.id, 0, notesByFolder);
        }
        for (const auto &folder : rootFolders) {
            if (FolderCatalog::isRecycleBinId(folder.id))
                appendFolder(&rows, folder.id, 0, notesByFolder);
        }
    }
    return rows;
}

void FolderNotesModel::replaceRows(QList<Row> nextRows)
{
    const auto keysAreUnique = [](const QList<Row> &rows) {
        QSet<QString> keys;
        for (const auto &row : rows) {
            const auto key = rowKey(row);
            if (key.isEmpty() || keys.contains(key))
                return false;
            keys.insert(key);
        }
        return true;
    };

    if (!keysAreUnique(rows_) || !keysAreUnique(nextRows)) {
        beginResetModel();
        rows_ = std::move(nextRows);
        endResetModel();
        emit countChanged();
        return;
    }

    QSet<QString> nextKeys;
    for (const auto &row : nextRows)
        nextKeys.insert(rowKey(row));

    for (qsizetype index = rows_.size() - 1; index >= 0; --index) {
        if (nextKeys.contains(rowKey(rows_.at(index))))
            continue;
        beginRemoveRows({}, index, index);
        rows_.removeAt(index);
        endRemoveRows();
    }

    for (qsizetype target = 0; target < nextRows.size(); ++target) {
        const auto targetKey = rowKey(nextRows.at(target));
        qsizetype  current   = 0;
        while (current < rows_.size() && rowKey(rows_.at(current)) != targetKey)
            ++current;

        if (current == rows_.size()) {
            beginInsertRows({}, target, target);
            rows_.insert(target, nextRows.at(target));
            endInsertRows();
        } else if (current != target) {
            const qsizetype destinationChild = current < target ? target + 1 : target;
            beginMoveRows({}, current, current, {}, destinationChild);
            rows_.move(current, target);
            endMoveRows();
        }
    }

    for (qsizetype index = 0; index < rows_.size(); ++index) {
        if (sameRow(rows_.at(index), nextRows.at(index)))
            continue;
        rows_[index] = nextRows.at(index);
        emit dataChanged(this->index(index, 0), this->index(index, 0));
    }
    emit countChanged();
}

QString FolderNotesModel::rowKey(const Row &row)
{
    switch (row.kind) {
    case FolderRow:
        return QStringLiteral("folder:") + row.folderId.toString(QUuid::WithoutBraces);
    case NoteRow:
        return QStringLiteral("note:") + QString::number(row.storageId.size()) + QChar(0x1f) + row.storageId
            + row.noteId;
    case UnsortedRow:
        return QStringLiteral("unsorted");
    case DraftsRow:
        return QStringLiteral("drafts");
    }
    return {};
}

bool FolderNotesModel::sameRow(const Row &left, const Row &right)
{
    return left.kind == right.kind && left.folderId == right.folderId && left.parentFolderId == right.parentFolderId
        && left.storageId == right.storageId && left.noteId == right.noteId && left.title == right.title
        && left.preview == right.preview && left.storageName == right.storageName && left.depth == right.depth
        && left.accessible == right.accessible && left.collapsed == right.collapsed && left.favorite == right.favorite
        && left.archived == right.archived && left.systemFolder == right.systemFolder
        && left.pendingDraft == right.pendingDraft && left.draftState == right.draftState
        && left.draftError == right.draftError && left.childFolderCount == right.childFolderCount
        && left.noteCount == right.noteCount;
}

bool FolderNotesModel::matchesSearch(const Note &note) const
{
    if (!searchModel_ || searchModel_->searchText().trimmed().isEmpty())
        return true;

    const QString query = searchModel_->searchText().trimmed();
    if (searchModel_->searchInBody() && searchModel_->hasBodyMatch(note.storageId(), note.id()))
        return true;
    if (note.title().contains(query, Qt::CaseInsensitive))
        return true;

    QString tagQuery = query;
    if (tagQuery.startsWith(QLatin1Char('#')))
        tagQuery.remove(0, 1);
    if (tagQuery.isEmpty())
        return false;
    for (const auto &tag : note.tags()) {
        if (tag.contains(tagQuery, Qt::CaseInsensitive))
            return true;
    }
    const auto storage = NoteManager::instance()->storage(note.storageId());
    return storage && storage->name().contains(query, Qt::CaseInsensitive);
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

void FolderNotesModel::appendFolder(QList<Row> *rows, const QUuid &folderId, int depth,
                                    const QHash<QUuid, QList<Note>> &notesByFolder) const
{
    if (!rows || !catalogManager_)
        return;
    const auto &catalog = catalogManager_->catalog();
    const auto *folder  = catalog.folder(folderId);
    if (!folder)
        return;

    Row row;
    row.kind             = FolderRow;
    row.folderId         = folder->id;
    row.parentFolderId   = folder->parentId;
    row.systemFolder     = FolderCatalog::isRecycleBinId(folder->id);
    row.title            = row.systemFolder ? tr("Recycle Bin") : folder->name;
    row.depth            = depth;
    row.collapsed        = folder->collapsed;
    row.favorite         = folder->favorite;
    row.archived         = folder->archived;
    row.childFolderCount = catalog.children(folder->id).size();
    row.noteCount        = notesByFolder.value(folder->id).size();
    rows->append(std::move(row));

    if (folder->collapsed)
        return;
    for (const auto &child : catalog.children(folder->id))
        appendFolder(rows, child.id, depth + 1, notesByFolder);
    appendNotes(rows, notesByFolder.value(folder->id), folder->id, depth + 1);
}

void FolderNotesModel::appendNotes(QList<Row> *rows, const QList<Note> &notes, const QUuid &folderId, int depth) const
{
    if (!rows)
        return;
    for (const auto &note : notes) {
        Row row;
        row.kind           = NoteRow;
        row.folderId       = folderId;
        row.storageId      = note.storageId();
        row.noteId         = note.id();
        row.title          = note.displayTitle();
        row.preview        = notePreview(note);
        row.depth          = depth;
        row.noteCount      = 1;
        const auto storage = NoteManager::instance()->storage(row.storageId);
        row.storageName    = storage ? storage->name() : row.storageId;
        row.accessible     = storage && storage->isAccessible();
        rows->append(std::move(row));
    }
}

void FolderNotesModel::appendFolderPickerItems(const QUuid &folderId, int depth, bool includeArchived,
                                               QVariantList *items) const
{
    if (!catalogManager_ || !items)
        return;
    const auto &catalog = catalogManager_->catalog();
    const auto *folder  = catalog.folder(folderId);
    if (!folder || FolderCatalog::isRecycleBinId(folder->id) || (!includeArchived && folder->archived))
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

} // namespace AnyKeep
