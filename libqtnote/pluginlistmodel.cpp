#include "pluginlistmodel.h"

#include <QDataStream>
#include <QFont>
#include <QGuiApplication>
#include <QIODevice>
#include <QMimeData>
#include <QPalette>

#include <utility>

namespace QtNote {

namespace {
    constexpr auto MimeType = "application/qtnote.plugin.id";

    Qt::CheckState checkStateForPolicy(PluginListSource::LoadPolicy loadPolicy)
    {
        switch (loadPolicy) {
        case PluginListSource::LP_Auto:
            return Qt::PartiallyChecked;
        case PluginListSource::LP_Disabled:
            return Qt::Unchecked;
        case PluginListSource::LP_Enabled:
            return Qt::Checked;
        }
        return Qt::Unchecked;
    }
}

PluginListModel::PluginListModel(PluginListSource *source, QObject *parent) :
    QAbstractListModel(parent), source_(source)
{
    if (source_) {
        connect(source_, &PluginListSource::pluginsReset, this, &PluginListModel::resetFromSource);
        connect(source_, &PluginListSource::pluginChanged, this, &PluginListModel::updatePlugin);
        connect(source_, &QObject::destroyed, this, [this]() {
            source_.clear();
            resetFromSource();
        });
    }
    resetFromSource();
}

int PluginListModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : pluginIds_.count(); }

QVariant PluginListModel::data(const QModelIndex &index, int role) const
{
    if (!source_ || !index.isValid() || index.row() < 0 || index.row() >= pluginIds_.size())
        return {};

    const auto entry = source_->pluginEntry(pluginIds_.at(index.row()));
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return entry.name;
    case Qt::DecorationRole:
    case IconRole:
        return entry.icon.isNull() ? QIcon(QStringLiteral(":/icons/plugin")) : entry.icon;
    case Qt::CheckStateRole:
        return checkStateForPolicy(entry.loadPolicy);
    case Qt::ToolTipRole:
    case TooltipRole:
        return entry.tooltip;
    case Qt::FontRole: {
        QFont font;
        font.setBold(entry.loadStatus == PluginListSource::LS_Initialized);
        return font;
    }
    case Qt::ForegroundRole: {
        QColor color = qGuiApp ? qGuiApp->palette().color(QPalette::WindowText) : QColor(Qt::black);
        if (entry.loadStatus != PluginListSource::LS_Initialized)
            color.setAlpha(128);
        return color;
    }
    case PluginIdRole:
        return entry.id;
    case DescriptionRole:
        return entry.description;
    case VersionTextRole:
        return entry.versionText;
    case LoadPolicyRole:
        return int(entry.loadPolicy);
    case LoadStatusRole:
        return int(entry.loadStatus);
    case LoadedRole:
        return entry.loaded;
    case EnabledRole:
        return entry.loadPolicy != PluginListSource::LP_Disabled;
    case ConfigurableRole:
        return entry.configurable;
    case IconSourceRole:
        return entry.iconSource;
    default:
        return {};
    }
}

bool PluginListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!source_ || !index.isValid() || index.row() < 0 || index.row() >= pluginIds_.size())
        return false;

    if (role == Qt::CheckStateRole) {
        Qt::CheckState checkState = Qt::CheckState(value.toInt());
        if (data(index, role) == Qt::Unchecked)
            checkState = Qt::PartiallyChecked;
        return setLoadPolicy(
            index.row(),
            checkState == Qt::PartiallyChecked
                ? PluginListSource::LP_Auto
                : (checkState == Qt::Checked ? PluginListSource::LP_Enabled : PluginListSource::LP_Disabled));
    }
    if (role == LoadPolicyRole)
        return setLoadPolicy(index.row(), value.toInt());
    if (role == EnabledRole)
        return setEnabled(index.row(), value.toBool());
    return false;
}

Qt::ItemFlags PluginListModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return QAbstractListModel::flags(index) | Qt::ItemIsDropEnabled;
    return QAbstractListModel::flags(index) | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsUserTristate
        | Qt::ItemIsUserCheckable;
}

QHash<int, QByteArray> PluginListModel::roleNames() const
{
    return {
        { PluginIdRole, "pluginId" },
        { NameRole, "name" },
        { DescriptionRole, "description" },
        { VersionTextRole, "versionText" },
        { LoadPolicyRole, "loadPolicy" },
        { LoadStatusRole, "loadStatus" },
        { LoadedRole, "loaded" },
        { EnabledRole, "enabled" },
        { ConfigurableRole, "configurable" },
        { TooltipRole, "tooltip" },
        { IconRole, "icon" },
        { IconSourceRole, "iconSource" },
    };
}

