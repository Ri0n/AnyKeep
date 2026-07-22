#include "notesmodel.h"

#include "notemanager.h"
#include "storageiconimageprovider.h"

#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QSet>

#include <algorithm>
#include <utility>

namespace QtNote {

namespace {
    QString notePreview(const Note &note)
    {
        if (!note.isLoaded())
            return {};
        QString preview = note.text().simplified();
        if (preview.size() > 180)
            preview = preview.left(177) + QStringLiteral("...");
        return preview;
    }
}

class NMMItem {
public:
    explicit NMMItem(const NoteStorage::Ptr &storage) : type(NotesModel::ItemStorage), storage(storage)
    {
        id    = storage ? storage->systemName() : QString();
        title = storage ? storage->name() : QString();
    }

    NMMItem(const Note &note, NMMItem *parent) : parent(parent), type(NotesModel::ItemNote) { assign(note); }

    ~NMMItem() { qDeleteAll(children); }

    void assign(const Note &note)
    {
        summary    = note;
        id         = note.id();
        title      = note.displayTitle();
        tags       = note.tags();
        lastChange = note.lastChangeUTC();
        preview    = notePreview(note);
    }

    void sortAllNotes() { std::sort(allNotes.begin(), allNotes.end(), noteListItemModifyComparer); }

    int allNoteIndex(const QString &noteId) const
    {
        for (int i = 0; i < allNotes.size(); ++i) {
            if (allNotes.at(i).id() == noteId)
                return i;
        }
        return -1;
    }

    NMMItem             *parent { nullptr };
    NotesModel::ItemType type { NotesModel::ItemStorage };
    NoteStorage::Ptr     storage;
    QList<NMMItem *>     children;
    QList<Note>          allNotes;
    Note                 summary;
    QString              title;
    QString              id;
    QStringList          tags;
    QString              preview;
    QDateTime            lastChange;
    QString              errorString;
    bool                 loading { false };
    quint64              refreshGeneration { 0 };
    int                  preSearchVisibleCount { 0 };
};

NotesModel::NotesModel(QObject *parent) : QAbstractItemModel(parent)
{
    const auto manager = NoteManager::instance();
    for (const auto &storage : manager->prioritizedStorages(true))
        storageAdded(storage);

    connect(manager, &NoteManager::storageAdded, this, &NotesModel::storageAdded);
    connect(manager, &NoteManager::storageAboutToBeRemoved, this, &NotesModel::storageAboutToBeRemoved);
    connect(manager, &NoteManager::storageChanged, this, &NotesModel::storageChanged);
    connect(manager, &NoteManager::storageReady, this, &NotesModel::storageReady);
}

NotesModel::~NotesModel() { qDeleteAll(storages_); }

QModelIndex NotesModel::index(int row, int column, const QModelIndex &parentIndex) const
{
    if (column != 0 || row < 0)
        return {};
    if (!parentIndex.isValid()) {
        if (row >= storages_.size())
            return {};
        return createIndex(row, column, storages_.at(row));
    }
    auto *parentItem = static_cast<NMMItem *>(parentIndex.internalPointer());
    if (!parentItem || parentItem->type != ItemStorage || row >= parentItem->children.size())
        return {};
    return createIndex(row, column, parentItem->children.at(row));
}

QModelIndex NotesModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    auto *item = static_cast<NMMItem *>(child.internalPointer());
    if (!item || !item->parent)
        return {};
    const int row = storages_.indexOf(item->parent);
    return row < 0 ? QModelIndex() : createIndex(row, 0, item->parent);
}

int NotesModel::rowCount(const QModelIndex &parentIndex) const
{
    if (!parentIndex.isValid())
        return storages_.size();
    auto *item = static_cast<NMMItem *>(parentIndex.internalPointer());
    return item && item->type == ItemStorage ? item->children.size() : 0;
}

int NotesModel::columnCount(const QModelIndex &) const { return 1; }

