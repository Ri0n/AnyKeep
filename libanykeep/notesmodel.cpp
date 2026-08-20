#include "notesmodel.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notetitleresolver.h"
#include "storageiconimageprovider.h"

#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QSet>

#include <algorithm>
#include <utility>

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

    bool isUnpublishedDraft(const DraftRecord &draft)
    {
        return draft.remoteNoteId.isEmpty() && draft.removeSourceStorageId.isEmpty()
            && draft.removeSourceNoteId.isEmpty();
    }
}

class NMMItem {
public:
    explicit NMMItem(const NoteStorage::Ptr &storage) : type(NotesModel::ItemStorage), storage(storage)
    {
        id    = storage ? storage->systemName() : QString();
        title = storage ? storage->name() : QString();
    }

    NMMItem(QString syntheticId, QString syntheticTitle) : type(NotesModel::ItemStorage), syntheticStorage(true)
    {
        id    = std::move(syntheticId);
        title = std::move(syntheticTitle);
    }

    NMMItem(const Note &note, NMMItem *parent) : parent(parent), type(NotesModel::ItemNote) { assign(note); }

    NMMItem(const DraftRecord &draft, NMMItem *parent, QString presentedId) : parent(parent), type(NotesModel::ItemNote)
    {
        id = std::move(presentedId);
        assignDraft(draft);
    }

    ~NMMItem() { qDeleteAll(children); }

    void assign(const Note &note)
    {
        summary      = note;
        id           = note.id();
        title        = note.displayTitle();
        tags         = note.tags();
        lastChange   = note.lastChangeUTC();
        preview      = notePreview(note);
        pendingDraft = false;
        draftId      = {};
        draftState.clear();
        draftError.clear();
    }

    void assignDraft(const DraftRecord &draft)
    {
        pendingDraft = true;
        draftId      = draft.id;
        title        = NoteTitleResolver::displayTitle(draft.title, draft.body, draft.format);
        if (title.isEmpty())
            title = QObject::tr("Untitled note");
        tags       = draft.tags;
        lastChange = draft.updatedAt;
        preview    = draftPreview(draft);
        draftState = draftStateName(draft.state);
        draftError = draft.lastError;
    }

    NMMItem             *parent { nullptr };
    NotesModel::ItemType type { NotesModel::ItemStorage };
    NoteStorage::Ptr     storage;
    QList<NMMItem *>     children;
    Note                 summary;
    QString              title;
    QString              id;
    QStringList          tags;
    QString              preview;
    QDateTime            lastChange;
    QUuid                draftId;
    QString              draftState;
    QString              draftError;
    int                  preSearchVisibleCount { 0 };
    int                  indexedCount { 0 };
    bool                 indexedCountInitialized { false };
    bool                 syntheticStorage { false };
    bool                 pendingDraft { false };
};

NotesModel::NotesModel(QObject *parent) : NotesModel(nullptr, DraftManager::instance(), parent) {}

NotesModel::NotesModel(FolderCatalogManager *folderCatalogManager, QObject *parent) :
    NotesModel(folderCatalogManager, DraftManager::instance(), parent)
{
}

