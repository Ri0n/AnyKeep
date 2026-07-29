#include <QElapsedTimer>
#include <QIcon>
#include <QJsonDocument>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest>

#include "desktopeditorplatformbackend.h"
#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "draftstore.h"
#include "noteblockmodel.h"
#include "notedata.h"
#include "noteeditor.h"
#include "notesmanagerwindow.h"
#include "pluginlistmodel.h"
#include "settingscontroller.h"
#include "themediconimageprovider.h"

using namespace QtNote;

namespace {
class MemoryDraftStore final : public DraftStore {
public:
    DraftStoreError write(const DraftRecord &record) override
    {
        records_.insert(record.id, record);
        return {};
    }
    DraftStoreResult<DraftRecord> load(const QUuid &id) const override
    {
        const auto it = records_.constFind(id);
        if (it == records_.cend())
            return { {}, { DraftStoreError::NotFound, QStringLiteral("not found") } };
        return { it.value(), {} };
    }
    DraftStoreResult<QList<DraftRecord>> records() const override { return { records_.values(), {} }; }
    DraftStoreError                      transition(const QUuid &id, DraftRecord::State state) override
    {
        auto it = records_.find(id);
        if (it == records_.end())
            return { DraftStoreError::NotFound, QStringLiteral("not found") };
        it->state = state;
        return {};
    }
    DraftStoreError remove(const QUuid &id) override
    {
        return records_.remove(id) ? DraftStoreError {}
                                   : DraftStoreError { DraftStoreError::NotFound, QStringLiteral("not found") };
    }

private:
    QHash<QUuid, DraftRecord> records_;
};

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

Note plainNote()
{
    Note note(new NoteData(nullptr));
    note.setTitle(QStringLiteral("Title"));
    note.setText(QStringLiteral("Body"), Note::PlainText);
    return note;
}

QQuickItem *quickItemByName(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem *child : root->childItems())
        if (auto *match = quickItemByName(child, name))
            return match;
    return nullptr;
}
}

class DesktopNoteEditorHostTest : public QObject {
    Q_OBJECT

private slots:
    void themedIconsStayUntintedAndFallbackRecoloringIsExplicit()
    {
        QQmlEngine engine;
        installThemedIconImageProvider(&engine);
        auto *provider = dynamic_cast<QQuickImageProvider *>(engine.imageProvider(QStringLiteral("qtnoteicons")));
        QVERIFY(provider);

        const auto request = [provider](const QString &id) {
            QSize  actualSize;
            QImage image = provider->requestImage(id, &actualSize, QSize(20, 20));
            return image;
        };

        const QIcon themed = QIcon::fromTheme(QStringLiteral("preferences-system-symbolic"));
        if (!themed.isNull()) {
            const QImage expected = themed.pixmap(20, 20).toImage();
            const QImage actual
                = request(QStringLiteral("preferences-system-symbolic/preferences-system-symbolic.svg/%23ff0000"));
            QCOMPARE(actual, expected);
        }

        const QString missingTheme = QStringLiteral("__missing_theme_icon__/");
        const QImage  original     = request(missingTheme + QStringLiteral("preferences-system-symbolic.svg/original"));
        const QImage  recolored = request(missingTheme + QStringLiteral("preferences-system-symbolic.svg/%23ff0000"));
        QVERIFY(!original.isNull());
        QVERIFY(!recolored.isNull());
        QCOMPARE(original.size(), recolored.size());
        QVERIFY(original != recolored);

        bool foundRedPixel = false;
        for (int y = 0; y < recolored.height() && !foundRedPixel; ++y) {
            for (int x = 0; x < recolored.width(); ++x) {
                const auto pixel = recolored.pixel(x, y);
                if (qAlpha(pixel) > 0 && qRed(pixel) == 255 && qGreen(pixel) == 0 && qBlue(pixel) == 0) {
                    foundRedPixel = true;
                    break;
                }
            }
        }
        QVERIFY(foundRedPixel);
    }

    void genericSettingsFormCreatesBoundEditors()
    {
        SettingsFormTestController controller;
        QQmlEngine                 engine;
        QQmlComponent              component(&engine, QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")));
        QCOMPARE(component.status(), QQmlComponent::Ready);

        QVariantMap properties;
        properties.insert(QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject *>(&controller)));
        std::unique_ptr<QObject> form(component.createWithInitialProperties(properties));
        QVERIFY2(form, qPrintable(component.errorString()));
        auto *formItem = qobject_cast<QQuickItem *>(form.get());
        QVERIFY(formItem);
        for (int row = 0; row < controller.rowCount(); ++row) {
            const auto objectName = QStringLiteral("settingsFieldEditor-%1").arg(row);
            QTRY_VERIFY2(quickItemByName(formItem, objectName), qPrintable(objectName));
        }
        auto *usage = quickItemByName(formItem, QStringLiteral("settingsFieldEditor-3"));
        QVERIFY(usage);
        QVERIFY(usage->property("textFormat").toInt() != 0);
    }