QVariant NotesModel::data(const QModelIndex &modelIndex, int role) const
{
    if (!modelIndex.isValid())
        return {};
    auto *item = static_cast<NMMItem *>(modelIndex.internalPointer());
    if (!item)
        return {};

    const bool storage = item->type == ItemStorage;
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return item->title;
    case Qt::DecorationRole:
        if (storage)
            return item->storage ? item->storage->storageIcon() : QIcon();
        return item->parent && item->parent->storage ? item->parent->storage->noteIcon() : QIcon();
    case StorageIdRole:
        return storage ? item->id : item->parent->id;
    case NoteIdRole:
        return storage ? QString() : item->id;
    case ItemTypeRole:
        return item->type;
    case TagsRole:
        return storage ? QStringList() : item->tags;
    case PreviewRole:
        return storage ? QString() : item->preview;
    case ModifiedTimeRole:
        return storage ? QVariant() : item->lastChange;
    case StorageNameRole:
        return storage ? item->title : item->parent->title;
    case AccessibleRole:
        return storage ? bool(item->storage && item->storage->isAccessible())
                       : bool(item->parent->storage && item->parent->storage->isAccessible());
    case LoadingRole:
        return storage && item->loading;
    case ErrorStringRole:
        return storage ? item->errorString : QString();
    case HasMoreRole:
        return storage && item->children.size() < item->allNotes.size();
    case NoteCountRole:
        return storage ? item->allNotes.size() : 0;
    case IconSourceRole:
        return storageIconSource(storage ? item->id : item->parent->id, !storage);
    default:
        return {};
    }
}

Qt::ItemFlags NotesModel::flags(const QModelIndex &modelIndex) const
{
    if (!modelIndex.isValid())
        return Qt::ItemIsEnabled;
    auto *item = static_cast<NMMItem *>(modelIndex.internalPointer());
    if (item->type == ItemStorage)
        return Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QHash<int, QByteArray> NotesModel::roleNames() const
{
    return {
        { StorageIdRole, "storageId" },
        { NoteIdRole, "noteId" },
        { ItemTypeRole, "itemType" },
        { TagsRole, "tags" },
        { TitleRole, "title" },
        { PreviewRole, "preview" },
        { ModifiedTimeRole, "modifiedTime" },
        { StorageNameRole, "storageName" },
        { AccessibleRole, "accessible" },
        { LoadingRole, "loading" },
        { ErrorStringRole, "errorString" },
        { HasMoreRole, "hasMore" },
        { NoteCountRole, "noteCount" },
        { IconSourceRole, "iconSource" },
    };
}

bool NotesModel::canFetchMore(const QModelIndex &parentIndex) const
{
    if (!parentIndex.isValid())
        return false;
    auto *item = static_cast<NMMItem *>(parentIndex.internalPointer());
    return item && item->type == ItemStorage && item->children.size() < item->allNotes.size();
}

void NotesModel::fetchMore(const QModelIndex &parentIndex)
{
    if (!canFetchMore(parentIndex))
        return;
    auto     *item  = static_cast<NMMItem *>(parentIndex.internalPointer());
    const int first = item->children.size();
    const int count = qMin(pageSize_, item->allNotes.size() - first);
    beginInsertRows(parentIndex, first, first + count - 1);
    for (int i = 0; i < count; ++i)
        item->children.append(new NMMItem(item->allNotes.at(first + i), item));
    endInsertRows();
    emit dataChanged(parentIndex, parentIndex, { HasMoreRole });
}

Qt::DropActions NotesModel::supportedDropActions() const { return Qt::MoveAction; }

QStringList NotesModel::mimeTypes() const { return { QStringLiteral("application/qtnote.notes.list") }; }

QMimeData *NotesModel::mimeData(const QModelIndexList &indexes) const
{
    auto         *mime = new QMimeData;
    QByteArray    payload;
    QDataStream   stream(&payload, QIODevice::WriteOnly);
    QSet<QString> seen;
    for (const auto &modelIndex : indexes) {
        if (!modelIndex.isValid())
            continue;
        auto *item = static_cast<NMMItem *>(modelIndex.internalPointer());
        if (!item || item->type != ItemNote)
            continue;
        const QString key = item->parent->id + QLatin1Char('\n') + item->id;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        stream << item->parent->id << item->id;
    }
    mime->setData(QStringLiteral("application/qtnote.notes.list"), payload);
    return mime;
}

bool NotesModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int, int column,
                              const QModelIndex &parentIndex)
{
    if (action == Qt::IgnoreAction)
        return true;
    if (!data || !data->hasFormat(QStringLiteral("application/qtnote.notes.list")) || column > 0
        || !parentIndex.isValid()) {
        return false;
    }
    auto *destination = static_cast<NMMItem *>(parentIndex.internalPointer());
    if (!destination || destination->type != ItemStorage)
        return false;

    QByteArray  payload = data->data(QStringLiteral("application/qtnote.notes.list"));
    QDataStream stream(&payload, QIODevice::ReadOnly);
    QStringList sourceStorageIds;
    QStringList noteIds;
    while (!stream.atEnd()) {
        QString storageId;
        QString noteId;
        stream >> storageId >> noteId;
        if (!storageId.isEmpty() && !noteId.isEmpty() && storageId != destination->id) {
            sourceStorageIds.append(storageId);
            noteIds.append(noteId);
        }
    }
    if (noteIds.isEmpty())
        return false;
    emit notesDropRequested(sourceStorageIds, noteIds, destination->id);
    return true;
}

