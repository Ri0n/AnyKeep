#pragma once

#include <QAbstractListModel>

#include "pluginlistmodel.h"
#include "settingscontroller.h"

namespace QtNote::TestSupport {

class SettingsReorderTestModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        StorageIdRole = Qt::UserRole + 1,
        PluginIdRole,
        NameRole,
        VersionTextRole,
        LoadStatusRole,
        AccessibleRole,
        ConfigurableRole,
        TooltipRole,
        IconSourceRole,
        LoadPolicyRole,
    };

    explicit SettingsReorderTestModel(QObject *parent = nullptr) : QAbstractListModel(parent)
    {
        ids_ = { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };
    }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : ids_.size(); }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= ids_.size())
            return {};
        const QString id = ids_.at(index.row());
        switch (role) {
        case StorageIdRole:
        case PluginIdRole:
            return id;
        case NameRole:
            return id.toUpper();
        case VersionTextRole:
            return QStringLiteral("1.0");
        case LoadStatusRole:
            return 2;
        case AccessibleRole:
            return true;
        case ConfigurableRole:
            return id != QStringLiteral("b");
        case TooltipRole:
        case IconSourceRole:
            return QString();
        case LoadPolicyRole:
            return 0;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {
            { StorageIdRole, "storageId" },       { PluginIdRole, "pluginId" },     { NameRole, "name" },
            { VersionTextRole, "versionText" },   { LoadStatusRole, "loadStatus" }, { AccessibleRole, "accessible" },
            { ConfigurableRole, "configurable" }, { TooltipRole, "tooltip" },       { IconSourceRole, "iconSource" },
            { LoadPolicyRole, "loadPolicy" },
        };
    }

    Q_INVOKABLE bool reorderStorage(int sourceRow, int destinationRow)
    {
        ++storageMoves;
        return moveTo(sourceRow, destinationRow);
    }

    Q_INVOKABLE bool movePlugin(int sourceRow, int destinationRow)
    {
        ++pluginMoves;
        return moveTo(sourceRow, destinationRow);
    }

    Q_INVOKABLE bool setLoadPolicy(int, int) { return true; }

    QStringList ids() const { return ids_; }

    int storageMoves { 0 };
    int pluginMoves { 0 };

private:
    bool moveTo(int sourceRow, int destinationRow)
    {
        if (sourceRow < 0 || sourceRow >= ids_.size() || destinationRow < 0 || destinationRow >= ids_.size()
            || sourceRow == destinationRow) {
            return false;
        }
        const int destinationChild = destinationRow > sourceRow ? destinationRow + 1 : destinationRow;
        beginMoveRows({}, sourceRow, sourceRow, {}, destinationChild);
        ids_.move(sourceRow, destinationRow);
        endMoveRows();
        return true;
    }

    QStringList ids_;
};

