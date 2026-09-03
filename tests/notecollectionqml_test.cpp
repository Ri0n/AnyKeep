#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest>

#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteeditor.h"
#include "notesmanagerwindow.h"
#include "themediconimageprovider.h"

#include "desktopqmltestsupport.h"
#include "editortestsupport.h"
#include "notesmanagerqml_test.h"
#include "quicktestsupport.h"

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

void NotesManagerQmlTest::recentNoteSwipeClosesEveryDeleteAction()
{
    QQuickWidget quick;
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(360, 220);
    installThemedIconImageProvider(quick.engine());

    QStandardItemModel notesModel;
    notesModel.setItemRoleNames({
        { Qt::UserRole + 1, "storageId" },
        { Qt::UserRole + 2, "noteId" },
        { Qt::UserRole + 3, "itemType" },
        { Qt::UserRole + 4, "title" },
    });
    const auto appendNote = [&notesModel](const QString &id) {
        auto *item = new QStandardItem;
        item->setData(QStringLiteral("storage"), Qt::UserRole + 1);
        item->setData(id, Qt::UserRole + 2);
        item->setData(1, Qt::UserRole + 3);
        item->setData(id, Qt::UserRole + 4);
        notesModel.appendRow(item);
    };
    appendNote(QStringLiteral("first"));
    appendNote(QStringLiteral("second"));
    quick.rootContext()->setContextProperty(QStringLiteral("swipeNotesModel"), &notesModel);

    QQmlComponent component(quick.engine());
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls
        import "notelist" as NoteList

        Item {
            NoteList.NoteCollectionView {
                anchors.fill: parent
                model: swipeNotesModel
                nativeModelHierarchy: false
                touchActions: true
                swipeDeleteEnabled: true
                allowNoteDrag: false
                allowGroupDrag: false
                rowObjectNameProvider: function(item) {
                    return "swipeRow-" + item.noteId
                }
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/SwipeHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/SwipeHarness.qml")), &component, root);
    quick.show();

    auto       *rootItem = qobject_cast<QQuickItem *>(root);
    QQuickItem *second   = nullptr;
    QQuickItem *action   = nullptr;
    QTRY_VERIFY((second = quickItemByName(rootItem, QStringLiteral("swipeRow-second"))));

    QVERIFY(QMetaObject::invokeMethod(second, "openDeleteSwipe"));
    QTRY_VERIFY((action = quickItemByName(rootItem, QStringLiteral("noteSwipeDelete-storage-second"))));
    QTRY_VERIFY(action->opacity() > 0.99);
    QVERIFY(QMetaObject::invokeMethod(second, "closeDeleteSwipe"));
    QTRY_VERIFY(action->opacity() < 0.01);
}

void NotesManagerQmlTest::touchNoteCollectionUsesHandleAndSelectionMode()
{
    QQuickWidget quick;
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(360, 220);
    installThemedIconImageProvider(quick.engine());

    QStandardItemModel notesModel;
    notesModel.setItemRoleNames({
        { Qt::UserRole + 1, "storageId" },
        { Qt::UserRole + 2, "noteId" },
        { Qt::UserRole + 3, "itemType" },
        { Qt::UserRole + 4, "title" },
    });
    for (const auto &id : { QStringLiteral("first"), QStringLiteral("second") }) {
        auto *item = new QStandardItem;
        item->setData(QStringLiteral("storage"), Qt::UserRole + 1);
        item->setData(id, Qt::UserRole + 2);
        item->setData(1, Qt::UserRole + 3);
        item->setData(id, Qt::UserRole + 4);
        notesModel.appendRow(item);
    }
    quick.rootContext()->setContextProperty(QStringLiteral("touchSelectionNotesModel"), &notesModel);

    QQmlComponent component(quick.engine());
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls
        import "notelist" as NoteList

        Item {
            id: harness
            property int activationCount: 0
            property int contextCount: 0

            NoteList.NoteCollectionView {
                id: notes
                objectName: "touchSelectionCollection"
                anchors.fill: parent
                model: touchSelectionNotesModel
                nativeModelHierarchy: false
                flatNoteRows: true
                touchActions: true
                allowNoteDrag: true
                allowGroupDrag: false
                rowObjectNameProvider: function(item) {
                    return "touchSelectionRow-" + item.noteId
                }
                noteActivateHandler: function(item) { ++harness.activationCount }
                noteContextHandler: function(item, position) { ++harness.contextCount }
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/TouchSelectionHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/TouchSelectionHarness.qml")), &component, root);
    quick.show();

    auto       *rootItem   = qobject_cast<QQuickItem *>(root);
    QQuickItem *collection = nullptr;
    QQuickItem *first      = nullptr;
    QQuickItem *second     = nullptr;
    QQuickItem *firstCheck = nullptr;
    QQuickItem *firstGrip  = nullptr;
    QTRY_VERIFY((collection = quickItemByName(rootItem, QStringLiteral("touchSelectionCollection"))));
    QTRY_VERIFY((first = quickItemByName(rootItem, QStringLiteral("touchSelectionRow-first"))));
    QTRY_VERIFY((second = quickItemByName(rootItem, QStringLiteral("touchSelectionRow-second"))));
    QTRY_VERIFY((firstCheck = quickItemByName(rootItem, QStringLiteral("noteSelectionCheckBox-storage-first"))));
    QTRY_VERIFY((firstGrip = quickItemByName(rootItem, QStringLiteral("noteReorderHandle-storage-first"))));

    QVERIFY(!collection->property("selectionMode").toBool());
    QVERIFY(!first->property("selectionCheckBoxVisible").toBool());
    QVERIFY(!firstCheck->isVisible());
    QVERIFY(first->property("touchReorderHandleVisible").toBool());
    QVERIFY(firstGrip->isVisible());

    const auto invokeWithItem = [](QObject *target, const char *method, QObject *item) {
        return QMetaObject::invokeMethod(target, method,
                                         Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject *>(item))));
    };

    // A plain touch activation opens the note and does not leave a selected
    // row (or permanent checkbox column) behind.
    QVERIFY(invokeWithItem(collection, "activateNote", first));
    QTRY_COMPARE(root->property("activationCount").toInt(), 1);
    QCOMPARE(collection->property("selectedNotes").toMap().size(), 0);
    QVERIFY(!collection->property("selectionMode").toBool());
    QVERIFY(!firstCheck->isVisible());

    // The long-press path selects the context row. Once one note is selected,
    // all row checkboxes become available and taps toggle the selection.
    QVERIFY(QMetaObject::invokeMethod(collection, "requestContextMenu",
                                      Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject *>(first))),
                                      Q_ARG(QVariant, QVariant())));
    QTRY_COMPARE(root->property("contextCount").toInt(), 1);
    QTRY_VERIFY(collection->property("selectionMode").toBool());
    QTRY_COMPARE(collection->property("selectedNotes").toMap().size(), 1);
    QTRY_VERIFY(firstCheck->isVisible());
    QTRY_VERIFY(second->property("selectionCheckBoxVisible").toBool());

    QVERIFY(invokeWithItem(collection, "activateNote", second));
    QTRY_COMPARE(collection->property("selectedNotes").toMap().size(), 2);
    QCOMPARE(root->property("activationCount").toInt(), 1);

    QVERIFY(invokeWithItem(collection, "activateNote", first));
    QTRY_COMPARE(collection->property("selectedNotes").toMap().size(), 1);
    QVERIFY(invokeWithItem(collection, "activateNote", second));
    QTRY_COMPARE(collection->property("selectedNotes").toMap().size(), 0);
    QTRY_VERIFY(!collection->property("selectionMode").toBool());
    QTRY_VERIFY(!firstCheck->isVisible());
}

