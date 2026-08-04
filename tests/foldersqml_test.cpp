#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
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

void NotesManagerQmlTest::foldersPageUsesInlineRenameAndSharedDragLifecycle()
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
                function createFolder(name, parentFolderId) { return "created-folder" }
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
                function isRecycledNote(storageId, noteId) { return noteId === "note-c" }
                function assignNoteFolder(storageId, noteId, folderId) {
                    ++assignmentCount
                    assignedFolderId = folderId
                    return testFoldersModel.assignNoteFolder(storageId, noteId, folderId)
                }
            }

            FoldersPage {
                id: page
                objectName: "foldersPage"
                anchors.fill: parent
                workspace: workspace
                currentStorageId: "storage"
                currentNoteId: "note-a"
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
    auto *inbox     = quickVisibleItemByName(page, QStringLiteral("foldersRow-folder-inbox"));
    auto *archive   = quickVisibleItemByName(page, QStringLiteral("foldersRow-folder-archive"));
    auto *noteA     = quickVisibleItemByName(page, QStringLiteral("foldersRow-note-storage-note-a"));
    auto *noteC     = quickVisibleItemByName(page, QStringLiteral("foldersRow-note-storage-note-c"));
    auto *noteB     = quickVisibleItemByName(page, QStringLiteral("foldersRow-note-storage-note-b"));
    auto *workspace = root->findChild<QObject *>(QStringLiteral("foldersWorkspace"));
    QVERIFY(page);
    QVERIFY(inbox);
    QVERIFY(archive);
    QVERIFY(noteA);
    QVERIFY(noteC);
    QVERIFY(noteB);
    QVERIFY(workspace);

    QVERIFY(QMetaObject::invokeMethod(page, "createFolder", Q_ARG(QVariant, QStringLiteral("inbox"))));
    QCOMPARE(page->property("selectedFolderId").toString(), QStringLiteral("created-folder"));
    QVERIFY(!page->property("unsortedSelected").toBool());

    const QPointF notePoint = noteA->mapToItem(rootItem, QPointF(noteA->width() / 2, noteA->height() / 2));
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, notePoint.toPoint());
    QTRY_COMPARE(page->property("selectedFolderId").toString(), QString());
    QVERIFY(!inbox->property("selectedGroup").toBool());

    const QPointF recycledNotePoint = noteC->mapToItem(rootItem, QPointF(noteC->width() / 2, noteC->height() / 2));
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, recycledNotePoint.toPoint());
    QTRY_COMPARE(page->property("selectedFolderId").toString(), QString());
    QVERIFY(!inbox->property("selectedGroup").toBool());

    QVERIFY(QMetaObject::invokeMethod(page, "beginFolderRename", Q_ARG(QVariant, QStringLiteral("inbox"))));
    QQuickItem *rename = nullptr;
    QTRY_VERIFY((rename = quickVisibleItemByName(page, QStringLiteral("folderRenameField-inbox"))));
    QTRY_VERIFY(rename->hasActiveFocus());
    // A pooled TreeView delegate may emit editingFinished while its model
    // identity is being replaced. It must not commit and close the active
    // inline editor.
    QTest::qWait(80);
    QCOMPARE(page->property("editingFolderId").toString(), QStringLiteral("inbox"));
    QCOMPARE(workspace->property("renameCount").toInt(), 0);
    rename->setProperty("text", QStringLiteral("Renamed Inbox"));
    auto *renameRow = ancestorWithProperty(rename, "editing");
    QVERIFY(renameRow);
    QVERIFY(QMetaObject::invokeMethod(renameRow, "commitRename"));
    QTRY_COMPARE(workspace->property("renameCount").toInt(), 1);
    QCOMPARE(workspace->property("renamedFolderId").toString(), QStringLiteral("inbox"));
    QCOMPARE(workspace->property("renamedFolderName").toString(), QStringLiteral("Renamed Inbox"));

    const QPointF archiveMenuPoint = archive->mapToItem(rootItem, QPointF(archive->width() / 2, archive->height() / 2));
    QTest::mouseClick(&quick, Qt::RightButton, Qt::NoModifier, archiveMenuPoint.toPoint());
    auto *renameAction
        = qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("folderContextRenameAction")));
    QVERIFY(renameAction);
    QTRY_VERIFY(renameAction->isVisible());
    const QPointF renameActionPoint
        = renameAction->mapToItem(rootItem, QPointF(renameAction->width() / 2, renameAction->height() / 2));
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, renameActionPoint.toPoint());
    QTRY_COMPARE(page->property("editingFolderId").toString(), QStringLiteral("archive"));
    QQuickItem *archiveRename = nullptr;
    QTRY_VERIFY((archiveRename = quickVisibleItemByName(page, QStringLiteral("folderRenameField-archive"))));
    QTRY_VERIFY(archiveRename->hasActiveFocus());

    const QPointF noteAPoint = noteA->mapToItem(rootItem, QPointF(noteA->width() / 2, noteA->height() / 2));
    const QPointF noteBPoint = noteB->mapToItem(rootItem, QPointF(noteB->width() / 2, noteB->height() / 2));
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
    QTRY_COMPARE(page->property("editingFolderId").toString(), QString());
    QVERIFY(!archiveRename->hasActiveFocus());
    QTRY_COMPARE(workspace->property("renameCount").toInt(), 2);

    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::ControlModifier, noteBPoint.toPoint());
    QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);

    const QPointF archiveSelectPoint
        = archive->mapToItem(rootItem, QPointF(archive->width() / 2, archive->height() / 2));
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, archiveSelectPoint.toPoint());
    QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 0);
    QTRY_COMPARE(page->property("selectedFolderId").toString(), QStringLiteral("archive"));
    QVERIFY(archive->property("selectedGroup").toBool());
    QVERIFY(!noteA->property("highlighted").toBool());

    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::ControlModifier, noteBPoint.toPoint());
    QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);

    const QPointF noteDragStart = noteA->mapToItem(rootItem, QPointF(noteA->width() - 12, noteA->height() / 2));
    const QPointF archivePoint  = archive->mapToItem(rootItem, QPointF(archive->width() / 2, archive->height() / 2));
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, noteDragStart.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&quick, (noteDragStart + (archivePoint - noteDragStart) * (qreal(step) / 8)).toPoint(), 15);
    QTRY_VERIFY(page->property("dragging").toBool());
    QTRY_COMPARE(page->property("previewCount").toInt(), 2);
    auto *firstPreview  = quickItemByName(page, QStringLiteral("folderDragPreviewItem-0"));
    auto *secondPreview = quickItemByName(page, QStringLiteral("folderDragPreviewItem-1"));
    QTRY_VERIFY(firstPreview);
    QTRY_VERIFY(secondPreview);
    QVERIFY(qAbs(secondPreview->y() - firstPreview->y() - firstPreview->height()) < 0.5);
    // The stable drop contract opens the gap after the row that will own
    // the notes. It must remain there after the animation settles.
    QTest::qWait(220);
    QTest::mouseMove(&quick, (archivePoint + QPointF(1, 0)).toPoint(), 15);
    QTRY_VERIFY(archive->property("dropAfter").toBool());
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, archivePoint.toPoint());
    QTRY_VERIFY(!page->property("dragging").toBool());
    QTRY_COMPARE(workspace->property("assignmentCount").toInt(), 2);
    QCOMPARE(workspace->property("assignedFolderId").toString(), QStringLiteral("archive"));

    QTRY_VERIFY((inbox = quickVisibleItemByName(page, QStringLiteral("foldersRow-folder-inbox"))));
    QTRY_VERIFY((archive = quickVisibleItemByName(page, QStringLiteral("foldersRow-folder-archive"))));
    const QPointF archiveDragStart = archive->mapToItem(rootItem, QPointF(80, archive->height() / 2));
    const QPointF inboxBottom      = inbox->mapToItem(rootItem, QPointF(80, inbox->height()));
    // One indent step to the right while targeting the gap below Inbox
    // makes Archive its child. Keeping x unchanged would keep both at the
    // root level.
    const QPointF inboxChildPoint = inboxBottom + QPointF(18, archive->height() / 2);
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, archiveDragStart.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&quick,
                         (archiveDragStart + (inboxChildPoint - archiveDragStart) * (qreal(step) / 8)).toPoint(), 15);
    QTRY_VERIFY(page->property("dragging").toBool());
    QTRY_COMPARE(page->property("previewCount").toInt(), 1);
    QTRY_VERIFY(inbox->property("dropAfter").toBool());
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, inboxChildPoint.toPoint());
    QTRY_VERIFY(!page->property("dragging").toBool());
    QTRY_COMPARE(workspace->property("folderMoveCount").toInt(), 1);
    QCOMPARE(workspace->property("movedFolderId").toString(), QStringLiteral("archive"));
    QCOMPARE(workspace->property("movedParentFolderId").toString(), QStringLiteral("inbox"));
    QCOMPARE(workspace->property("movedBeforeFolderId").toString(), QString());

    QTest::qWait(220);
    QTRY_VERIFY((inbox = quickVisibleItemByName(page, QStringLiteral("foldersRow-folder-inbox"))));
    QTRY_VERIFY((archive = quickVisibleItemByName(page, QStringLiteral("foldersRow-folder-archive"))));
    const QPointF siblingStart  = archive->mapToItem(rootItem, QPointF(80, archive->height() / 2));
    const QPointF siblingTarget = inbox->mapToItem(rootItem, QPointF(80, inbox->height() + archive->height() / 2));
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, siblingStart.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&quick, (siblingStart + (siblingTarget - siblingStart) * (qreal(step) / 8)).toPoint(), 15);
    QTRY_VERIFY(page->property("dragging").toBool());
    QTRY_VERIFY(inbox->property("dropAfter").toBool());
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, siblingTarget.toPoint());
    QTRY_COMPARE(workspace->property("folderMoveCount").toInt(), 2);
    QCOMPARE(workspace->property("movedParentFolderId").toString(), QString());
    QCOMPARE(workspace->property("movedBeforeFolderId").toString(), QString());
}

