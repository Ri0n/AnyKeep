#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest>

#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notesmanagerwindow.h"
#include "notesmodel.h"
#include "notesworkspacecontroller.h"
#include "ptfstorage.h"
#include "recentnotesmodel.h"
#include "themediconimageprovider.h"

#include "desktopqmltestsupport.h"
#include "editortestsupport.h"
#include "managerinteractionfixture.h"
#include "notesmanagerqml_test.h"
#include "quicktestsupport.h"

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

void NotesManagerQmlTest::loadsNotesManagerQmlShell()
{
    DraftManager       drafts(std::make_unique<MemoryDraftStore>());
    NotesManagerWindow manager;
    QVERIFY(manager.isReady());
}

void NotesManagerQmlTest::recentDragChangesTheNoteTimestamp_data()
{
    QTest::addColumn<bool>("mixedStorages");
    QTest::newRow("one-storage") << false;
    QTest::newRow("mixed-storages") << true;
}

void NotesManagerQmlTest::recentDragChangesTheNoteTimestamp()
{
    QFETCH(bool, mixedStorages);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    class TestStorage final : public PTFStorage {
    public:
        TestStorage(const QString &id, const QString &path) : id_(id) { notesDir.setPath(path); }
        bool          init() override { return isAccessible(); }
        const QString systemName() const override { return id_; }

    private:
        QString id_;
    };

    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("other")));
    const auto            otherPath = directory.filePath(QStringLiteral("other"));
    auto                  storage   = std::make_unique<TestStorage>(QStringLiteral("recent-source"), directory.path());
    auto                 *raw       = storage.get();
    auto                  other     = std::make_unique<TestStorage>(QStringLiteral("recent-neighbor"), otherPath);
    QPointer<TestStorage> otherRaw(mixedStorages ? other.get() : nullptr);

    const auto newestTime = QDateTime::currentDateTimeUtc().addSecs(-60);
    for (int row = 0; row < 3; ++row) {
        const auto name = QStringLiteral("recent-%1.md").arg(row);
        const auto path = mixedStorages && row < 2 ? QDir(otherPath).filePath(name) : directory.filePath(name);
        QFile      file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("# Test note\n\nBody\n") > 0);
        QVERIFY(file.flush());
        QVERIFY(file.setFileTime(newestTime.addSecs(-row * 10), QFileDevice::FileModificationTime));
    }

    auto *manager = NoteManager::instance();
    manager->registerStorage(std::move(storage));
    if (mixedStorages)
        manager->registerStorage(std::move(other));
    const auto               cleanup = qScopeGuard([manager, raw, otherRaw] {
        manager->unregisterStorage(raw);
        if (otherRaw)
            manager->unregisterStorage(otherRaw);
    });
    DraftManager             drafts(std::make_unique<MemoryDraftStore>());
    NotesWorkspaceController workspace(nullptr, &drafts, nullptr);
    auto                    *recent = workspace.recentNotesModel();
    QTRY_COMPARE(recent->rowCount(), 3);

    ManagerInteractionFixture fixture(workspace.groupedNotesModel(), recent);
    QVERIFY2(fixture.isReady(), qPrintable(fixture.errorString()));
    QVERIFY(fixture.page->setProperty("workspace", QVariant::fromValue(static_cast<QObject *>(&workspace))));
    QVERIFY(fixture.page->setProperty("viewMode", fixture.page->property("recentMode")));
    auto *tree = fixture.root->findChild<QQuickItem *>(QStringLiteral("recentNotes"));
    QVERIFY(tree);
    auto *collection = tree->property("collectionOwner").value<QObject *>();
    QVERIFY(collection);

    const auto    rowId   = [recent](int row) { return recent->index(row, 0).data(NotesModel::NoteIdRole).toString(); };
    const QString movedId = rowId(2);
    const QString anchorId = rowId(1);
    const QPersistentModelIndex moved(recent->index(2, 0));
    QSignalSpy                  resets(recent, &QAbstractItemModel::modelReset);
    const auto                  anchorStorageId = recent->index(1, 0).data(NotesModel::StorageIdRole).toString();
    const auto                  delegateFor     = [tree](const QString &storageId, const QString &id) {
        return quickVisibleItemByName(tree, QStringLiteral("recentDelegate-note:%1-%2").arg(storageId, id));
    };
    QQuickItem *source = nullptr;
    QQuickItem *target = nullptr;
    QTRY_VERIFY((source = delegateFor(raw->systemName(), movedId))
                && (target = delegateFor(anchorStorageId, anchorId)));
    const QPointF from = fixture.center(source);
    const QPointF to   = fixture.center(target) - QPointF(0, target->height() / 2 - 2);
    QTest::mousePress(&fixture.quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&fixture.quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
    QTRY_VERIFY(collection->property("dragging").toBool());
    QTest::mouseRelease(&fixture.quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());

    QTRY_COMPARE(rowId(1), movedId);
    QCOMPARE(moved.row(), 1);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(recent->index(1, 0).data(NotesModel::StorageIdRole).toString(), raw->systemName());
    QCOMPARE(raw->note(movedId).lastChangeUTC(), newestTime.addSecs(-5));
    QTRY_VERIFY((source = delegateFor(raw->systemName(), movedId)) && source->property("rowIndex").toInt() == 1);
    if (otherRaw)
        QCOMPARE(otherRaw->note(anchorId).lastChangeUTC(), newestTime.addSecs(-10));
}

