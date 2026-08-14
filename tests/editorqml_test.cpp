#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFont>
#include <QMimeData>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QtTest>

#include <algorithm>

#include "desktopeditorplatformbackend.h"
#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include "editortestsupport.h"
#include "quicktestsupport.h"

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
    for (QQuickItem *editor : textEditors(root))
        if (editor->property("blockIndex").toInt() == blockIndex && editor->property("tableCellIndex").toInt() < 0
            && editor->property("listItemIndex").toInt() < 0) {
            return editor;
        }
    return nullptr;
}

QList<QQuickItem *> tableCellEditors(QQuickItem *root, int blockIndex)
{
    QList<QQuickItem *> result;
    for (QQuickItem *editor : textEditors(root))
        if (editor->property("blockIndex").toInt() == blockIndex && editor->property("tableCellIndex").toInt() >= 0) {
            result.append(editor);
        }
    std::sort(result.begin(), result.end(), [](QQuickItem *left, QQuickItem *right) {
        return left->property("tableCellIndex").toInt() < right->property("tableCellIndex").toInt();
    });
    return result;
}

} // namespace

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

    void editorFontPreviewUpdatesTextAndHeadings()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("# heading\n\nbody"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 360);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *title   = nullptr;
        QQuickItem *heading = nullptr;
        QTRY_VERIFY(([&]() {
            title   = textEditorForBlock(root, 0);
            heading = textEditorForBlock(root, 1);
            return title && heading;
        })());
        const QVariant documentProperty = QQmlProperty(title, QStringLiteral("textDocument")).read();
        auto          *quickDocument    = qobject_cast<QQuickTextDocument *>(documentProperty.value<QObject *>());
        QVERIFY(quickDocument);
        QTextCharFormat titleFormat;
        const auto      readTitleFormat = [&]() {
            const QTextBlock block = quickDocument->textDocument()->begin();
            if (!block.isValid() || !block.layout())
                return false;
            for (const auto &range : block.layout()->formats()) {
                if (range.format.foreground().style() == Qt::NoBrush)
                    continue;
                titleFormat = range.format;
                return true;
            }
            return false;
        };

        QFont preview = QGuiApplication::font();
        preview.setPointSizeF(12.0);
        host.platformBackend()->setEditorFont(preview);
        QTRY_VERIFY(qAbs(title->property("font").value<QFont>().pointSizeF() - 12.0) < 0.01);
        QTRY_VERIFY(qAbs(heading->property("font").value<QFont>().pointSizeF() - 20.4) < 0.01);
        QTRY_VERIFY(readTitleFormat() && qAbs(titleFormat.fontPointSize() - 18.0) < 0.01);

        preview.setPointSizeF(18.0);
        host.platformBackend()->setEditorFont(preview);
        QTRY_VERIFY(qAbs(title->property("font").value<QFont>().pointSizeF() - 18.0) < 0.01);
        QTRY_VERIFY(qAbs(heading->property("font").value<QFont>().pointSizeF() - 30.6) < 0.01);
        QTRY_VERIFY(readTitleFormat() && qAbs(titleFormat.fontPointSize() - 27.0) < 0.01);

        const QColor previewTitleColor(35, 145, 235);
        host.platformBackend()->setTitleHighlightColor(previewTitleColor);
        QTRY_VERIFY(readTitleFormat() && titleFormat.foreground().color() == previewTitleColor);
    }

    void removingHeadingKeepsCursorInsideCoalescedText()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("before **bold**\n\n## heading\n\nafter"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 360);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *heading = nullptr;
        QTRY_VERIFY((heading = textEditorForBlock(root, 2)));
        heading->forceActiveFocus();
        heading->setProperty("cursorPosition", 3);
        QTRY_VERIFY(heading->hasActiveFocus());
        auto *blockEditor = ancestorWithProperty(heading, "currentFindText");
        QVERIFY(blockEditor);

        QVariant converted;
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "convertActiveToHeading", Q_RETURN_ARG(QVariant, converted),
                                          Q_ARG(QVariant, 0)));
        QVERIFY(converted.toBool());
        QTRY_COMPARE(editor.model()->rowCount(), 2);

        QQuickItem *text = nullptr;
        QTRY_VERIFY((text = textEditorForBlock(root, 1)));
        QVariant plain;
        QVERIFY(QMetaObject::invokeMethod(text, "currentPlainText", Q_RETURN_ARG(QVariant, plain)));
        const int expectedPosition = plain.toString().indexOf(QStringLiteral("heading")) + 3;
        QVERIFY(expectedPosition >= 3);
        QTRY_COMPARE(text->property("cursorPosition").toInt(), expectedPosition);
    }

    void applyingHeadingAndQuoteKeepsCursorInsideSelectedParagraph()
    {
        const auto verifyConversion = [](bool quote) {
            Note note(new NoteData(nullptr));
            note.setTitle(QStringLiteral("title"));
            note.setText(QStringLiteral("before **bold**\n\ntarget text\n\nafter"), Note::Markdown);
            DraftManager          drafts(std::make_unique<MemoryDraftStore>());
            NoteEditor            editor(note, drafts);
            DesktopNoteEditorHost host(&editor);
            host.resize(520, 360);
            host.show();

            auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
            QVERIFY(root);
            QQuickItem *body = nullptr;
            QTRY_VERIFY((body = textEditorForBlock(root, 1)));
            QVariant plain;
            QVERIFY(QMetaObject::invokeMethod(body, "currentPlainText", Q_RETURN_ARG(QVariant, plain)));
            const int targetPosition = plain.toString().indexOf(QStringLiteral("target")) + 3;
            QVERIFY(targetPosition >= 3);
            body->forceActiveFocus();
            body->setProperty("cursorPosition", targetPosition);
            QTRY_VERIFY(body->hasActiveFocus());
            auto *blockEditor = ancestorWithProperty(body, "currentFindText");
            QVERIFY(blockEditor);

            QVariant converted;
            if (quote) {
                QVERIFY(QMetaObject::invokeMethod(blockEditor, "convertActiveToQuote",
                                                  Q_RETURN_ARG(QVariant, converted), Q_ARG(QVariant, true)));
            } else {
                QVERIFY(QMetaObject::invokeMethod(blockEditor, "convertActiveToHeading",
                                                  Q_RETURN_ARG(QVariant, converted), Q_ARG(QVariant, 2)));
            }
            QVERIFY(converted.toBool());
            QTRY_COMPARE(editor.model()->rowCount(), 4);
            QCOMPARE(editor.model()->blockTypeAt(2), int(quote ? NoteBlockModel::BlockQuote : NoteBlockModel::Heading));

            QQuickItem *structured = nullptr;
            QTRY_VERIFY((structured = textEditorForBlock(root, 2)));
            QTRY_VERIFY(structured->hasActiveFocus());
            QTRY_COMPARE(structured->property("cursorPosition").toInt(), 3);
        };

        verifyConversion(false);
        verifyConversion(true);
    }

    void blockQuoteConvertsEveryParagraphInTopDownSelection()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("before\n\nfirst selected\n\nsecond selected\n\nthird selected\n\nafter"),
                     Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        host.resize(520, 360);
        host.show();

        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QVariant plain;
        QVERIFY(QMetaObject::invokeMethod(body, "currentPlainText", Q_RETURN_ARG(QVariant, plain)));
        const int selectionStart = plain.toString().indexOf(QStringLiteral("first selected"));
        const int selectionEnd
            = plain.toString().indexOf(QStringLiteral("third selected")) + QStringLiteral("third selected").size();
        QVERIFY(selectionStart >= 0);
        QVERIFY(selectionEnd > selectionStart);
        body->forceActiveFocus();
        QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, selectionStart), Q_ARG(int, selectionEnd)));
        QTRY_COMPARE(body->property("cursorPosition").toInt(), selectionEnd);
        auto *blockEditor = ancestorWithProperty(body, "currentFindText");
        QVERIFY(blockEditor);

        auto *toolbarButton = root->findChild<QQuickItem *>(QStringLiteral("editorFolderPickerButton"));
        QVERIFY(toolbarButton);
        toolbarButton->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(!body->hasActiveFocus());

        QVariant converted;
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "insertBlockQuoteBlock", Q_RETURN_ARG(QVariant, converted)));
        QVERIFY(converted.toBool());
        QTRY_COMPARE(editor.model()->rowCount(), 4);
        QCOMPARE(editor.model()->blockTypeAt(2), int(NoteBlockModel::BlockQuote));
        QCOMPARE(editor.model()->data(editor.model()->index(2), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("first selected\n\nsecond selected\n\nthird selected"));

        QQuickItem *quote = nullptr;
        QTRY_VERIFY((quote = textEditorForBlock(root, 2)));
        QTRY_VERIFY(quote->hasActiveFocus());
        QTRY_VERIFY(quote->property("selectedText").toString().contains(QStringLiteral("first selected")));
        QTRY_VERIFY(quote->property("selectedText").toString().contains(QStringLiteral("second selected")));
        QTRY_VERIFY(quote->property("selectedText").toString().contains(QStringLiteral("third selected")));
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

    void tableColumnsUseCachedIntrinsicWidths()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        // Font weight is not required to change glyph advances (Segoe UI on
        // Windows can report identical widths for regular and bold text). Use
        // different text lengths so this cache/distribution test is independent
        // of the platform font while still exercising rich text in the cell.
        note.setText(QStringLiteral("| A | B |\n| --- | --- |\n| iiiiiiii | **iiiiiiiiiiiiiiiiiiiiiiii** |"),
                     Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::Table));
        host.resize(620, 420);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);

        QList<QQuickItem *> cells;
        QTRY_VERIFY(([&]() {
            cells = tableCellEditors(root, 1);
            return cells.size() == 4;
        })());
        QTRY_VERIFY(cells.at(2)->property("comfortableWidth").toReal() > 0);
        QTRY_VERIFY(cells.at(3)->property("comfortableWidth").toReal() > 0);

        const qreal narrowWidth = cells.at(2)->property("comfortableWidth").toReal();
        const qreal wideWidth   = cells.at(3)->property("comfortableWidth").toReal();
        QVERIFY2(wideWidth > narrowWidth,
                 qPrintable(QStringLiteral("wide=%1 narrow=%2").arg(wideWidth).arg(narrowWidth)));
        QTRY_VERIFY(cells.at(1)->width() > cells.at(0)->width());
        QVERIFY(qAbs(cells.at(0)->width() - cells.at(2)->width()) < 0.5);
        QVERIFY(qAbs(cells.at(1)->width() - cells.at(3)->width()) < 0.5);

        editor.model()->setTableCell(1, 2, QStringLiteral("iiii\niiii"));
        QTRY_VERIFY(cells.at(2)->property("comfortableWidth").toReal() < narrowWidth - 0.25);

        const QList<qreal> intrinsicBeforeResize { cells.at(0)->property("comfortableWidth").toReal(),
                                                   cells.at(1)->property("comfortableWidth").toReal(),
                                                   cells.at(2)->property("comfortableWidth").toReal(), wideWidth };
        const qreal        totalWidthBeforeResize = cells.at(0)->width() + cells.at(1)->width();
        host.resize(820, 420);
        QTRY_VERIFY(cells.at(0)->width() + cells.at(1)->width() > totalWidthBeforeResize);
        for (int index = 0; index < cells.size(); ++index)
            QCOMPARE(cells.at(index)->property("comfortableWidth").toReal(), intrinsicBeforeResize.at(index));

        cells.at(2)->forceActiveFocus(Qt::MouseFocusReason);
        cells.at(2)->setProperty("text", QString(160, QLatin1Char('i')));
        QTRY_VERIFY(cells.at(2)->property("comfortableWidth").toReal() > wideWidth * 2);
        QTRY_VERIFY(cells.at(0)->width() > cells.at(1)->width());

        editor.model()->insertTableRow(1, 2);
        QTRY_VERIFY(([&]() {
            cells = tableCellEditors(root, 1);
            return cells.size() == 6;
        })());
        for (QQuickItem *cell : std::as_const(cells))
            QTRY_VERIFY(cell->property("comfortableWidth").toReal() > 0);
    }

    void codeBlockBorderUsesTableGridColor()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("title"));
        note.setText(QStringLiteral("```cpp\nint main() {}\n```"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::CodeBlock));
        host.resize(520, 420);
        host.show();

        QQuickItem *codeBlock = nullptr;
        QTRY_VERIFY(([&]() {
            QList<QQuickItem *> pending { qobject_cast<QQuickItem *>(host.quickWidget()->rootObject()) };
            while (!pending.isEmpty()) {
                QQuickItem *candidate = pending.takeLast();
                if (!candidate)
                    continue;
                if (candidate->objectName() == QLatin1String("codeBlock")) {
                    codeBlock = candidate;
                    return true;
                }
                pending.append(candidate->childItems());
            }
            return false;
        })());
        const QColor border = codeBlock->property("codeBorderColor").value<QColor>();
        const QColor text   = codeBlock->property("codeTextColor").value<QColor>();
        QVERIFY(border.isValid());
        QVERIFY(text.isValid());
        QVERIFY(qAbs(border.redF() - text.redF()) < 0.01);
        QVERIFY(qAbs(border.greenF() - text.greenF()) < 0.01);
        QVERIFY(qAbs(border.blueF() - text.blueF()) < 0.01);
        QVERIFY(qAbs(border.alphaF() - 0.28) < 0.01);
    }

