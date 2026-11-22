#include "notesmodel.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "notemanager.h"
#include "notepresentationorder.h"
#include "notesindex.h"
#include "notetitleresolver.h"
#include "projectionrowdiff_p.h"
#include "storageiconimageprovider.h"

#include <QAbstractProxyModel>
#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QScopedValueRollback>
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
        case DraftRecord::Deleting:
            return QStringLiteral("deleting");
        }
        return {};
    }

    bool isUnpublishedDraft(const DraftRecord &draft)
    {
        return draft.remoteNoteId.isEmpty() && draft.removeSourceStorageId.isEmpty()
            && draft.removeSourceNoteId.isEmpty();
    }
}

struct NMMNote {
    NMMNote() = default;
    explicit NMMNote(const Note &note)
    {
        id    = note.id();
        title = note.displayTitle();
        // Loaded notes and PTF summaries are not always able to synthesize a
        // display title (for example, a note starting with a non-text block).
        // Present the same fallback as an unpublished draft instead of
        // rendering a visually empty list row.
        if (title.isEmpty())
            title = QObject::tr("Untitled note");
        tags       = note.tags();
        lastChange = note.lastChangeUTC();
        preview    = notePreview(note);
        favorite   = note.isFavorite();
    }

    void assignDraft(const DraftRecord &draft)
    {
        pendingDraft = true;
        title        = NoteTitleResolver::displayTitle(draft.title, draft.body, draft.format);
        if (title.isEmpty())
            title = QObject::tr("Untitled note");
        tags                   = draft.tags;
        lastChange             = draft.updatedAt;
        preview                = draftPreview(draft);
        const auto favoriteKey = QString::fromLatin1(FavoriteBackendKey);
        if (draft.backendData.contains(favoriteKey))
            favorite = draft.backendData.value(favoriteKey).toBool();
        draftState = draftStateName(draft.state);
        draftError = draft.lastError;
    }

    QList<int> changedRoles(const NMMNote &next, bool syntheticStorage) const
    {
        using Model = NotesModel;
        QList<int> roles;
        if (title != next.title)
            roles.append({ Qt::DisplayRole, Model::TitleRole });
        if (tags != next.tags)
            roles.append(Model::TagsRole);
        if (preview != next.preview)
            roles.append(Model::PreviewRole);
        if (lastChange != next.lastChange)
            roles.append(Model::ModifiedTimeRole);
        if (favorite != next.favorite)
            roles.append(Model::FavoriteRole);
        if (pendingDraft != next.pendingDraft)
            roles.append(Model::PendingDraftRole);
        if (draftState != next.draftState) {
            roles.append(Model::DraftStateRole);
            if (syntheticStorage
                && (draftState == QLatin1String("publishing")) != (next.draftState == QLatin1String("publishing")))
                roles.append(Model::AccessibleRole);
        }
        if (draftError != next.draftError)
            roles.append({ Model::DraftErrorRole, Model::ErrorStringRole });
        if (recycled != next.recycled)
            roles.append(Model::RecycledRole);
        return roles;
    }

    QString     id;
    QString     title;
    QStringList tags;
    QString     preview;
    QDateTime   lastChange;
    QString     draftState;
    QString     draftError;
    bool        favorite { false };
    bool        pendingDraft { false };
    bool        recycled { false };
};

class NMMItem {
public:
    explicit NMMItem(const NoteStorage::Ptr &storage) : storage(storage)
    {
        values.id    = storage->systemName();
        values.title = storage->name();
        accessible   = storage->isAccessible();
    }

    NMMItem(QString syntheticId, QString syntheticTitle) : syntheticStorage(true), accessible(true)
    {
        values.id    = std::move(syntheticId);
        values.title = std::move(syntheticTitle);
    }

    NMMItem(NMMNote note, NMMItem *parent) : parent(parent), type(NotesModel::ItemNote), values(std::move(note)) {}

    ~NMMItem() { qDeleteAll(children); }