NotesModel::NotesModel(FolderCatalogManager *folderCatalogManager, DraftManager *draftManager, QObject *parent) :
    QAbstractItemModel(parent)
{
    folderCatalogManager_ = folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance();
    draftManager_         = draftManager ? draftManager : DraftManager::instance();

    const auto manager = NoteManager::instance();
    for (const auto &storage : manager->prioritizedStorages(true))
        storageAdded(storage);

    connect(manager, &NoteManager::storageAdded, this, &NotesModel::storageAdded);
    connect(manager, &NoteManager::storageAboutToBeRemoved, this, &NotesModel::storageAboutToBeRemoved);
    connect(manager, &NoteManager::storageChanged, this, &NotesModel::storageChanged);
    connect(manager, &NoteManager::storageReady, this, &NotesModel::storageReady);
    connect(manager, &NoteManager::storageOrderChanged, this, &NotesModel::storageOrderChanged);
    connect(manager->notesIndex(), &NotesIndex::storageNotesChanged, this, &NotesModel::storageIndexChanged);
    connect(manager->notesIndex(), &NotesIndex::storageStateChanged, this, &NotesModel::storageIndexStateChanged);
    connect(draftManager_, &DraftManager::draftsChanged, this, &NotesModel::draftsChanged);
    connect(folderCatalogManager_, &FolderCatalogManager::catalogChanged, this, [this] {
        for (auto *item : std::as_const(storages_)) {
            const int total   = projectedNoteCount(item);
            const int desired = searchActive_ ? total : qMax(item->children.size(), pageSize_);
            replaceVisibleNotes(item, desired);
        }
        emit statsChanged();
    });
    draftsChanged();
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
        if (storage && item->syntheticStorage)
            return true;
        if (!storage && item->parent && item->parent->syntheticStorage)
            return item->draftState != QLatin1String("publishing");
        return storage ? bool(item->storage && item->storage->isAccessible())
                       : bool(item->parent && item->parent->storage && item->parent->storage->isAccessible());
    case LoadingRole:
        return storage && !item->syntheticStorage && NoteManager::instance()->notesIndex()->isLoading(item->id);
    case ErrorStringRole:
        return storage && !item->syntheticStorage ? NoteManager::instance()->notesIndex()->errorString(item->id)
                                                  : item->draftError;
    case HasMoreRole:
        return storage && item->children.size() < projectedNoteCount(item);
    case NoteCountRole:
        return storage ? projectedNoteCount(item) : 0;
    case IconSourceRole:
        if (item->syntheticStorage || (!storage && item->parent && item->parent->syntheticStorage))
            return QStringLiteral("qrc:/icons/document-open-recent-symbolic.svg");
        return storageIconSource(storage ? item->id : item->parent->id, !storage);
    case PendingDraftRole:
        return !storage && item->pendingDraft;
    case DraftStateRole:
        return !storage ? item->draftState : QString();
    case DraftErrorRole:
        return !storage ? item->draftError : QString();
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
        return item->syntheticStorage ? Qt::ItemIsEnabled : (Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
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
        { PendingDraftRole, "pendingDraft" },
        { DraftStateRole, "draftState" },
        { DraftErrorRole, "draftError" },
    };
}

bool NotesModel::canFetchMore(const QModelIndex &parentIndex) const
{
    if (!parentIndex.isValid())
        return false;
    auto *item = static_cast<NMMItem *>(parentIndex.internalPointer());
    return item && item->type == ItemStorage && !item->syntheticStorage
        && item->children.size() < projectedNoteCount(item);
}

void NotesModel::fetchMore(const QModelIndex &parentIndex)
{
    if (!canFetchMore(parentIndex))
        return;
    auto *item = static_cast<NMMItem *>(parentIndex.internalPointer());
    replaceVisibleNotes(item, qMin(projectedNoteCount(item), item->children.size() + pageSize_));
}

bool NotesModel::fetchMoreNear(const QString &storageId, const QString &lastVisibleNoteId)
{
    auto *item = storageItem(storageId);
    if (!item || lastVisibleNoteId.isEmpty())
        return false;

    const auto parentIndex = storageIndex(storageId);
    if (!canFetchMore(parentIndex))
        return false;

    const auto visible
        = std::find_if(item->children.cbegin(), item->children.cend(),
                       [&lastVisibleNoteId](NMMItem *note) { return note && note->id == lastVisibleNoteId; });
    if (visible == item->children.cend())
        return false;

    constexpr int PrefetchDistance = 5;
    const int     visibleRow       = int(std::distance(item->children.cbegin(), visible));
    if (visibleRow < item->children.size() - PrefetchDistance)
        return false;

    const int previousCount = item->children.size();
    fetchMore(parentIndex);
    return item->children.size() > previousCount;
}

Qt::DropActions NotesModel::supportedDropActions() const { return Qt::MoveAction; }

QStringList NotesModel::mimeTypes() const { return { QStringLiteral("application/anykeep.notes.list") }; }

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
    mime->setData(QStringLiteral("application/anykeep.notes.list"), payload);
    return mime;
}

bool NotesModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int, int column,
                              const QModelIndex &parentIndex)
{
    if (action == Qt::IgnoreAction)
        return true;
    if (!data || !data->hasFormat(QStringLiteral("application/anykeep.notes.list")) || column > 0
        || !parentIndex.isValid()) {
        return false;
    }
    auto *destination = static_cast<NMMItem *>(parentIndex.internalPointer());
    if (!destination || destination->type != ItemStorage || destination->syntheticStorage)
        return false;

    QByteArray  payload = data->data(QStringLiteral("application/anykeep.notes.list"));
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
        count += projectedNoteCount(storage);
    return count;
}