void NotesManagerQmlTest::notesManagerOutsideDropRecyclesOrPermanentlyDeletes()
{
    QQuickWidget quick;
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(420, 300);
    installThemedIconImageProvider(quick.engine());

    QQmlComponent component(quick.engine());
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls

        Item {
            id: harness
            objectName: "permanentDropHarness"
            property bool askPermanent: false

            ListModel {
                id: notes
                ListElement { storageId: "storage"; noteId: "ordinary"; itemType: 1; title: "Ordinary" }
                ListElement { storageId: "storage"; noteId: "recycled"; itemType: 1; title: "Recycled" }
            }

            QtObject {
                id: workspace
                objectName: "permanentDropWorkspace"
                property var groupedNotesModel: notes
                property var recentNotesModel: notes
                property var folderNotesModel: null
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
                property int noteCount: 2
                property var storages: []
                property int trashCount: 0
                property int deleteCount: 0

                function saveCurrentNote() { return true }
                function closeCurrentNote() { return true }
                function reloadCurrentNote() { return true }
                function openNote(storageId, noteId) { return true }
                function createNote(storageId) { return true }
                function createNoteInFolder(folderId, storageId) { return true }
                function folderIdForNote(storageId, noteId) { return "" }
                function assignNoteFolder(storageId, noteId, folderId) { return true }
                function openStandalone(storageId, noteId) { return true }
                function deleteNote(storageId, noteId) { ++deleteCount; return true }
                function trashNote(storageId, noteId) { ++trashCount; return true }
                function restoreRecycledNote(storageId, noteId) { return true }
                function isRecycledNote(storageId, noteId) { return noteId === "recycled" }
                function askBeforePermanentDelete() { return harness.askPermanent }
                function copyNote(sourceStorageId, noteId, destinationStorageId) { return true }
                function moveNote(sourceStorageId, noteId, destinationStorageId) { return true }
                function moveNotes(notes, destinationStorageId, anchorNoteId, insertAfter) { return true }
                function moveStorage(sourceStorageId, destinationStorageId) { return true }
                function moveStorageToRow(sourceStorageId, destinationRow) { return true }
                function openStorageSettings(storageId) {}
            }

            NotesManagerPage {
                id: page
                objectName: "permanentDropPage"
                anchors.fill: parent
                workspace: workspace
                embeddedEditor: false
                showCreateButton: false
                showViewModeSelector: false
                viewMode: recentMode
            }

            function dropNotes(notes) { return page.handleNotesDroppedOutside(notes) }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/PermanentDropHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/PermanentDropHarness.qml")), &component, root);
    quick.show();

    auto *workspace = root->findChild<QObject *>(QStringLiteral("permanentDropWorkspace"));
    auto *page      = root->findChild<QQuickItem *>(QStringLiteral("permanentDropPage"));
    QVERIFY(workspace);
    QVERIFY(page);

    const QVariantList mixedNotes {
        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("storage") },
                      { QStringLiteral("noteId"), QStringLiteral("ordinary") },
                      { QStringLiteral("title"), QStringLiteral("Ordinary") } },
        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("storage") },
                      { QStringLiteral("noteId"), QStringLiteral("recycled") },
                      { QStringLiteral("title"), QStringLiteral("Recycled") } },
    };
    QVariant dropped;
    QVERIFY(QMetaObject::invokeMethod(root, "dropNotes", Q_RETURN_ARG(QVariant, dropped), Q_ARG(QVariant, mixedNotes)));
    QVERIFY(dropped.toBool());
    QCOMPARE(workspace->property("trashCount").toInt(), 1);
    QCOMPARE(workspace->property("deleteCount").toInt(), 1);

    root->setProperty("askPermanent", true);
    const QVariantList recycledOnly { mixedNotes.constLast() };
    QVERIFY(
        QMetaObject::invokeMethod(root, "dropNotes", Q_RETURN_ARG(QVariant, dropped), Q_ARG(QVariant, recycledOnly)));
    QVERIFY(dropped.toBool());
    QCOMPARE(workspace->property("deleteCount").toInt(), 1);
    QObject *dialog = root->findChild<QObject *>(QStringLiteral("permanentDeleteDialog"));
    QTRY_VERIFY(dialog && dialog->property("visible").toBool());
    QTRY_VERIFY(dialog->property("height").toReal() >= 140.0);

    QVariant committed;
    QVERIFY(QMetaObject::invokeMethod(page, "commitPermanentDeletion", Q_RETURN_ARG(QVariant, committed)));
    QVERIFY(committed.toBool());
    QCOMPARE(workspace->property("deleteCount").toInt(), 2);
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
}

