/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef FOLDERNOTESMODEL_H
#define FOLDERNOTESMODEL_H

#include "note.h"
#include "qtnote_export.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QUuid>
#include <QVariantList>

namespace QtNote {

class FolderCatalogManager;
class NotesSearchModel;
/**
 * Flat visible projection of the global folder tree and note summaries.
 *
 * The model deliberately owns no folder state.  It combines FolderCatalog's
 * persisted hierarchy/overlay assignments with NotesIndex snapshots so QML
 * can render one nested list without querying individual storages.  The
 * virtual Unsorted row is always present and represents a null folder ID.
 */
class QTNOTE_EXPORT FolderNotesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool catalogAvailable READ catalogAvailable NOTIFY catalogAvailableChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum RowKind {
        FolderRow,
        NoteRow,
        UnsortedRow,
    };
    Q_ENUM(RowKind)

    enum Role {
        RowKindRole = Qt::UserRole + 1,
        FolderIdRole,
        ParentFolderIdRole,
        StorageIdRole,
        NoteIdRole,
        TitleRole,
        DepthRole,
        CollapsedRole,
        FavoriteRole,
        ArchivedRole,
        ChildFolderCountRole,
        NoteCountRole,
        ItemTypeRole,
        GroupKindRole,
        GroupIdRole,
        PreviewRole,
        StorageNameRole,
        AccessibleRole,
        LoadingRole,
        ErrorStringRole,
        HasMoreRole,
        IconSourceRole,
        SystemFolderRole,
    };
    Q_ENUM(Role)

    explicit FolderNotesModel(FolderCatalogManager *catalogManager = nullptr, QObject *parent = nullptr);

    int                    rowCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool catalogAvailable() const;
    int  count() const { return rows_.size(); }

    Q_INVOKABLE QVariantMap itemAt(int row) const;
    Q_INVOKABLE int         rowForFolder(const QString &folderId) const;
    /** A full folder tree for pickers; it intentionally ignores collapsed branches. */
    Q_INVOKABLE QVariantList folderPickerItems(bool includeArchived = false) const;

    /**
     * The Unsorted group is virtual, so its expanded state belongs to this
     * projection rather than to FolderCatalog.  Keeping it here also avoids
     * inventing a persistent pseudo-folder in the shared catalog.
     */
    bool setUnsortedCollapsed(bool collapsed);
    void setSearchModel(NotesSearchModel *model);

signals:
    void catalogAvailableChanged();
    void countChanged();

private:
    struct Row {
        RowKind kind { FolderRow };
        QUuid   folderId;
        QUuid   parentFolderId;
        QString storageId;
        QString noteId;
        QString title;
        QString preview;
        QString storageName;
        int     depth { 0 };
        bool    accessible { true };
        bool    collapsed { false };
        bool    favorite { false };
        bool    archived { false };
        bool    systemFolder { false };
        int     childFolderCount { 0 };
        int     noteCount { 0 };
    };

    FolderCatalogManager *catalogManager_ { nullptr };
    NotesSearchModel     *searchModel_ { nullptr };
    QList<Row>            rows_;
    bool                  catalogAvailable_ { false };
    bool                  unsortedCollapsed_ { false };

    void           rebuild();
    QList<Row>     buildRows() const;
    void           replaceRows(QList<Row> nextRows);
    static QString rowKey(const Row &row);
    static bool    sameRow(const Row &left, const Row &right);
    bool           matchesSearch(const Note &note) const;
    QUuid          effectiveFolderId(const Note &note) const;
    void           appendFolder(QList<Row> *rows, const QUuid &folderId, int depth,
                                const QHash<QUuid, QList<Note>> &notesByFolder) const;
    void           appendNotes(QList<Row> *rows, const QList<Note> &notes, const QUuid &folderId, int depth) const;
    void appendFolderPickerItems(const QUuid &folderId, int depth, bool includeArchived, QVariantList *items) const;
};

} // namespace QtNote

#endif // FOLDERNOTESMODEL_H
