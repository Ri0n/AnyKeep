#include <QJsonDocument>
#include <QQuickItem>
#include <QtTest>

#include "managerinteractionfixture.h"
#include "notesmanagerqml_test.h"
#include "quicktestsupport.h"

using namespace QtNote::TestSupport;

void NotesManagerQmlTest::notesManagerContextMenusAndSelectionWork()
{
    ManagerInteractionFixture fixture;
    QVERIFY2(fixture.isReady(), qPrintable(fixture.errorString()));
    auto       &quick    = fixture.quick;
    auto       *root     = fixture.root;
    auto       *page     = fixture.page;
    QQuickItem *storageA = nullptr;
    QQuickItem *noteA    = nullptr;
    QQuickItem *noteA2   = nullptr;
    QQuickItem *noteB    = nullptr;
    QTRY_VERIFY((storageA = fixture.delegate(0)));
    QTRY_VERIFY((noteA = fixture.delegate(1)));
    QTRY_VERIFY((noteA2 = fixture.delegate(2)));
    QTRY_VERIFY((noteB = fixture.delegate(4)));
    QVERIFY(!noteA->property("selectionCheckBoxVisible").toBool());
    auto *workspace = root->findChild<QObject *>(QStringLiteral("managerWorkspace"));
    QVERIFY(workspace);

    const QVariantList contextNotes {
        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("storage-a") },
                      { QStringLiteral("noteId"), QStringLiteral("note-a") },
                      { QStringLiteral("title"), QStringLiteral("Note A") },
                      { QStringLiteral("order"), 1 } },
        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("storage-a") },
                      { QStringLiteral("noteId"), QStringLiteral("note-a2") },
                      { QStringLiteral("title"), QStringLiteral("Note A2") },
                      { QStringLiteral("order"), 2 } },
        QVariantMap { { QStringLiteral("storageId"), QStringLiteral("storage-b") },
                      { QStringLiteral("noteId"), QStringLiteral("note-b") },
                      { QStringLiteral("title"), QStringLiteral("Note B") },
                      { QStringLiteral("order"), 4 } },
    };
    page->setProperty("contextMenuNotes", contextNotes);
    QCoreApplication::processEvents();

    auto *contextOpen  = root->findChild<QObject *>(QStringLiteral("noteContextOpen"));
    auto *folderPicker = root->findChild<QObject *>(QStringLiteral("noteFolderPicker"));
    QVERIFY(contextOpen);
    QVERIFY(folderPicker);
    QVERIFY(!contextOpen->property("enabled").toBool());
    QVERIFY(folderPicker->property("selectionMixed").toBool());

    const auto invokeContextAction = [page](const char *method, const QVariant &argument = {}) {
        QVariant   result;
        const bool invoked = argument.isValid()
            ? QMetaObject::invokeMethod(page, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, argument))
            : QMetaObject::invokeMethod(page, method, Q_RETURN_ARG(QVariant, result));
        return invoked && result.toBool();
    };

    QVERIFY(invokeContextAction("copyContextNotesToStorage", QStringLiteral("storage-c")));
    QCOMPARE(workspace->property("copiedNotes").toInt(), 3);

    QVERIFY(invokeContextAction("moveContextNotesToStorage", QStringLiteral("storage-c")));
    QCOMPARE(workspace->property("movedNotes").toInt(), 3);
    workspace->setProperty("movedNotes", 0);

    QVERIFY(invokeContextAction("assignContextNotesFolder", QStringLiteral("folder-c")));
    QCOMPARE(workspace->property("assignedNotes").toInt(), 3);

    workspace->setProperty("recycleAll", false);
    QVERIFY(invokeContextAction("requestContextNotesDeletion"));
    QCOMPARE(workspace->property("trashedNotes").toInt(), 3);

    workspace->setProperty("recycleAll", true);
    QVERIFY(invokeContextAction("restoreContextNotes"));
    QCOMPARE(workspace->property("restoredNotes").toInt(), 3);

    page->setProperty("confirmDelete", false);
    QVERIFY(invokeContextAction("requestContextNotesDeletion"));
    QCOMPARE(workspace->property("deletedNotes").toInt(), 3);
    page->setProperty("confirmDelete", true);
    workspace->setProperty("recycleAll", false);

    const QPointF storageAPoint
        = storageA->mapToItem(qobject_cast<QQuickItem *>(root), QPointF(storageA->width() / 2, storageA->height() / 2));
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
    QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 1);
    QTRY_COMPARE(page->property("contextMenuNotes").toList().size(), 1);
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

    const QPointF noteA2ContextPoint
        = noteA2->mapToItem(qobject_cast<QQuickItem *>(root), QPointF(noteA2->width() / 2, noteA2->height() / 2));
    QTest::mouseClick(&quick, Qt::RightButton, Qt::NoModifier, noteA2ContextPoint.toPoint());
    QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 3);
    QTRY_COMPARE(page->property("contextMenuNotes").toList().size(), 3);
    QTRY_VERIFY(noteContextMenu->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(noteContextMenu, "close"));
    QTRY_VERIFY(!noteContextMenu->property("visible").toBool());

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
}