private:
    void pendingSourceSyncPreservesToolbarSelection()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("before selected after"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(560, 420);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        auto *blockEditor = ancestorWithProperty(body, "currentFindText");
        QVERIFY(blockEditor);

        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(body->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, 7), Q_ARG(int, 15)));
        QCOMPARE(body->property("selectedText").toString(), QStringLiteral("selected"));

        // Reapplying a pending model value is what used to reset the cursor
        // when a toolbar button or another window took focus.
        body->setProperty("sourceTextPending", true);
        QVariant synchronized;
        QVERIFY(QMetaObject::invokeMethod(body, "applyPendingSourceText", Q_RETURN_ARG(QVariant, synchronized)));
        QVERIFY(synchronized.toBool());
        QCOMPARE(body->property("selectionStart").toInt(), 7);
        QCOMPARE(body->property("selectionEnd").toInt(), 15);

        QVariant opened;
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "editActiveLink", Q_RETURN_ARG(QVariant, opened)));
        QVERIFY(opened.toBool());
        QObject *linkPopup = blockEditor->property("linkEditorPopup").value<QObject *>();
        QVERIFY(linkPopup);
        QCOMPARE(linkPopup->property("selectionStart").toInt(), 7);
        QCOMPARE(linkPopup->property("selectionEnd").toInt(), 15);
        QVERIFY(QMetaObject::invokeMethod(linkPopup, "close"));

        QTRY_VERIFY(body->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, 7), Q_ARG(int, 15)));
        body->setProperty("sourceTextPending", true);
        QVERIFY(QMetaObject::invokeMethod(body, "applyPendingSourceText", Q_RETURN_ARG(QVariant, synchronized)));
        QVERIFY(synchronized.toBool());

        // A real toolbar click takes active focus before its onClicked handler
        // runs. The editor reference and native selection must survive that
        // focus transfer and be restored by the formatting command.
        auto *toolbarButton = root->findChild<QQuickItem *>(QStringLiteral("editorFolderPickerButton"));
        QVERIFY(toolbarButton);
        toolbarButton->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(!body->hasActiveFocus());

        QVariant formatted;
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "applyActiveInlineStyle", Q_RETURN_ARG(QVariant, formatted),
                                          Q_ARG(QVariant, QStringLiteral("code"))));
        QVERIFY(formatted.toBool());
        QTRY_VERIFY(editor.model()->contents().contains(QStringLiteral("`selected`")));
        QTRY_VERIFY(body->hasActiveFocus());
        QTRY_COMPARE(body->property("selectionStart").toInt(), 7);
        QTRY_COMPARE(body->property("selectionEnd").toInt(), 15);
    }

    void codeActionConvertsMultilineTextSelectionWithoutBlankParagraphs()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("keep before selected first\n\nselected second keep after"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);
        const QString         originalText = editor.text();

        host.resize(620, 440);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        auto *blockEditor = ancestorWithProperty(body, "currentFindText");
        QVERIFY(blockEditor);

        QVariant renderedValue;
        QVERIFY(QMetaObject::invokeMethod(body, "currentPlainText", Q_RETURN_ARG(QVariant, renderedValue)));
        const QString rendered   = renderedValue.toString();
        const int     firstStart = rendered.indexOf(QStringLiteral("selected first"));
        const int     lastEnd
            = rendered.indexOf(QStringLiteral("selected second")) + QStringLiteral("selected second").size();
        QVERIFY(firstStart >= 0);
        QVERIFY(lastEnd > firstStart);
        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(body->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, firstStart), Q_ARG(int, lastEnd)));
        QTRY_VERIFY(body->property("selectedText").toString().contains(QStringLiteral("selected first")));
        QTRY_VERIFY(body->property("selectedText").toString().contains(QStringLiteral("selected second")));

        QTest::keyClick(host.quickWidget(), Qt::Key_QuoteLeft, Qt::ControlModifier);

        QTRY_COMPARE(editor.model()->rowCount(), 4);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::Text));
        QCOMPARE(editor.model()->blockTypeAt(2), int(NoteBlockModel::CodeBlock));
        QCOMPARE(editor.model()->blockTypeAt(3), int(NoteBlockModel::Text));
        QCOMPARE(editor.model()->data(editor.model()->index(2), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("selected first\nselected second"));
        QVERIFY(!editor.model()
                     ->data(editor.model()->index(2), NoteBlockModel::TextRole)
                     .toString()
                     .contains(QStringLiteral("\n\n")));
        QVERIFY2(editor.text().contains(QStringLiteral("```\nselected first\nselected second\n```")),
                 qPrintable(editor.text()));

        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.text(), originalText);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::Text));
        QVERIFY(editor.redo());
        QTRY_COMPARE(editor.model()->blockTypeAt(2), int(NoteBlockModel::CodeBlock));
        QCOMPARE(editor.model()->data(editor.model()->index(2), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("selected first\nselected second"));

        // The same action remains inline for a one-line selection.
        QQuickItem *prefix = nullptr;
        QTRY_VERIFY((prefix = textEditorForBlock(root, 1)));
        prefix->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(prefix->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(prefix, "select", Q_ARG(int, 0), Q_ARG(int, 4)));
        QVariant converted;
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "applyActiveInlineStyle", Q_RETURN_ARG(QVariant, converted),
                                          Q_ARG(QVariant, QStringLiteral("code"))));
        QVERIFY(converted.toBool());
        QTRY_COMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TextRole).toString(),
                     QStringLiteral("`keep` before"));
        int codeBlocks = 0;
        for (int row = 0; row < editor.model()->rowCount(); ++row)
            codeBlocks += editor.model()->blockTypeAt(row) == NoteBlockModel::CodeBlock;
        QCOMPARE(codeBlocks, 1);
    }

    void deletingAcrossAdjacentCodeBlocksKeepsLiteralLineBreaks()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("```cpp\n"
                                    "first 1\n"
                                    "first 2\n"
                                    "first 3\n"
                                    "```\n\n"
                                    "```python\n"
                                    "second 1\n"
                                    "second 2\n"
                                    "second 3\n"
                                    "```"),
                     Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 460);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *first = nullptr;
        QQuickItem *last  = nullptr;
        QTRY_VERIFY((first = textEditorForBlock(root, 1)));
        QTRY_VERIFY((last = textEditorForBlock(root, 2)));
        QVERIFY(first->property("codeDocument").toBool());
        QVERIFY(last->property("codeDocument").toBool());
        auto *blockEditor = ancestorWithProperty(first, "currentFindText");
        QVERIFY(blockEditor);

        const QString firstText  = first->property("text").toString();
        const int     firstStart = firstText.indexOf(QStringLiteral("first 3")) + QStringLiteral("first ").size();
        const int     lastEnd    = QStringLiteral("second ").size();
        QVERIFY(firstStart > 0);
        QVERIFY(QMetaObject::invokeMethod(
            blockEditor, "applyDocumentSelection", Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject *>(first))),
            Q_ARG(QVariant, firstStart), Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject *>(last))),
            Q_ARG(QVariant, lastEnd), Q_ARG(QVariant, false)));
        QTRY_VERIFY(first->property("selectionStart").toInt() != first->property("selectionEnd").toInt());
        QTRY_VERIFY(last->property("selectionStart").toInt() != last->property("selectionEnd").toInt());

        QTest::keyClick(host.quickWidget(), Qt::Key_Delete);

        QTRY_COMPARE(editor.model()->rowCount(), 3);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::CodeBlock));
        QCOMPARE(editor.model()->blockTypeAt(2), int(NoteBlockModel::CodeBlock));
        const QString firstRemainder
            = editor.model()->data(editor.model()->index(1), NoteBlockModel::TextRole).toString();
        const QString lastRemainder
            = editor.model()->data(editor.model()->index(2), NoteBlockModel::TextRole).toString();
        QCOMPARE(firstRemainder, QStringLiteral("first 1\nfirst 2\nfirst "));
        QCOMPARE(lastRemainder, QStringLiteral("1\nsecond 2\nsecond 3"));
        QVERIFY(!firstRemainder.contains(QStringLiteral("\n\n")));
        QVERIFY(!lastRemainder.contains(QStringLiteral("\n\n")));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("cpp"));
        QCOMPARE(editor.model()->data(editor.model()->index(2), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("python"));
    }

    void draggingTextSelectionAcrossTrailingAudioHighlightsPlayer()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("Select this text"), Note::Markdown);
        DraftManager  drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor    editor(note, drafts);
        const QString source = QStringLiteral("anykeep-media:/00000000-0000-0000-0000-000000000001/audio.m4a");
        editor.model()->insertAudio(editor.model()->rowCount(), source, QStringLiteral("Voice memo"), 2500);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *quick = host.quickWidget();
        auto *root  = qobject_cast<QQuickItem *>(quick->rootObject());
        QVERIFY(root);
        QQuickItem *body  = nullptr;
        QQuickItem *audio = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QTRY_VERIFY((audio = quickItemByName(root, QStringLiteral("audioBlockEditor-2"))));
        QVERIFY(!audio->property("selected").toBool());

        const QPoint start = body->mapToScene(QPointF(body->width() * 0.25, body->height() * 0.5)).toPoint();
        const QPoint end   = audio->mapToScene(QPointF(audio->width() * 0.5, audio->height() + 10)).toPoint();
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(quick, end, 50);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, end);

        QTRY_VERIFY(audio->property("selected").toBool());
    }

    void draggingUpFromTrailingAreaKeepsTemporarySelectionAnchor()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("Select this text"), Note::Markdown);
        DraftManager  drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor    editor(note, drafts);
        const QString source = QStringLiteral("anykeep-media:/00000000-0000-0000-0000-000000000001/audio.m4a");
        editor.model()->insertAudio(editor.model()->rowCount(), source, QStringLiteral("Voice memo"), 2500);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *quick = host.quickWidget();
        auto *root  = qobject_cast<QQuickItem *>(quick->rootObject());
        QVERIFY(root);
        QQuickItem *body  = nullptr;
        QQuickItem *audio = nullptr;
        QQuickItem *card  = nullptr;
        QQuickItem *title = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QTRY_VERIFY((audio = quickItemByName(root, QStringLiteral("audioBlockEditor-2"))));
        QTRY_VERIFY((card = quickItemByName(root, QStringLiteral("audioCard-2"))));
        QTRY_VERIFY((title = quickItemByName(root, QStringLiteral("audioTitle-2"))));
        auto *blockEditor = ancestorWithProperty(body, "currentFindText");
        QVERIFY(blockEditor);

        const QColor unselectedFill = card->property("color").value<QColor>();
        QCOMPARE(QQmlProperty(card, QStringLiteral("border.width")).read().toInt(), 1);
        QVERIFY(!QQmlProperty(title, QStringLiteral("font.bold")).read().toBool());
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "selectAllDocument"));
        QTRY_VERIFY(audio->property("selected").toBool());
        QTRY_COMPARE(QQmlProperty(card, QStringLiteral("border.width")).read().toInt(), 2);
        QTRY_VERIFY(QQmlProperty(title, QStringLiteral("font.bold")).read().toBool());
        QTRY_VERIFY(card->property("color").value<QColor>() != unselectedFill);
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "clearDocumentSelection"));
        QTRY_COMPARE(QQmlProperty(card, QStringLiteral("border.width")).read().toInt(), 1);

        const QPoint trailingPoint = audio->mapToScene(QPointF(audio->width() * 0.5, audio->height() + 20)).toPoint();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, trailingPoint);
        QTRY_COMPARE(editor.model()->rowCount(), 4);
        QQuickItem *temporaryParagraph = nullptr;
        QTRY_VERIFY((temporaryParagraph = textEditorForBlock(root, 3)));
        QTRY_VERIFY(temporaryParagraph->hasActiveFocus());

        const QPoint selectionStart
            = temporaryParagraph
                  ->mapToScene(QPointF(temporaryParagraph->width() * 0.5, temporaryParagraph->height() + 20))
                  .toPoint();
        const QPoint audioPoint = audio->mapToScene(QPointF(audio->width() * 0.5, audio->height() - 2)).toPoint();
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, selectionStart);
        QTRY_COMPARE(blockEditor->property("blankSelectionBoundary").toInt(), 4);
        QVERIFY(!blockEditor->property("mouseSelectionActive").toBool());
        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(!temporaryParagraph->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(
            blockEditor, "scheduleDiscardEmptyInsertedParagraph",
            Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject *>(temporaryParagraph)))));
        QTest::qWait(50);
        QCOMPARE(editor.model()->rowCount(), 4);
        QTest::mouseMove(quick, audioPoint, 50);
        QTRY_VERIFY(audio->property("selected").toBool());
        QTRY_COMPARE(QQmlProperty(card, QStringLiteral("border.width")).read().toInt(), 2);
        QTRY_VERIFY(QQmlProperty(title, QStringLiteral("font.bold")).read().toBool());
        QTRY_VERIFY(card->property("color").value<QColor>() != unselectedFill);
        QTest::qWait(100);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, audioPoint);

        QTRY_VERIFY(audio->property("selected").toBool());
        QTest::qWait(200);
        QVERIFY(audio->property("selected").toBool());
        QCOMPARE(QQmlProperty(card, QStringLiteral("border.width")).read().toInt(), 2);
        QVERIFY(QQmlProperty(title, QStringLiteral("font.bold")).read().toBool());
        QVERIFY(card->property("color").value<QColor>() != unselectedFill);
        QCOMPARE(editor.model()->rowCount(), 4);

        QVERIFY(QMetaObject::invokeMethod(blockEditor, "clearDocumentSelection"));
        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_COMPARE(editor.model()->rowCount(), 3);
    }

    void draggingUpFromTrailingAreaVisuallySelectsFinalImage()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("Select this text"), Note::Markdown);
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor   editor(note, drafts);
        editor.model()->insertImage(editor.model()->rowCount(), QStringLiteral("qrc:/svg/anykeep"),
                                    QStringLiteral("Diagram"));
        editor.model()->setImageWidth(2, 240);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *quick = host.quickWidget();
        auto *root  = qobject_cast<QQuickItem *>(quick->rootObject());
        QVERIFY(root);
        QQuickItem *body    = nullptr;
        QQuickItem *image   = nullptr;
        QQuickItem *outline = nullptr;
        QQuickItem *alt     = nullptr;
        QQuickItem *actions = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QTRY_VERIFY((image = quickItemByName(root, QStringLiteral("imageBlockEditor-2"))));
        QTRY_VERIFY((outline = quickItemByName(root, QStringLiteral("imageSelectionOutline-2"))));
        QTRY_VERIFY((alt = quickItemByName(root, QStringLiteral("imageAltEditor-2"))));
        QTRY_VERIFY((actions = quickItemByName(root, QStringLiteral("imageActions-2"))));
        auto *blockEditor = ancestorWithProperty(body, "currentFindText");
        QVERIFY(blockEditor);

        QVERIFY(!image->property("selected").toBool());
        QVERIFY(!outline->isVisible());
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "selectAllDocument"));
        QTRY_VERIFY(image->property("selected").toBool());
        QTRY_VERIFY(outline->isVisible());
        QVERIFY(!alt->isVisible());
        QVERIFY(!actions->isVisible());
        QVERIFY(QMetaObject::invokeMethod(blockEditor, "clearDocumentSelection"));
        QTRY_VERIFY(!outline->isVisible());

        const QPoint trailingPoint = image->mapToScene(QPointF(image->width() * 0.5, image->height() + 20)).toPoint();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, trailingPoint);
        QTRY_COMPARE(editor.model()->rowCount(), 4);
        QQuickItem *temporaryParagraph = nullptr;
        QTRY_VERIFY((temporaryParagraph = textEditorForBlock(root, 3)));
        QTRY_VERIFY(temporaryParagraph->hasActiveFocus());

        const QPoint selectionStart
            = temporaryParagraph
                  ->mapToScene(QPointF(temporaryParagraph->width() * 0.5, temporaryParagraph->height() + 20))
                  .toPoint();
        const QPoint imagePoint = image->mapToScene(QPointF(image->width() * 0.5, image->height() - 2)).toPoint();
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, selectionStart);
        QTRY_COMPARE(blockEditor->property("blankSelectionBoundary").toInt(), 4);
        QVERIFY(!blockEditor->property("mouseSelectionActive").toBool());
        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(!temporaryParagraph->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(
            blockEditor, "scheduleDiscardEmptyInsertedParagraph",
            Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject *>(temporaryParagraph)))));
        QTest::qWait(50);
        QCOMPARE(editor.model()->rowCount(), 4);
        QTest::mouseMove(quick, imagePoint, 50);
        QTRY_VERIFY(image->property("selected").toBool());
        QTRY_VERIFY(outline->isVisible());
        QVERIFY(!alt->isVisible());
        QVERIFY(!actions->isVisible());
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, imagePoint);

        QTest::qWait(200);
        QVERIFY(image->property("selected").toBool());
        QVERIFY(outline->isVisible());
        QVERIFY(!alt->isVisible());
        QVERIFY(!actions->isVisible());

        QVERIFY(QMetaObject::invokeMethod(blockEditor, "clearDocumentSelection"));
        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_COMPARE(editor.model()->rowCount(), 3);
    }

    void externalTextInsertionUsesDropPosition()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("alpha omega"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(520, 360);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));

        QString firstLogLine = QStringLiteral("Qt Creator log one:");
        for (int index = 0; index < 24; ++index)
            firstLogLine += QStringLiteral(" payload");
        const QString secondLogLine = QStringLiteral("Qt Creator log two");
        QMimeData     mimeData;
        mimeData.setData(QStringLiteral("text/plain;charset=utf-8"),
                         (firstLogLine + QLatin1Char('\n') + secondLogLine).toUtf8());
        const QPointF   dragPoint = body->mapToItem(root, QPointF(body->width() / 2, body->height() / 2));
        QDragEnterEvent enterEvent(dragPoint.toPoint(), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &enterEvent);
        QVERIFY(enterEvent.isAccepted());
        QCOMPARE(enterEvent.dropAction(), Qt::CopyAction);
        QDropEvent dropEvent(dragPoint, Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &dropEvent);
        QVERIFY(dropEvent.isAccepted());
        QCOMPARE(dropEvent.dropAction(), Qt::CopyAction);
        QTRY_VERIFY(editor.model()->contents().contains(QStringLiteral("Qt Creator log one")));
        const QString droppedMarkdown = editor.model()->contents();
        QVERIFY2(droppedMarkdown.contains(firstLogLine + QStringLiteral("  \n") + secondLogLine)
                     || droppedMarkdown.contains(firstLogLine + QStringLiteral("<br>") + secondLogLine),
                 qPrintable(droppedMarkdown));
        QVERIFY(!droppedMarkdown.contains(QStringLiteral("ANYKEEP")));

        QVariant inserted;
        QVERIFY(QMetaObject::invokeMethod(body, "insertExternalText", Q_RETURN_ARG(QVariant, inserted),
                                          Q_ARG(QVariant, QStringLiteral("browser")), Q_ARG(QVariant, 6)));
        QVERIFY(inserted.toBool());
        QTRY_VERIFY(editor.model()->contents().contains(QStringLiteral("browser")));
        QCOMPARE(body->property("cursorPosition").toInt(), 13);
    }

    void findRunsWhileTheQueryIsTyped()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("alpha needle omega"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(560, 420);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        QQuickItem *field = nullptr;
        QTRY_VERIFY((field = quickVisibleItemByName(root, QStringLiteral("noteFindField"))));
        field->forceActiveFocus(Qt::ShortcutFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClicks(host.quickWidget(), QStringLiteral("needle"));

        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        QTRY_COMPARE(body->property("selectedText").toString(), QStringLiteral("needle"));
    }

    void tableShiftUpSelectsTheRowAndDeleteClearsIt()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("| Left | Right |\n| --- | --- |\n| lower left | lower right |"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QList<QQuickItem *> cells;
        QTRY_VERIFY(([&]() {
            cells = tableCellEditors(root, 1);
            return cells.size() == 4;
        })());

        QQuickItem *lowerRight = cells.at(3);
        lowerRight->forceActiveFocus(Qt::MouseFocusReason);
        lowerRight->setProperty("cursorPosition", lowerRight->property("length"));
        QTRY_VERIFY(lowerRight->hasActiveFocus());
        QTest::keyClick(host.quickWidget(), Qt::Key_Up, Qt::ShiftModifier);

        QTRY_COMPARE(cells.at(2)->property("selectionStart").toInt(), 0);
        QCOMPARE(cells.at(2)->property("selectionEnd").toInt(), cells.at(2)->property("length").toInt());
        QCOMPARE(cells.at(3)->property("selectionStart").toInt(), 0);
        QCOMPARE(cells.at(3)->property("selectionEnd").toInt(), cells.at(3)->property("length").toInt());
        QTRY_VERIFY(cells.at(1)->hasActiveFocus());
        QCOMPARE(cells.at(1)->property("cursorPosition").toInt(), cells.at(1)->property("length").toInt());

        QTest::keyClick(host.quickWidget(), Qt::Key_Delete);
        QTRY_VERIFY(([&]() {
            const QStringList values = editor.model()
                                           ->data(editor.model()->index(1), NoteBlockModel::CellsRole)
                                           .toMap()
                                           .value(QStringLiteral("values"))
                                           .toStringList();
            return values.size() == 4 && values.at(2).isEmpty() && values.at(3).isEmpty();
        })());
        QTRY_VERIFY(cells.at(1)->hasActiveFocus());
    }

    void lineSelectionHelperSelectsOnlyTheClickedLine()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("first line\nsecond line\nthird line"), Note::PlainText);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(520, 420);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));

        QVariant ignored;
        QVERIFY(QMetaObject::invokeMethod(body, "selectLineAt", Q_RETURN_ARG(QVariant, ignored), Q_ARG(QVariant, 15)));
        QCOMPARE(body->property("selectedText").toString(), QStringLiteral("second line"));
        QCOMPARE(body->property("selectionStart").toInt(), 11);
        QCOMPARE(body->property("selectionEnd").toInt(), 22);
    }

    void enterAfterLinkLeavesLinkFormatting()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("[example](https://example.org)"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(520, 360);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));

        body->forceActiveFocus(Qt::MouseFocusReason);
        body->setProperty("cursorPosition", body->property("length"));
        QTRY_VERIFY(body->hasActiveFocus());
        QTest::keyClick(host.quickWidget(), Qt::Key_Return);
        QTest::keyClicks(host.quickWidget(), QStringLiteral("plain"));

        QTRY_VERIFY(editor.model()->contents().contains(QStringLiteral("plain")));
        const QString markdown = editor.model()->contents();
        QVERIFY2(markdown.contains(QStringLiteral("[example](https://example.org)")), qPrintable(markdown));
        QVERIFY2(!markdown.contains(QStringLiteral("[plain](")), qPrintable(markdown));
        QVERIFY2(!markdown.contains(QStringLiteral("example\nplain](")), qPrintable(markdown));
    }

    void enterAfterTypedTagPromotesTagLineAndFocusesFollowingText()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("placeholder"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(520, 360);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));

        body->forceActiveFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(body->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, 0), Q_ARG(int, body->property("length").toInt())));
        QTest::keyClicks(host.quickWidget(), QStringLiteral("#work"));

        QTest::keyClick(host.quickWidget(), Qt::Key_Return);

        QTRY_COMPARE(editor.model()->rowCount(), 3);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::TagLine));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TagsRole).toStringList(),
                 QStringList({ QStringLiteral("work") }));
        QCOMPARE(editor.model()->blockTypeAt(2), int(NoteBlockModel::Text));
        QQuickItem *following = nullptr;
        QTRY_VERIFY((following = textEditorForBlock(root, 2)));
        QTRY_VERIFY(following->hasActiveFocus());
        QCOMPARE(following->property("cursorPosition").toInt(), 0);
    }

    void plainTextDropKeepsLiteralNewlines()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("alpha omega"), Note::PlainText);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(520, 360);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QTRY_VERIFY(!textEditors(root).isEmpty());
        QQuickItem *target = textEditors(root).constFirst();

        QMimeData mimeData;
        mimeData.setText(QStringLiteral("plain one\nplain two"));
        const QPointF   point = target->mapToItem(root, QPointF(target->width() / 2, target->height() / 2));
        QDragEnterEvent enterEvent(point.toPoint(), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &enterEvent);
        QVERIFY(enterEvent.isAccepted());
        QDropEvent dropEvent(point, Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &dropEvent);
        QVERIFY(dropEvent.isAccepted());

        QTRY_VERIFY(editor.model()->contents().contains(QStringLiteral("plain one\nplain two")));
    }

    void codeMimeCreatesCodeBlockButCodeTargetKeepsItsBlock()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("alpha omega"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(560, 400);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));

        QMimeData mimeData;
        mimeData.setText(QStringLiteral("int main() {\n    return 0;\n}"));
        mimeData.setData(QStringLiteral("text/x-c++src"), QByteArrayLiteral("code metadata"));
        const QPointF   point = body->mapToItem(root, QPointF(body->width() / 2, body->height() / 2));
        QDragEnterEvent enterEvent(point.toPoint(), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &enterEvent);
        QVERIFY(enterEvent.isAccepted());
        QDropEvent dropEvent(point, Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &dropEvent);
        QVERIFY(dropEvent.isAccepted());

        int codeRow = -1;
        QTRY_VERIFY(([&]() {
            for (int row = 0; row < editor.model()->rowCount(); ++row) {
                if (editor.model()->blockTypeAt(row) == NoteBlockModel::CodeBlock) {
                    codeRow = row;
                    return true;
                }
            }
            return false;
        })());
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("cpp"));
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("int main() {\n    return 0;\n}"));

        QQuickItem *codeEditor = nullptr;
        QTRY_VERIFY(([&]() {
            for (QQuickItem *candidate : textEditors(root)) {
                if (candidate->property("blockIndex").toInt() == codeRow
                    && candidate->property("codeDocument").toBool()) {
                    codeEditor = candidate;
                    return true;
                }
            }
            return false;
        })());
        QMimeData nestedMime;
        nestedMime.setText(QStringLiteral("first\nsecond"));
        nestedMime.setData(QStringLiteral("text/x-python"), QByteArrayLiteral("python metadata"));
        const QPointF nestedPoint
            = codeEditor->mapToItem(root, QPointF(codeEditor->width() / 2, codeEditor->height() / 2));
        QDragEnterEvent nestedEnter(nestedPoint.toPoint(), Qt::CopyAction, &nestedMime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &nestedEnter);
        QVERIFY(nestedEnter.isAccepted());
        QDropEvent nestedDrop(nestedPoint, Qt::CopyAction, &nestedMime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &nestedDrop);
        QVERIFY(nestedDrop.isAccepted());

        int codeBlocks = 0;
        for (int row = 0; row < editor.model()->rowCount(); ++row)
            codeBlocks += editor.model()->blockTypeAt(row) == NoteBlockModel::CodeBlock;
        QCOMPARE(codeBlocks, 1);
        QTRY_VERIFY(editor.model()
                        ->data(editor.model()->index(codeRow), NoteBlockModel::TextRole)
                        .toString()
                        .contains(QStringLiteral("first\nsecond")));
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("cpp"));
    }

    void qtCreatorPlainTextCodeSurvivesFocusFlushAndFormatChanges()
    {
        const QString source
            = QStringLiteral("std::vector<std::string> visibleLabels(const std::vector<Record> &records)\n"
                             "{\n"
                             "    std::vector<std::string> labels;\n"
                             "    for (const auto &record : records) {\n"
                             "        if (!record.hidden)\n"
                             "            labels.push_back(record.label);\n"
                             "    }\n"
                             "    return labels;\n"
                             "}");
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("alpha omega"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));

        // Qt Creator may advertise a source selection as text/plain only.
        QMimeData mimeData;
        mimeData.setText(source);
        const QPointF   point = body->mapToItem(root, QPointF(body->width() / 2, body->height() / 2));
        QDragEnterEvent enterEvent(point.toPoint(), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &enterEvent);
        QVERIFY(enterEvent.isAccepted());
        QDropEvent dropEvent(point, Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(host.quickWidget(), &dropEvent);
        QVERIFY(dropEvent.isAccepted());

        int codeRow = -1;
        QTRY_VERIFY(([&]() {
            for (int row = 0; row < editor.model()->rowCount(); ++row) {
                if (editor.model()->blockTypeAt(row) == NoteBlockModel::CodeBlock) {
                    codeRow = row;
                    return true;
                }
            }
            return false;
        })());
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("cpp"));
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::TextRole).toString(), source);

        // Window deactivation follows this same flush path. It must not apply
        // a stale rendered source and roll the newly inserted block back.
        QVERIFY(QMetaObject::invokeMethod(root, "flushPendingEditorChanges"));
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::TextRole).toString(), source);
        QVERIFY(!editor.text().contains(QStringLiteral("ANYKEEPHARDLINEBREAK")));

        editor.setMarkdown(false);
        QTRY_VERIFY(!editor.isMarkdown());
        const QString fencedSource = QStringLiteral("```cpp\n") + source + QStringLiteral("\n```");
        QVERIFY2(editor.text().contains(fencedSource), qPrintable(editor.text()));
        QVERIFY(!editor.text().contains(QStringLiteral("ANYKEEP")));

        editor.setMarkdown(true);
        QTRY_VERIFY(editor.isMarkdown());
        int restoredCodeRow = -1;
        for (int row = 0; row < editor.model()->rowCount(); ++row) {
            if (editor.model()->blockTypeAt(row) == NoteBlockModel::CodeBlock) {
                restoredCodeRow = row;
                break;
            }
        }
        QVERIFY(restoredCodeRow >= 0);
        QCOMPARE(editor.model()->data(editor.model()->index(restoredCodeRow), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("cpp"));
        QCOMPARE(editor.model()->data(editor.model()->index(restoredCodeRow), NoteBlockModel::TextRole).toString(),
                 source);
        QVERIFY(!editor.text().contains(QStringLiteral("ANYKEEP")));
    }

    void pythonClipboardUsesExactPlainTextInsteadOfRichHtmlFragments()
    {
        const QString source
            = QStringLiteral("def test_cache_refresh_preserves_active_session(workspace: Workspace):\n"
                             "    cache = workspace.create_cache()\n"
                             "    session = workspace.open_session()\n"
                             "    cache.store(\"theme\", \"midnight\")\n"
                             "\n"
                             "    monitor: EventMonitor = workspace.event_monitor()\n"
                             "    client = workspace.client(event_sink=monitor)\n"
                             "    clock: TestClock = workspace.test_clock\n"
                             "\n"
                             "    SessionAssertions.check_connected(client, session)\n"
                             "    monitor.expect(\"CacheRefreshStarted\").wait(10)\n"
                             "\n"
                             "    with client.refresh_cache(force=True, namespace=\"settings\"):\n"
                             "        monitor.expect(\"CacheRefreshFinished\", cache_id=cache.id).wait()");
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("paste here"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        body->forceActiveFocus(Qt::MouseFocusReason);
        body->setProperty("cursorPosition", body->property("length"));
        QTRY_VERIFY(body->hasActiveFocus());

        auto *mimeData = new QMimeData;
        mimeData->setText(source);
        // Source editors commonly add presentation HTML. It must not outrank
        // the exact text/plain representation once the payload is code.
        mimeData->setHtml(QStringLiteral(
            "<pre><span>def test_cache_refresh_preserves_active_session(workspace: Workspace):</span><br><br>"
            "    cache = workspace.create_cache()<br><br>    session = workspace.open_session()<br><br>"
            "    cache.store(&quot;theme&quot;, &quot;midnight&quot;)<br><br>"
            "    monitor: EventMonitor = workspace.event_monitor()<br><br>"
            "    with client.refresh_cache(force=True, namespace=&quot;settings&quot;):"
            "</pre>"));
        QGuiApplication::clipboard()->setMimeData(mimeData);

        QVariant pasted;
        QVERIFY(QMetaObject::invokeMethod(root, "pasteClipboard", Q_RETURN_ARG(QVariant, pasted)));
        QVERIFY(pasted.toBool());

        int codeRow    = -1;
        int codeBlocks = 0;
        QTRY_VERIFY(([&]() {
            codeBlocks = 0;
            codeRow    = -1;
            for (int row = 0; row < editor.model()->rowCount(); ++row) {
                if (editor.model()->blockTypeAt(row) != NoteBlockModel::CodeBlock)
                    continue;
                ++codeBlocks;
                codeRow = row;
            }
            return codeBlocks > 0;
        })());
        QCOMPARE(codeBlocks, 1);
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("python"));
        QCOMPARE(editor.model()->data(editor.model()->index(codeRow), NoteBlockModel::TextRole).toString(), source);
        const QString fencedSource = QStringLiteral("```python\n") + source + QStringLiteral("\n```");
        QVERIFY2(editor.text().contains(fencedSource), qPrintable(editor.text()));
    }

    void pastedWebColorsAreRemovedImmediately()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("- item"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *itemEditor = nullptr;
        QTRY_VERIFY(([&] {
            for (QQuickItem *candidate : textEditors(root)) {
                if (candidate->property("listItemIndex").toInt() == 0) {
                    itemEditor = candidate;
                    return true;
                }
            }
            return false;
        })());
        itemEditor->forceActiveFocus(Qt::MouseFocusReason);
        QVERIFY(QMetaObject::invokeMethod(itemEditor, "select", Q_ARG(int, 0),
                                          Q_ARG(int, itemEditor->property("length").toInt())));
        QTRY_VERIFY(itemEditor->hasActiveFocus());

        auto *mimeData = new QMimeData;
        mimeData->setText(QStringLiteral("colored"));
        mimeData->setHtml(
            QStringLiteral("<span style=\"color:#ff0000;background-color:#0000ff\"><b>colored</b></span>"));
        QGuiApplication::clipboard()->setMimeData(mimeData);

        QVariant pasted;
        QVERIFY(QMetaObject::invokeMethod(root, "pasteClipboard", Q_RETURN_ARG(QVariant, pasted)));
        QVERIFY(pasted.toBool());

        const QVariant documentProperty = QQmlProperty(itemEditor, QStringLiteral("textDocument")).read();
        auto          *quickDocument    = qobject_cast<QQuickTextDocument *>(documentProperty.value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        QTextCursor    cursor(document);
        cursor = document->find(QStringLiteral("colored"));
        QVERIFY(!cursor.isNull());
        const QTextCharFormat format = cursor.charFormat();
        QCOMPARE(format.foreground().style(), Qt::NoBrush);
        QCOMPARE(format.background().style(), Qt::NoBrush);
        QVERIFY(!format.hasProperty(QTextFormat::TextOutline));
        QVERIFY(!format.hasProperty(QTextFormat::TextUnderlineColor));
        QVERIFY(format.fontWeight() >= QFont::Bold);
        QVERIFY(QMetaObject::invokeMethod(root, "flushPendingEditorChanges"));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("**colored**") }));
    }

    void plainTextXmlPasteSurvivesFormatRoundTrip()
    {
        const QString source = QStringLiteral("<svg width=\"24px\" height=\"24px\" viewBox=\"0 0 24 24\" role=\"img\" "
                                              "xmlns=\"http://www.w3.org/2000/svg\"><title>OpenAI icon</title>"
                                              "<path d=\"M22.2819 9.8211a5.9847 5.9847 0 0 0-.5157-4.9108\"");
        Note          note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("replace me"), Note::Markdown);
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(note, drafts);
        DesktopNoteEditorHost host(&editor);

        host.resize(620, 440);
        host.show();
        auto *root = qobject_cast<QQuickItem *>(host.quickWidget()->rootObject());
        QVERIFY(root);
        QQuickItem *body = nullptr;
        QTRY_VERIFY((body = textEditorForBlock(root, 1)));
        body->forceActiveFocus(Qt::MouseFocusReason);
        QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, 0), Q_ARG(int, body->property("length").toInt())));
        QTRY_VERIFY(body->hasActiveFocus());

        auto *mimeData = new QMimeData;
        mimeData->setText(source);
        QGuiApplication::clipboard()->setMimeData(mimeData);
        QVariant pasted;
        QVERIFY(QMetaObject::invokeMethod(root, "pasteClipboard", Q_RETURN_ARG(QVariant, pasted)));
        QVERIFY(pasted.toBool());

        QTRY_COMPARE(editor.model()->rowCount(), 2);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::CodeBlock));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("xml"));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TextRole).toString(), source);

        editor.setMarkdown(false);
        QTRY_VERIFY(!editor.isMarkdown());
        QVERIFY(editor.text().contains(QStringLiteral("```xml\n") + source + QStringLiteral("\n```")));
        editor.setMarkdown(true);
        QTRY_VERIFY(editor.isMarkdown());
        QCOMPARE(editor.model()->rowCount(), 2);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::CodeBlock));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TextRole).toString(), source);
    }

