#include "storageprioritymodel.h"

#include "filestorage.h"
#include "notemanager.h"
#include "storageiconimageprovider.h"

#include <QColor>
#include <QDataStream>
#include <QGuiApplication>
#include <QIODevice>
#include <QMimeData>
#include <QPalette>

namespace QtNote {

namespace {
    constexpr auto MimeType = "application/qtnote.storage.id";
}

StoragePriorityModel::StoragePriorityModel(QObject *parent) : QAbstractListModel(parent)
{
    resetFromManager();
    connect(NoteManager::instance(), &NoteManager::storageAdded, this, [this](const NoteStorage::Ptr &) {
        beginResetModel();
        resetFromManager();
        endResetModel();
    });
    connect(NoteManager::instance(), &NoteManager::storageRemoved, this, [this](const NoteStorage::Ptr &) {
        beginResetModel();
        resetFromManager();
        endResetModel();
    });
    connect(NoteManager::instance(), &NoteManager::storageChanged, this, [this](const NoteStorage::Ptr &storage) {
        const int row = storage ? rowForStorage(storage->systemName()) : -1;
        if (row >= 0)
            emit dataChanged(index(row), index(row));
    });
}

int StoragePriorityModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : items_.count(); }

QVariant StoragePriorityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
        return {};

    const auto &item    = items_.at(index.row());
    const auto  storage = NoteManager::instance()->storages(true).value(item.storageId);
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return item.name;
    case Qt::DecorationRole:
    case IconRole:
        return storage ? storage->storageIcon() : QIcon();
    case Qt::ToolTipRole:
    case TooltipRole:
        return storage ? storage->tooltip() : QString();
    case Qt::ForegroundRole: {
        QColor color = qGuiApp ? qGuiApp->palette().color(QPalette::WindowText) : QColor(Qt::black);
        if (storage && !storage->isAccessible())
            color.setAlpha(128);
        return color;
    }
    case StorageIdRole:
        return item.storageId;
    case IconSourceRole:
        return storageIconSource(item.storageId);
    case AccessibleRole:
        return storage ? storage->isAccessible() : false;
    case ConfigurableRole:
        return storage ? storage->isConfigurable() : false;
    default:
        return {};
    }
}

Qt::ItemFlags StoragePriorityModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags flags = QAbstractListModel::flags(index) & ~(Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
    if (index.isValid())
        return flags | Qt::ItemIsDragEnabled;
    return flags | Qt::ItemIsDropEnabled;
}

QHash<int, QByteArray> StoragePriorityModel::roleNames() const
{
    return {
        { StorageIdRole, "storageId" },       { NameRole, "name" },       { AccessibleRole, "accessible" },
        { ConfigurableRole, "configurable" }, { TooltipRole, "tooltip" }, { IconRole, "icon" },
        { IconSourceRole, "iconSource" },
    };
}

QStringList StoragePriorityModel::mimeTypes() const { return { QLatin1String(MimeType) }; }

QMimeData *StoragePriorityModel::mimeData(const QModelIndexList &indexes) const
{
    if (indexes.isEmpty())
        return nullptr;

    const auto row = indexes.first().row();
    if (row < 0 || row >= items_.size())
        return nullptr;

    auto       *mimeData = new QMimeData();
    QByteArray  encodedData;
    QDataStream out(&encodedData, QIODevice::WriteOnly);
    out << items_.at(row).storageId;
    mimeData->setData(QLatin1String(MimeType), encodedData);
    return mimeData;
}

bool StoragePriorityModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                        const QModelIndex &parent)
{
    Q_UNUSED(column)

    if (action == Qt::IgnoreAction)
        return true;
    if (action != Qt::MoveAction || !data->hasFormat(QLatin1String(MimeType)))
        return false;

    QString     storageId;
    QByteArray  encodedData = data->data(QLatin1String(MimeType));
    QDataStream in(&encodedData, QIODevice::ReadOnly);
    in >> storageId;

    const auto sourceRow = rowForStorage(storageId);
    if (sourceRow < 0)
        return false;

    int destinationRow = row;
    if (destinationRow < 0)
        destinationRow = parent.isValid() ? parent.row() : items_.size();
    return moveRows({}, sourceRow, 1, {}, destinationRow);
}

Qt::DropActions StoragePriorityModel::supportedDragActions() const { return Qt::MoveAction; }

Qt::DropActions StoragePriorityModel::supportedDropActions() const { return Qt::MoveAction; }

bool StoragePriorityModel::moveRows(const QModelIndex &sourceParent, int sourceRow, int count,
                                    const QModelIndex &destinationParent, int destinationChild)
{
    if (sourceParent.isValid() || destinationParent.isValid() || count != 1 || sourceRow < 0
        || sourceRow >= items_.size() || destinationChild < 0 || destinationChild > items_.size()
        || destinationChild == sourceRow || destinationChild == sourceRow + 1) {
        return false;
    }

    beginMoveRows(sourceParent, sourceRow, sourceRow, destinationParent, destinationChild);
    items_.move(sourceRow, destinationChild > sourceRow ? destinationChild - 1 : destinationChild);
    endMoveRows();
    return true;
}

QString StoragePriorityModel::storageId(const QModelIndex &index) const
{
    return index.isValid() && index.row() >= 0 && index.row() < items_.size() ? items_.at(index.row()).storageId
                                                                              : QString();
}

QStringList StoragePriorityModel::priorityList() const
{
    QStringList priorities;
    priorities.reserve(items_.size());
    for (const auto &item : items_)
        priorities.append(item.storageId);
    return priorities;
}

void StoragePriorityModel::resetFromManager()
{
    items_.clear();
    for (const auto &storage : NoteManager::instance()->prioritizedStorages(true)) {
        if (storage)
            items_.append({ storage->systemName(), storage->name() });
    }
}

int StoragePriorityModel::rowForStorage(const QString &storageId) const
{
    for (int row = 0; row < items_.size(); ++row) {
        if (items_.at(row).storageId == storageId)
            return row;
    }
    return -1;
}

} // namespace QtNote
