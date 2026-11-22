#include "recentnotesmodel.h"

#include "notepresentationorder.h"
#include "notesmodel.h"
#include "projectionrowdiff_p.h"

#include <QDateTime>

#include <algorithm>
#include <utility>

namespace AnyKeep {

RecentNotesModel::RecentNotesModel(QObject *parent) : QAbstractListModel(parent) {}

RecentNotesModel::RecentNotesModel(QAbstractItemModel *sourceModel, QObject *parent) : QAbstractListModel(parent)
{
    setSourceModel(sourceModel);
}

int RecentNotesModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : entries_.size(); }

QVariant RecentNotesModel::data(const QModelIndex &index, int role) const
{
    if (!sourceModel_ || !index.isValid() || index.row() < 0 || index.row() >= entries_.size())
        return {};
    const auto &sourceIndex = entries_.at(index.row());
    return sourceIndex.data(role);
}

QHash<int, QByteArray> RecentNotesModel::roleNames() const
{
    if (sourceModel_)
        return sourceModel_->roleNames();
    return {
        { NotesModel::StorageIdRole, "storageId" },
        { NotesModel::NoteIdRole, "noteId" },
        { NotesModel::ItemTypeRole, "itemType" },
        { NotesModel::TagsRole, "tags" },
        { NotesModel::TitleRole, "title" },
        { NotesModel::PreviewRole, "preview" },
        { NotesModel::ModifiedTimeRole, "modifiedTime" },
        { NotesModel::StorageNameRole, "storageName" },
        { NotesModel::AccessibleRole, "accessible" },
        { NotesModel::IconSourceRole, "iconSource" },
        { NotesModel::FavoriteRole, "favorite" },
        { NotesModel::PendingDraftRole, "pendingDraft" },
        { NotesModel::DraftStateRole, "draftState" },
        { NotesModel::DraftErrorRole, "draftError" },
        { NotesModel::RecycledRole, "recycled" },
    };
}

void RecentNotesModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    if (sourceModel_ == sourceModel)
        return;
    disconnectSource();
    sourceModel_ = sourceModel;
    connectSource();
    resetProjection();
    emit sourceModelChanged();
}

void RecentNotesModel::setMaximumCount(int maximumCount)
{
    maximumCount = qBound(1, maximumCount, 500);
    if (maximumCount_ == maximumCount)
        return;
    maximumCount_ = maximumCount;
    emit maximumCountChanged();
    rebuild();
}

QList<QPersistentModelIndex> RecentNotesModel::projectedEntries() const
{
    if (!sourceModel_)
        return {};

    struct Candidate {
        QModelIndex index;
        QDateTime   modified;
        QString     title;
        bool        pending;
        bool        favorite;
    };
    QList<Candidate> candidates;
    const int        storageCount = sourceModel_->rowCount();
    for (int storageRow = 0; storageRow < storageCount; ++storageRow) {
        const auto storageIndex = sourceModel_->index(storageRow, 0);
        const int  noteCount    = sourceModel_->rowCount(storageIndex);
        for (int noteRow = 0; noteRow < noteCount; ++noteRow) {
            const auto note = sourceModel_->index(noteRow, 0, storageIndex);
            if (note.data(NotesModel::ItemTypeRole).toInt() != NotesModel::ItemNote
                || note.data(NotesModel::RecycledRole).toBool())
                continue;
            candidates.append(Candidate {
                note, note.data(NotesModel::ModifiedTimeRole).toDateTime(), note.data(NotesModel::TitleRole).toString(),
                note.data(NotesModel::PendingDraftRole).toBool(), note.data(NotesModel::FavoriteRole).toBool() });
        }
    }
    const int count = qMin(maximumCount_, candidates.size());
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
        return notePresentationComesBefore(left.pending, left.favorite, left.modified, left.title, right.pending,
                                           right.favorite, right.modified, right.title);
    });
    QList<QPersistentModelIndex> result;
    result.reserve(count);
    for (int row = 0; row < count; ++row)
        result.append(QPersistentModelIndex(candidates.at(row).index));
    return result;
}

void RecentNotesModel::resetProjection()
{
    // Replacing or resetting the source invalidates every source identity.
    const auto previousCount = entries_.size();
    auto       next          = projectedEntries();
    beginResetModel();
    entries_ = std::move(next);
    endResetModel();
    if (previousCount != entries_.size())
        emit noteCountChanged();
}

void RecentNotesModel::rebuild()
{
    const auto next          = projectedEntries();
    const auto previousCount = entries_.size();
    reconcileProjectionRows(
        entries_, next,
        [this](int first, int last) {
            beginRemoveRows({}, first, last);
            entries_.remove(first, last - first + 1);
            endRemoveRows();
        },
        [this](int from, int to) {
            beginMoveRows({}, from, from, {}, from < to ? to + 1 : to);
            entries_.move(from, to);
            endMoveRows();
        },
        [this, &next](int first, int last) {
            beginInsertRows({}, first, last);
            for (int row = first; row <= last; ++row)
                entries_.insert(row, next.at(row));
            endInsertRows();
        });
    if (previousCount != entries_.size())
        emit noteCountChanged();
}

void RecentNotesModel::sourceDataChanged(const QModelIndex &first, const QModelIndex &last, const QList<int> &roles)
{
    const auto parent = first.parent();
    if (!parent.isValid())
        return;

    QSet<QPersistentModelIndex> affected;
    for (const auto &entry : std::as_const(entries_)) {
        if (entry.isValid() && entry.parent() == parent && entry.row() >= first.row() && entry.row() <= last.row())
            affected.insert(entry);
    }
    if (roles.isEmpty() || roles.contains(NotesModel::ItemTypeRole) || roles.contains(NotesModel::RecycledRole)
        || roles.contains(NotesModel::PendingDraftRole) || roles.contains(NotesModel::FavoriteRole)
        || roles.contains(NotesModel::ModifiedTimeRole) || roles.contains(NotesModel::TitleRole))
        rebuild();

    // Forward the source roles at the new positions. Newly inserted rows
    // already expose current data through their insertion notification.
    for (int firstRow = 0; firstRow < entries_.size();) {
        if (!affected.contains(entries_.at(firstRow))) {
            ++firstRow;
            continue;
        }
        int lastRow = firstRow;
        while (lastRow + 1 < entries_.size() && affected.contains(entries_.at(lastRow + 1)))
            ++lastRow;
        emit dataChanged(index(firstRow), index(lastRow), roles);
        firstRow = lastRow + 1;
    }
}

void RecentNotesModel::disconnectSource()
{
    for (const auto &connection : std::as_const(sourceConnections_))
        disconnect(connection);
    sourceConnections_.clear();
}

void RecentNotesModel::connectSource()
{
    if (!sourceModel_)
        return;
    const auto schedule = [this]() { rebuild(); };
    sourceConnections_.append(
        connect(sourceModel_, &QAbstractItemModel::modelReset, this, &RecentNotesModel::resetProjection));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::layoutChanged, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::rowsInserted, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::rowsRemoved, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::rowsMoved, this, schedule));
    sourceConnections_.append(
        connect(sourceModel_, &QAbstractItemModel::dataChanged, this, &RecentNotesModel::sourceDataChanged));
    sourceConnections_.append(connect(sourceModel_, &QObject::destroyed, this, [this]() {
        sourceModel_.clear();
        disconnectSource();
        resetProjection();
        emit sourceModelChanged();
    }));
}

} // namespace AnyKeep