class FolderPageTestModel final : public QAbstractListModel {
    Q_OBJECT

public:
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
    };

    struct Row {
        int     rowKind { 0 };
        QString folderId;
        QString parentFolderId;
        QString storageId;
        QString noteId;
        QString title;
        int     depth { 0 };
        bool    collapsed { false };
        bool    favorite { false };
        bool    archived { false };
        int     childFolderCount { 0 };
        int     noteCount { 0 };
    };

    FolderPageTestModel()
    {
        rows_ = {
            { 0, QStringLiteral("inbox"), {}, {}, {}, QStringLiteral("Inbox"), 0, false, false, false, 0, 2 },
            { 1,
              QStringLiteral("inbox"),
              {},
              QStringLiteral("storage"),
              QStringLiteral("note-a"),
              QStringLiteral("Note A"),
              1 },
            { 1,
              QStringLiteral("inbox"),
              {},
              QStringLiteral("storage"),
              QStringLiteral("note-c"),
              QStringLiteral("Note C"),
              1 },
            { 0, QStringLiteral("archive"), {}, {}, {}, QStringLiteral("Archive"), 0, false, false, false, 0, 0 },
            { 2, {}, {}, {}, {}, QStringLiteral("Unsorted"), 0, false, false, false, 0, 1 },
            { 1, {}, {}, QStringLiteral("storage"), QStringLiteral("note-b"), QStringLiteral("Note B"), 1 },
        };
    }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : rows_.size(); }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size())
            return {};
        const auto &row = rows_.at(index.row());
        switch (role) {
        case RowKindRole:
            return row.rowKind;
        case FolderIdRole:
            return row.folderId;
        case ParentFolderIdRole:
            return row.parentFolderId;
        case StorageIdRole:
            return row.storageId;
        case NoteIdRole:
            return row.noteId;
        case TitleRole:
            return row.title;
        case DepthRole:
            return row.depth;
        case CollapsedRole:
            return row.collapsed;
        case FavoriteRole:
            return row.favorite;
        case ArchivedRole:
            return row.archived;
        case ChildFolderCountRole:
            return row.childFolderCount;
        case NoteCountRole:
            return row.noteCount;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {
            { RowKindRole, "rowKind" },
            { FolderIdRole, "folderId" },
            { ParentFolderIdRole, "parentFolderId" },
            { StorageIdRole, "storageId" },
            { NoteIdRole, "noteId" },
            { TitleRole, "title" },
            { DepthRole, "depth" },
            { CollapsedRole, "collapsed" },
            { FavoriteRole, "favorite" },
            { ArchivedRole, "archived" },
            { ChildFolderCountRole, "childFolderCount" },
            { NoteCountRole, "noteCount" },
        };
    }

    Q_INVOKABLE QVariantMap itemAt(int index) const
    {
        if (index < 0 || index >= rows_.size())
            return {};
        const auto &row = rows_.at(index);
        return {
            { QStringLiteral("rowKind"), row.rowKind },
            { QStringLiteral("folderId"), row.folderId },
            { QStringLiteral("parentFolderId"), row.parentFolderId },
            { QStringLiteral("storageId"), row.storageId },
            { QStringLiteral("noteId"), row.noteId },
            { QStringLiteral("title"), row.title },
            { QStringLiteral("depth"), row.depth },
            { QStringLiteral("collapsed"), row.collapsed },
            { QStringLiteral("favorite"), row.favorite },
            { QStringLiteral("archived"), row.archived },
            { QStringLiteral("childFolderCount"), row.childFolderCount },
            { QStringLiteral("noteCount"), row.noteCount },
        };
    }

    Q_INVOKABLE int rowForFolder(const QString &folderId) const
    {
        for (int index = 0; index < rows_.size(); ++index) {
            const auto &row = rows_.at(index);
            if (row.rowKind == 0 && row.folderId == folderId)
                return index;
        }
        return -1;
    }

    Q_INVOKABLE QVariantList folderPickerItems(bool = false) const
    {
        QVariantList items;
        for (const auto &row : rows_) {
            if (row.rowKind != 0)
                continue;
            const QVariantMap item {
                { QStringLiteral("folderId"), row.folderId }, { QStringLiteral("parentFolderId"), row.parentFolderId },
                { QStringLiteral("title"), row.title },       { QStringLiteral("depth"), row.depth },
                { QStringLiteral("favorite"), row.favorite }, { QStringLiteral("archived"), row.archived },
            };
            items.append(item);
        }
        return items;
    }

    Q_INVOKABLE bool assignNoteFolder(const QString &storageId, const QString &noteId, const QString &folderId)
    {
        for (auto &row : rows_) {
            if (row.rowKind != 1 || row.storageId != storageId || row.noteId != noteId)
                continue;
            beginResetModel();
            row.folderId = folderId;
            endResetModel();
            return true;
        }
        return false;
    }

private:
    QList<Row> rows_;
};

class SettingsPluginSource final : public PluginListSource {
public:
    using PluginListSource::PluginListSource;

    QStringList pluginIds() const override { return ids; }

    Entry pluginEntry(const QString &pluginId) const override
    {
        Entry entry;
        entry.id           = pluginId;
        entry.name         = pluginId.toUpper();
        entry.versionText  = QStringLiteral("1.0");
        entry.loadPolicy   = LP_Auto;
        entry.loadStatus   = LS_Initialized;
        entry.loaded       = true;
        entry.configurable = true;
        return entry;
    }

    bool setPluginLoadPolicy(const QString &, LoadPolicy) override { return true; }

    bool setPluginOrder(const QStringList &pluginIds) override
    {
        ids = pluginIds;
        ++orderChanges;
        emit pluginsReset();
        return true;
    }

    QUrl                settingsComponent(const QString &) const override { return {}; }
    SettingsController *createSettingsController(const QString &, QObject *) override { return nullptr; }

    QStringList ids { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };
    int         orderChanges { 0 };
};

class SettingsFormTestController final : public SettingsController {
public:
    SettingsFormTestController()
    {
        addField(
            { QStringLiteral("apiKey"), QStringLiteral("API key"), QString(), Password, QStringLiteral("secret") });
        addField({ QStringLiteral("model"), QStringLiteral("Model"), QString(), Text, QStringLiteral("gemini-test") });
        addField(
            { QStringLiteral("prompt"), QStringLiteral("Prompt"), QString(), Multiline, QStringLiteral("Transcribe") });
        addField({ QStringLiteral("usage"), QStringLiteral("Usage"), QString(), ReadOnly,
                   QStringLiteral("<b>Used:</b> 0 seconds") });
    }

protected:
    bool applyValues(const QVariantMap &, QString *) override { return true; }
};

} // namespace QtNote::TestSupport