int NotesModel::noteCount() const
{
    int count = 0;
    for (const auto *storage : storages_)
        count += storage->allNotes.size();
    return count;
}

void NotesModel::refresh()
{
    for (auto *item : std::as_const(storages_)) {
        if (item->storage)
            startStorageRefresh(item->storage);
    }
}

void NotesModel::refreshStorage(const QString &storageId)
{
    const auto storage = NoteManager::instance()->storage(storageId);
    if (storage)
        startStorageRefresh(storage);
}

QVariantMap NotesModel::itemData(int row, const QModelIndex &parentIndex) const
{
    const auto  modelIndex = index(row, 0, parentIndex);
    QVariantMap result;
    if (!modelIndex.isValid())
        return result;
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        result.insert(QString::fromLatin1(it.value()), data(modelIndex, it.key()));
    return result;
}

void NotesModel::setSearchActive(bool active)
{
    if (searchActive_ == active)
        return;
    searchActive_ = active;
    for (auto *item : std::as_const(storages_)) {
        if (active) {
            item->preSearchVisibleCount = item->children.size();
            replaceVisibleNotes(item, item->allNotes.size());
        } else {
            replaceVisibleNotes(item, qMax(pageSize_, item->preSearchVisibleCount));
            item->preSearchVisibleCount = 0;
        }
    }
}

void NotesModel::setPageSize(int pageSize)
{
    pageSize = qBound(10, pageSize, 500);
    if (pageSize_ == pageSize)
        return;
    pageSize_ = pageSize;
    emit pageSizeChanged();
    for (auto *item : std::as_const(storages_))
        replaceVisibleNotes(item, qMax(item->children.size(), pageSize_));
}

void NotesModel::storageAdded(const NoteStorage::Ptr &storage)
{
    if (!storage || storageItem(storage->systemName()))
        return;
    int row = 0;
    for (const auto &candidate : NoteManager::instance()->prioritizedStorages(true)) {
        if (candidate == storage)
            break;
        if (candidate && storageItem(candidate->systemName()))
            ++row;
    }
    beginInsertRows({}, row, row);
    auto *item = new NMMItem(storage);
    storages_.insert(row, item);
    endInsertRows();
    setStorageSignalHandlers(storage);
    startStorageRefresh(storage);
    emit statsChanged();
}

void NotesModel::storageAboutToBeRemoved(const NoteStorage::Ptr &storage)
{
    if (!storage)
        return;
    const int row = storages_.indexOf(storageItem(storage->systemName()));
    if (row < 0)
        return;
    if (refreshJobs_.contains(storage->systemName()) && refreshJobs_.value(storage->systemName()))
        refreshJobs_.value(storage->systemName())->cancel();
    refreshJobs_.remove(storage->systemName());
    beginRemoveRows({}, row, row);
    delete storages_.takeAt(row);
    endRemoveRows();
    emit statsChanged();
}

void NotesModel::storageChanged(const NoteStorage::Ptr &storage)
{
    auto *item = storageItem(storage ? storage->systemName() : QString());
    if (!item || !storage)
        return;
    item->storage         = storage;
    item->title           = storage->name();
    const auto modelIndex = storageIndex(item->id);
    if (modelIndex.isValid())
        emit dataChanged(modelIndex, modelIndex,
                         { Qt::DisplayRole, Qt::DecorationRole, TitleRole, StorageNameRole, AccessibleRole });
}

void NotesModel::storageReady(const NoteStorage::Ptr &storage)
{
    storageChanged(storage);
    if (storage)
        startStorageRefresh(storage);
}

void NotesModel::noteAdded(const Note &note) { updateNoteSummary(note, false); }
void NotesModel::noteModified(const Note &note) { updateNoteSummary(note, false); }
void NotesModel::noteRemoved(const Note &note) { updateNoteSummary(note, true); }

void NotesModel::storageInvalidated()
{
    auto *storageObject = qobject_cast<NoteStorage *>(sender());
    if (storageObject)
        refreshStorage(storageObject->systemName());
}