void NotesManagerQmlTest::flatNoteCollectionUsesSharedTreeDragAnimation()
{
    QQuickWidget quick;
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(360, 220);
    installThemedIconImageProvider(quick.engine());

    QStandardItemModel notesModel;
    notesModel.setItemRoleNames({
        { Qt::UserRole + 1, "storageId" },
        { Qt::UserRole + 2, "noteId" },
        { Qt::UserRole + 3, "itemType" },
        { Qt::UserRole + 4, "title" },
    });
    QStringList noteIds { QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third") };
    for (int index = 3; index < 30; ++index)
        noteIds.push_back(QStringLiteral("note-%1").arg(index, 2, 10, QLatin1Char('0')));
    for (const auto &id : noteIds) {
        auto *item = new QStandardItem;
        item->setData(QStringLiteral("storage"), Qt::UserRole + 1);
        item->setData(id, Qt::UserRole + 2);
        item->setData(1, Qt::UserRole + 3);
        item->setData(id, Qt::UserRole + 4);
        notesModel.appendRow(item);
    }
    quick.rootContext()->setContextProperty(QStringLiteral("flatTreeNotesModel"), &notesModel);

    QQmlComponent component(quick.engine());
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls
        import "notelist" as NoteList

        Item {
            id: harness
            property int commitCount: 0
            property string draggedNoteId: ""
            property int incrementalFetchRequests: 0
            property string incrementalFetchNoteId: ""

            NoteList.NoteCollectionView {
                id: notes
                objectName: "flatTreeCollection"
                anchors.fill: parent
                model: flatTreeNotesModel
                nativeModelHierarchy: false
                flatNoteRows: true
                viewObjectName: "flatTreeView"
                previewObjectName: "flatTreePreview"
                previewObjectNamePrefix: "flatTreePreviewItem-"
                rowObjectNameProvider: function(item) {
                    return "flatTreeRow-" + item.noteId
                }
                incrementalFetchHandler: function(storageId, noteId) {
                    ++harness.incrementalFetchRequests
                    harness.incrementalFetchNoteId = noteId
                    return false
                }
                boundaryProvider: function(view, payload, items) {
                    return view.boundaries(items, function(item, after) {
                        return {
                            anchorNoteId: item ? item.noteId : "",
                            insertAfter: after
                        }
                    })
                }
                directTargetProvider: function() { return null }
                commitHandler: function(payload) {
                    ++harness.commitCount
                    harness.draggedNoteId = payload.notes[0].noteId
                    return true
                }
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/FlatTreeDragHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/FlatTreeDragHarness.qml")), &component, root);
    quick.show();

    auto       *rootItem   = qobject_cast<QQuickItem *>(root);
    auto       *collection = quickItemByName(rootItem, QStringLiteral("flatTreeCollection"));
    auto       *tree       = quickItemByName(rootItem, QStringLiteral("flatTreeView"));
    auto       *preview    = quickItemByName(rootItem, QStringLiteral("flatTreePreview"));
    QQuickItem *first      = nullptr;
    QQuickItem *second     = nullptr;
    QQuickItem *third      = nullptr;
    QVERIFY(collection);
    QVERIFY(tree);
    QVERIFY(preview);
    QTRY_COMPARE(tree->property("rows").toInt(), 30);
    QTRY_VERIFY((first = quickItemByName(rootItem, QStringLiteral("flatTreeRow-first"))));
    QTRY_VERIFY((second = quickItemByName(rootItem, QStringLiteral("flatTreeRow-second"))));
    QTRY_VERIFY((third = quickItemByName(rootItem, QStringLiteral("flatTreeRow-third"))));
    QVERIFY(first->property("compactFlatNoteRow").toBool());
    QCOMPARE(first->property("leadingInset").toReal(), 8.0);

    const QPointF from = first->mapToItem(rootItem, QPointF(first->width() / 2, first->height() / 2));
    QPointF       to   = third->mapToItem(rootItem, QPointF(third->width() / 2, third->height() - 2));
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);

    QTRY_VERIFY(collection->property("dragging").toBool());
    QTRY_COMPARE(preview->property("previewCount").toInt(), 1);
    QTRY_VERIFY(second->property("reorderOffset").toReal() < -1);
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QTRY_VERIFY(!collection->property("dragging").toBool());
    QTRY_COMPARE(root->property("commitCount").toInt(), 1);
    QCOMPARE(root->property("draggedNoteId").toString(), QStringLiteral("first"));

    const int fetchRequestsBeforeScroll = root->property("incrementalFetchRequests").toInt();
    tree->setProperty("contentY", 600);
    QTRY_VERIFY(root->property("incrementalFetchRequests").toInt() > fetchRequestsBeforeScroll);
    QVERIFY(!root->property("incrementalFetchNoteId").toString().isEmpty());
    QQuickItem *beforeScrolledSource = nullptr;
    QQuickItem *scrolledSource       = nullptr;
    QQuickItem *scrolledTarget       = nullptr;
    QTRY_VERIFY((beforeScrolledSource = quickItemByName(rootItem, QStringLiteral("flatTreeRow-note-19"))));
    QTRY_VERIFY((scrolledSource = quickItemByName(rootItem, QStringLiteral("flatTreeRow-note-20"))));
    QTRY_VERIFY((scrolledTarget = quickItemByName(rootItem, QStringLiteral("flatTreeRow-note-22"))));
    const QPointF scrolledFrom
        = scrolledSource->mapToItem(rootItem, QPointF(scrolledSource->width() / 2, scrolledSource->height() / 2));
    const QPointF scrolledTo
        = scrolledTarget->mapToItem(rootItem, QPointF(scrolledTarget->width() / 2, scrolledTarget->height() - 2));
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, scrolledFrom.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&quick, (scrolledFrom + (scrolledTo - scrolledFrom) * (qreal(step) / 8)).toPoint(), 15);
    QTRY_VERIFY(collection->property("dragging").toBool());
    QTest::qWait(220);
    QVERIFY2(qAbs(beforeScrolledSource->property("reorderOffset").toReal()) < 1,
             "A row above the scrolled drag source must not animate");
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, scrolledTo.toPoint());
    QTRY_VERIFY(!collection->property("dragging").toBool());
    QTRY_COMPARE(root->property("commitCount").toInt(), 2);
    QCOMPARE(root->property("draggedNoteId").toString(), QStringLiteral("note-20"));
}