void NotesManagerQmlTest::notesManagerInternalDragsWork()
{
    ManagerInteractionFixture fixture;
    QVERIFY2(fixture.isReady(), qPrintable(fixture.errorString()));
    auto       &quick    = fixture.quick;
    auto       *root     = fixture.root;
    auto       *page     = fixture.page;
    auto       *preview  = fixture.preview;
    QQuickItem *storageA = nullptr;
    QQuickItem *noteA    = nullptr;
    QQuickItem *noteA2   = nullptr;
    QQuickItem *storageB = nullptr;
    QQuickItem *noteB    = nullptr;
    QTRY_VERIFY((storageA = fixture.delegate(0)));
    QTRY_VERIFY((noteA = fixture.delegate(1)));
    QTRY_VERIFY((noteA2 = fixture.delegate(2)));
    QTRY_VERIFY((storageB = fixture.delegate(3)));
    QTRY_VERIFY((noteB = fixture.delegate(4)));
    const QPointF noteAPoint  = fixture.center(noteA);
    const QPointF noteA2Point = fixture.center(noteA2);

    QTest::mouseClick(&quick, Qt::LeftButton, Qt::NoModifier, noteAPoint.toPoint());
    QTest::mouseClick(&quick, Qt::LeftButton, Qt::ControlModifier, noteA2Point.toPoint());
    QTRY_COMPARE(page->property("selectedNotes").toMap().size(), 2);
    QVERIFY2(fixture.drag(noteA, noteB, 2), "Dragging a Ctrl-selected note group failed");
    QTRY_COMPARE(root->property("movedNotes").toInt(), 2);
    QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-b"));

    const QPointF noteA2OriginBeforeEarlyDrag   = fixture.contentOrigin(noteA2);
    const QPointF storageBOriginBeforeEarlyDrag = fixture.contentOrigin(storageB);
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
    QTRY_VERIFY(storageA->property("dropAfterSpace").toReal() > 0);
    QCOMPARE(noteA2->property("dropSpace").toReal(), 0.0);
    const QPointF     noteA2OriginDuringEarlyDrag   = fixture.contentOrigin(noteA2);
    const QPointF     storageBOriginDuringEarlyDrag = fixture.contentOrigin(storageB);
    const QVariantMap rowExtents                    = page->property("groupedRowExtents").toMap();
    QVERIFY2(
        (noteA2OriginDuringEarlyDrag - noteA2OriginBeforeEarlyDrag).manhattanLength() < 1,
        qPrintable(QStringLiteral("second row moved on early drag: before=%1 during=%2 extents=%3")
                       .arg(noteA2OriginBeforeEarlyDrag.y())
                       .arg(noteA2OriginDuringEarlyDrag.y())
                       .arg(QString::fromUtf8(QJsonDocument::fromVariant(rowExtents).toJson(QJsonDocument::Compact)))));
    QVERIFY2((storageBOriginDuringEarlyDrag - storageBOriginBeforeEarlyDrag).manhattanLength() < 1,
             qPrintable(QStringLiteral("following row moved on early drag: before=%1 during=%2")
                            .arg(storageBOriginBeforeEarlyDrag.y())
                            .arg(storageBOriginDuringEarlyDrag.y())));
    const QPointF crossedHalfPoint = noteAPoint + QPointF(0, 20);
    QTest::mouseMove(&quick, crossedHalfPoint.toPoint(), 15);
    QTRY_VERIFY(noteA2->property("dropAfterSpace").toReal() > 0);
    QTest::qWait(220);
    QVERIFY(fixture.contentOrigin(noteA2).y() < noteA2OriginBeforeEarlyDrag.y() - 1);
    QVERIFY((fixture.contentOrigin(storageB) - storageBOriginBeforeEarlyDrag).manhattanLength() < 1);
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, crossedHalfPoint.toPoint());
    QTRY_COMPARE(preview->property("previewCount").toInt(), 0);
    QTRY_VERIFY(!page->property("dragSelectionSuppressed").toBool());
    QTRY_VERIFY((noteA = fixture.delegate(1)));
    QTRY_VERIFY((noteA2 = fixture.delegate(2)));
    QTRY_VERIFY((storageB = fixture.delegate(3)));
    QTRY_VERIFY((noteB = fixture.delegate(4)));
    QTRY_COMPARE(noteA->height(), noteA->property("baseHeight").toReal());

    // Exercise delegate replacement between gestures. This is what real
    // storage notifications do after every successful reorder.
    for (int iteration = 0; iteration < 6; ++iteration) {
        const QString firstId  = fixture.notesModel.index(1, 0).data(Qt::UserRole + 2).toString();
        const QString secondId = fixture.notesModel.index(2, 0).data(Qt::UserRole + 2).toString();
        QQuickItem   *first    = nullptr;
        QQuickItem   *second   = nullptr;
        QTRY_VERIFY((first = fixture.delegateForNote(firstId)) && first->property("row").toInt() == 1);
        QTRY_VERIFY((second = fixture.delegateForNote(secondId)) && second->property("row").toInt() == 2);
        QVERIFY2(fixture.drag(first, second, 1), "A repeated note drag did not keep its animated displacement");
        QTRY_COMPARE(root->property("movedNotes").toInt(), 1);
        QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-a"));
        QVERIFY(!root->property("lastDraggedNoteId").toString().isEmpty());
        QVERIFY(root->property("lastDraggedNoteId").toString() != root->property("noteAnchor").toString());
        QVERIFY(fixture.applyRecordedMove());
        QTRY_VERIFY(fixture.delegate(1));
        QTRY_VERIFY(fixture.delegate(2));
    }

    QTRY_VERIFY((noteA = fixture.delegateForNote(QStringLiteral("note-a"))));
    QTRY_VERIFY((noteA2 = fixture.delegateForNote(QStringLiteral("note-a2"))));
    QTRY_VERIFY((storageB = fixture.delegate(3)));
    QTRY_VERIFY((noteB = fixture.delegateForNote(QStringLiteral("note-b"))));

    QVERIFY(fixture.drag(noteA, noteB, 1));
    QTRY_COMPARE(root->property("movedNotes").toInt(), 1);
    QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-b"));
    QCOMPARE(root->property("noteAnchor").toString(), QStringLiteral("note-b"));
    QVERIFY(root->property("noteInsertAfter").toBool());

    QTRY_VERIFY((storageA = fixture.delegate(0)));
    QTRY_VERIFY((storageB = fixture.delegate(3)));
    QVERIFY(fixture.drag(storageA, storageB, 3));
    QTRY_COMPARE(root->property("movedStorages").toInt(), 1);
    QCOMPARE(root->property("storageDestinationRow").toInt(), 1);
}