void NotesManagerQmlTest::folderInlineRenameSurvivesDelegateReuseInQuickWindow()
{
    FolderPageTestModel foldersModel;
    QQuickView          quick;
    quick.setResizeMode(QQuickView::SizeRootObjectToView);
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
                objectName: "reuseWorkspace"
                property var folderNotesModel: testFoldersModel
                property bool folderCatalogAvailable: true
                property var currentEditor: null
                property int noteCount: 3
                property int renameCount: 0
                property string renamedFolderId: ""
                property string renamedFolderName: ""

                function createNoteInFolder(folderId, storageId) { return true }
                function createFolder(name, parentFolderId) {
                    return testFoldersModel.addFolder("created-folder", "New folder")
                }
                function renameFolder(folderId, name) {
                    ++renameCount
                    renamedFolderId = folderId
                    renamedFolderName = name
                    return true
                }
                function setFolderCollapsed(folderId, collapsed) { return true }
                function setFolderFlags(folderId, favorite, archived) { return true }
                function collapseAllFolders() { return true }
                function isRecycledNote(storageId, noteId) { return false }
                function assignNoteFolder(storageId, noteId, folderId) { return true }
            }

            FoldersPage {
                id: page
                objectName: "reuseFoldersPage"
                anchors.fill: parent
                workspace: workspace
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/FolderReuseHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/FolderReuseHarness.qml")), &component, root);
    quick.show();
    quick.requestActivate();

    auto *rootItem = qobject_cast<QQuickItem *>(root);
    auto *page     = quickVisibleItemByName(rootItem, QStringLiteral("reuseFoldersPage"));
    auto *button   = quickVisibleItemByName(page, QStringLiteral("newFolderButton"));
    QVERIFY(page);
    QVERIFY(button);
    const QPointF buttonPoint = button->mapToItem(rootItem, QPointF(button->width() / 2, button->height() / 2));
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, buttonPoint.toPoint());

    QTRY_VERIFY(quick.activeFocusItem());
    QTRY_COMPARE(quick.activeFocusItem()->objectName(), QStringLiteral("folderRenameField-created-folder"));

    page->setProperty("editingFolderId", QString());
    QVERIFY(foldersModel.removeFolder(QStringLiteral("created-folder")));
    QVERIFY(QMetaObject::invokeMethod(page, "beginFolderRename", Q_ARG(QVariant, QStringLiteral("inbox"))));

    QTRY_VERIFY(quick.activeFocusItem());
    QTRY_COMPARE(quick.activeFocusItem()->objectName(), QStringLiteral("folderRenameField-inbox"));
    QQuickItem *rename = quick.activeFocusItem();
    QVERIFY(rename->hasActiveFocus());
    QTest::qWait(80);
    QCOMPARE(page->property("editingFolderId").toString(), QStringLiteral("inbox"));

    rename->setProperty("text", QStringLiteral("Reusable Inbox"));
    auto *renameRow = ancestorWithProperty(rename, "editing");
    QVERIFY(renameRow);
    QCOMPARE(renameRow->property("groupId").toString(), QStringLiteral("inbox"));
    QVERIFY(renameRow->property("editing").toBool());
    QCOMPARE(page->property("editingFolderId").toString(), QStringLiteral("inbox"));
    QVERIFY(QMetaObject::invokeMethod(renameRow, "commitRename"));
    auto *workspace = root->findChild<QObject *>(QStringLiteral("reuseWorkspace"));
    QVERIFY(workspace);
    QTRY_COMPARE(workspace->property("renameCount").toInt(), 1);
    QCOMPARE(workspace->property("renamedFolderId").toString(), QStringLiteral("inbox"));
    QCOMPARE(workspace->property("renamedFolderName").toString(), QStringLiteral("Reusable Inbox"));
}