void NotesManagerQmlTest::noteUpdatesKeepVisibleRowsAndCollapsedStorages()
{
    QStandardItemModel source;
    source.setItemRoleNames({ { NotesModel::StorageIdRole, "storageId" },
                              { NotesModel::NoteIdRole, "noteId" },
                              { NotesModel::ItemTypeRole, "itemType" },
                              { NotesModel::TitleRole, "title" },
                              { NotesModel::PreviewRole, "preview" },
                              { NotesModel::ModifiedTimeRole, "modifiedTime" } });
    const auto makeRow = [](const QString &storageId, const QString &noteId, const QString &title) {
        auto *row = new QStandardItem;
        row->setData(storageId, NotesModel::StorageIdRole);
        row->setData(noteId, NotesModel::NoteIdRole);
        row->setData(noteId.isEmpty() ? NotesModel::ItemStorage : NotesModel::ItemNote, NotesModel::ItemTypeRole);
        row->setData(title, NotesModel::TitleRole);
        return row;
    };
    auto *firstStorage  = makeRow(QStringLiteral("storage-a"), {}, QStringLiteral("Storage A"));
    auto *secondStorage = makeRow(QStringLiteral("storage-b"), {}, QStringLiteral("Storage B"));
    secondStorage->appendRow(makeRow(QStringLiteral("storage-b"), QStringLiteral("other"), QStringLiteral("Other")));
    source.appendRow(secondStorage);
    source.appendRow(firstStorage);
    const auto timestamp = QDateTime::currentDateTimeUtc();
    for (int row = 0; row < 22; ++row) {
        auto *note = makeRow(QStringLiteral("storage-a"), QStringLiteral("note-%1").arg(row),
                             QStringLiteral("Note %1").arg(row));
        note->setData(timestamp.addSecs(-row), NotesModel::ModifiedTimeRole);
        firstStorage->appendRow(note);
    }
    RecentNotesModel          recent(&source);
    ManagerInteractionFixture fixture(&source, &recent);
    QVERIFY2(fixture.isReady(), qPrintable(fixture.errorString()));
    QTRY_COMPARE(fixture.tree->property("rows").toInt(), 25);
    bool expanded = false;
    QVERIFY(QMetaObject::invokeMethod(fixture.tree, "isExpanded", Q_RETURN_ARG(bool, expanded), Q_ARG(int, 0)));
    QVERIFY(expanded);
    QVERIFY(QMetaObject::invokeMethod(fixture.tree, "collapse", Q_ARG(int, 0)));
    QTRY_COMPARE(fixture.tree->property("rows").toInt(), 24);

    QVERIFY(fixture.tree->setProperty("contentY", 155.0));
    QVERIFY(QMetaObject::invokeMethod(fixture.tree, "forceLayout"));
    QQuickItem *selected = nullptr;
    QTRY_VERIFY((selected = fixture.delegateForNote(QStringLiteral("note-8"))));
    auto *selection = fixture.root->findChild<QObject *>(QStringLiteral("notesManagerSelectionController"));
    QVERIFY(selection);
    QVERIFY(QMetaObject::invokeMethod(selection, "setSelected",
                                      Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject *>(selected))),
                                      Q_ARG(QVariant, true)));
    QVERIFY(selected->property("noteSelected").toBool());
    const QPointer<QQuickItem> selectedDelegate(selected);
    const auto                 scroll = fixture.tree->property("contentY").toDouble();
    firstStorage->child(9)->setData(QStringLiteral("Grouped preview"), NotesModel::PreviewRole);
    QCOMPARE(fixture.delegateForNote(QStringLiteral("note-8")), selectedDelegate.data());
    firstStorage->appendRow(makeRow(QStringLiteral("storage-a"), QStringLiteral("new"), QStringLiteral("New")));
    QTRY_COMPARE(fixture.tree->property("rows").toInt(), 25);
    secondStorage->appendRow(makeRow(QStringLiteral("storage-b"), QStringLiteral("hidden"), QStringLiteral("Hidden")));
    QTRY_COMPARE(fixture.tree->property("rows").toInt(), 25);
    QTRY_VERIFY((selected = fixture.delegateForNote(QStringLiteral("note-8"))));
    QVERIFY(selected->property("noteSelected").toBool());
    QCOMPARE(fixture.tree->property("contentY").toDouble(), scroll);

    QVERIFY(fixture.page->setProperty("viewMode", fixture.page->property("recentMode")));
    auto *recentTree = fixture.root->findChild<QQuickItem *>(QStringLiteral("recentNotes"));
    QVERIFY(recentTree);
    QTRY_COMPARE(recentTree->property("rows").toInt(), 25);
    QVERIFY(recentTree->setProperty("contentY", 155.0));
    QVERIFY(QMetaObject::invokeMethod(recentTree, "forceLayout"));
    const QString selectedName   = QStringLiteral("recentDelegate-note:storage-a-note-8");
    QQuickItem   *recentSelected = nullptr;
    QTRY_VERIFY((recentSelected = quickVisibleItemByName(recentTree, selectedName)));
    const QPointer<QQuickItem> recentDelegate(recentSelected);
    recentSelected->forceActiveFocus();
    QTRY_VERIFY(recentSelected->hasActiveFocus());
    const auto    recentScroll = recentTree->property("contentY").toDouble();
    const QString changedName  = QStringLiteral("recentDelegate-note:storage-a-note-9");
    firstStorage->child(9)->setData(QStringLiteral("Changed preview"), NotesModel::PreviewRole);
    QQuickItem *changed = nullptr;
    QTRY_VERIFY((changed = quickVisibleItemByName(recentTree, changedName)));
    QTRY_COMPARE(changed->property("preview").toString(), QStringLiteral("Changed preview"));
    QCOMPARE(quickVisibleItemByName(recentTree, selectedName), recentDelegate.data());
    QVERIFY(recentDelegate);
    QVERIFY(recentDelegate->hasActiveFocus());
    QVERIFY(recentDelegate->property("noteSelected").toBool());
    QCOMPARE(recentTree->property("contentY").toDouble(), recentScroll);
}

