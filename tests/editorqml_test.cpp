#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QtTest>

#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include "editortestsupport.h"
#include "quicktestsupport.h"

using namespace QtNote;
using namespace QtNote::TestSupport;

class EditorQmlTest : public QObject {
    Q_OBJECT

private slots:
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

    void interBlockInsertionIsMarkdownOnly()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        auto *layer = host.quickWidget()->rootObject()->findChild<QQuickItem *>(QStringLiteral("interBlockHitLayer"));
        QVERIFY(layer);
        QVERIFY(!layer->property("formatEnabled").toBool());
        auto *copyMarkdown
            = host.quickWidget()->rootObject()->findChild<QObject *>(QStringLiteral("copyMarkdownMenuItem"));
        QVERIFY(copyMarkdown);
        QVERIFY(!copyMarkdown->property("formatEnabled").toBool());

        editor.setMarkdown(true);
        QTRY_VERIFY(layer->property("formatEnabled").toBool());
        QTRY_VERIFY(copyMarkdown->property("formatEnabled").toBool());

        editor.setMarkdown(false);
        QTRY_VERIFY(!layer->property("formatEnabled").toBool());
        QTRY_VERIFY(!copyMarkdown->property("formatEnabled").toBool());
    }

    void tableBordersUseSinglePixelTextColorGrid()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("| First | Second |\n| --- | --- |\n| value | value |"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::Table));
        host.resize(520, 420);
        host.show();

        QList<QQuickItem *> tableCells;
        QTRY_VERIFY(([&]() {
            tableCells.clear();
            QList<QQuickItem *> pending { qobject_cast<QQuickItem *>(host.quickWidget()->rootObject()) };
            while (!pending.isEmpty()) {
                QQuickItem *candidate = pending.takeLast();
                if (!candidate)
                    continue;
                if (candidate->property("tableCell").toBool())
                    tableCells.append(candidate);
                pending.append(candidate->childItems());
            }
            return tableCells.size() == 4;
        })());

        QQuickItem    *tableCell = tableCells.constFirst();
        const QColor   border    = tableCell->property("gridBorderColor").value<QColor>();
        const QPalette palette   = tableCell->property("palette").value<QPalette>();
        const QColor   text      = palette.color(QPalette::Text);
        QVERIFY(border.isValid());
        QVERIFY(text.isValid());
        QVERIFY(qAbs(border.redF() - text.redF()) < 0.01);
        QVERIFY(qAbs(border.greenF() - text.greenF()) < 0.01);
        QVERIFY(qAbs(border.blueF() - text.blueF()) < 0.01);
        QVERIFY(qAbs(border.alphaF() - 0.28) < 0.01);

        int rightBorderOwners  = 0;
        int bottomBorderOwners = 0;
        for (QQuickItem *cell : std::as_const(tableCells)) {
            const int  column      = cell->property("columnIndex").toInt();
            const int  row         = cell->property("tableRow").toInt();
            const bool drawsRight  = cell->property("drawsRightGridBorder").toBool();
            const bool drawsBottom = cell->property("drawsBottomGridBorder").toBool();
            QCOMPARE(drawsRight, column == 1);
            QCOMPARE(drawsBottom, row == 1);
            rightBorderOwners += drawsRight;
            bottomBorderOwners += drawsBottom;
        }
        QCOMPARE(rightBorderOwners, 2);
        QCOMPARE(bottomBorderOwners, 2);
    }

    void wholeListDragUsesItemLevelStructuralBoundaries()
    {
        const QString  document = QStringLiteral("title\n\n"
                                                  "- sdfsdf\n"
                                                  "- 4354\n"
                                                  "- fdsf\n\n"
                                                  "Another list\n\n"
                                                  "1. 1111\n"
                                                  "2. 2222\n\n"
                                                  "### Later\n\n"
                                                  "- [ ] review backlog\n"
                                                  "- [ ] update documentation\n\n"
                                                  "### Long term\n\n"
                                                  "- recurring review\n"
                                                  "- metrics review\n\n"
                                                  "Reference work\n\n"
                                                  "- add adapter\n"
                                                  "- stream logs");
        NoteBlockModel model;
        model.load(document, true);
        QCOMPARE(model.rowCount(), 10);
        QCOMPARE(model.blockTypeAt(1), int(NoteBlockModel::BulletList));
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::Text));
        QCOMPARE(model.blockTypeAt(3), int(NoteBlockModel::NumberedList));

        QQuickWidget quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(520, 420);
        quick.rootContext()->setContextProperty(QStringLiteral("noteBlockModel"), &model);
        QQmlComponent component(quick.engine());
        component.setData(R"QML(
            import QtQuick
            import "qrc:/qml/editor" as Editor

            Item {
                QtObject {
                    id: backend
                    property bool markdown: true
                    property string undoText: ""
                    property string redoText: ""
                    property bool canUndo: false
                    property bool canRedo: false
                    function beginHistoryTransaction(kind, state) {}
                    function endHistoryTransaction(state) {}
                }

                Editor.NoteBlockEditorImpl {
                    anchors.fill: parent
                    blockModel: noteBlockModel
                    editorBackend: backend
                }
            }
        )QML",
                          QUrl(QStringLiteral("qrc:/qml/WholeListDragHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *harness = component.create();
        QVERIFY2(harness, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/WholeListDragHarness.qml")), &component, harness);
        quick.show();

        auto *root = qobject_cast<QQuickItem *>(quick.rootObject());
        QVERIFY(root);

        QQuickItem *sourceHandle    = nullptr;
        QQuickItem *followingHandle = nullptr;
        QQuickItem *lastHandle      = nullptr;
        QTRY_VERIFY((sourceHandle = quickItemByName(root, QStringLiteral("listLevelReorderHandle-1-0-0"))));
        QTRY_VERIFY((followingHandle = quickItemByName(root, QStringLiteral("blockReorderHandle-2"))));
        QTRY_VERIFY((lastHandle = quickItemByName(root, QStringLiteral("listLevelReorderHandle-3-0-0"))));

        auto *source    = ancestorWithProperty(sourceHandle, "reorderSourceActive");
        auto *following = ancestorWithProperty(followingHandle, "reorderSourceActive");
        auto *last      = ancestorWithProperty(lastHandle, "reorderSourceActive");
        QVERIFY(source);
        QVERIFY(following);
        QVERIFY(last);

        const qreal   sourceHeightBefore = source->height();
        const qreal   followingYBefore   = following->mapToItem(root, QPointF()).y();
        const qreal   lastYBefore        = last->mapToItem(root, QPointF()).y();
        const QPointF from
            = sourceHandle->mapToItem(root, QPointF(sourceHandle->width() / 2, sourceHandle->height() / 2));
        const QPointF to = from + QPointF(80, 0);

        auto *controller = root->findChild<QObject *>(QStringLiteral("editorReorderController"));
        QVERIFY(controller);
        auto *interBlockLayer = root->findChild<QQuickItem *>(QStringLiteral("interBlockHitLayer"));
        QVERIFY(interBlockLayer);
        QVERIFY(interBlockLayer->isVisible());

        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
        moveMouseAlong(&quick, from, to, 6);

        QTRY_VERIFY(controller->property("dragging").toBool());
        QTRY_VERIFY(!interBlockLayer->isVisible());
        QVERIFY(controller->property("wholeListBlockDrag").toBool());
        QTest::qWait(220);

        QVERIFY2(qAbs(source->height() - sourceHeightBefore) < 1,
                 "Whole-list drag must not collapse both the block and its internal rows");
        QVERIFY2(qAbs(following->mapToItem(root, QPointF()).y() - followingYBefore) < 1,
                 "The paragraph below a horizontal list drag moved vertically");
        QVERIFY2(qAbs(last->mapToItem(root, QPointF()).y() - lastYBefore) < 1,
                 "The following list moved vertically during a horizontal list drag");

        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
        QTRY_VERIFY(!controller->property("dragging").toBool());
        QTRY_VERIFY(interBlockLayer->isVisible());
        QCOMPARE(model.contents(), document);

        auto *destinationFirst       = quickItemByName(root, QStringLiteral("listRow-3-0"));
        auto *destinationSecond      = quickItemByName(root, QStringLiteral("listRow-3-1"));
        auto *destinationFirstMarker = quickItemByName(root, QStringLiteral("listMarker-3-0"));
        auto *destinationMarker      = quickItemByName(root, QStringLiteral("listMarker-3-1"));
        QVERIFY(destinationFirst);
        QVERIFY(destinationSecond);
        QVERIFY(destinationFirstMarker);
        QVERIFY(destinationMarker);
        const qreal firstMarkerYBefore   = destinationFirstMarker->mapToItem(root, QPointF()).y();
        const qreal markerDistanceBefore = destinationMarker->mapToItem(root, QPointF()).y()
            - destinationFirstMarker->mapToItem(root, QPointF()).y();
        const QPointF attachFrom
            = sourceHandle->mapToItem(root, QPointF(sourceHandle->width() / 2, sourceHandle->height() / 2));
        auto *editorView = qobject_cast<QQuickItem *>(controller->property("editorView").value<QObject *>());
        QVERIFY(editorView);
        const qreal contentHeightBeforeAttachment = editorView->property("contentHeight").toReal();
        const qreal structuralExtent              = source->height() + editorView->property("spacing").toReal();
        const qreal pointerLeadingOffset          = attachFrom.y() - source->mapToItem(root, QPointF()).y();
        const auto  pointerYForBoundary           = [structuralExtent, pointerLeadingOffset](qreal naturalBoundaryY) {
            return naturalBoundaryY - structuralExtent + pointerLeadingOffset;
        };
        const qreal destinationFirstBoundaryY = destinationFirst->mapToItem(root, QPointF()).y();
        const qreal destinationBoundaryY      = destinationSecond->mapToItem(root, QPointF()).y();
        const qreal destinationEndBoundaryY
            = destinationSecond->mapToItem(root, QPointF(0, destinationSecond->property("naturalHeight").toReal())).y();
        const QPointF attachTo(attachFrom.x(), pointerYForBoundary(destinationBoundaryY));
        const qreal   paragraphBoundaryY = following->mapToItem(root, QPointF()).y();
        const qreal   paragraphSwitchY   = pointerYForBoundary((paragraphBoundaryY + destinationFirstBoundaryY) / 2);

        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, attachFrom.toPoint());
        const QPointF beforeParagraphSwitch(attachFrom.x(), paragraphSwitchY - 2);
        moveMouseAlong(&quick, attachFrom, beforeParagraphSwitch, 8, 15, 50);
        QVERIFY2(qAbs(following->property("reorderOffset").toReal()) < 1,
                 "The paragraph moved before the dragged list crossed its midpoint");

        const QPointF afterParagraphSwitch(attachFrom.x(), paragraphSwitchY + 2);
        QTest::mouseMove(&quick, afterParagraphSwitch.toPoint(), 15);
        QCOMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QCOMPARE(controller->property("targetItem").toInt(), 0);
        QVERIFY(controller->property("blockAnimationActive").toBool());
        QTest::qWait(30);
        const qreal paragraphOffsetDuringAnimation = following->property("reorderOffset").toReal();
        QVERIFY2(paragraphOffsetDuringAnimation < -1 && paragraphOffsetDuringAnimation > -structuralExtent + 1,
                 qPrintable(QStringLiteral("The paragraph did not animate after crossing: offset=%1 extent=%2")
                                .arg(paragraphOffsetDuringAnimation)
                                .arg(structuralExtent)));

        QTest::qWait(220);
        QVERIFY2(qAbs(destinationFirstMarker->mapToItem(root, QPointF()).y() - firstMarkerYBefore) < 1,
                 "The destination list moved after the preceding paragraph crossed upward");
        QTest::mouseMove(&quick, beforeParagraphSwitch.toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("block"));
        QTest::qWait(30);
        const qreal paragraphOffsetDuringReverse = following->property("reorderOffset").toReal();
        QVERIFY2(paragraphOffsetDuringReverse < -1 && paragraphOffsetDuringReverse > -structuralExtent + 1,
                 qPrintable(QStringLiteral("The paragraph did not animate backward: offset=%1 extent=%2")
                                .arg(paragraphOffsetDuringReverse)
                                .arg(structuralExtent)));
        QVERIFY2(qAbs(destinationFirstMarker->mapToItem(root, QPointF()).y() - firstMarkerYBefore) < 1,
                 "The destination list moved with the preceding paragraph during the reverse animation");
        QTest::qWait(220);
        QVERIFY2(qAbs(destinationFirstMarker->mapToItem(root, QPointF()).y() - firstMarkerYBefore) < 1,
                 "The destination list did not remain stationary after the reverse animation");
        QTest::mouseMove(&quick, afterParagraphSwitch.toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 0);

        bool      targetAnimatedAsWholeBlock = false;
        bool      targetRetreated            = false;
        bool      firstMarkerReversed        = false;
        int       previousTargetItem         = 0;
        qreal     previousFirstMarkerY       = destinationFirstMarker->mapToItem(root, QPointF()).y();
        const int slowSteps                  = qMax(1, int(qAbs(attachTo.y() - afterParagraphSwitch.y())));
        for (int step = 1; step <= slowSteps; ++step) {
            QTest::mouseMove(
                &quick,
                (afterParagraphSwitch + (attachTo - afterParagraphSwitch) * (qreal(step) / slowSteps)).toPoint(), 15);
            QTest::qWait(40);
            if (controller->property("targetKind").toString() == QStringLiteral("list")) {
                const int currentTargetItem = controller->property("targetItem").toInt();
                if (currentTargetItem < previousTargetItem)
                    targetRetreated = true;
                previousTargetItem = currentTargetItem;
            } else if (qAbs(last->property("reorderOffset").toReal()) > 1) {
                targetAnimatedAsWholeBlock = true;
            }
            const qreal currentFirstMarkerY = destinationFirstMarker->mapToItem(root, QPointF()).y();
            if (currentFirstMarkerY > previousFirstMarkerY + 0.5)
                firstMarkerReversed = true;
            previousFirstMarkerY = currentFirstMarkerY;
        }

        QTRY_VERIFY(controller->property("dragging").toBool());
        QVERIFY2(!targetAnimatedAsWholeBlock, "The destination list animated as a whole before item-level attachment");
        QVERIFY2(!targetRetreated, "The item-level target moved backwards during a downward drag");
        QVERIFY2(!firstMarkerReversed, "The first destination item reversed direction during a slow downward drag");
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 1);
        QTRY_VERIFY(destinationSecond->property("dropSpace").toReal() > 1);
        QTRY_VERIFY(destinationMarker->mapToItem(root, QPointF()).y()
                        - destinationFirstMarker->mapToItem(root, QPointF()).y()
                    > markerDistanceBefore + 1);
        QTest::qWait(220);
        QVERIFY2(qAbs(editorView->property("contentHeight").toReal() - contentHeightBeforeAttachment) < 1,
                 "Attaching a whole list must not duplicate its extent in the document layout");

        const qreal firstMarkerBeforeReverse = destinationFirstMarker->mapToItem(root, QPointF()).y();
        QTest::mouseMove(&quick, QPointF(attachFrom.x(), pointerYForBoundary(destinationFirstBoundaryY)).toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 0);
        QTest::qWait(30);
        const qreal firstMarkerDuringReverse = destinationFirstMarker->mapToItem(root, QPointF()).y();
        QVERIFY2(firstMarkerDuringReverse > firstMarkerBeforeReverse + 1
                     && firstMarkerDuringReverse < firstMarkerYBefore - 1,
                 qPrintable(QStringLiteral("The first destination item did not animate backward: before=%1 during=%2 "
                                           "destination=%3")
                                .arg(firstMarkerBeforeReverse)
                                .arg(firstMarkerDuringReverse)
                                .arg(firstMarkerYBefore)));
        QTRY_VERIFY(qAbs(destinationFirstMarker->mapToItem(root, QPointF()).y() - firstMarkerYBefore) < 1);
        QTest::mouseMove(&quick, QPointF(attachFrom.x(), pointerYForBoundary(destinationEndBoundaryY)).toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 2);
        QTest::mouseMove(&quick, attachTo.toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 1);

        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, attachTo.toPoint());
        QTRY_VERIFY(!controller->property("dragging").toBool());
        QCOMPARE(model.rowCount(), 9);
        QCOMPARE(model.data(model.index(2), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("1111"), QStringLiteral("sdfsdf"), QStringLiteral("4354"),
                               QStringLiteral("fdsf"), QStringLiteral("2222") }));
    }
};

QTEST_MAIN(EditorQmlTest)

#include "editorqml_test.moc"