    NMMItem             *parent { nullptr };
    NotesModel::ItemType type { NotesModel::ItemStorage };
    NoteStorage::Ptr     storage;
    QList<NMMItem *>     children;
    NMMNote              values;
    QString              storageError;
    int                  projectedCount { 0 };
    int                  preSearchVisibleCount { 0 };
    int                  indexedCount { 0 };
    bool                 indexedCountInitialized { false };
    bool                 syntheticStorage { false };
    bool                 accessible { false };
    bool                 loading { false };
    bool                 reconciling { false };
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
    const auto refreshFolderProjection = [this] {
        const int previousCount = noteCount();
        for (auto *item : std::as_const(storages_))
            reconcileVisibleNotes(item, qMax(item->children.size(), pageSize_));
        if (noteCount() != previousCount)
            emit statsChanged();
    };
    connect(folderCatalogManager_, &FolderCatalogManager::catalogChanged, this, refreshFolderProjection);
    // A loaded catalog can already contain recycle-bin assignments. Refresh
    // even when loading it only changes availability, otherwise a model that
    // was created during startup can keep presenting those notes.
    connect(folderCatalogManager_, &FolderCatalogManager::availabilityChanged, this,
            [refreshFolderProjection](bool) { refreshFolderProjection(); });
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
        return item->values.title;
    case Qt::DecorationRole:
        if (storage)
            return item->storage ? item->storage->storageIcon() : QIcon();
        return item->parent && item->parent->storage ? item->parent->storage->noteIcon() : QIcon();
    case StorageIdRole:
        return storage ? item->values.id : item->parent->values.id;
    case NoteIdRole:
        return storage ? QString() : item->values.id;
    case ItemTypeRole:
        return item->type;
    case TagsRole:
        return storage ? QStringList() : item->values.tags;
    case PreviewRole:
        return storage ? QString() : item->values.preview;
    case ModifiedTimeRole:
        return storage ? QVariant() : item->values.lastChange;
    case StorageNameRole:
        return storage ? item->values.title : item->parent->values.title;
    case AccessibleRole:
        if (!storage && item->parent && item->parent->syntheticStorage)
            return item->values.draftState != QLatin1String("publishing");
        return storage ? item->accessible : item->parent->accessible;
    case LoadingRole:
        return storage && item->loading;
    case ErrorStringRole:
        return storage ? item->storageError : item->values.draftError;
    case HasMoreRole:
        return storage && item->children.size() < item->projectedCount;
    case NoteCountRole:
        return storage ? item->projectedCount : 0;
    case IconSourceRole:
        if (item->syntheticStorage || (!storage && item->parent && item->parent->syntheticStorage))
            return QStringLiteral("qrc:/icons/document-open-recent-symbolic.svg");
        return storageIconSource(storage ? item->values.id : item->parent->values.id, !storage);
    case FavoriteRole:
        return !storage && item->values.favorite;
    case PendingDraftRole:
        return !storage && item->values.pendingDraft;
    case DraftStateRole:
        return !storage ? item->values.draftState : QString();
    case DraftErrorRole:
        return !storage ? item->values.draftError : QString();
    case RecycledRole:
        return !storage && item->values.recycled;
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
        { FavoriteRole, "favorite" },
        { PendingDraftRole, "pendingDraft" },
        { DraftStateRole, "draftState" },
        { DraftErrorRole, "draftError" },
        { RecycledRole, "recycled" },
    };
}

bool NotesModel::canFetchMore(const QModelIndex &parentIndex) const
{
    if (!parentIndex.isValid())
        return false;
    auto *item = static_cast<NMMItem *>(parentIndex.internalPointer());
    return item && item->type == ItemStorage && !item->syntheticStorage && !item->reconciling
        && item->children.size() < item->projectedCount;
}

void NotesModel::fetchMore(const QModelIndex &parentIndex)
{
    if (!canFetchMore(parentIndex))
        return;
    auto *item = static_cast<NMMItem *>(parentIndex.internalPointer());
    reconcileVisibleNotes(item, item->children.size() + pageSize_);
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
                       [&lastVisibleNoteId](NMMItem *note) { return note && note->values.id == lastVisibleNoteId; });
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
        const QString key = item->parent->values.id + QLatin1Char('\n') + item->values.id;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        stream << item->parent->values.id << item->values.id;
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
        if (!storageId.isEmpty() && !noteId.isEmpty() && storageId != destination->values.id) {
            sourceStorageIds.append(storageId);
            noteIds.append(noteId);
        }
    }
    if (noteIds.isEmpty())
        return false;
    emit notesDropRequested(sourceStorageIds, noteIds, destination->values.id);
    return true;
}

