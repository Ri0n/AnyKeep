#ifndef RECENTNOTESMODEL_H
#define RECENTNOTESMODEL_H

#include "qtnote_export.h"

#include <QAbstractListModel>
#include <QPersistentModelIndex>
#include <QPointer>

namespace QtNote {

class QTNOTE_EXPORT RecentNotesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *sourceModel READ sourceModel WRITE setSourceModel NOTIFY sourceModelChanged)
    Q_PROPERTY(int maximumCount READ maximumCount WRITE setMaximumCount NOTIFY maximumCountChanged)
    Q_PROPERTY(int noteCount READ rowCount NOTIFY noteCountChanged)

public:
    explicit RecentNotesModel(QObject *parent = nullptr);
    explicit RecentNotesModel(QAbstractItemModel *sourceModel, QObject *parent = nullptr);

    int                    rowCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QAbstractItemModel *sourceModel() const { return sourceModel_; }
    int                 maximumCount() const { return maximumCount_; }

public slots:
    void setSourceModel(QAbstractItemModel *sourceModel);
    void setMaximumCount(int maximumCount);
    void rebuild();

signals:
    void sourceModelChanged();
    void maximumCountChanged();
    void noteCountChanged();

private:
    void disconnectSource();
    void connectSource();

    QPointer<QAbstractItemModel>   sourceModel_;
    QList<QPersistentModelIndex>   entries_;
    QList<QMetaObject::Connection> sourceConnections_;
    int                            maximumCount_ { 30 };
};

} // namespace QtNote

#endif // RECENTNOTESMODEL_H