QStringList PluginListModel::mimeTypes() const { return { QLatin1String(MimeType) }; }

QMimeData *PluginListModel::mimeData(const QModelIndexList &indexes) const
{
    if (indexes.isEmpty())
        return nullptr;
    const auto row = indexes.first().row();
    if (row < 0 || row >= pluginIds_.size())
        return nullptr;
    auto       *mimeData = new QMimeData();
    QByteArray  encodedData;
    QDataStream out(&encodedData, QIODevice::WriteOnly);
    out << pluginIds_.at(row);
    mimeData->setData(QLatin1String(MimeType), encodedData);
    return mimeData;
}

bool PluginListModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                   const QModelIndex &parent)
{
    Q_UNUSED(column)
    if (action == Qt::IgnoreAction)
        return true;
    if (action != Qt::MoveAction || !data || !data->hasFormat(QLatin1String(MimeType)))
        return false;

    QString     pluginId;
    QByteArray  encodedData = data->data(QLatin1String(MimeType));
    QDataStream in(&encodedData, QIODevice::ReadOnly);
    in >> pluginId;
    const auto sourceRow = pluginIds_.indexOf(pluginId);
    if (sourceRow < 0)
        return false;
    int destinationRow = row;
    if (destinationRow < 0)
        destinationRow = parent.isValid() ? parent.row() : pluginIds_.size();
    return moveRows({}, sourceRow, 1, {}, destinationRow);
}

Qt::DropActions PluginListModel::supportedDragActions() const { return Qt::MoveAction; }
Qt::DropActions PluginListModel::supportedDropActions() const { return Qt::MoveAction; }

bool PluginListModel::moveRows(const QModelIndex &sourceParent, int sourceRow, int count,
                               const QModelIndex &destinationParent, int destinationChild)
{
    if (!source_ || sourceParent.isValid() || destinationParent.isValid() || count != 1 || sourceRow < 0
        || sourceRow >= pluginIds_.size() || destinationChild < 0 || destinationChild > pluginIds_.size()
        || destinationChild == sourceRow || destinationChild == sourceRow + 1) {
        return false;
    }

    QStringList reordered = pluginIds_;
    reordered.move(sourceRow, destinationChild > sourceRow ? destinationChild - 1 : destinationChild);
    // The source owns ordering and emits pluginsReset() after persisting it.
    // Do not nest beginMoveRows() around that synchronous model reset.
    return source_->setPluginOrder(reordered);
}

QString PluginListModel::pluginId(int row) const
{
    return row >= 0 && row < pluginIds_.size() ? pluginIds_.at(row) : QString();
}

bool PluginListModel::setLoadPolicy(int row, PluginListSource::LoadPolicy loadPolicy)
{
    if (!source_ || row < 0 || row >= pluginIds_.size())
        return false;
    return source_->setPluginLoadPolicy(pluginIds_.at(row), loadPolicy);
}

bool PluginListModel::setLoadPolicy(int row, int loadPolicy)
{
    if (loadPolicy < PluginListSource::LP_Auto || loadPolicy > PluginListSource::LP_Disabled)
        return false;
    return setLoadPolicy(row, PluginListSource::LoadPolicy(loadPolicy));
}

bool PluginListModel::setEnabled(int row, bool enabled)
{
    return setLoadPolicy(row, enabled ? PluginListSource::LP_Auto : PluginListSource::LP_Disabled);
}

void PluginListModel::resetFromSource()
{
    beginResetModel();
    pluginIds_ = source_ ? source_->pluginIds() : QStringList();
    endResetModel();
}

void PluginListModel::updatePlugin(const QString &pluginId)
{
    const int row = pluginIds_.indexOf(pluginId);
    if (row >= 0)
        emit dataChanged(index(row), index(row));
    else
        resetFromSource();
}

QString PluginListModel::statusText(PluginListSource::LoadStatus status)
{
    switch (status) {
    case PluginListSource::LS_ErrAbi:
        return tr("ABI mismatch");
    case PluginListSource::LS_ErrMetadata:
        return tr("Incompatible metadata");
    case PluginListSource::LS_ErrVersion:
        return tr("Incompatible version");
    case PluginListSource::LS_Loaded:
        return tr("Loaded");
    case PluginListSource::LS_Initialized:
        return tr("Initialized");
    case PluginListSource::LS_ErrNotPlugin:
        return tr("Not a plugin");
    case PluginListSource::LS_Undefined:
    case PluginListSource::LS_Unloaded:
        return tr("Not loaded");
    case PluginListSource::LS_Errors:
        break;
    }
    return tr("Unknown");
}

} // namespace QtNote
