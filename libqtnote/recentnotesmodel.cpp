#include "recentnotesmodel.h"

#include "notesmodel.h"

#include <QDateTime>

#include <algorithm>
#include <utility>

namespace QtNote {

RecentNotesModel::RecentNotesModel(QObject *parent) : QAbstractListModel(parent) { }

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
    return sourceIndex.isValid() ? sourceModel_->data(sourceIndex, role) : QVariant();
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
    };
}

void RecentNotesModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    if (sourceModel_ == sourceModel)
        return;
    disconnectSource();
    sourceModel_ = sourceModel;
    connectSource();
    emit sourceModelChanged();
    rebuild();
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

void RecentNotesModel::rebuild()
{
    QList<QPersistentModelIndex> next;
    if (sourceModel_) {
        const int storageCount = sourceModel_->rowCount();
        for (int storageRow = 0; storageRow < storageCount; ++storageRow) {
            const QModelIndex storageIndex = sourceModel_->index(storageRow, 0);
            const int         noteCount    = sourceModel_->rowCount(storageIndex);
            for (int noteRow = 0; noteRow < noteCount; ++noteRow) {
                const QModelIndex noteIndex = sourceModel_->index(noteRow, 0, storageIndex);
                if (sourceModel_->data(noteIndex, NotesModel::ItemTypeRole).toInt() == NotesModel::ItemNote)
                    next.append(QPersistentModelIndex(noteIndex));
            }
        }

        std::stable_sort(next.begin(), next.end(), [this](const auto &left, const auto &right) {
            const QDateTime leftTime  = sourceModel_->data(left, NotesModel::ModifiedTimeRole).toDateTime();
            const QDateTime rightTime = sourceModel_->data(right, NotesModel::ModifiedTimeRole).toDateTime();
            if (leftTime != rightTime)
                return leftTime > rightTime;
            return sourceModel_->data(left, NotesModel::TitleRole)
                       .toString()
                       .localeAwareCompare(sourceModel_->data(right, NotesModel::TitleRole).toString())
                < 0;
        });
        if (next.size() > maximumCount_)
            next.resize(maximumCount_);
    }

    beginResetModel();
    entries_ = std::move(next);
    endResetModel();
    emit noteCountChanged();
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
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::modelReset, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::layoutChanged, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::rowsInserted, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::rowsRemoved, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::rowsMoved, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QAbstractItemModel::dataChanged, this, schedule));
    sourceConnections_.append(connect(sourceModel_, &QObject::destroyed, this, [this]() {
        sourceModel_.clear();
        disconnectSource();
        rebuild();
    }));
}

} // namespace QtNote
