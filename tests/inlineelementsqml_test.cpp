#include <QDate>
#include <QJSValue>
#include <QQuickItem>
#include <QQuickWidget>
#include <QtTest>

#include <algorithm>
#include <memory>

#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include "editortestsupport.h"

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

namespace {

QList<QQuickItem *> textEditors(QQuickItem *root)
{
    QList<QQuickItem *> result;
    if (!root)
        return result;
    if (root->objectName() == QLatin1String("noteBlockTextArea"))
        result.append(root);
    for (QQuickItem *child : root->childItems())
        result.append(textEditors(child));
    return result;
}

QQuickItem *textEditorForBlock(QQuickItem *root, int blockIndex)
{
    for (QQuickItem *editor : textEditors(root)) {
        if (editor->property("blockIndex").toInt() == blockIndex && editor->property("tableCellIndex").toInt() < 0
            && editor->property("listItemIndex").toInt() < 0) {
            return editor;
        }
    }
    return nullptr;
}

QList<QQuickItem *> tableCellEditors(QQuickItem *root, int blockIndex)
{
    QList<QQuickItem *> result;
    for (QQuickItem *editor : textEditors(root)) {
        if (editor->property("blockIndex").toInt() == blockIndex && editor->property("tableCellIndex").toInt() >= 0)
            result.append(editor);
    }
    std::sort(result.begin(), result.end(), [](QQuickItem *left, QQuickItem *right) {
        return left->property("tableCellIndex").toInt() < right->property("tableCellIndex").toInt();
    });
    return result;
}

QObject *inlineLayer(QQuickItem *editor)
{
    return editor ? editor->findChild<QObject *>(QStringLiteral("inlineElementLayer")) : nullptr;
}

QVariantList inlineElements(QObject *layer)
{
    if (!layer)
        return {};
    const QVariant value = layer->property("elements");
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        const QJSValue array = value.value<QJSValue>();
        QVariantList result;
        const int length = array.property(QStringLiteral("length")).toInt();
        result.reserve(length);
        for (int index = 0; index < length; ++index)
            result.append(array.property(index).toVariant());
        return result;
    }
    return value.toList();
}

QString currentPlainText(QQuickItem *editor)
{
    QVariant value;
    if (!editor || !QMetaObject::invokeMethod(editor, "currentPlainText", Q_RETURN_ARG(QVariant, value)))
        return {};
    return value.toString();
}

void refreshInlineLayer(QObject *layer)
{
    if (layer)
        QMetaObject::invokeMethod(layer, "refresh");
}

} // namespace

class InlineElementsQmlTest : public QObject {
    Q_OBJECT

private slots:
    void recognizesOnlyStandaloneSemanticDates()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("due 2030-12-31 invalid 2030-02-30 code `2030-01-01` "
                                    "link [2030-03-04](https://example.com) "
                                    "url https://example.com/2030-05-06"),
                     Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(700, 360);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QObject *layer = nullptr;
        QTRY_VERIFY((layer = inlineLayer(body)));
        refreshInlineLayer(layer);