void NotesModel::setStorageSignalHandlers(const NoteStorage::Ptr &storage)
{
    connect(storage, &NoteStorage::noteAdded, this, &NotesModel::noteAdded);
    connect(storage, &NoteStorage::noteModified, this, &NotesModel::noteModified);
    connect(storage, &NoteStorage::noteRemoved, this, &NotesModel::noteRemoved);
    connect(storage, &NoteStorage::invalidated, this, &NotesModel::storageInvalidated);
}

void NotesModel::startStorageRefresh(const NoteStorage::Ptr &storage)
{
    auto *item = storageItem(storage ? storage->systemName() : QString());
    if (!item || !storage)
        return;

    if (refreshJobs_.contains(item->id) && refreshJobs_.value(item->id))
        refreshJobs_.value(item->id)->cancel();

    item->loading = true;
    item->errorString.clear();
    ++item->refreshGeneration;
    const quint64 generation  = item->refreshGeneration;
    const auto    parentIndex = storageIndex(item->id);
    emit          dataChanged(parentIndex, parentIndex, { LoadingRole, ErrorStringRole });

    auto *job = storage->refreshNotesAsync(0, this);
    refreshJobs_.insert(item->id, job);
    connect(job, &StorageJob::finished, this, [this, storageId = item->id, generation, job]() {
        auto *current = storageItem(storageId);
        if (!current || generation != current->refreshGeneration) {
            job->deleteLater();
            return;
        }
        refreshJobs_.remove(storageId);
        current->loading = false;
        if (job->state() == StorageJob::Succeeded) {
            current->allNotes = job->result();
            current->sortAllNotes();
            current->errorString.clear();
            replaceVisibleNotes(current, searchActive_ ? current->allNotes.size() : pageSize_);
        } else if (job->state() != StorageJob::Cancelled) {
            current->errorString = job->error().message.isEmpty() ? tr("Failed to load notes") : job->error().message;
        }
        const auto parentIndex = storageIndex(storageId);
        if (parentIndex.isValid())
            emit dataChanged(parentIndex, parentIndex,
                             { LoadingRole, ErrorStringRole, HasMoreRole, NoteCountRole, AccessibleRole });
        emit statsChanged();
        job->deleteLater();
    });
}

void NotesModel::replaceVisibleNotes(NMMItem *storageItem, int desiredCount)
{
    if (!storageItem)
        return;
    const auto parentIndex   = storageIndex(storageItem->id);
    const int  previousCount = storageItem->children.size();
    if (previousCount > 0) {
        beginRemoveRows(parentIndex, 0, previousCount - 1);
        qDeleteAll(storageItem->children);
        storageItem->children.clear();
        endRemoveRows();
    }
    if (desiredCount < 0)
        desiredCount = pageSize_;
    const int count = qMin(desiredCount, storageItem->allNotes.size());
    if (count > 0) {
        beginInsertRows(parentIndex, 0, count - 1);
        for (int i = 0; i < count; ++i)
            storageItem->children.append(new NMMItem(storageItem->allNotes.at(i), storageItem));
        endInsertRows();
    }
    if (parentIndex.isValid())
        emit dataChanged(parentIndex, parentIndex, { HasMoreRole, NoteCountRole });
}

QModelIndex NotesModel::storageIndex(const QString &storageId) const
{
    auto     *item = storageItem(storageId);
    const int row  = storages_.indexOf(item);
    return row < 0 ? QModelIndex() : createIndex(row, 0, item);
}

QModelIndex NotesModel::noteIndex(const QString &storageId, const QString &noteId) const
{
    auto *storage = storageItem(storageId);
    if (!storage)
        return {};
    for (int i = 0; i < storage->children.size(); ++i) {
        if (storage->children.at(i)->id == noteId)
            return createIndex(i, 0, storage->children.at(i));
    }
    return {};
}

NMMItem *NotesModel::storageItem(const QString &storageId) const
{
    for (auto *item : storages_) {
        if (item->id == storageId)
            return item;
    }
    return nullptr;
}

void NotesModel::updateNoteSummary(const Note &note, bool remove)
{
    if (note.isNull() || !note.storage())
        return;
    auto *storage = storageItem(note.storage()->systemName());
    if (!storage)
        return;

    const int previousVisibleCount = storage->children.size();
    const int existing             = storage->allNoteIndex(note.id());
    if (existing >= 0)
        storage->allNotes.removeAt(existing);
    if (!remove)
        storage->allNotes.append(note);
    storage->sortAllNotes();
    replaceVisibleNotes(storage, searchActive_ ? storage->allNotes.size() : qMax(previousVisibleCount, pageSize_));
    emit statsChanged();
}

} // namespace QtNote
