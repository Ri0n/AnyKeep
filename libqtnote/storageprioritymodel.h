#ifndef STORAGEPRIORITYMODEL_H
#define STORAGEPRIORITYMODEL_H

#include "qtnote_export.h"

#include <QAbstractListModel>

namespace QtNote {

class QTNOTE_EXPORT StoragePriorityModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        StorageIdRole = Qt::UserRole + 1,
        NameRole,
        AccessibleRole,
        ConfigurableRole,
        TooltipRole,
        IconRole,
        IconSourceRole,
    };
    Q_ENUM(Role)

    explicit StoragePriorityModel(QObject *parent = nullptr);

    int                    rowCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
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

    QString     storageId(const QModelIndex &index) const;
    QStringList priorityList() const;

private:
    struct Item {
        QString storageId;
        QString name;
    };

    void resetFromManager();
    int  rowForStorage(const QString &storageId) const;

    QList<Item> items_;
};

} // namespace QtNote

#endif // STORAGEPRIORITYMODEL_H