    void settingsListsUseAnimatedReordering()
    {
        const auto exercise = [](bool pluginMode) {
            SettingsReorderTestModel model;
            QQuickWidget             quick;
            quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
            quick.resize(360, 180);
            installThemedIconImageProvider(quick.engine());
            quick.rootContext()->setContextProperty(QStringLiteral("settingsReorderModel"), &model);
            quick.rootContext()->setContextProperty(QStringLiteral("settingsPluginMode"), pluginMode);
            quick.setSource(QUrl(QStringLiteral("qrc:/qml/AnimatedSettingsList.qml")));
            QCOMPARE(quick.status(), QQuickWidget::Ready);
            quick.show();
            QTest::qWait(30);

            auto *root = qobject_cast<QQuickItem *>(quick.rootObject());
            QVERIFY(root);
            QCOMPARE(root->property("backgroundColor").value<QColor>(),
                     QGuiApplication::palette().color(QPalette::Base));
            auto *first  = quickItemByName(root, QStringLiteral("settingsRow-a"));
            auto *second = quickItemByName(root, QStringLiteral("settingsRow-b"));
            auto *last   = quickItemByName(root, QStringLiteral("settingsRow-c"));
            QTRY_VERIFY(first);
            QTRY_VERIFY(second);
            QTRY_VERIFY(last);
            if (pluginMode) {
                auto *firstCheck  = quickItemByName(root, QStringLiteral("settingsPolicyCheck-a"));
                auto *secondCheck = quickItemByName(root, QStringLiteral("settingsPolicyCheck-b"));
                auto *configure   = quickItemByName(root, QStringLiteral("settingsConfigureButton-a"));
                QTRY_VERIFY(firstCheck);
                QTRY_VERIFY(secondCheck);
                QTRY_VERIFY(configure);
                const qreal firstX  = firstCheck->mapToItem(root, QPointF()).x();
                const qreal secondX = secondCheck->mapToItem(root, QPointF()).x();
                QVERIFY(qAbs(firstX - secondX) < 0.5);
                QVERIFY(configure->mapToItem(root, QPointF()).x() < firstX);
            }

            const QPointF from = first->mapToItem(root, QPointF(13, first->height() / 2));
            const QPointF to   = last->mapToItem(root, QPointF(13, last->height() / 2));
            QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            for (int step = 1; step <= 8; ++step)
                QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
            QTRY_VERIFY(root->property("dragging").toBool());
            QTRY_COMPARE(root->property("previewCount").toInt(), 1);
            QTRY_VERIFY(second->property("reorderOffset").toReal() < -1);

            QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
            QTRY_VERIFY(!root->property("dragging").toBool());
            QCOMPARE(model.ids(), QStringList({ QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("a") }));
            QCOMPARE(model.storageMoves, pluginMode ? 0 : 1);
            QCOMPARE(model.pluginMoves, pluginMode ? 1 : 0);
        };

        exercise(false);
        exercise(true);

        // PluginListModel persists through its source and synchronously resets
        // itself during the drop. Keep that real lifecycle covered as well.
        SettingsPluginSource source;
        PluginListModel      model(&source);
        QQuickWidget         quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(360, 180);
        installThemedIconImageProvider(quick.engine());
        quick.rootContext()->setContextProperty(QStringLiteral("settingsReorderModel"), &model);
        quick.rootContext()->setContextProperty(QStringLiteral("settingsPluginMode"), true);
        quick.setSource(QUrl(QStringLiteral("qrc:/qml/AnimatedSettingsList.qml")));
        QCOMPARE(quick.status(), QQuickWidget::Ready);
        quick.show();
        QTest::qWait(30);

        auto *root  = qobject_cast<QQuickItem *>(quick.rootObject());
        auto *first = quickItemByName(root, QStringLiteral("settingsRow-a"));
        auto *last  = quickItemByName(root, QStringLiteral("settingsRow-c"));
        QTRY_VERIFY(first);
        QTRY_VERIFY(last);
        const QPointF from = first->mapToItem(root, QPointF(13, first->height() / 2));
        const QPointF to   = last->mapToItem(root, QPointF(13, last->height() / 2));
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
        QTRY_VERIFY(root->property("dragging").toBool());
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
        QTRY_VERIFY(!root->property("dragging").toBool());
        QCOMPARE(source.ids, QStringList({ QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("a") }));
        QCOMPARE(source.orderChanges, 1);
    }

