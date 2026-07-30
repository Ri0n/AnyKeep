#ifndef NOTESMODEL_H
#define NOTESMODEL_H

#include "notestorage.h"
#include "qtnote_export.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QStringList>
#include <QVariantMap>

namespace QtNote {

class NMMItem;
class Note;
class FolderCatalogManager;

class QTNOTE_EXPORT NotesModel final : public QAbstractItemModel {
    Q_OBJECT
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY pageSizeChanged)
    Q_PROPERTY(int noteCount READ noteCount NOTIFY statsChanged)

public:
    enum DataRole {
        StorageIdRole = Qt::UserRole + 1,
        NoteIdRole,
        ItemTypeRole,
        TagsRole,
        TitleRole,
        PreviewRole,
        ModifiedTimeRole,
        StorageNameRole,
        AccessibleRole,
        LoadingRole,
        ErrorStringRole,
        HasMoreRole,
        NoteCountRole,
        IconSourceRole,
    };
    Q_ENUM(DataRole)

    enum ItemType { ItemStorage, ItemNote };
    Q_ENUM(ItemType)

    explicit NotesModel(QObject *parent = nullptr);
    NotesModel(FolderCatalogManager *folderCatalogManager, QObject *parent);
    ~NotesModel() override;

    QModelIndex            index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex            parent(const QModelIndex &index) const override;
    int                    rowCount(const QModelIndex &parent = {}) const override;
    int                    columnCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags          flags(const QModelIndex &index) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    Qt::DropActions supportedDropActions() const override;
    QStringList     mimeTypes() const override;
    QMimeData      *mimeData(const QModelIndexList &indexes) const override;
    bool            dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                 const QModelIndex &parent) override;

    int pageSize() const { return pageSize_; }
    int noteCount() const;

    Q_INVOKABLE void        refresh();
    Q_INVOKABLE void        refreshStorage(const QString &storageId);
    Q_INVOKABLE QVariantMap itemData(int row, const QModelIndex &parent = {}) const;
    void                    setSearchActive(bool active);

public slots:
    void setPageSize(int pageSize);

signals:
    void statsChanged();
    void pageSizeChanged();
    void notesDropRequested(const QStringList &sourceStorageIds, const QStringList &noteIds,
                            const QString &destinationStorageId);

private slots:
    void storageAdded(const NoteStorage::Ptr &storage);
    void storageAboutToBeRemoved(const NoteStorage::Ptr &storage);
    void storageChanged(const NoteStorage::Ptr &storage);
    void storageReady(const NoteStorage::Ptr &storage);
    void storageOrderChanged();
    void storageIndexChanged(const QString &storageId);
    void storageIndexStateChanged(const QString &storageId);

private:
    void        replaceVisibleNotes(NMMItem *storageItem, int desiredCount = -1);
    QModelIndex storageIndex(const QString &storageId) const;
    QModelIndex noteIndex(const QString &storageId, const QString &noteId) const;
    NMMItem    *storageItem(const QString &storageId) const;
    QList<Note> indexedNotes(const QString &storageId) const;

    QList<NMMItem *>      storages_;
    int                   pageSize_ { 30 };
    bool                  searchActive_ { false };
    FolderCatalogManager *folderCatalogManager_ { nullptr };
};

} // namespace QtNote

#endif // NOTESMODEL_H