int NotesModel::noteCount() const
{
    int count = 0;
    for (const auto *storage : storages_)
        count += storage->projectedCount;
    return count;
}

void NotesModel::refresh()
{
    auto *index = NoteManager::instance()->notesIndex();
    for (auto *item : std::as_const(storages_)) {
        if (!item->syntheticStorage)
            index->refreshStorage(item->values.id);
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
            reconcileVisibleNotes(item, pageSize_);
        } else {
            reconcileVisibleNotes(item, qMax(pageSize_, item->preSearchVisibleCount));
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
        reconcileVisibleNotes(item, qMax(item->children.size(), pageSize_));
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
    auto       *item              = new NMMItem(storage);
    const auto *noteIndex         = NoteManager::instance()->notesIndex();
    item->loading                 = noteIndex->isLoading(item->values.id);
    item->storageError            = noteIndex->errorString(item->values.id);
    item->indexedCount            = indexedNotes(item->values.id).size();
    item->indexedCountInitialized = noteIndex->hasSnapshot(item->values.id);
    storages_.insert(row, item);
    endInsertRows();
    reconcileVisibleNotes(item, pageSize_);
    if (item->projectedCount != 0)
        emit statsChanged();
}

void NotesModel::storageAboutToBeRemoved(const NoteStorage::Ptr &storage)
{
    if (!storage)
        return;
    const int row = storages_.indexOf(storageItem(storage->systemName()));
    if (row < 0)
        return;
    const bool countChanged = storages_.at(row)->projectedCount != 0;
    beginRemoveRows({}, row, row);
    delete storages_.takeAt(row);
    endRemoveRows();
    if (countChanged)
        emit statsChanged();
}

void NotesModel::storageChanged(const NoteStorage::Ptr &storage)
{
    auto *item = storageItem(storage ? storage->systemName() : QString());
    if (!item || !storage)
        return;
    QList<int> parentRoles;
    QList<int> childRoles;
    if (std::exchange(item->storage, storage) != storage) {
        parentRoles.append(Qt::DecorationRole);
        childRoles.append(Qt::DecorationRole);
    }
    const auto title = storage->name();
    if (std::exchange(item->values.title, title) != title) {
        parentRoles.append({ Qt::DisplayRole, TitleRole, StorageNameRole });
        childRoles.append(StorageNameRole);
    }
    const bool accessible = storage->isAccessible();
    if (std::exchange(item->accessible, accessible) != accessible) {
        parentRoles.append(AccessibleRole);
        childRoles.append(AccessibleRole);
    }
    const auto *noteIndex = NoteManager::instance()->notesIndex();
    const auto  loading   = noteIndex->isLoading(item->values.id);
    const auto  error     = noteIndex->errorString(item->values.id);
    if (std::exchange(item->loading, loading) != loading)
        parentRoles.append(LoadingRole);
    if (std::exchange(item->storageError, error) != error)
        parentRoles.append(ErrorStringRole);
    const auto modelIndex = storageIndex(item->values.id);
    if (!parentRoles.isEmpty())
        emit dataChanged(modelIndex, modelIndex, parentRoles);
    if (!childRoles.isEmpty() && !item->children.isEmpty())
        emit dataChanged(index(0, 0, modelIndex), index(item->children.size() - 1, 0, modelIndex), childRoles);
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
    for (int destination = 0; destination < ordered.size(); ++destination) {
        const int source = storages_.indexOf(ordered.at(destination), destination);
        if (source == destination)
            continue;
        beginMoveRows({}, source, source, {}, destination);
        storages_.move(source, destination);
        endMoveRows();
    }
}

void NotesModel::storageIndexChanged(const QString &storageId)
{
    auto *item = storageItem(storageId);
    if (!item)
        return;
    const int indexedCount = indexedNotes(storageId).size();
    int       desiredCount = qMax(item->children.size(), pageSize_);
    if (!searchActive_ && item->indexedCountInitialized && indexedCount > item->indexedCount) {
        // Keep the previously visible tail when a newly published note lands
        // inside the current page. Otherwise the old last row is displaced
        // just beyond the pagination boundary and appears to have vanished.
        desiredCount += indexedCount - item->indexedCount;
    }
    item->indexedCount            = indexedCount;
    item->indexedCountInitialized = true;
    const int previousCount       = item->projectedCount;
    reconcileVisibleNotes(item, desiredCount);
    if (item->projectedCount != previousCount)
        emit statsChanged();
}

void NotesModel::storageIndexStateChanged(const QString &storageId)
{
    if (auto *item = storageItem(storageId))
        storageChanged(item->storage);
}

void NotesModel::reconcileVisibleNotes(NMMItem *storageItem, int desiredCount)
{
    if (storageItem->reconciling)
        return;
    const QScopedValueRollback guard(storageItem->reconciling, true);
    const auto                 parentIndex       = storageIndex(storageItem->values.id);
    auto                      &children          = storageItem->children;
    const bool                 previouslyHadMore = children.size() < storageItem->projectedCount;
    auto                       projection        = projectStorageNotes(storageItem);
    const int                  total             = projection.size();
    if (!storageItem->syntheticStorage && !searchActive_ && desiredCount < total)
        projection.resize(desiredCount);

    QList<QString> currentKeys;
    currentKeys.reserve(children.size());
    for (const auto *child : std::as_const(children))
        currentKeys.append(child->values.id);
    QList<QString> nextKeys;
    nextKeys.reserve(projection.size());
    for (const auto &note : std::as_const(projection))
        nextKeys.append(note.id);

    reconcileProjectionRows(
        std::move(currentKeys), nextKeys,
        [&](int first, int last) {
            beginRemoveRows(parentIndex, first, last);
            qDeleteAll(children.cbegin() + first, children.cbegin() + last + 1);
            children.remove(first, last - first + 1);
            endRemoveRows();
        },
        [&](int from, int to) {
            beginMoveRows(parentIndex, from, from, parentIndex, from < to ? to + 1 : to);
            children.move(from, to);
            endMoveRows();
        },
        [&](int first, int last) {
            beginInsertRows(parentIndex, first, last);
            for (int row = first; row <= last; ++row)
                children.insert(row, new NMMItem(projection.at(row), storageItem));
            endInsertRows();
        });

    for (int row = 0; row < children.size(); ++row) {
        auto       &current = children.at(row)->values;
        const auto &next    = projection.at(row);
        const auto  roles   = current.changedRoles(next, storageItem->syntheticStorage);
        if (roles.isEmpty())
            continue;
        current              = next;
        const auto noteIndex = index(row, 0, parentIndex);
        emit       dataChanged(noteIndex, noteIndex, roles);
    }

    QList<int> parentRoles;
    if (std::exchange(storageItem->projectedCount, total) != total)
        parentRoles.append(NoteCountRole);
    if (previouslyHadMore != (children.size() < total))
        parentRoles.append(HasMoreRole);
    if (!parentRoles.isEmpty())
        emit dataChanged(parentIndex, parentIndex, parentRoles);
}

QList<NMMNote> NotesModel::projectStorageNotes(const NMMItem *storageItem) const
{
    QList<NMMNote> projected;
    const auto     pending     = draftManager_ ? draftManager_->pendingDrafts() : QList<DraftRecord> {};
    const auto     appendDraft = [&](const DraftRecord &draft, const QString &presentedId) {
        NMMNote row;
        row.id = presentedId;
        row.assignDraft(draft);
        projected.append(std::move(row));
    };
    QSet<QString> movedSourceIds;
    if (!storageItem->syntheticStorage) {
        for (const auto &draft : pending) {
            if ((draft.operation == DraftRecord::Delete || draft.state == DraftRecord::Deleting)
                && draft.storageId == storageItem->values.id && !draft.remoteNoteId.isEmpty())
                movedSourceIds.insert(draft.remoteNoteId);
            if (draft.removeSourceStorageId == storageItem->values.id && !draft.removeSourceNoteId.isEmpty())
                movedSourceIds.insert(draft.removeSourceNoteId);
        }
    }
    if (storageItem->syntheticStorage) {
        for (const auto &draft : pending) {
            if (!isUnpublishedDraft(draft))
                continue;
            appendDraft(draft, draft.id.toString(QUuid::WithoutBraces));
        }
        if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
            for (auto &note : projected)
                note.recycled = folderCatalogManager_->catalog().isRecycled(storageItem->values.id, note.id);
        }
    } else {
        QHash<QString, DraftRecord> draftByNote;
        for (const auto &draft : pending) {
            if (draft.operation == DraftRecord::Delete || draft.state == DraftRecord::Deleting
                || (folderCatalogManager_ && folderCatalogManager_->isAvailable() && !draft.remoteNoteId.isEmpty()
                    && folderCatalogManager_->catalog().isRecycled(storageItem->values.id, draft.remoteNoteId)))
                continue;
            if (draft.storageId != storageItem->values.id || isUnpublishedDraft(draft))
                continue;
            const QString presentedId
                = draft.remoteNoteId.isEmpty() ? draft.id.toString(QUuid::WithoutBraces) : draft.remoteNoteId;
            const auto it = draftByNote.constFind(presentedId);
            if (it == draftByNote.cend() || draft.updatedAt > it->updatedAt)
                draftByNote.insert(presentedId, draft);
        }

        QSet<QString> presented;
        for (const auto &note : indexedNotes(storageItem->values.id)) {
            if (movedSourceIds.contains(note.id()) || presented.contains(note.id()))
                continue;
            NMMNote    item(note);
            const auto draft = draftByNote.constFind(note.id());
            if (draft != draftByNote.cend())
                item.assignDraft(*draft);
            projected.append(std::move(item));
            presented.insert(note.id());
        }
        for (auto it = draftByNote.cbegin(); it != draftByNote.cend(); ++it) {
            if (!presented.contains(it.key()))
                appendDraft(it.value(), it.key());
        }
    }

    const auto comesBefore = [](const NMMNote &left, const NMMNote &right) {
        return notePresentationComesBefore(left.pendingDraft, left.favorite, left.lastChange, left.title,
                                           right.pendingDraft, right.favorite, right.lastChange, right.title);
    };
    std::sort(projected.begin(), projected.end(), [&comesBefore](const NMMNote &left, const NMMNote &right) {
        return comesBefore(left, right) || (!comesBefore(right, left) && left.id < right.id);
    });

    return projected;
}