    void loadsSharedQmlShell()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        auto *quick = host.quickWidget();
        QVERIFY(quick);
        QCOMPARE(quick->status(), QQuickWidget::Ready);
        QVERIFY(quick->rootObject());
        QVariant viewState;
        QVERIFY(
            QMetaObject::invokeMethod(quick->rootObject(), "captureEditorState", Q_RETURN_ARG(QVariant, viewState)));
        QVERIFY(viewState.canConvert<QVariantMap>());
        QCOMPARE(host.model(), editor.model());
    }

    void loadsNotesManagerQmlShell()
    {
        DraftManager       drafts(std::make_unique<MemoryDraftStore>());
        NotesManagerWindow manager;
        QVERIFY(manager.isReady());
    }

    void notesManagerContextMenusAndInternalDragsWork()
    {
        QQuickWidget quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(620, 480);
        QStandardItemModel           notesModel;
        const QHash<int, QByteArray> roles {
            { Qt::UserRole + 1, "storageId" },    { Qt::UserRole + 2, "noteId" },
            { Qt::UserRole + 3, "itemType" },     { Qt::UserRole + 4, "title" },
            { Qt::UserRole + 5, "preview" },      { Qt::UserRole + 6, "loading" },
            { Qt::UserRole + 7, "errorString" },  { Qt::UserRole + 8, "hasMore" },
            { Qt::UserRole + 9, "noteCount" },    { Qt::UserRole + 10, "iconSource" },
            { Qt::UserRole + 11, "tags" },        { Qt::UserRole + 12, "modifiedTime" },
            { Qt::UserRole + 13, "storageName" }, { Qt::UserRole + 14, "accessible" },
        };
        notesModel.setItemRoleNames(roles);
        const auto appendItem = [&notesModel](const QString &storageId, const QString &noteId, int itemType,
                                              const QString &title) {
            auto *item = new QStandardItem;
            item->setData(storageId, Qt::UserRole + 1);
            item->setData(noteId, Qt::UserRole + 2);
            item->setData(itemType, Qt::UserRole + 3);
            item->setData(title, Qt::UserRole + 4);
            item->setData(QString(), Qt::UserRole + 5);
            item->setData(false, Qt::UserRole + 6);
            item->setData(QString(), Qt::UserRole + 7);
            item->setData(false, Qt::UserRole + 8);
            item->setData(itemType == 0 && storageId == QStringLiteral("storage-a") ? 1 : 0, Qt::UserRole + 9);
            item->setData(QString(), Qt::UserRole + 10);
            item->setData(QStringList(), Qt::UserRole + 11);
            item->setData(QDateTime(), Qt::UserRole + 12);
            item->setData(itemType == 0 ? title
                                        : (storageId == QStringLiteral("storage-a") ? QStringLiteral("Storage A")
                                                                                    : QStringLiteral("Storage B")),
                          Qt::UserRole + 13);
            item->setData(true, Qt::UserRole + 14);
            notesModel.appendRow(item);
        };
        appendItem(QStringLiteral("storage-a"), {}, 0, QStringLiteral("Storage A"));
        appendItem(QStringLiteral("storage-a"), QStringLiteral("note-a"), 1, QStringLiteral("Note A"));
        appendItem(QStringLiteral("storage-a"), QStringLiteral("note-a2"), 1, QStringLiteral("Note A2"));
        appendItem(QStringLiteral("storage-b"), {}, 0, QStringLiteral("Storage B"));
        appendItem(QStringLiteral("storage-b"), QStringLiteral("note-b"), 1, QStringLiteral("Note B"));
        installThemedIconImageProvider(quick.engine());
        quick.rootContext()->setContextProperty(QStringLiteral("testNotesModel"), &notesModel);
        QQmlComponent component(quick.engine());
        component.setData(R"QML(
            import QtQuick
            import QtQuick.Controls

            Item {
                id: harness
                objectName: "managerInteractionHarness"
                property int movedNotes: workspace.movedNotes
                property string noteDestination: workspace.noteDestination
                property string noteAnchor: workspace.noteAnchor
                property bool noteInsertAfter: workspace.noteInsertAfter
                property int movedStorages: workspace.movedStorages
                property string storageDestination: workspace.storageDestination
                property string lastDraggedNoteId: ""

                QtObject {
                    id: workspace
                    property var groupedNotesModel: testNotesModel
                    property var recentNotesModel: testNotesModel
                    property var folderNotesModel: null
                    property bool folderCatalogAvailable: false
                    property var currentEditor: null
                    property string currentStorageId: ""
                    property string currentNoteId: ""
                    property string currentTitle: ""
                    property string errorString: ""
                    property string searchText: ""
                    property bool searchInBody: false
                    property bool loading: false
                    property bool busy: false
                    property int noteCount: 1
                    property var storages: []
                    property int movedNotes: 0
                    property string noteDestination: ""
                    property string noteAnchor: ""
                    property bool noteInsertAfter: false
                    property int movedStorages: 0
                    property string storageDestination: ""
                    function saveCurrentNote() { return true }
                    function closeCurrentNote() { return true }
                    function reloadCurrentNote() { return true }
                    function openNote(storageId, noteId) { return true }
                    function createNote(storageId) { return true }
                    function folderIdForNote(storageId, noteId) { return "" }
                    function assignNoteFolder(storageId, noteId, folderId) { return true }
                    function openStandalone(storageId, noteId) { return true }
                    function deleteNote(storageId, noteId) { return true }
                    function copyNote(sourceStorageId, noteId, destinationStorageId) { return true }
                    function moveNote(sourceStorageId, noteId, destinationStorageId) { return true }
                    function openStorageSettings(storageId) {}
                    function moveNotes(notes, destinationStorageId, anchorNoteId, insertAfter) {
                        movedNotes = notes.length
                        noteDestination = destinationStorageId
                        noteAnchor = anchorNoteId
                        noteInsertAfter = insertAfter
                        return true
                    }
                    function moveStorage(sourceStorageId, destinationStorageId) {
                        ++movedStorages
                        storageDestination = destinationStorageId
                        return true
                    }
                }

                NotesManagerPage {
                    id: page
                    objectName: "managerPage"
                    anchors.fill: parent
                    workspace: workspace
                    embeddedEditor: false
                    showCreateButton: false
                    showViewModeSelector: false
                    viewMode: groupedByStorageMode
                }
            }
        )QML",
                          QUrl(QStringLiteral("qrc:/qml/ManagerInteractionHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/ManagerInteractionHarness.qml")), &component, root);
        quick.show();
        QTest::qWait(50);

        const auto delegate = [](QQuickItem *page, int row) -> QQuickItem * {
            QVariant value;
            if (!QMetaObject::invokeMethod(page, "groupedItemAtRow", Q_RETURN_ARG(QVariant, value),
                                           Q_ARG(QVariant, row))) {
                return nullptr;
            }
            return qobject_cast<QQuickItem *>(value.value<QObject *>());
        };
        QQuickItem *storageA = nullptr;
        QQuickItem *noteA    = nullptr;
        QQuickItem *noteA2   = nullptr;
        QQuickItem *storageB = nullptr;
        QQuickItem *noteB    = nullptr;
        auto       *page     = root->findChild<QQuickItem *>(QStringLiteral("managerPage"));
        auto       *tree     = root->findChild<QQuickItem *>(QStringLiteral("notesTree"));
        auto       *preview  = root->findChild<QQuickItem *>(QStringLiteral("managerDragPreview"));
        QVERIFY(page);
        QVERIFY(tree);
        QVERIFY(preview);
        QTRY_VERIFY((storageA = delegate(page, 0)));
        QTRY_VERIFY((noteA = delegate(page, 1)));
        QTRY_VERIFY((noteA2 = delegate(page, 2)));
        QTRY_VERIFY((storageB = delegate(page, 3)));
        QTRY_VERIFY((noteB = delegate(page, 4)));
        QVERIFY(!noteA->property("selectionCheckBoxVisible").toBool());

        const QPointF storageAPoint = storageA->mapToItem(qobject_cast<QQuickItem *>(root),
                                                          QPointF(storageA->width() / 2, storageA->height() / 2));
        QTest::mouseClick(&quick, Qt::RightButton, Qt::NoModifier, storageAPoint.toPoint());
        QTRY_COMPARE(page->property("selectedStorageId").toString(), QStringLiteral("storage-a"));
        QTRY_VERIFY(root->findChild<QObject *>(QStringLiteral("storageContextMenu"))->property("visible").toBool());
        auto *storageContextMenu = root->findChild<QObject *>(QStringLiteral("storageContextMenu"));
        QVERIFY(QMetaObject::invokeMethod(storageContextMenu, "close"));
        QTRY_VERIFY(!storageContextMenu->property("visible").toBool());

        const QPointF noteAPoint
            = noteA->mapToItem(qobject_cast<QQuickItem *>(root), QPointF(noteA->width() / 2, noteA->height() / 2));
        QTest::mouseClick(&quick, Qt::RightButton, Qt::NoModifier, noteAPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNoteId").toString(), QStringLiteral("note-a"));
        auto *noteContextMenu = root->findChild<QObject *>(QStringLiteral("noteContextMenu"));
        QTRY_VERIFY(noteContextMenu->property("visible").toBool());
        QCOMPARE(noteContextMenu->property("modal").toBool(), true);
        QVERIFY(QMetaObject::invokeMethod(noteContextMenu, "close"));
        QTRY_VERIFY(!noteContextMenu->property("visible").toBool());

        QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 1);
        QVERIFY(!noteContextMenu->property("visible").toBool());
        const QPointF noteBPoint
            = noteB->mapToItem(qobject_cast<QQuickItem *>(root), QPointF(noteB->width() / 2, noteB->height() / 2));
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::ShiftModifier, noteBPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 3);
        QVERIFY(page->property("selectedNotes").toMap().contains(QStringLiteral("storage-a\nnote-a")));
        QVERIFY(page->property("selectedNotes").toMap().contains(QStringLiteral("storage-a\nnote-a2")));
        QVERIFY(page->property("selectedNotes").toMap().contains(QStringLiteral("storage-b\nnote-b")));

        QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 1);
        const QPointF noteA2Point
            = noteA2->mapToItem(qobject_cast<QQuickItem *>(root), QPointF(noteA2->width() / 2, noteA2->height() / 2));
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::ControlModifier, noteA2Point.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::ShiftModifier, noteBPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);
        QVERIFY(page->property("selectedNotes").toMap().contains(QStringLiteral("storage-a\nnote-a2")));
        QVERIFY(page->property("selectedNotes").toMap().contains(QStringLiteral("storage-b\nnote-b")));

        const auto delegateForNote = [&delegate, page, tree](const QString &noteId) -> QQuickItem * {
            for (int row = 0; row < tree->property("rows").toInt(); ++row) {
                if (auto *item = delegate(page, row);
                    item && item->property("noteId").toString() == noteId && item->property("itemType").toInt() == 1) {
                    return item;
                }
            }
            return nullptr;
        };
        const auto waitFor = [](const std::function<bool()> &condition, int timeout = 1500) {
            QElapsedTimer timer;
            timer.start();
            while (!condition() && timer.elapsed() < timeout)
                QTest::qWait(10);
            return condition();
        };
        const auto drag = [&quick, root, preview, page, tree, &delegate,
                           &waitFor](QQuickItem *source, QQuickItem *destination, int previewItems) {
            if (!source || !destination)
                return false;
            // A QStandardItemModel move can replace a TreeView delegate one
            // event-loop turn after the previous drop animation has finished.
            // Begin the next gesture only once both reused delegates have
            // their normal geometry again; otherwise the synthetic press can
            // land in a transient zero-height row.
            if (!waitFor(
                    [&]() {
                        return preview->property("previewCount").toInt() == 0
                            && !page->property("dragSelectionSuppressed").toBool()
                            && qAbs(source->property("reorderOffset").toReal()) < 0.5
                            && qAbs(destination->property("reorderOffset").toReal()) < 0.5 && source->width() > 0
                            && source->height() > 0 && destination->width() > 0 && destination->height() > 0;
                    },
                    2500)) {
                return false;
            }
            auto         *rootItem = qobject_cast<QQuickItem *>(root);
            const QPointF from     = source->mapToItem(rootItem, QPointF(source->width() / 2, source->height() / 2));
            QPointF to = destination->mapToItem(rootItem, QPointF(destination->width() / 2, destination->height() / 2));
            to.ry() += from.y() < to.y() ? destination->height() / 2 - 2 : -destination->height() / 2 + 2;
            QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            for (int step = 1; step <= 8; ++step)
                QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
            if (!waitFor([&]() { return preview->property("previewCount").toInt() == previewItems; }))
                return false;
            const auto animatedSource = [&]() {
                for (int row = 0; row < tree->property("rows").toInt(); ++row) {
                    if (auto *item = delegate(page, row); item && item->property("partOfActiveDrag").toBool()
                        && item->property("collapseSpace").toReal() > 0) {
                        return true;
                    }
                }
                return false;
            };
            if (!waitFor(animatedSource))
                return false;
            root->setProperty("lastDraggedNoteId", page->property("activeDraggedNoteId"));
            QObject *dropTarget = nullptr;
            if (!waitFor([&]() {
                    dropTarget = page->property("dropTargetDelegate").value<QObject *>();
                    return dropTarget && dropTarget->property("dragHovered").toBool()
                        && (dropTarget->property("dropSpace").toReal() > 0
                            || dropTarget->property("dropAfterSpace").toReal() > 0);
                })) {
                return false;
            }
            const auto displacedNeighbor = [&]() {
                for (int row = 0; row < tree->property("rows").toInt(); ++row) {
                    if (auto *item = delegate(page, row); item && !item->property("partOfActiveDrag").toBool()
                        && qAbs(item->property("reorderOffset").toReal()) > 1) {
                        return true;
                    }
                }
                return false;
            };
            if (!waitFor(displacedNeighbor))
                return false;
            QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
            return waitFor([&]() {
                if (preview->property("previewCount").toInt() != 0)
                    return false;
                for (int row = 0; row < tree->property("rows").toInt(); ++row)
                    if (auto *item = delegate(page, row);
                        item && qAbs(item->property("reorderOffset").toReal()) > 0.5) {
                        return false;
                    }
                return true;
            });
        };

        QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::ControlModifier, noteA2Point.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);
        QVERIFY2(drag(noteA, noteB, 2), "Dragging a Ctrl-selected note group failed");
        QTRY_COMPARE(root->property("movedNotes").toInt(), 2);
        QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-b"));

        const auto contentOrigin = [root](QQuickItem *item) {
            auto *content
                = item ? qobject_cast<QQuickItem *>(item->property("contentItem").value<QObject *>()) : nullptr;
            return content ? content->mapToItem(qobject_cast<QQuickItem *>(root), QPointF()) : QPointF();
        };
        const QPointF noteA2OriginBeforeEarlyDrag   = contentOrigin(noteA2);
        const QPointF storageBOriginBeforeEarlyDrag = contentOrigin(storageB);
        const QPointF earlyDragPoint                = noteAPoint + QPointF(0, 12);
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
        QTest::mouseMove(&quick, earlyDragPoint.toPoint(), 15);
        QTRY_COMPARE(preview->property("previewCount").toInt(), 1);
        QTRY_VERIFY(page->property("dragSelectionSuppressed").toBool());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 0);
        QTRY_VERIFY(!noteA->property("highlighted").toBool());
        QTRY_VERIFY(!noteA2->property("highlighted").toBool());
        QTRY_VERIFY(!noteA->property("hoverEnabled").toBool());
        QTRY_VERIFY(!noteA2->property("hoverEnabled").toBool());
        QTest::qWait(220);
        QTRY_VERIFY(noteA2->property("dropSpace").toReal() > 0);
        QCOMPARE(noteA2->property("dropAfterSpace").toReal(), 0.0);
        const QPointF     noteA2OriginDuringEarlyDrag   = contentOrigin(noteA2);
        const QPointF     storageBOriginDuringEarlyDrag = contentOrigin(storageB);
        const QVariantMap rowExtents                    = page->property("groupedRowExtents").toMap();
        QVERIFY2((noteA2OriginDuringEarlyDrag - noteA2OriginBeforeEarlyDrag).manhattanLength() < 1,
                 qPrintable(QStringLiteral("second row moved on early drag: before=%1 during=%2 extents=%3")
                                .arg(noteA2OriginBeforeEarlyDrag.y())
                                .arg(noteA2OriginDuringEarlyDrag.y())
                                .arg(QString::fromUtf8(
                                    QJsonDocument::fromVariant(rowExtents).toJson(QJsonDocument::Compact)))));
        QVERIFY2((storageBOriginDuringEarlyDrag - storageBOriginBeforeEarlyDrag).manhattanLength() < 1,
                 qPrintable(QStringLiteral("following row moved on early drag: before=%1 during=%2")
                                .arg(storageBOriginBeforeEarlyDrag.y())
                                .arg(storageBOriginDuringEarlyDrag.y())));
        const QPointF crossedHalfPoint = noteAPoint + QPointF(0, 20);
        QTest::mouseMove(&quick, crossedHalfPoint.toPoint(), 15);
        QTRY_VERIFY(noteA2->property("dropAfterSpace").toReal() > 0);
        QTest::qWait(220);
        QVERIFY(contentOrigin(noteA2).y() < noteA2OriginBeforeEarlyDrag.y() - 1);
        QVERIFY((contentOrigin(storageB) - storageBOriginBeforeEarlyDrag).manhattanLength() < 1);
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, crossedHalfPoint.toPoint());
        QTRY_COMPARE(preview->property("previewCount").toInt(), 0);
        QTRY_VERIFY(!page->property("dragSelectionSuppressed").toBool());
        QTRY_VERIFY((noteA = delegate(page, 1)));
        QTRY_VERIFY((noteA2 = delegate(page, 2)));
        QTRY_VERIFY((storageB = delegate(page, 3)));
        QTRY_VERIFY((noteB = delegate(page, 4)));
        QTRY_COMPARE(noteA->height(), noteA->property("baseHeight").toReal());

        const auto modelRowForNote = [&notesModel](const QString &noteId) {
            for (int row = 0; row < notesModel.rowCount(); ++row) {
                if (notesModel.index(row, 0).data(Qt::UserRole + 2).toString() == noteId)
                    return row;
            }
            return -1;
        };
        const auto applyRecordedMove = [&]() {
            const QString movedId  = root->property("lastDraggedNoteId").toString();
            const QString anchorId = root->property("noteAnchor").toString();
            int           movedRow = modelRowForNote(movedId);
            QVERIFY(movedRow >= 0);
            auto movedItems = notesModel.takeRow(movedRow);
            int  anchorRow  = modelRowForNote(anchorId);
            QVERIFY(anchorRow >= 0);
            const int insertionRow = anchorRow + (root->property("noteInsertAfter").toBool() ? 1 : 0);
            notesModel.insertRow(insertionRow, movedItems);
        };

        // Exercise delegate replacement between gestures. This is what real
        // storage notifications do after every successful reorder.
        for (int iteration = 0; iteration < 6; ++iteration) {
            const QString firstId  = notesModel.index(1, 0).data(Qt::UserRole + 2).toString();
            const QString secondId = notesModel.index(2, 0).data(Qt::UserRole + 2).toString();
            QQuickItem   *first    = nullptr;
            QQuickItem   *second   = nullptr;
            QTRY_VERIFY((first = delegateForNote(firstId)));
            QTRY_VERIFY((second = delegateForNote(secondId)));
            QVERIFY2(drag(first, second, 1), "A repeated note drag did not keep its animated displacement");
            QTRY_COMPARE(root->property("movedNotes").toInt(), 1);
            QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-a"));
            QVERIFY(!root->property("lastDraggedNoteId").toString().isEmpty());
            QVERIFY(root->property("lastDraggedNoteId").toString() != root->property("noteAnchor").toString());
            applyRecordedMove();
            QTRY_VERIFY(delegate(page, 1));
            QTRY_VERIFY(delegate(page, 2));
        }

        QTRY_VERIFY((noteA = delegateForNote(QStringLiteral("note-a"))));
        QTRY_VERIFY((noteA2 = delegateForNote(QStringLiteral("note-a2"))));
        QTRY_VERIFY((storageB = delegate(page, 3)));
        QTRY_VERIFY((noteB = delegateForNote(QStringLiteral("note-b"))));

        QVERIFY(drag(noteA, noteB, 1));
        QTRY_COMPARE(root->property("movedNotes").toInt(), 1);
        QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-b"));
        QCOMPARE(root->property("noteAnchor").toString(), QStringLiteral("note-b"));
        QVERIFY(root->property("noteInsertAfter").toBool());

        QTRY_VERIFY((storageA = delegate(page, 0)));
        QTRY_VERIFY((storageB = delegate(page, 3)));
        QVERIFY(drag(storageA, storageB, 3));
        QTRY_COMPARE(root->property("movedStorages").toInt(), 1);
        QCOMPARE(root->property("storageDestination").toString(), QStringLiteral("storage-b"));

        // Keep target notes visible while their storage header is above the
        // viewport. Drop boundaries must still be attributed to storage A.
        auto storageBRow = notesModel.takeRow(3);
        auto noteBRow    = notesModel.takeRow(3);
        for (int index = 0; index < 14; ++index)
            appendItem(QStringLiteral("storage-a"), QStringLiteral("scroll-note-%1").arg(index), 1,
                       QStringLiteral("Scroll note %1").arg(index));
        notesModel.appendRow(storageBRow);
        notesModel.appendRow(noteBRow);
        QTRY_COMPARE(tree->property("rows").toInt(), 19);

        // Scrolling can reuse the delegate that initiated a drag for a
        // different row. The preview must remain a frozen image of the note,
        // and the newly represented row must not be hidden.
        tree->setProperty("contentY", 0);
        QQuickItem *recycledSource = nullptr;
        QTRY_VERIFY((recycledSource = delegateForNote(QStringLiteral("note-a"))));
        const QPointF recycledSourcePoint = recycledSource->mapToItem(
            qobject_cast<QQuickItem *>(root), QPointF(recycledSource->width() / 2, recycledSource->height() / 2));
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, recycledSourcePoint.toPoint());
        QTest::mouseMove(&quick, (recycledSourcePoint + QPointF(0, 12)).toPoint(), 15);
        QTRY_COMPARE(preview->property("previewCount").toInt(), 1);
        auto *frozenPreview = quickItemByName(preview, QStringLiteral("managerDragPreviewItem-0"));
        QTRY_VERIFY(frozenPreview);
        QVERIFY(!frozenPreview->property("live").toBool());
        QVERIFY(!frozenPreview->property("hideSource").toBool());

        tree->setProperty("contentY", qMax(0.0, tree->property("contentHeight").toReal() - tree->height()));
        QTRY_VERIFY(delegateForNote(QStringLiteral("note-a")) == nullptr);
        QTRY_COMPARE(page->property("draggedItemType").toInt(), 1);
        QTRY_COMPARE(page->property("activeDraggedNoteId").toString(), QStringLiteral("note-a"));
        if (recycledSource->property("noteId").toString() != QStringLiteral("note-a"))
            QCOMPARE(recycledSource->opacity(), 1.0);
        QVERIFY(QMetaObject::invokeMethod(page, "cancelGroupedDrag"));
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, recycledSourcePoint.toPoint());
        QTRY_COMPARE(preview->property("previewCount").toInt(), 0);

        tree->setProperty("contentY", qMax(0.0, tree->property("contentHeight").toReal() - tree->height()));
        QTRY_VERIFY(delegate(page, 0) == nullptr);

        QQuickItem *nonConsecutiveFirst  = nullptr;
        QQuickItem *nonConsecutiveSecond = nullptr;
        QTRY_VERIFY((nonConsecutiveFirst = delegateForNote(QStringLiteral("scroll-note-10"))));
        QTRY_VERIFY((nonConsecutiveSecond = delegateForNote(QStringLiteral("scroll-note-12"))));
        const auto    rootItem                 = qobject_cast<QQuickItem *>(root);
        const QPointF nonConsecutiveFirstPoint = nonConsecutiveFirst->mapToItem(
            rootItem, QPointF(nonConsecutiveFirst->width() / 2, nonConsecutiveFirst->height() / 2));
        const QPointF nonConsecutiveSecondPoint = nonConsecutiveSecond->mapToItem(
            rootItem, QPointF(nonConsecutiveSecond->width() / 2, nonConsecutiveSecond->height() / 2));
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, nonConsecutiveFirstPoint.toPoint());
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::ControlModifier, nonConsecutiveSecondPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, nonConsecutiveFirstPoint.toPoint());
        QTest::mouseMove(&quick, (nonConsecutiveFirstPoint + QPointF(0, 12)).toPoint(), 15);
        QTRY_COMPARE(preview->property("previewCount").toInt(), 2);
        auto *compactFirst  = quickItemByName(preview, QStringLiteral("managerDragPreviewItem-0"));
        auto *compactSecond = quickItemByName(preview, QStringLiteral("managerDragPreviewItem-1"));
        QTRY_VERIFY(compactFirst);
        QTRY_VERIFY(compactSecond);
        QVERIFY(qAbs(compactSecond->y() - compactFirst->y() - compactFirst->height()) < 0.5);
        QVERIFY(QMetaObject::invokeMethod(page, "cancelGroupedDrag"));
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, nonConsecutiveFirstPoint.toPoint());
        QTRY_COMPARE(preview->property("previewCount").toInt(), 0);

        QQuickItem *scrolledSource = nullptr;
        QQuickItem *scrolledTarget = nullptr;
        QTRY_VERIFY((scrolledSource = delegateForNote(QStringLiteral("note-b"))));
        QTRY_VERIFY((scrolledTarget = delegateForNote(QStringLiteral("scroll-note-12"))));
        QVERIFY2(drag(scrolledSource, scrolledTarget, 1),
                 "Notes whose storage header is scrolled out must remain valid animated drop targets");
        QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-a"));

        QQuickItem *visibleStorageB = nullptr;
        QTRY_VERIFY((visibleStorageB = delegate(page, 17)));
        const QPointF sourcePoint
            = scrolledTarget->mapToItem(rootItem, QPointF(scrolledTarget->width() / 2, scrolledTarget->height() / 2));
        const QPointF headerPoint = visibleStorageB->mapToItem(
            rootItem, QPointF(visibleStorageB->width() / 2, visibleStorageB->height() / 2));
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, sourcePoint.toPoint());
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(&quick, (sourcePoint + (headerPoint - sourcePoint) * (qreal(step) / 8)).toPoint(), 15);
        QTRY_VERIFY(visibleStorageB->property("storageDropHovered").toBool());
        QVERIFY(!visibleStorageB->property("dropBefore").toBool());
        QVERIFY(!visibleStorageB->property("dropAfter").toBool());
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, headerPoint.toPoint());
        QTRY_VERIFY(!page->property("dragSelectionSuppressed").toBool());
        QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-b"));
        QCOMPARE(root->property("noteAnchor").toString(), QString());
        QVERIFY(!root->property("noteInsertAfter").toBool());
    }

    void foldersPageUsesInlineRenameAndSharedDragLifecycle()
    {
        FolderPageTestModel foldersModel;
        QQuickWidget        quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(420, 430);
        installThemedIconImageProvider(quick.engine());
        quick.rootContext()->setContextProperty(QStringLiteral("testFoldersModel"), &foldersModel);

        QQmlComponent component(quick.engine());
        component.setData(R"QML(
            import QtQuick
            import QtQuick.Controls

            Item {
                id: harness
                objectName: "foldersHarness"

                QtObject {
                    id: workspace
                    objectName: "foldersWorkspace"
                    property var folderNotesModel: testFoldersModel
                    property bool folderCatalogAvailable: true
                    property var currentEditor: null
                    property int noteCount: 3
                    property int renameCount: 0
                    property string renamedFolderId: ""
                    property string renamedFolderName: ""
                    property int assignmentCount: 0
                    property string assignedFolderId: ""
                    property int folderMoveCount: 0
                    property string movedFolderId: ""
                    property string movedParentFolderId: ""
                    property string movedBeforeFolderId: ""

                    function createNoteInFolder(folderId, storageId) { return true }
                    function createFolder(name, parentFolderId) { return "" }
                    function renameFolder(folderId, name) {
                        ++renameCount
                        renamedFolderId = folderId
                        renamedFolderName = name
                        return true
                    }
                    function moveFolderBefore(folderId, parentFolderId, beforeFolderId) {
                        ++folderMoveCount
                        movedFolderId = folderId
                        movedParentFolderId = parentFolderId
                        movedBeforeFolderId = beforeFolderId
                        return true
                    }
                    function setFolderCollapsed(folderId, collapsed) { return true }
                    function setFolderFlags(folderId, favorite, archived) { return true }
                    function collapseAllFolders() { return true }
                    function assignNoteFolder(storageId, noteId, folderId) {
                        ++assignmentCount
                        assignedFolderId = folderId
                        return true
                    }
                }

                FoldersPage {
                    id: page
                    objectName: "foldersPage"
                    anchors.fill: parent
                    workspace: workspace
                    checkpointHandler: function() { return true }
                }
            }
        )QML",
                          QUrl(QStringLiteral("qrc:/qml/FoldersPageHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/FoldersPageHarness.qml")), &component, root);
        quick.show();
        QTest::qWait(60);

        auto *rootItem  = qobject_cast<QQuickItem *>(root);
        auto *page      = quickItemByName(rootItem, QStringLiteral("foldersPage"));
        auto *inbox     = quickItemByName(page, QStringLiteral("foldersRow-folder-inbox"));
        auto *archive   = quickItemByName(page, QStringLiteral("foldersRow-folder-archive"));
        auto *noteA     = quickItemByName(page, QStringLiteral("foldersRow-note-storage-note-a"));
        auto *noteB     = quickItemByName(page, QStringLiteral("foldersRow-note-storage-note-b"));
        auto *workspace = root->findChild<QObject *>(QStringLiteral("foldersWorkspace"));
        QVERIFY(page);
        QVERIFY(inbox);
        QVERIFY(archive);
        QVERIFY(noteA);
        QVERIFY(noteB);
        QVERIFY(workspace);

        page->setProperty("editingFolderId", QStringLiteral("inbox"));
        QQuickItem *rename = nullptr;
        QTRY_VERIFY((rename = quickItemByName(page, QStringLiteral("folderRenameField-inbox"))));
        rename->setProperty("text", QStringLiteral("Renamed Inbox"));
        QVERIFY(QMetaObject::invokeMethod(inbox, "commitRename"));
        QTRY_COMPARE(workspace->property("renameCount").toInt(), 1);
        QCOMPARE(workspace->property("renamedFolderId").toString(), QStringLiteral("inbox"));
        QCOMPARE(workspace->property("renamedFolderName").toString(), QStringLiteral("Renamed Inbox"));

        const QPointF noteAPoint = noteA->mapToItem(rootItem, QPointF(noteA->width() / 2, noteA->height() / 2));
        const QPointF noteBPoint = noteB->mapToItem(rootItem, QPointF(noteB->width() / 2, noteB->height() / 2));
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::ControlModifier, noteBPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);

        const QPointF noteDragStart = noteA->mapToItem(rootItem, QPointF(noteA->width() - 12, noteA->height() / 2));
        const QPointF archivePoint = archive->mapToItem(rootItem, QPointF(archive->width() / 2, archive->height() / 2));
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, noteDragStart.toPoint());
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(&quick, (noteDragStart + (archivePoint - noteDragStart) * (qreal(step) / 8)).toPoint(),
                             15);
        QTRY_VERIFY(page->property("dragging").toBool());
        QTRY_COMPARE(page->property("previewCount").toInt(), 2);
        auto *firstPreview  = quickItemByName(page, QStringLiteral("folderDragPreviewItem-0"));
        auto *secondPreview = quickItemByName(page, QStringLiteral("folderDragPreviewItem-1"));
        QTRY_VERIFY(firstPreview);
        QTRY_VERIFY(secondPreview);
        QVERIFY(qAbs(secondPreview->y() - firstPreview->y() - firstPreview->height()) < 0.5);
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, archivePoint.toPoint());
        QTRY_VERIFY(!page->property("dragging").toBool());
        QTRY_COMPARE(workspace->property("assignmentCount").toInt(), 2);
        QCOMPARE(workspace->property("assignedFolderId").toString(), QStringLiteral("archive"));

        const QPointF archiveDragStart
            = archive->mapToItem(rootItem, QPointF(archive->width() - 12, archive->height() / 2));
        const QPointF inboxPoint = inbox->mapToItem(rootItem, QPointF(inbox->width() / 2, inbox->height() / 2));
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, archiveDragStart.toPoint());
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(&quick, (archiveDragStart + (inboxPoint - archiveDragStart) * (qreal(step) / 8)).toPoint(),
                             15);
        QTRY_VERIFY(page->property("dragging").toBool());
        QTRY_COMPARE(page->property("previewCount").toInt(), 1);
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, inboxPoint.toPoint());
        QTRY_VERIFY(!page->property("dragging").toBool());
        QTRY_COMPARE(workspace->property("folderMoveCount").toInt(), 1);
        QCOMPARE(workspace->property("movedFolderId").toString(), QStringLiteral("archive"));
        QCOMPARE(workspace->property("movedParentFolderId").toString(), QStringLiteral("inbox"));
        QCOMPARE(workspace->property("movedBeforeFolderId").toString(), QString());
    }

    void folderPickerMenuBuildsTheCompleteFolderTree()
    {
        FolderPageTestModel foldersModel;
        QQuickWidget        quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(360, 240);
        quick.rootContext()->setContextProperty(QStringLiteral("testFoldersModel"), &foldersModel);

        QQmlComponent component(quick.engine());
        component.setData(R"QML(
            import QtQuick
            import QtQuick.Controls

            Item {
                QtObject {
                    id: workspace
                    property var folderNotesModel: testFoldersModel
                    property bool folderCatalogAvailable: true
                }

                FolderPickerMenu {
                    id: picker
                    objectName: "folderPicker"
                    workspace: workspace
                    currentFolderId: "inbox"
                }
            }
        )QML",
                          QUrl(QStringLiteral("qrc:/qml/FolderPickerHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/FolderPickerHarness.qml")), &component, root);
        quick.show();

        QObject *picker = nullptr;
        QTRY_VERIFY((picker = root->findChild<QObject *>(QStringLiteral("folderPicker"))));
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QObject *inbox   = nullptr;
        QObject *archive = nullptr;
        QTRY_VERIFY((inbox = picker->findChild<QObject *>(QStringLiteral("folderPickerItem-inbox"))));
        QTRY_VERIFY((archive = picker->findChild<QObject *>(QStringLiteral("folderPickerItem-archive"))));
        QVERIFY(inbox->property("checked").toBool());
        QVERIFY(!archive->property("checked").toBool());
    }

    void editorToolbarFolderPickerAssignsTheActiveNote()
    {
        FolderPageTestModel foldersModel;
        QQuickWidget        quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(420, 56);
        installThemedIconImageProvider(quick.engine());
        quick.rootContext()->setContextProperty(QStringLiteral("testFoldersModel"), &foldersModel);

        QQmlComponent component(quick.engine());
        component.setData(R"QML(
            import QtQuick
            import QtQuick.Controls

            Item {
                QtObject {
                    id: editorBackend
                    property bool markdown: true
                    property string undoText: ""
                    property string redoText: ""
                    property bool canUndo: false
                    property bool canRedo: false
                    property bool canInsertImages: false
                    function beginHistoryTransaction(kind, beforeView) {}
                    function endHistoryTransaction(afterView) {}
                    function copyDocumentToClipboard() {}
                    function undo() {}
                    function redo() {}
                }

                QtObject {
                    id: blockEditor
                    property var activeEditor: null
                    property var blockModel: null
                    function flushPendingEditorChanges() {}
                    function captureEditorState() { return ({}) }
                    function insertionBlockIndex() { return 0 }
                    function insertListBlock(type) { return true }
                    function insertBlockQuoteBlock() { return true }
                    function focusBlock(row) {}
                    function convertActiveToHeading(level) {}
                    function convertActiveToQuote(enabled) {}
                    function applyActiveInlineStyle(style) {}
                    function editActiveLink() {}
                }

                QtObject {
                    id: workspace
                    objectName: "editorFolderWorkspace"
                    property var folderNotesModel: testFoldersModel
                    property bool folderCatalogAvailable: true
                    property string currentFolderId: "inbox"
                    property string assignedFolderId: ""
                    function assignCurrentNoteFolder(folderId) {
                        assignedFolderId = folderId
                        currentFolderId = folderId
                        return true
                    }
                }

                EditorToolbar {
                    id: toolbar
                    objectName: "editorToolbar"
                    anchors.fill: parent
                    editorBackend: editorBackend
                    blockEditor: blockEditor
                    folderWorkspace: workspace
                }
            }
        )QML",
                          QUrl(QStringLiteral("qrc:/qml/EditorToolbarFolderHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/EditorToolbarFolderHarness.qml")), &component, root);
        quick.show();

        auto *rootItem = qobject_cast<QQuickItem *>(root);
        QVERIFY(rootItem);
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = quickItemByName(rootItem, QStringLiteral("editorFolderPickerButton"))));
        QVERIFY(button->isVisible());
        const QPointF buttonPoint = button->mapToItem(rootItem, QPointF(button->width() / 2, button->height() / 2));
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, buttonPoint.toPoint());

        auto *picker    = root->findChild<QObject *>(QStringLiteral("editorFolderPicker"));
        auto *workspace = root->findChild<QObject *>(QStringLiteral("editorFolderWorkspace"));
        QVERIFY(picker);
        QVERIFY(workspace);
        QTRY_VERIFY(picker->property("visible").toBool());
        QObject *archive = nullptr;
        QTRY_VERIFY((archive = picker->findChild<QObject *>(QStringLiteral("folderPickerItem-archive"))));
        QVERIFY(QMetaObject::invokeMethod(picker, "selectFolder", Q_ARG(QVariant, QStringLiteral("archive"))));
        QTRY_COMPARE(workspace->property("assignedFolderId").toString(), QStringLiteral("archive"));
        QCOMPARE(workspace->property("currentFolderId").toString(), QStringLiteral("archive"));
    }

    void customSpellingDictionaryPersistsAndCanBeEdited()
    {
        QSettings      settings;
        const QString  key      = QStringLiteral("editor/customSpellingDictionary");
        const QVariant previous = settings.value(key);

        {
            DesktopEditorPlatformBackend backend;
            backend.setCustomSpellingDictionary(
                { QStringLiteral("Beta"), QStringLiteral("alpha"), QStringLiteral("ALPHA"), QStringLiteral(" ") });
            const auto words = backend.customSpellingDictionary();
            QCOMPARE(words.size(), 2);
            QVERIFY(words.contains(QStringLiteral("alpha")));
            QVERIFY(words.contains(QStringLiteral("Beta")));
        }
        {
            DesktopEditorPlatformBackend backend;
            const auto                   words = backend.customSpellingDictionary();
            QCOMPARE(words.size(), 2);
            QVERIFY(words.contains(QStringLiteral("alpha")));
            QVERIFY(words.contains(QStringLiteral("Beta")));
        }

        if (previous.isValid())
            settings.setValue(key, previous);
        else
            settings.remove(key);
    }

    void modelAndControllerStayShared()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.model()->setBlockText(0, QStringLiteral("Changed\nBody"));
        QCOMPARE(editor.text(), QStringLiteral("Changed\nBody"));
        QVERIFY(editor.isDirty());

        editor.setMarkdown(true);
        QVERIFY(editor.isMarkdown());
        QVERIFY(host.model()->markdown());
    }

    void structuralCommandsUseTheCommonHistory()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.setMarkdown(true);
        const int before = editor.model()->rowCount();
        host.insertTable();
        QCoreApplication::processEvents();
        QCOMPARE(editor.model()->rowCount(), before + 1);
        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->rowCount(), before);
    }

    void focusAdapterEmitsCheckpointSignal()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);
        QSignalSpy            lost(&host, &DesktopNoteEditorHost::focusLost);

        host.show();
        QTest::qWait(20);
        QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(host.quickWidget(), &event);
        QCOMPARE(lost.size(), 1);
    }
};

QTEST_MAIN(DesktopNoteEditorHostTest)

#include "desktopnoteeditorhost_test.moc"