void NotesManagerQmlTest::genericReorderUsesOutsideDropHandler()
{
    QQuickWidget quick;
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(180, 120);

    QQmlComponent component(quick.engine());
    component.setData(R"QML(
        import QtQuick
        import "reorder" as Reorder

        Item {
            id: root
            property int normalDropCount: 0
            property int outsideDropCount: 0

            Rectangle {
                id: source
                width: 80
                height: 24
            }

            Reorder.GenericReorderController {
                id: controller
                anchors.fill: parent
                geometryItem: root
                boundaryProvider: function() { return [] }
                commitHandler: function() {
                    ++root.normalDropCount
                    return true
                }
                outsideDropProvider: function() { return true }
                outsideDropHandler: function(payload) {
                    if (payload.kind !== "notes")
                        return false
                    ++root.outsideDropCount
                    return true
                }
            }

            function dropOutside() {
                controller.beginDrag({
                    sources: [source],
                    pointerItem: source,
                    payload: { kind: "notes" }
                })
                return controller.finishDrag()
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/OutsideDropHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/OutsideDropHarness.qml")), &component, root);

    QVERIFY(QMetaObject::invokeMethod(root, "dropOutside"));
    QCOMPARE(root->property("outsideDropCount").toInt(), 1);
    QCOMPARE(root->property("normalDropCount").toInt(), 0);
}