void NotesManagerQmlTest::foldersPageOffersEmptyRecycleBinAction()
{
    QQuickWidget quick;
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(360, 240);
    installThemedIconImageProvider(quick.engine());

    QQmlComponent component(quick.engine());
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls

        Item {
            id: harness

            ListModel {
                id: folders
                ListElement {
                    rowKind: 0
                    folderId: "recycle"
                    title: "Recycle Bin"
                    collapsed: false
                    systemFolder: true
                    noteCount: 1
                }
            }

            QtObject {
                id: workspace
                objectName: "recycleWorkspace"
                property var folderNotesModel: folders
                property bool folderCatalogAvailable: true
                property int noteCount: 1
                property var currentEditor: null
                property int emptyCount: 0
                function emptyRecycleBin() { ++emptyCount; return true }
            }

            FoldersPage {
                objectName: "foldersPage"
                anchors.fill: parent
                workspace: workspace
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/RecycleMenuHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *root = component.create();
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/RecycleMenuHarness.qml")), &component, root);
    quick.show();

    auto       *rootItem   = qobject_cast<QQuickItem *>(root);
    auto       *page       = quickItemByName(rootItem, QStringLiteral("foldersPage"));
    QQuickItem *recycleBin = nullptr;
    QTRY_VERIFY((recycleBin = quickItemByName(page, QStringLiteral("foldersRow-folder-recycle"))));
    const QPointF point = recycleBin->mapToItem(rootItem, QPointF(recycleBin->width() / 2, recycleBin->height() / 2));
    QTest::mouseClick(&quick, Qt::RightButton, Qt::NoModifier, point.toPoint());

    QObject *menu = nullptr;
    QTRY_VERIFY((menu = root->findChild<QObject *>(QStringLiteral("recycleBinContextMenu"))));
    QTRY_VERIFY(menu->property("visible").toBool());
    auto *folderMenu = root->findChild<QObject *>(QStringLiteral("folderContextMenu"));
    QVERIFY(folderMenu);
    QVERIFY(!folderMenu->property("visible").toBool());
    QObject *empty = nullptr;
    QTRY_VERIFY((empty = root->findChild<QObject *>(QStringLiteral("emptyRecycleBinAction"))));
    QVERIFY(empty->property("visible").toBool());
    QVERIFY(empty->property("enabled").toBool());
    QCOMPARE(empty->property("text").toString(), QStringLiteral("Empty Recycle Bin"));
    QVERIFY(QMetaObject::invokeMethod(page, "emptyRecycleBin"));
    auto *workspace = root->findChild<QObject *>(QStringLiteral("recycleWorkspace"));
    QVERIFY(workspace);
    QCOMPARE(workspace->property("emptyCount").toInt(), 1);
}

void NotesManagerQmlTest::folderPickerMenuBuildsTheCompleteFolderTree()
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

void NotesManagerQmlTest::editorToolbarFolderPickerAssignsTheActiveNote()
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
