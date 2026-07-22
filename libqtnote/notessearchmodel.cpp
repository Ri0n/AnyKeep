#include "notessearchmodel.h"

#include "notemanager.h"
#include "notesmodel.h"

namespace QtNote {

NotesSearchModel::NotesSearchModel(QObject *parent) : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setFilterRole(NotesModel::TitleRole);
    setRecursiveFilteringEnabled(true);
    setAutoAcceptChildRows(false);
}

bool NotesSearchModel::hasBodyMatch(const QString &storageId, const QString &noteId) const
{
    return foundCache_.value(storageId).contains(noteId);
}

void NotesSearchModel::setSearchText(const QString &text)
{
    if (text_ == text)
        return;
    text_ = text;
    emit searchTextChanged();
    if (auto *notes = qobject_cast<NotesModel *>(sourceModel()))
        notes->setSearchActive(!text.trimmed().isEmpty());
    setFilterFixedString(text.trimmed());
    restartBodySearch();
}

void NotesSearchModel::setSearchInBody(bool allow)
{
    if (searchInBody_ == allow)
        return;
    searchInBody_ = allow;
    emit searchInBodyChanged();
    restartBodySearch();
}

bool NotesSearchModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (!sourceModel())
        return false;

    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!index.isValid())
        return false;

    if (!sourceParent.isValid()) {
        if (text_.trimmed().isEmpty())
            return true;
        const QString storageName = sourceModel()->data(index, NotesModel::StorageNameRole).toString();
        return storageName.contains(text_.trimmed(), Qt::CaseInsensitive);
    }

    const QString storageId = sourceModel()->data(index, NotesModel::StorageIdRole).toString();
    const QString noteId    = sourceModel()->data(index, NotesModel::NoteIdRole).toString();
    if (searchInBody_ && hasBodyMatch(storageId, noteId))
        return true;

    if (QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent))
        return true;

    QString tagFilter = text_.trimmed();
    if (tagFilter.startsWith(QLatin1Char('*')))
        tagFilter.remove(0, 1);
    if (tagFilter.isEmpty())
        return false;

    const auto tags = sourceModel()->data(index, NotesModel::TagsRole).toStringList();
    for (const auto &tag : tags) {
        if (tag.contains(tagFilter, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

void NotesSearchModel::noteFound(const QString &storageId, const QString &noteId)
{
    if (sender() && sender() != finder_)
        return;
    auto &notes = foundCache_[storageId];
    if (!notes.contains(noteId))
        notes.append(noteId);
    invalidateRowsFilter();
}

void NotesSearchModel::searchCompleted()
{
    if (sender() && sender() != finder_)
        return;
    finder_.clear();
    setSearching(false);
    invalidateRowsFilter();
}

void NotesSearchModel::restartBodySearch()
{
    if (finder_)
        finder_->abort();
    finder_.clear();
    foundCache_.clear();
    setSearching(false);
    invalidateRowsFilter();

    if (!searchInBody_ || text_.trimmed().isEmpty())
        return;

    finder_ = NoteManager::search();
    finder_->setParent(this);
    connect(finder_, &GlobalNoteFinder::found, this, &NotesSearchModel::noteFound);
    connect(finder_, &GlobalNoteFinder::completed, this, &NotesSearchModel::searchCompleted);
    setSearching(true);
    finder_->start(text_.trimmed());
}

void NotesSearchModel::setSearching(bool searching)
{
    if (searching_ == searching)
        return;
    searching_ = searching;
    emit searchingChanged();
}

void NotesSearchModel::invalidateRowsFilter()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

} // namespace QtNote
