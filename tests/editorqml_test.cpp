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

        editor.setMarkdown(true);
        QTRY_VERIFY(layer->property("formatEnabled").toBool());

        editor.setMarkdown(false);
        QTRY_VERIFY(!layer->property("formatEnabled").toBool());
    }

    void wholeListDragUsesItemLevelStructuralBoundaries()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("title\n\n- sdfsdf\n- 4354\n- fdsf\n\nAnother list\n\n1. 1111\n2. 2222"), true);
        QCOMPARE(model.rowCount(), 4);
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

        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
        moveMouseAlong(&quick, from, to, 6);

        auto *controller = root->findChild<QObject *>(QStringLiteral("editorReorderController"));
        QVERIFY(controller);
        QTRY_VERIFY(controller->property("dragging").toBool());
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
        QCOMPARE(model.contents(),
                 QStringLiteral("title\n\n- sdfsdf\n- 4354\n- fdsf\n\nAnother list\n\n1. 1111\n2. 2222"));

        auto *destinationFirst       = quickItemByName(root, QStringLiteral("listRow-3-0"));
        auto *destinationSecond      = quickItemByName(root, QStringLiteral("listRow-3-1"));
        auto *destinationFirstMarker = quickItemByName(root, QStringLiteral("listMarker-3-0"));
        auto *destinationMarker      = quickItemByName(root, QStringLiteral("listMarker-3-1"));
        QVERIFY(destinationFirst);
        QVERIFY(destinationSecond);
        QVERIFY(destinationFirstMarker);
        QVERIFY(destinationMarker);
        const qreal markerDistanceBefore = destinationMarker->mapToItem(root, QPointF()).y()
            - destinationFirstMarker->mapToItem(root, QPointF()).y();
        const QPointF attachFrom
            = sourceHandle->mapToItem(root, QPointF(sourceHandle->width() / 2, sourceHandle->height() / 2));
        auto *editorView = qobject_cast<QQuickItem *>(controller->property("editorView").value<QObject *>());
        QVERIFY(editorView);
        const qreal structuralExtent     = source->height() + editorView->property("spacing").toReal();
        const qreal pointerLeadingOffset = attachFrom.y() - source->mapToItem(root, QPointF()).y();
        const auto  pointerYForBoundary  = [structuralExtent, pointerLeadingOffset](qreal naturalBoundaryY) {
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

        QTest::mouseMove(&quick, QPointF(attachFrom.x(), pointerYForBoundary(destinationFirstBoundaryY)).toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 0);
        QTest::mouseMove(&quick, QPointF(attachFrom.x(), pointerYForBoundary(destinationEndBoundaryY)).toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 2);
        QTest::mouseMove(&quick, attachTo.toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 1);

        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, attachTo.toPoint());
        QTRY_VERIFY(!controller->property("dragging").toBool());
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(2), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("1111"), QStringLiteral("sdfsdf"), QStringLiteral("4354"),
                               QStringLiteral("fdsf"), QStringLiteral("2222") }));
    }
};

QTEST_MAIN(EditorQmlTest)

#include "editorqml_test.moc"