void NotesManagerQmlTest::notesManagerFoldersTabInstantiatesInOwnerContext()
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

            QtObject {
                id: workspace
                objectName: "managerFoldersWorkspace"
                property var groupedNotesModel: null
                property var recentNotesModel: null
                property var folderNotesModel: testFoldersModel
                property bool folderCatalogAvailable: true
                property var currentEditor: null
                property string currentStorageId: ""
                property string currentNoteId: ""
                property string currentTitle: ""
                property string errorString: ""
                property string searchText: ""
                property bool searchInBody: false
                property bool loading: false
                property bool busy: false
                property bool metadataBatchBusy: false
                property bool movePreparationBusy: false
                signal noteMovesPrepared(var preparedNotes)
                property int trashCount: 0
                property string recycledNoteId: ""
                property string restoredNoteId: ""
                property int noteCount: 3
                property var storages: [
                    { storageId: "store", name: "Source storage", writable: true },
                    { storageId: "destination", name: "Destination storage", writable: true,
                      iconSource: "qrc:/svg/folder-symbolic.svg" }
                ]
                property string movedToStorage: ""
                property string movedToFolder: ""
                property int folderNoteCreateCount: 0
                property string createdNoteFolderId: ""

                function saveCurrentNote() { return true }
                function closeCurrentNote() { return true }
                function reloadCurrentNote() { return true }
                function openNote(storageId, noteId) { return true }
                function createNote(storageId) { return true }
                function createNoteInFolder(folderId, storageId) {
                    ++folderNoteCreateCount
                    createdNoteFolderId = folderId
                    return true
                }
                function createFolder(name, parentFolderId) { return "" }
                function renameFolder(folderId, name) { return true }
                function moveFolderBefore(folderId, parentFolderId, beforeFolderId) { return true }
                function setFolderCollapsed(folderId, collapsed) { return true }
                function setFolderFlags(folderId, favorite, archived) { return true }
                function collapseAllFolders() { return true }
                function folderIdForNote(storageId, noteId) { return "" }
                function assignNoteFolder(storageId, noteId, folderId) {
                    movedToFolder = folderId
                    return true
                }
                function openStandalone(storageId, noteId) { return true }
                function deleteNote(storageId, noteId) { return true }
                function trashNote(storageId, noteId) { ++trashCount; return true }
                function restoreRecycledNote(storageId, noteId) {
                    restoredNoteId = noteId
                    recycledNoteId = ""
                    return true
                }
                function isRecycledNote(storageId, noteId) {
                    return recycledNoteId.length > 0 && recycledNoteId === noteId
                }
                function copyNote(sourceStorageId, noteId, destinationStorageId) { return true }
                function moveNote(sourceStorageId, noteId, destinationStorageId) { return true }
                function moveSelectedNotes(notes, destinationStorageId) {
                    movedToStorage = destinationStorageId
                    movePreparationBusy = true
                    return true
                }
                function finishMove(preparedNotes) {
                    noteMovesPrepared(preparedNotes)
                    movePreparationBusy = false
                }
                function moveStorage(sourceStorageId, destinationStorageId) { return true }
                function moveStorageToRow(sourceStorageId, destinationRow) { return true }
                function openStorageSettings(storageId) {}
            }

            NotesManagerPage {
                id: page
                objectName: "managerFoldersPage"
                anchors.fill: parent
                workspace: workspace
                embeddedEditor: false
                showCreateButton: false
                showViewModeSelector: false
                viewMode: foldersMode
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/ManagerFoldersHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/ManagerFoldersHarness.qml")), &component, root.release());
    quick.show();

    auto *page = quick.rootObject()->findChild<QQuickItem *>(QStringLiteral("managerFoldersPage"));
    QVERIFY(page);
    QQuickItem *foldersList = nullptr;
    QTRY_VERIFY((foldersList = quickItemByName(page, QStringLiteral("foldersList"))));
    QVERIFY(foldersList->isVisible());
    auto *folderPicker = page->findChild<QObject *>(QStringLiteral("noteFolderPicker"));
    QVERIFY(folderPicker);
    QVERIFY(folderPicker->property("enabled").toBool());
    QVERIFY(!folderPicker->property("visible").toBool());

    auto *foldersPage = quickVisibleItemByName(page, QStringLiteral("foldersPage"));
    auto *workspace   = quick.rootObject()->findChild<QObject *>(QStringLiteral("managerFoldersWorkspace"));
    QVERIFY(foldersPage);
    QVERIFY(workspace);
    foldersPage->setProperty("selectedFolderId", QStringLiteral("inbox"));
    QVERIFY(QMetaObject::invokeMethod(page, "createNote"));
    QCOMPARE(workspace->property("folderNoteCreateCount").toInt(), 1);
    QCOMPARE(workspace->property("createdNoteFolderId").toString(), QStringLiteral("inbox"));

    auto *searchField = quickVisibleItemByName(page, QStringLiteral("notesSearchField"));
    auto *inbox       = quickVisibleItemByName(page, QStringLiteral("foldersRow-folder-inbox"));
    QVERIFY(searchField);
    QVERIFY(inbox);
    searchField->forceActiveFocus(Qt::MouseFocusReason);
    QTRY_VERIFY(searchField->hasActiveFocus());
    const QPointF inboxPoint = inbox->mapToItem(quick.rootObject(), QPointF(inbox->width() / 2, inbox->height() / 2));
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, inboxPoint.toPoint());
    QTRY_VERIFY(!searchField->hasActiveFocus());
    QTRY_VERIFY(inbox->hasActiveFocus());

    for (int index = 0; index < 12; ++index) {
        const auto name = QStringLiteral("folder-%1").arg(index);
        foldersModel.addFolder(name, name);
    }
    quick.resize(320, 430);
    page->setProperty("touchActions", true);
    auto *selection = page->findChild<QObject *>(QStringLiteral("notesManagerSelectionController"));
    QVERIFY(selection);
    const QVariantMap selectedNotes { { QStringLiteral("store\nnote"),
                                        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("store") },
                                                      { QStringLiteral("noteId"), QStringLiteral("note") },
                                                      { QStringLiteral("title"), QStringLiteral("Note") },
                                                      { QStringLiteral("favorite"), false } } } };
    selection->setProperty("selectedNotes", selectedNotes);
    QTRY_VERIFY(page->property("selectionMode").toBool());

    auto *actions  = quickVisibleItemByName(page, QStringLiteral("mobileNoteSelectionActionBar"));
    auto *deselect = quickVisibleItemByName(page, QStringLiteral("mobileSelectionClear"));
    auto *more     = quickVisibleItemByName(page, QStringLiteral("mobileSelectionMore"));
    auto *menu     = page->findChild<QObject *>(QStringLiteral("mobileSelectionMoreMenu"));
    auto *strip    = quickVisibleItemByName(page, QStringLiteral("mobileSelectionActionStrip"));
    QVERIFY(actions);
    QVERIFY(strip);
    QVERIFY(deselect);
    QVERIFY(more);
    QVERIFY(menu);

    const auto click = [&quick](QQuickItem *item) {
        const QPointF center(item->width() / 2, item->height() / 2);
        QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier,
                          item->mapToItem(quick.rootObject(), center).toPoint());
    };
    const auto originalDirection = QGuiApplication::layoutDirection();
    const auto restoreDirection
        = qScopeGuard([originalDirection] { QGuiApplication::setLayoutDirection(originalDirection); });
    for (const auto direction : { Qt::LeftToRight, Qt::RightToLeft }) {
        QGuiApplication::setLayoutDirection(direction);
        QTRY_COMPARE(page->width(), 320.0);
        for (const auto &name : { "mobileSelectionMore", "mobileSelectionClear" }) {
            auto *button = quickVisibleItemByName(page, QString::fromLatin1(name));
            QVERIFY2(button, name);
            QVERIFY(button->width() >= 48);
            QVERIFY(button->height() >= 48);
            auto *icon = button->property("contentItem").value<QQuickItem *>();
            QVERIFY(icon);
            QVERIFY(icon->property("pixelSize").toInt() >= 28);
            const auto bounds = button->mapRectToItem(actions, QRectF(0, 0, button->width(), button->height()));
            QVERIFY(QRectF(0, 0, actions->width(), actions->height()).contains(bounds));
        }

        QTRY_VERIFY(strip->width() >= 100 && strip->property("contentWidth").toReal() > strip->width());
        QVERIFY(QMetaObject::invokeMethod(strip, "positionViewAtEnd"));
        auto *storage = quickVisibleItemByName(strip, QStringLiteral("mobileSelectionStorageButton"));
        QVERIFY(storage);
        QCOMPARE(storage->width(), 52.0);
        QCOMPARE(storage->height(), 52.0);
        auto *icon = storage->property("contentItem").value<QQuickItem *>();
        QVERIFY(icon);
        QCOMPARE(icon->property("pixelSize").toInt(), 28);
        const QRectF stripBounds(0, 0, strip->width(), strip->height());
        QTRY_VERIFY(stripBounds.contains(storage->mapRectToItem(strip, QRectF(0, 0, 52, 52))));

        QVERIFY(!menu->property("visible").toBool());
        click(more);
        QTRY_VERIFY(menu->property("opened").toBool());
        QCOMPARE(workspace->property("trashCount").toInt(), 0);
        auto *menuContent = menu->property("contentItem").value<QQuickItem *>();
        QVERIFY(menuContent);
        QVERIFY(!quickVisibleItemByName(menuContent, QStringLiteral("mobileSelectionRestore")));
        auto *move = quickVisibleItemByName(menuContent, QStringLiteral("mobileSelectionMoveStorage"));
        QVERIFY(move);
        QTRY_VERIFY(move->mapToItem(menuContent, QPointF()).y() > 0);
        const auto menuBounds
            = menuContent->mapRectToItem(page, QRectF(0, 0, menuContent->width(), menuContent->height()));
        QVERIFY(QRectF(0, 0, page->width(), page->height()).contains(menuBounds));
        for (const auto &name : { "mobileSelectionMenuTag", "mobileSelectionMenuFavorite", "mobileSelectionMoveFolder",
                                  "mobileSelectionMoveStorage", "mobileSelectionArchive", "mobileSelectionMenuClear",
                                  "mobileSelectionDelete" }) {
            auto *item = quickVisibleItemByName(menuContent, QString::fromLatin1(name));
            QVERIFY2(item, name);
            QVERIFY(item->height() >= 48);
            const auto bounds = item->mapRectToItem(page, QRectF(0, 0, item->width(), item->height()));
            QVERIFY(QRectF(0, 0, page->width(), page->height()).contains(bounds));
        }
        auto *moveDialog = page->findChild<QObject *>(QStringLiteral("mobileMoveDestinationDialog"));
        QVERIFY(moveDialog);
        click(move);
        QTRY_VERIFY(moveDialog->property("opened").toBool());
        QTRY_VERIFY(!menu->property("visible").toBool());
        auto *destinations = moveDialog->property("contentItem").value<QQuickItem *>();
        QVERIFY(destinations);
        QQuickItem *storageDestination = nullptr;
        QTRY_VERIFY((storageDestination
                     = quickVisibleItemByName(destinations, QStringLiteral("mobileMoveDestination-destination"))));
        auto *currentStorage = quickVisibleItemByName(destinations, QStringLiteral("mobileMoveDestination-store"));
        QVERIFY(currentStorage);
        QVERIFY(!currentStorage->isEnabled());
        QVERIFY(storageDestination->isEnabled());
        auto *storageIcon = quickVisibleItemByName(storageDestination, QStringLiteral("mobileDestinationStorageIcon"));
        QVERIFY(storageIcon);
        QCOMPARE(storageIcon->property("source").toUrl(), QUrl(QStringLiteral("qrc:/svg/folder-symbolic.svg")));
        QVERIFY(quickVisibleItemByName(currentStorage, QStringLiteral("mobileDestinationCurrentMark")));
        QVERIFY(!quickVisibleItemByName(storageDestination, QStringLiteral("mobileDestinationCurrentMark")));
        auto *destinationName = quickVisibleItemByName(storageDestination, QStringLiteral("mobileDestinationName"));
        auto *currentName     = quickVisibleItemByName(currentStorage, QStringLiteral("mobileDestinationName"));
        QVERIFY(destinationName);
        QVERIFY(currentName);
        QCOMPARE(destinationName->property("font").value<QFont>().pixelSize(), 18);
        QCOMPARE(destinationName->property("color").value<QColor>(), currentName->property("color").value<QColor>());
        QVERIFY(QMetaObject::invokeMethod(moveDialog, "reject"));
        QTRY_VERIFY(!moveDialog->property("visible").toBool());
        QVERIFY(page->property("selectionMode").toBool());

        click(more);
        QTRY_VERIFY(menu->property("opened").toBool());
        auto *moveFolder = quickVisibleItemByName(menuContent, QStringLiteral("mobileSelectionMoveFolder"));
        QVERIFY(moveFolder);
        QTRY_VERIFY(moveFolder->mapToItem(menuContent, QPointF()).y() > 0);
        click(moveFolder);
        QTRY_VERIFY(moveDialog->property("opened").toBool());
        QQuickItem *folder = nullptr;
        QTRY_VERIFY((folder = quickVisibleItemByName(destinations, QStringLiteral("mobileMoveDestination-inbox"))));
        QTRY_VERIFY(destinations->property("contentHeight").toReal() > destinations->height());
        const auto bounds
            = destinations->mapRectToItem(page, QRectF(0, 0, destinations->width(), destinations->height()));
        QVERIFY(QRectF(0, 0, page->width(), page->height()).contains(bounds));
        if (direction == Qt::RightToLeft) {
            click(folder);
            QTRY_COMPARE(workspace->property("movedToFolder").toString(), QStringLiteral("inbox"));
            QTRY_VERIFY(!page->property("selectionMode").toBool());
            selection->setProperty("selectedNotes", selectedNotes);
        } else {
            QVERIFY(QMetaObject::invokeMethod(moveDialog, "reject"));
        }
        QTRY_VERIFY(!moveDialog->property("visible").toBool());
    }

    quick.resize(480, 430);
    QTRY_VERIFY(strip->width() >= strip->property("contentWidth").toReal());
    QVERIFY(!strip->property("interactive").toBool());

    auto          moveSelection = selectedNotes;
    const QString failedKey     = QStringLiteral("store\nfailed");
    const QString laterKey      = QStringLiteral("store\nlater");
    moveSelection.insert(failedKey,
                         QVariantMap { { QStringLiteral("storageId"), QStringLiteral("store") },
                                       { QStringLiteral("noteId"), QStringLiteral("failed") } });
    selection->setProperty("selectedNotes", moveSelection);

    auto *storageButton = quickVisibleItemByName(strip, QStringLiteral("mobileSelectionStorageButton"));
    QVERIFY(storageButton);
    click(storageButton);
    auto *moveDialog = page->findChild<QObject *>(QStringLiteral("mobileMoveDestinationDialog"));
    QTRY_VERIFY(moveDialog->property("opened").toBool());
    auto       *destinations = moveDialog->property("contentItem").value<QQuickItem *>();
    QQuickItem *destination  = nullptr;
    QTRY_VERIFY(
        (destination = quickVisibleItemByName(destinations, QStringLiteral("mobileMoveDestination-destination"))));
    click(destination);
    QTRY_COMPARE(workspace->property("movedToStorage").toString(), QStringLiteral("destination"));
    QTRY_VERIFY(!moveDialog->property("visible").toBool());
    QCOMPARE(page->property("selectedNotes").toMap(), moveSelection);
    QVERIFY(!storageButton->isEnabled());

    moveSelection.insert(laterKey,
                         QVariantMap { { QStringLiteral("storageId"), QStringLiteral("store") },
                                       { QStringLiteral("noteId"), QStringLiteral("later") } });
    selection->setProperty("selectedNotes", moveSelection);
    const QVariantList preparedNotes { selectedNotes.first() };
    QVERIFY(QMetaObject::invokeMethod(workspace, "finishMove", Q_ARG(QVariant, QVariant(preparedNotes))));
    moveSelection.remove(selectedNotes.firstKey());
    QTRY_COMPARE(page->property("selectedNotes").toMap(), moveSelection);
    QVERIFY(actions->isVisible());
    QVERIFY(storageButton->isEnabled());

    click(deselect);
    QTRY_VERIFY(!actions->isVisible());
    QVERIFY(!page->property("selectionMode").toBool());
    QCOMPARE(workspace->property("trashCount").toInt(), 0);

    workspace->setProperty("recycledNoteId", QStringLiteral("note"));
    selection->setProperty("selectedNotes", selectedNotes);
    QTRY_VERIFY(more->isVisible());
    click(more);
    QTRY_VERIFY(menu->property("opened").toBool());
    // Polish the newly visible menu row before sending pointer input.
    QVERIFY(!quick.grabFramebuffer().isNull());
    auto       *menuContent = menu->property("contentItem").value<QQuickItem *>();
    QQuickItem *restore     = nullptr;
    QTRY_VERIFY((restore = quickVisibleItemByName(menuContent, QStringLiteral("mobileSelectionRestore"))));
    QVERIFY(restore->isEnabled());
    QTRY_VERIFY(QRectF(0, 0, menuContent->width(), menuContent->height())
                    .contains(restore->mapRectToItem(menuContent, QRectF(0, 0, restore->width(), restore->height()))));
    click(restore);
    QTRY_COMPARE(workspace->property("restoredNoteId").toString(), QStringLiteral("note"));
    QTRY_VERIFY(!page->property("selectionMode").toBool());
    QCOMPARE(workspace->property("trashCount").toInt(), 0);
}