        QTRY_COMPARE(int(inlineElements(layer).size()), 1);
        const QVariantMap element = inlineElements(layer).constFirst().toMap();
        QCOMPARE(element.value(QStringLiteral("type")).toString(), QStringLiteral("date"));
        QCOMPARE(element.value(QStringLiteral("dateText")).toString(), QStringLiteral("2030-12-31"));
    }

    void dateUrgencyUsesLocalCalendarDays()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("date 2030-12-31"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 320);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QObject *layer = nullptr;
        QTRY_VERIFY((layer = inlineLayer(body)));

        const QDate today = QDate::currentDate();
        const auto stateFor = [&](const QDate &date, QVariant *result) {
            return QMetaObject::invokeMethod(layer, "dateState", Q_RETURN_ARG(QVariant, *result),
                                             Q_ARG(QVariant, QVariant(date.toString(Qt::ISODate))));
        };

        QVariant state;
        QVERIFY(stateFor(today.addDays(-1), &state));
        QCOMPARE(state.toString(), QStringLiteral("overdue"));
        QVERIFY(stateFor(today, &state));
        QCOMPARE(state.toString(), QStringLiteral("soon"));
        QVERIFY(stateFor(today.addDays(7), &state));
        QCOMPARE(state.toString(), QStringLiteral("soon"));
        QVERIFY(stateFor(today.addDays(8), &state));
        QCOMPARE(state.toString(), QStringLiteral("future"));
    }

    void cursorNavigationTreatsDateAsAtomic()
    {
        const QString dateText = QStringLiteral("2030-12-31");
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("before %1 after").arg(dateText), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 320);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QObject *layer = nullptr;
        QTRY_VERIFY((layer = inlineLayer(body)));
        refreshInlineLayer(layer);
        QTRY_COMPARE(int(inlineElements(layer).size()), 1);

        const int start = currentPlainText(body).indexOf(dateText);
        QVERIFY(start >= 0);
        const int end = start + dateText.size();
        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(body->hasActiveFocus());

        QVariant handled;
        body->setProperty("cursorPosition", start);
        QVERIFY(QMetaObject::invokeMethod(layer, "moveAcrossDate", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, QVariant(1))));
        QVERIFY(handled.toBool());
        QCOMPARE(body->property("cursorPosition").toInt(), end);

        handled = {};
        QVERIFY(QMetaObject::invokeMethod(layer, "moveAcrossDate", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, QVariant(-1))));
        QVERIFY(handled.toBool());
        QCOMPARE(body->property("cursorPosition").toInt(), start);

        body->setProperty("cursorPosition", start + 4);
        handled = {};
        QVERIFY(QMetaObject::invokeMethod(layer, "moveAcrossDate", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, QVariant(1))));
        QVERIFY(handled.toBool());
        QCOMPARE(body->property("cursorPosition").toInt(), end);
    }

    void backspaceAndDeleteTreatDateAsAtomic()
    {
        const QString dateText = QStringLiteral("2030-12-31");
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("before %1 after").arg(dateText), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 320);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QObject *layer = nullptr;
        QTRY_VERIFY((layer = inlineLayer(body)));
        refreshInlineLayer(layer);
        QTRY_COMPARE(int(inlineElements(layer).size()), 1);

        int start = currentPlainText(body).indexOf(dateText);
        QVERIFY(start >= 0);
        body->forceActiveFocus(Qt::MouseFocusReason);
        body->setProperty("cursorPosition", start + dateText.size());
        QTRY_VERIFY(body->hasActiveFocus());

        QVariant handled;
        QVERIFY(QMetaObject::invokeMethod(layer, "deleteDateAtCursor", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY(handled.toBool());
        QTRY_VERIFY(!editor.text().contains(dateText));

        QVERIFY(editor.undo());
        QTRY_VERIFY(editor.text().contains(dateText));

        body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        layer = nullptr;
        QTRY_VERIFY((layer = inlineLayer(body)));
        refreshInlineLayer(layer);
        QTRY_COMPARE(int(inlineElements(layer).size()), 1);
        start = currentPlainText(body).indexOf(dateText);
        QVERIFY(start >= 0);
        body->forceActiveFocus(Qt::MouseFocusReason);
        body->setProperty("cursorPosition", start);
        QTRY_VERIFY(body->hasActiveFocus());

        handled = {};
        QVERIFY(QMetaObject::invokeMethod(layer, "deleteDateAtCursor", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, QVariant(false))));
        QVERIFY(handled.toBool());
        QTRY_VERIFY(!editor.text().contains(dateText));
    }

    void shortcutReplacementAddsSeparatorAndMovesCaret()
    {
        const QString dateText = QStringLiteral("2030-12-31");
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("before //"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 320);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QObject *layer = nullptr;
        QTRY_VERIFY((layer = inlineLayer(body)));

        const int shortcutStart = currentPlainText(body).indexOf(QStringLiteral("//"));
        QVERIFY(shortcutStart >= 0);
        QVariant dateValue;
        QVERIFY(QMetaObject::invokeMethod(layer, "dateFromIso", Q_RETURN_ARG(QVariant, dateValue),
                                          Q_ARG(QVariant, QVariant(dateText))));
        QVERIFY(dateValue.isValid());

        QVariant handled;
        QVERIFY(QMetaObject::invokeMethod(layer, "replaceRangeWithDate", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, QVariant(shortcutStart)),
                                          Q_ARG(QVariant, QVariant(shortcutStart + 2)),
                                          Q_ARG(QVariant, dateValue), Q_ARG(QVariant, QVariant(true))));
        QVERIFY(handled.toBool());
        QTRY_COMPARE(editor.text(), QStringLiteral("before 2030-12-31 "));

        body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QTRY_COMPARE(currentPlainText(body), QStringLiteral("before 2030-12-31 "));
        QCOMPARE(body->property("cursorPosition").toInt(), currentPlainText(body).size());

        layer = nullptr;
        QTRY_VERIFY((layer = inlineLayer(body)));
        refreshInlineLayer(layer);
        QTRY_COMPARE(int(inlineElements(layer).size()), 1);
        body->forceActiveFocus(Qt::MouseFocusReason);
        body->setProperty("cursorPosition", currentPlainText(body).size());
        QTRY_VERIFY(body->hasActiveFocus());

        handled = {};
        QVERIFY(QMetaObject::invokeMethod(layer, "deleteDateAtCursor", Q_RETURN_ARG(QVariant, handled),
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY(handled.toBool());
        QTRY_COMPARE(editor.text(), QStringLiteral("before "));
    }

    void tableCellsUseTheSameInlineDateLayer()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("| Date | Other |\n| --- | --- |\n| 2030-12-31 | value |"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 360);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::Table));
        QList<QQuickItem *> cells;
        QTRY_VERIFY(([&]() {
            cells = tableCellEditors(root, 1);
            return cells.size() == 4;
        })());

        QObject *layer = inlineLayer(cells.at(2));
        QVERIFY(layer);
        refreshInlineLayer(layer);
        QTRY_COMPARE(int(inlineElements(layer).size()), 1);
        QCOMPARE(inlineElements(layer).constFirst().toMap().value(QStringLiteral("dateText")).toString(),
                 QStringLiteral("2030-12-31"));
    }
};

QTEST_MAIN(InlineElementsQmlTest)
#include "inlineelementsqml_test.moc"
