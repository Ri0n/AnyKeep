#ifndef PLUGINLISTMODEL_H
#define PLUGINLISTMODEL_H

#include "anykeep_export.h"
#include "pluginlistsource.h"

#include <QAbstractListModel>
#include <QPointer>

namespace AnyKeep {

class ANYKEEP_EXPORT PluginListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        PluginIdRole = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        VersionTextRole,
        LoadPolicyRole,
        LoadStatusRole,
        LoadedRole,
        EnabledRole,
        ConfigurableRole,
        TooltipRole,
        IconRole,
        IconSourceRole,
    };
    Q_ENUM(Role)

    explicit PluginListModel(PluginListSource *source, QObject *parent = nullptr);

    int                    rowCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool                   setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    Qt::ItemFlags          flags(const QModelIndex &index) const override;
    QHash<int, QByteArray> roleNames() const override;
    QStringList            mimeTypes() const override;
    QMimeData             *mimeData(const QModelIndexList &indexes) const override;
    bool                   dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                        const QModelIndex &parent) override;
    Qt::DropActions        supportedDragActions() const override;
    Qt::DropActions        supportedDropActions() const override;
    bool moveRows(const QModelIndex &sourceParent, int sourceRow, int count, const QModelIndex &destinationParent,
                  int destinationChild) override;

    QString pluginId(int row) const;
    bool    setLoadPolicy(int row, PluginListSource::LoadPolicy loadPolicy);

    Q_INVOKABLE bool setLoadPolicy(int row, int loadPolicy);
    Q_INVOKABLE bool setEnabled(int row, bool enabled);
    Q_INVOKABLE bool movePlugin(int sourceRow, int destinationRow);

private slots:
    void resetFromSource();
    void updatePlugin(const QString &pluginId);

private:
    static QString statusText(PluginListSource::LoadStatus status);

    QPointer<PluginListSource> source_;
    QStringList                pluginIds_;
};

} // namespace AnyKeep

#endif // PLUGINLISTMODEL_H