void NotesManagerQmlTest::notesManagerVirtualizedDragsWork()
{
    ManagerInteractionFixture fixture;
    QVERIFY2(fixture.isReady(), qPrintable(fixture.errorString()));
    auto &quick   = fixture.quick;
    auto *root    = fixture.root;
    auto *page    = fixture.page;
    auto *tree    = fixture.tree;
    auto *preview = fixture.preview;

    // Keep target notes visible while their storage header is above the
    // viewport. Drop boundaries must still be attributed to storage A.
    auto storageBRow = fixture.notesModel.takeRow(3);
    auto noteBRow    = fixture.notesModel.takeRow(3);
    for (int index = 0; index < 14; ++index)
        fixture.appendItem(QStringLiteral("storage-a"), QStringLiteral("scroll-note-%1").arg(index), 1,
                           QStringLiteral("Scroll note %1").arg(index));
    fixture.notesModel.appendRow(storageBRow);
    fixture.notesModel.appendRow(noteBRow);
    QTRY_COMPARE(tree->property("rows").toInt(), 19);

    // Scrolling can reuse the delegate that initiated a drag for a
    // different row. The preview must remain a frozen image of the note,
    // and the newly represented row must not be hidden.
    tree->setProperty("contentY", 0);
    QQuickItem *recycledSource = nullptr;
    QTRY_VERIFY((recycledSource = fixture.delegateForNote(QStringLiteral("note-a"))));
    const QPointF recycledSourcePoint = recycledSource->mapToItem(
        qobject_cast<QQuickItem *>(root), QPointF(recycledSource->width() / 2, recycledSource->height() / 2));
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, recycledSourcePoint.toPoint());
    QTest::mouseMove(&quick, (recycledSourcePoint + QPointF(0, 12)).toPoint(), 15);
    QTRY_COMPARE(preview->property("previewCount").toInt(), 1);
    auto *frozenPreview = quickItemByName(preview, QStringLiteral("managerDragPreviewItem-0"));
    QTRY_VERIFY(frozenPreview);
    QVERIFY(!frozenPreview->property("live").toBool());
    QVERIFY(!frozenPreview->property("hideSource").toBool());
    QVERIFY(!frozenPreview->property("recursive").toBool());

    tree->setProperty("contentY", qMax(0.0, tree->property("contentHeight").toReal() - tree->height()));
    QTRY_VERIFY(fixture.delegateForNote(QStringLiteral("note-a")) == nullptr);
    QTRY_COMPARE(page->property("draggedItemType").toInt(), 1);
    QTRY_COMPARE(page->property("activeDraggedNoteId").toString(), QStringLiteral("note-a"));
    if (recycledSource->property("noteId").toString() != QStringLiteral("note-a"))
        QCOMPARE(recycledSource->opacity(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(page, "cancelGroupedDrag"));
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, recycledSourcePoint.toPoint());
    QTRY_COMPARE(preview->property("previewCount").toInt(), 0);

    tree->setProperty("contentY", qMax(0.0, tree->property("contentHeight").toReal() - tree->height()));
    QTRY_VERIFY(fixture.delegate(0) == nullptr);

    QQuickItem *nonConsecutiveFirst  = nullptr;
    QQuickItem *nonConsecutiveSecond = nullptr;
    QTRY_VERIFY((nonConsecutiveFirst = fixture.delegateForNote(QStringLiteral("scroll-note-10"))));
    QTRY_VERIFY((nonConsecutiveSecond = fixture.delegateForNote(QStringLiteral("scroll-note-12"))));
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
    const qreal previewTop = compactFirst->mapToItem(rootItem, QPointF()).y();
    const qreal expectedPreviewTop
        = nonConsecutiveFirst->mapToItem(rootItem, QPointF()).y() + page->property("dragTranslationY").toReal();
    QVERIFY2(qAbs(previewTop - expectedPreviewTop) < 1,
             qPrintable(QStringLiteral("Scrolled preview top %1, expected %2 (contentY %3)")
                            .arg(previewTop)
                            .arg(expectedPreviewTop)
                            .arg(tree->property("contentY").toReal())));
    QVERIFY(QMetaObject::invokeMethod(page, "cancelGroupedDrag"));
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, nonConsecutiveFirstPoint.toPoint());
    QTRY_COMPARE(preview->property("previewCount").toInt(), 0);

    QQuickItem *scrolledSource = nullptr;
    QQuickItem *scrolledTarget = nullptr;
    QTRY_VERIFY((scrolledSource = fixture.delegateForNote(QStringLiteral("note-b"))));
    QTRY_VERIFY((scrolledTarget = fixture.delegateForNote(QStringLiteral("scroll-note-12"))));
    QVERIFY2(fixture.drag(scrolledSource, scrolledTarget, 1),
             "Notes whose storage header is scrolled out must remain valid animated drop targets");
    QCOMPARE(root->property("lastDraggedNoteId").toString(), QStringLiteral("note-b"));
    QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-a"));

    QQuickItem *visibleStorageB = nullptr;
    QTRY_VERIFY((visibleStorageB = fixture.delegate(17)));
    const QPointF sourcePoint
        = scrolledTarget->mapToItem(rootItem, QPointF(scrolledTarget->width() / 2, scrolledTarget->height() / 2));
    const QPointF headerPoint = visibleStorageB->mapToItem(
        rootItem, QPointF(visibleStorageB->width() / 2, visibleStorageB->height() + scrolledTarget->height() / 2));
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, sourcePoint.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&quick, (sourcePoint + (headerPoint - sourcePoint) * (qreal(step) / 8)).toPoint(), 15);
    QObject *storageBTrailingTarget = nullptr;
    QTRY_VERIFY((storageBTrailingTarget = page->property("dropTargetDelegate").value<QObject *>())
                && storageBTrailingTarget->property("storageId").toString() == QStringLiteral("storage-b")
                && storageBTrailingTarget->property("dropAfter").toBool());
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, headerPoint.toPoint());
    QTRY_VERIFY(!page->property("dragSelectionSuppressed").toBool());
    QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-b"));
}