void NotesModel::refresh()
{
    auto *index = NoteManager::instance()->notesIndex();
    for (auto *item : std::as_const(storages_)) {
        if (!item->syntheticStorage)
            index->refreshStorage(item->id);
    }
}

void NotesModel::refreshStorage(const QString &storageId)
{
    if (storageId != DraftManager::draftsStorageId())
        NoteManager::instance()->notesIndex()->refreshStorage(storageId);
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
            replaceVisibleNotes(item, projectedNoteCount(item));
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
    int row = storageItem(DraftManager::draftsStorageId()) ? 1 : 0;
    for (const auto &candidate : NoteManager::instance()->prioritizedStorages(true)) {
        if (candidate == storage)
            break;
        if (candidate && storageItem(candidate->systemName()))
            ++row;
    }
    beginInsertRows({}, row, row);
    auto *item                    = new NMMItem(storage);
    item->indexedCount            = indexedNotes(item->id).size();
    item->indexedCountInitialized = NoteManager::instance()->notesIndex()->hasSnapshot(item->id);
    storages_.insert(row, item);
    endInsertRows();
    replaceVisibleNotes(item, searchActive_ ? projectedNoteCount(item) : pageSize_);
    emit statsChanged();
}

void NotesModel::storageAboutToBeRemoved(const NoteStorage::Ptr &storage)
{
    if (!storage)
        return;
    const int row = storages_.indexOf(storageItem(storage->systemName()));
    if (row < 0)
        return;
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

void NotesModel::storageReady(const NoteStorage::Ptr &storage) { storageChanged(storage); }

void NotesModel::storageOrderChanged()
{
    QList<NMMItem *> ordered;
    ordered.reserve(storages_.size());
    if (auto *drafts = storageItem(DraftManager::draftsStorageId()))
        ordered.append(drafts);
    for (const auto &storage : NoteManager::instance()->prioritizedStorages(true)) {
        if (auto *item = storageItem(storage ? storage->systemName() : QString()))
            ordered.append(item);
    }
    for (auto *item : std::as_const(storages_)) {
        if (!ordered.contains(item))
            ordered.append(item);
    }
    if (ordered == storages_)
        return;
    beginResetModel();
    storages_ = ordered;
    endResetModel();
}

void NotesModel::storageIndexChanged(const QString &storageId)
{
    auto *item = storageItem(storageId);
    if (!item)
        return;
    const int indexedCount = indexedNotes(storageId).size();
    int       desiredCount = searchActive_ ? projectedNoteCount(item) : qMax(item->children.size(), pageSize_);
    if (!searchActive_ && item->indexedCountInitialized && indexedCount > item->indexedCount) {
        // Keep the previously visible tail when a newly published note lands
        // inside the current page. Otherwise the old last row is displaced
        // just beyond the pagination boundary and appears to have vanished.
        desiredCount += indexedCount - item->indexedCount;
    }
    item->indexedCount            = indexedCount;
    item->indexedCountInitialized = true;
    replaceVisibleNotes(item, desiredCount);
    emit statsChanged();
}

void NotesModel::storageIndexStateChanged(const QString &storageId)
{
    const auto modelIndex = storageIndex(storageId);
    if (modelIndex.isValid())
        emit dataChanged(modelIndex, modelIndex,
                         { LoadingRole, ErrorStringRole, HasMoreRole, NoteCountRole, AccessibleRole });
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

    QList<NMMItem *> projected;
    const auto       pending = draftManager_ ? draftManager_->pendingDrafts() : QList<DraftRecord> {};
    if (storageItem->syntheticStorage) {
        for (const auto &draft : pending) {
            if (!isUnpublishedDraft(draft))
                continue;
            projected.append(new NMMItem(draft, storageItem, draft.id.toString(QUuid::WithoutBraces)));
        }
    } else {
        QHash<QString, DraftRecord> draftByNote;
        for (const auto &draft : pending) {
            if (draft.storageId != storageItem->id || isUnpublishedDraft(draft))
                continue;
            const QString presentedId
                = draft.remoteNoteId.isEmpty() ? draft.id.toString(QUuid::WithoutBraces) : draft.remoteNoteId;
            const auto it = draftByNote.constFind(presentedId);
            if (it == draftByNote.cend() || draft.updatedAt > it->updatedAt)
                draftByNote.insert(presentedId, draft);
        }

        QSet<QString> presented;
        for (const auto &note : indexedNotes(storageItem->id)) {
            auto      *item  = new NMMItem(note, storageItem);
            const auto draft = draftByNote.constFind(note.id());
            if (draft != draftByNote.cend())
                item->assignDraft(*draft);
            projected.append(item);
            presented.insert(note.id());
        }
        for (auto it = draftByNote.cbegin(); it != draftByNote.cend(); ++it) {
            if (!presented.contains(it.key()))
                projected.append(new NMMItem(it.value(), storageItem, it.key()));
        }
    }

    std::stable_sort(projected.begin(), projected.end(), [](const NMMItem *left, const NMMItem *right) {
        if (left->pendingDraft != right->pendingDraft)
            return left->pendingDraft;
        if (left->lastChange != right->lastChange)
            return left->lastChange > right->lastChange;
        return left->title.localeAwareCompare(right->title) < 0;
    });

    const int count = storageItem->syntheticStorage ? projected.size() : qMin(desiredCount, projected.size());
    while (projected.size() > count)
        delete projected.takeLast();
    if (count > 0) {
        beginInsertRows(parentIndex, 0, count - 1);
        storageItem->children = std::move(projected);
        endInsertRows();
    }
    if (parentIndex.isValid())
        emit dataChanged(parentIndex, parentIndex, { HasMoreRole, NoteCountRole });
}

void NotesModel::draftsChanged()
{
    const auto pending        = draftManager_ ? draftManager_->pendingDrafts() : QList<DraftRecord> {};
    const bool hasLocalDrafts = std::any_of(pending.cbegin(), pending.cend(),
                                            [](const DraftRecord &draft) { return isUnpublishedDraft(draft); });
    auto      *draftStorage   = storageItem(DraftManager::draftsStorageId());
    if (hasLocalDrafts && !draftStorage) {
        beginInsertRows({}, 0, 0);
        draftStorage = new NMMItem(DraftManager::draftsStorageId(), tr("Drafts"));
        storages_.prepend(draftStorage);
        endInsertRows();
    } else if (!hasLocalDrafts && draftStorage) {
        const int row = storages_.indexOf(draftStorage);
        beginRemoveRows({}, row, row);
        storages_.removeAt(row);
        delete draftStorage;
        endRemoveRows();
        draftStorage = nullptr;
    }

    for (auto *item : std::as_const(storages_)) {
        const int total = projectedNoteCount(item);
        const int desired
            = item->syntheticStorage ? total : (searchActive_ ? total : qMax(item->children.size(), pageSize_));
        replaceVisibleNotes(item, desired);
    }
    emit statsChanged();
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

QList<Note> NotesModel::indexedNotes(const QString &storageId) const
{
    auto notes = NoteManager::instance()->notesIndex()->notes(storageId);
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return notes;
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [this, &storageId](const Note &note) {
                                   return !note.isNull()
                                       && folderCatalogManager_->catalog().isRecycled(storageId, note.id());
                               }),
                notes.end());
    return notes;
}

int NotesModel::projectedNoteCount(const NMMItem *storageItem) const
{
    if (!storageItem)
        return 0;
    const auto pending = draftManager_ ? draftManager_->pendingDrafts() : QList<DraftRecord> {};
    if (storageItem->syntheticStorage) {
        return int(std::count_if(pending.cbegin(), pending.cend(),
                                 [](const DraftRecord &draft) { return isUnpublishedDraft(draft); }));
    }

    const auto    notes = indexedNotes(storageItem->id);
    QSet<QString> ids;
    ids.reserve(notes.size());
    for (const auto &note : notes)
        ids.insert(note.id());
    for (const auto &draft : pending) {
        if (draft.storageId != storageItem->id || isUnpublishedDraft(draft))
            continue;
        ids.insert(draft.remoteNoteId.isEmpty() ? draft.id.toString(QUuid::WithoutBraces) : draft.remoteNoteId);
    }
    return ids.size();
}

} // namespace AnyKeep