QVariantList NotesModel::storageNoteDescriptors(const QString &storageId, const QAbstractProxyModel *filter) const
{
    auto *storage = storageItem(storageId);
    if (!storage)
        return {};

    // Search already exposes every matching row. Without search, read the
    // full projection without fetching pages or touching the attached view.
    const auto   parent     = storageIndex(storageId);
    const auto   projection = filter ? QList<NMMNote> {} : projectStorageNotes(storage);
    const int    count      = filter ? storage->children.size() : projection.size();
    QVariantList result;
    result.reserve(count);
    for (int row = 0; row < count; ++row) {
        if (filter && !filter->mapFromSource(index(row, 0, parent)).isValid())
            continue;
        const auto &note = filter ? storage->children.at(row)->values : projection.at(row);
        result.append(QVariantMap {
            { QStringLiteral("storageId"), storageId },
            { QStringLiteral("noteId"), note.id },
            { QStringLiteral("title"), note.title },
            { QStringLiteral("favorite"), note.favorite },
            { QStringLiteral("order"), row },
        });
    }
    return result;
}

void NotesModel::draftsChanged()
{
    const int  previousCount  = noteCount();
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

    for (auto *item : std::as_const(storages_))
        reconcileVisibleNotes(item, qMax(item->children.size(), pageSize_));
    if (noteCount() != previousCount)
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
        if (storage->children.at(i)->values.id == noteId)
            return createIndex(i, 0, storage->children.at(i));
    }
    return {};
}

NMMItem *NotesModel::storageItem(const QString &storageId) const
{
    for (auto *item : storages_) {
        if (item->values.id == storageId)
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

} // namespace AnyKeep