private slots:
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
        // Start safely on the paragraph/block side of the interval. The exact mouse
        // coordinate of the structural midpoint is intentionally discovered below from
        // the controller transition rather than inferred from visual delegate geometry.
        const qreal beforeParagraphBoundaryY = paragraphBoundaryY * 0.75 + destinationFirstBoundaryY * 0.25;

        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, attachFrom.toPoint());
        const QPointF beforeParagraphSwitch(attachFrom.x(), pointerYForBoundary(beforeParagraphBoundaryY));
        moveMouseAlong(&quick, attachFrom, beforeParagraphSwitch, 8, 15, 50);
        QVERIFY2(qAbs(following->property("reorderOffset").toReal()) < 1,
                 "The paragraph moved before the dragged list reached the item-level list target");

        // The controller chooses a target in the post-removal structural geometry, while
        // QTest drives an integer mouse position through a QQuickWidget.  Font metrics and
        // layout rounding can therefore put the visual midpoint at a different pointer
        // coordinate on a headless runner.  Find the first real block -> list transition
        // instead of assuming a fixed visual probe is on the list side of that transition.
        QPointF   afterParagraphSwitch;
        bool      reachedFirstListTarget = false;
        const int switchProbeSteps       = qMax(1, int(qAbs(attachTo.y() - beforeParagraphSwitch.y())) + 1);
        for (int step = 1; step <= switchProbeSteps; ++step) {
            const QPointF probe
                = beforeParagraphSwitch + (attachTo - beforeParagraphSwitch) * (qreal(step) / switchProbeSteps);
            QTest::mouseMove(&quick, probe.toPoint(), 1);
            QCoreApplication::processEvents();
            if (controller->property("targetKind").toString() == QStringLiteral("list")
                && controller->property("targetItem").toInt() == 0) {
                afterParagraphSwitch   = probe;
                reachedFirstListTarget = true;
                break;
            }
        }
        QVERIFY2(reachedFirstListTarget,
                 "The downward whole-list drag never reached the first item-level list boundary");
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("list"));
        QTRY_COMPARE(controller->property("targetItem").toInt(), 0);
        QTRY_VERIFY(controller->property("blockAnimationActive").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(following->property("reorderOffset").toReal() < -1
                                     && following->property("reorderOffset").toReal() >= -structuralExtent - 1,
                                 200);

        QTest::qWait(220);
        QVERIFY2(qAbs(destinationFirstMarker->mapToItem(root, QPointF()).y() - firstMarkerYBefore) < 1,
                 "The destination list moved after the preceding paragraph crossed upward");
        QTest::mouseMove(&quick, beforeParagraphSwitch.toPoint(), 15);
        QTRY_COMPARE(controller->property("targetKind").toString(), QStringLiteral("block"));
        QTest::qWait(30);
        const qreal paragraphOffsetDuringReverse = following->property("reorderOffset").toReal();
        QVERIFY2(paragraphOffsetDuringReverse <= 1 && paragraphOffsetDuringReverse >= -structuralExtent - 1,
                 qPrintable(QStringLiteral("The reverse displacement left its valid range: offset=%1 extent=%2")
                                .arg(paragraphOffsetDuringReverse)
                                .arg(structuralExtent)));
        QVERIFY2(qAbs(destinationFirstMarker->mapToItem(root, QPointF()).y() - firstMarkerYBefore) < 1,
                 "The destination list moved with the preceding paragraph during the reverse animation");
        QTest::qWait(220);
        QVERIFY2(qAbs(following->property("reorderOffset").toReal()) < 1,
                 "The paragraph did not return after the reverse animation");
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
        QTRY_VERIFY_WITH_TIMEOUT(destinationFirstMarker->mapToItem(root, QPointF()).y() > firstMarkerBeforeReverse + 1,
                                 200);
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

    void nestedListItemDragKeepsExactGapAndAnimatesParagraph()
    {
        const QString  document = QStringLiteral("list\n\n"
                                                  "test\n\n"
                                                  "- 1\n"
                                                  "    - 2\n"
                                                  "    - 3\n"
                                                  "    - 4\n"
                                                  "    - 5");
        NoteBlockModel model;
        model.load(document, true);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::BulletList));

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
                          QUrl(QStringLiteral("qrc:/qml/NestedListDragHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *harness = component.create();
        QVERIFY2(harness, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/NestedListDragHarness.qml")), &component, harness);
        quick.show();

        auto *root = qobject_cast<QQuickItem *>(quick.rootObject());
        QVERIFY(root);

        QQuickItem *firstRow     = nullptr;
        QQuickItem *firstMarker  = nullptr;
        QQuickItem *sourceRow    = nullptr;
        QQuickItem *sourceHandle = nullptr;
        QQuickItem *textHandle   = nullptr;
        QTRY_VERIFY((firstRow = quickItemByName(root, QStringLiteral("listRow-2-0"))));
        QTRY_VERIFY((firstMarker = quickItemByName(root, QStringLiteral("listMarker-2-0"))));
        QTRY_VERIFY((sourceRow = quickItemByName(root, QStringLiteral("listRow-2-1"))));
        QTRY_VERIFY((sourceHandle = quickItemByName(root, QStringLiteral("listReorderHandle-2-1"))));
        QTRY_VERIFY((textHandle = quickItemByName(root, QStringLiteral("blockReorderHandle-1"))));

        auto *listBlock = ancestorWithProperty(sourceHandle, "itemSpacing");
        auto *textBlock = ancestorWithProperty(textHandle, "reorderSourceActive");
        QVERIFY(listBlock);
        QVERIFY(textBlock);

        auto *controller = root->findChild<QObject *>(QStringLiteral("editorReorderController"));
        QVERIFY(controller);
        QVERIFY(QMetaObject::invokeMethod(controller, "startListDrag", Q_ARG(QVariant, QVariant::fromValue(listBlock)),
                                          Q_ARG(QVariant, QVariant::fromValue(sourceRow))));
        QTRY_VERIFY(controller->property("dragging").toBool());
        QVERIFY(!controller->property("wholeListBlockDrag").toBool());
        QVERIFY(controller->property("blockAnimationActive").toBool());

        const qreal expectedListGap = sourceRow->property("naturalHeight").toReal()
            + (sourceRow->property("trailingSpace").toReal() <= 0 ? listBlock->property("itemSpacing").toReal() : 0);
        QVERIFY2(qAbs(controller->property("listDraggedHeight").toReal() - expectedListGap) < 1,
                 "A last list item must gain exactly one adjacency spacing at a list insertion target");

        const auto moveDrag = [controller](qreal dy) {
            return QMetaObject::invokeMethod(controller, "moveListDrag", Q_ARG(QVariant, 0.0), Q_ARG(QVariant, dy));
        };
        const qreal sourceTop = sourceRow->mapToItem(root, QPointF()).y();
        const qreal textTop   = textBlock->mapToItem(root, QPointF()).y();
        const int   finalDy   = qFloor(textTop - sourceTop - 100);

        int listTargetDy = 0;
        for (int dy = -1; dy >= finalDy; --dy) {
            QVERIFY(moveDrag(dy));
            QCoreApplication::processEvents();
            if (controller->property("targetKind").toString() == QStringLiteral("list")
                && controller->property("targetItem").toInt() == 0) {
                listTargetDy = dy;
                break;
            }
        }
        QVERIFY2(listTargetDy < 0, "The slow upward drag never reached the gap before the first list item");
        QTRY_VERIFY(qAbs(firstRow->property("dropSpace").toReal() - expectedListGap) < 1);
        const qreal firstMarkerYAtListTarget = firstMarker->mapToItem(root, QPointF()).y();

        const qreal structuralExtent = controller->property("structuralDraggedHeight").toReal();
        QVERIFY2(qAbs(structuralExtent - expectedListGap) < 1,
                 "A partial list range must keep one structural extent across list and block targets");
        bool  sawAnimatedFrame             = false;
        bool  firstRowMovedBeforeParagraph = false;
        qreal maximumFirstMarkerDeviation  = 0;
        for (int dy = listTargetDy - 1; dy >= finalDy; --dy) {
            QVERIFY(moveDrag(dy));
            QCoreApplication::processEvents();
            QTest::qWait(25);
            const qreal paragraphOffset = textBlock->property("reorderOffset").toReal();
            maximumFirstMarkerDeviation
                = qMax(maximumFirstMarkerDeviation,
                       qAbs(firstMarker->mapToItem(root, QPointF()).y() - firstMarkerYAtListTarget));
            if (paragraphOffset <= 0.5
                && qAbs(firstMarker->mapToItem(root, QPointF()).y() - firstMarkerYAtListTarget) > 1) {
                firstRowMovedBeforeParagraph = true;
                break;
            }
            if (paragraphOffset > 0.5) {
                sawAnimatedFrame = paragraphOffset < structuralExtent - 0.5;
                break;
            }
        }
        QVERIFY2(!firstRowMovedBeforeParagraph,
                 "The remaining list started a second animation before the paragraph moved");
        QVERIFY2(sawAnimatedFrame, "The paragraph jumped directly to its final position instead of animating there");
        for (int frame = 0; frame < 20; ++frame) {
            QTest::qWait(10);
            maximumFirstMarkerDeviation
                = qMax(maximumFirstMarkerDeviation,
                       qAbs(firstMarker->mapToItem(root, QPointF()).y() - firstMarkerYAtListTarget));
        }
        QTRY_VERIFY(qAbs(textBlock->property("reorderOffset").toReal() - structuralExtent) < 1);
        QTRY_VERIFY(qAbs(firstMarker->mapToItem(root, QPointF()).y() - firstMarkerYAtListTarget) < 1);
        QVERIFY2(maximumFirstMarkerDeviation < 1,
                 "The visible remaining list item moved during the list-to-block transition");

        QVERIFY(QMetaObject::invokeMethod(controller, "cancelDrag"));
        QTRY_VERIFY(!controller->property("dragging").toBool());
        QTRY_VERIFY(qAbs(firstRow->property("dropSpace").toReal()) < 1);
        QTRY_VERIFY(qAbs(sourceRow->property("collapseSpace").toReal()) < 1);
        QTRY_VERIFY(qAbs(textBlock->property("reorderOffset").toReal()) < 1);

        QQuickItem *levelHandle = nullptr;
        QQuickItem *thirdRow    = nullptr;
        QTRY_VERIFY((levelHandle = quickItemByName(root, QStringLiteral("listLevelReorderHandle-2-1-1"))));
        QTRY_VERIFY((thirdRow = quickItemByName(root, QStringLiteral("listRow-2-2"))));
        auto *editorView = qobject_cast<QQuickItem *>(controller->property("editorView").value<QObject *>());
        QVERIFY(editorView);
        auto *contentItem = qobject_cast<QQuickItem *>(editorView->property("contentItem").value<QObject *>());
        QVERIFY(contentItem);
        auto *thirdContent = qobject_cast<QQuickItem *>(thirdRow->property("dragContent").value<QObject *>());
        QVERIFY(thirdContent);
        auto *sourceContent = qobject_cast<QQuickItem *>(sourceRow->property("dragContent").value<QObject *>());
        QVERIFY(sourceContent);
        QTRY_VERIFY(thirdContent->mapToItem(contentItem, QPointF()).y()
                    > sourceContent->mapToItem(contentItem, QPointF()).y() + 1);
        const qreal sourceTopBeforeDrag = sourceContent->mapToItem(contentItem, QPointF()).y();
        const qreal thirdTopBeforeDrag  = thirdContent->mapToItem(contentItem, QPointF()).y();

        const qreal   handleHeightBeforeDrag = levelHandle->height();
        const qreal   handleTopBeforeDrag    = levelHandle->mapToItem(root, QPointF()).y();
        const QPointF handleFrom
            = levelHandle->mapToItem(root, QPointF(levelHandle->width() / 2, levelHandle->height() / 2));
        const QPointF handleTo = handleFrom + QPointF(0, -24);
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, handleFrom.toPoint());
        moveMouseAlong(&quick, handleFrom, handleTo, 4);
        QTRY_VERIFY(controller->property("dragging").toBool());
        QTest::qWait(220);
        QVERIFY2(qAbs(levelHandle->height() - handleHeightBeforeDrag) < 1,
                 "The active level handle collapsed with its source rows");
        QQuickItem *handlePreview = nullptr;
        QTRY_VERIFY((handlePreview = quickItemByName(root, QStringLiteral("editorDragPreview-4"))));
        QVERIFY2(qAbs(handlePreview->height() - handleHeightBeforeDrag) < 1,
                 "The level-handle preview did not preserve its captured height");
        const qreal expectedHandlePreviewTop = handleTopBeforeDrag + handleTo.y() - handleFrom.y();
        QVERIFY2(qAbs(handlePreview->mapToItem(root, QPointF()).y() - expectedHandlePreviewTop) < 2,
                 "The level-handle preview did not follow the drag translation");
        QVERIFY(QMetaObject::invokeMethod(controller, "cancelDrag"));
        QTRY_VERIFY(!controller->property("dragging").toBool());
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, handleTo.toPoint());
        QTRY_VERIFY(!levelHandle->property("dragging").toBool());
        QTRY_VERIFY(qAbs(levelHandle->height() - handleHeightBeforeDrag) < 1);

        QVariant markerCenterX;
        QVERIFY(QMetaObject::invokeMethod(listBlock, "markerCenterXForIndent", Q_RETURN_ARG(QVariant, markerCenterX),
                                          Q_ARG(QVariant, 1)));
        QVERIFY(QMetaObject::invokeMethod(
            controller, "startListRangeDrag", Q_ARG(QVariant, QVariant::fromValue(listBlock)), Q_ARG(QVariant, 1),
            Q_ARG(QVariant, 5), Q_ARG(QVariant, QVariant::fromValue(levelHandle)), Q_ARG(QVariant, markerCenterX)));
        QTRY_VERIFY(controller->property("dragging").toBool());

        const qreal textBoundary = textBlock->mapToItem(contentItem, QPointF()).y();
        const qreal listBoundary = firstRow->mapToItem(contentItem, QPointF()).y();
        const qreal switchProbe  = (textBoundary + listBoundary) / 2 - 2;
        const qreal rangeDy      = switchProbe - controller->property("startDraggedTopY").toReal();
        QVERIFY(rangeDy < 0);
        QVERIFY(moveDrag(rangeDy));
        QCoreApplication::processEvents();

        QCOMPARE(controller->property("targetKind").toString(), QStringLiteral("block"));
        const qreal thirdTopAfterMove = thirdTopBeforeDrag + rangeDy;
        const qreal textBottom        = textBoundary + textBlock->height();
        QVERIFY2(thirdTopAfterMove > textBottom,
                 qPrintable(QStringLiteral("The fixture must reach the paragraph boundary before the second "
                                           "dragged row overlaps it: third=%1 textBottom=%2 dy=%3")
                                .arg(thirdTopAfterMove)
                                .arg(textBottom)
                                .arg(rangeDy)));
        QTRY_VERIFY(textBlock->property("reorderOffset").toReal() > 0.5);

        QVERIFY(QMetaObject::invokeMethod(controller, "cancelDrag"));
        QTRY_VERIFY(!controller->property("dragging").toBool());
        QCOMPARE(model.contents(), document);
    }

    void regressionPendingSourceSyncPreservesToolbarSelection() { pendingSourceSyncPreservesToolbarSelection(); }

    void regressionCodeActionConvertsMultilineTextSelectionWithoutBlankParagraphs()
    {
        codeActionConvertsMultilineTextSelectionWithoutBlankParagraphs();
    }

    void regressionDeletingAcrossAdjacentCodeBlocksKeepsLiteralLineBreaks()
    {
        deletingAcrossAdjacentCodeBlocksKeepsLiteralLineBreaks();
    }

    void regressionDraggingTextSelectionAcrossTrailingAudioHighlightsPlayer()
    {
        draggingTextSelectionAcrossTrailingAudioHighlightsPlayer();
    }

    void regressionDraggingUpFromTrailingAreaKeepsTemporarySelectionAnchor()
    {
        draggingUpFromTrailingAreaKeepsTemporarySelectionAnchor();
    }

    void regressionDraggingUpFromTrailingAreaVisuallySelectsFinalImage()
    {
        draggingUpFromTrailingAreaVisuallySelectsFinalImage();
    }

    void regressionExternalTextInsertionUsesDropPosition() { externalTextInsertionUsesDropPosition(); }

    void regressionFindRunsWhileTheQueryIsTyped() { findRunsWhileTheQueryIsTyped(); }

    void regressionTableShiftUpSelectsTheRowAndDeleteClearsIt() { tableShiftUpSelectsTheRowAndDeleteClearsIt(); }

    void regressionLineSelectionHelperSelectsOnlyTheClickedLine() { lineSelectionHelperSelectsOnlyTheClickedLine(); }

    void regressionEnterAfterLinkLeavesLinkFormatting() { enterAfterLinkLeavesLinkFormatting(); }

    void regressionEnterAfterTypedTagPromotesTagLineAndFocusesFollowingText()
    {
        enterAfterTypedTagPromotesTagLineAndFocusesFollowingText();
    }

    void regressionPlainTextDropKeepsLiteralNewlines() { plainTextDropKeepsLiteralNewlines(); }

    void regressionCodeMimeCreatesCodeBlockButCodeTargetKeepsItsBlock()
    {
        codeMimeCreatesCodeBlockButCodeTargetKeepsItsBlock();
    }

    void regressionQtCreatorPlainTextCodeSurvivesFocusFlushAndFormatChanges()
    {
        qtCreatorPlainTextCodeSurvivesFocusFlushAndFormatChanges();
    }

    void regressionPythonClipboardUsesExactPlainTextInsteadOfRichHtmlFragments()
    {
        pythonClipboardUsesExactPlainTextInsteadOfRichHtmlFragments();
    }

    void regressionPastedWebColorsAreRemovedImmediately() { pastedWebColorsAreRemovedImmediately(); }

    void regressionPlainTextXmlPasteSurvivesFormatRoundTrip() { plainTextXmlPasteSurvivesFormatRoundTrip(); }
};

QTEST_MAIN(EditorQmlTest)

#include "editorqml_test.moc"
