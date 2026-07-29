#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJSValue>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWidget>
#include <QTemporaryDir>
#include <QtTest>

#include "highlighterext.h"
#include "noteblockmodel.h"
#include "notedata.h"
#include "notehighlighter.h"
#include "notetransfercontroller.h"
#include "notewidget.h"
#include "qmlnoteeditor.h"

#include <QUuid>

using namespace QtNote;

namespace {
NoteEditor *backend(QmlNoteEditor &editor)
{
    auto *result = editor.findChild<NoteEditor *>();
    Q_ASSERT(result);
    return result;
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

QQuickItem *listMarker(QObject *root, QObject *editor)
{
    if (!root || !editor)
        return nullptr;
    const QString name = QStringLiteral("listMarker-%1-%2")
                             .arg(editor->property("blockIndex").toInt())
                             .arg(editor->property("listItemIndex").toInt());
    return quickItemByName(qobject_cast<QQuickItem *>(root), name);
}
}

class CountingHighlighter : public SpellCheckExtension {
public:
    int     calls = 0;
    QString addedWord;
    void    reset() override { }
    void    highlight(NoteHighlighter *highlighter, const QString &text) override
    {
        ++calls;
        if (!text.isEmpty()) {
            QTextCharFormat format;
            format.setProperty(SpellCheckFormatProperty, true);
            highlighter->addFormat(0, text.size(), format);
        }
    }
    QStringList suggestions(const QString &word) const override { return { word + QStringLiteral("-fixed") }; }
    void        addToDictionary(const QString &word) override { addedWord = word; }
};

class QmlNoteEditorTest : public QObject {
    Q_OBJECT

private slots:
    void loadsQmlAndSwitchesFormats()
    {
        QmlNoteEditor editor;
        auto          quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QCOMPARE(quick->status(), QQuickWidget::Ready);

        editor.load(QStringLiteral("plain\ntext"), Note::PlainText);
        QCOMPARE(editor.model()->rowCount(), 1);
        QCOMPARE(editor.contents(), QStringLiteral("plain\ntext"));
        editor.insertText(QStringLiteral("dictated"));
        QCOMPARE(editor.contents(), QStringLiteral("plain\ntext dictated"));

        editor.load(QStringLiteral("- [ ] task\n\n| A | B |\n| --- | --- |\n| 1 | 2 |"), Note::Markdown);
        QCOMPARE(editor.model()->rowCount(), 2);
        QVERIFY(editor.isMarkdown());
    }

    void queuedDocumentRegistrationSurvivesDelegateReplacement()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*registerTextDocument.*null.*")));

        QmlNoteEditor editor;
        editor.load(QStringLiteral("10\n"
                                   "- [ ] task 1\n"
                                   "- [ ] task2\n"
                                   "- [ ] task3\n"
                                   "    1. hello\n"
                                   "    2. world"),
                    Note::Markdown);
        editor.load(QStringLiteral("replacement"), Note::PlainText);
        QCoreApplication::processEvents();

        QCOMPARE(editor.contents(), QStringLiteral("replacement"));
    }

    void loadPolicyDistinguishesReplacementFromFormatConversion()
    {
        QmlNoteEditor editor;
        QSignalSpy    checkpoint(&editor, &QmlNoteEditor::focusLost);

        editor.load(QStringLiteral("title\nbody"), Note::PlainText, NoteEditor::LoadPolicy::ResetHistory);
        QCoreApplication::processEvents();
        QCOMPARE(checkpoint.size(), 0);

        editor.load(editor.contents(), Note::Markdown, NoteEditor::LoadPolicy::ResetHistory);
        QCoreApplication::processEvents();
        QCOMPARE(checkpoint.size(), 0);

        editor.load(editor.contents(), Note::PlainText, NoteEditor::LoadPolicy::RecordFormatConversion);
        QTRY_COMPARE(checkpoint.size(), 1);

        checkpoint.clear();
        editor.load(editor.contents(), Note::Markdown, NoteEditor::LoadPolicy::HistoryRestore);
        QTest::qWait(10);
        QCOMPARE(checkpoint.size(), 0);
    }

    void sharedControllerOwnsLoadAndFormatConversion()
    {
        QmlNoteEditor host;
        auto         *editor = host.findChild<NoteEditor *>();
        QVERIFY(editor);

        editor->loadDocument(QStringLiteral("Title\nBody"), Note::PlainText, NoteEditor::LoadPolicy::ResetHistory);
        QVERIFY(!editor->isDirty());
        QVERIFY(!editor->canUndo());

        editor->loadDocument(QStringLiteral("Title\n\nBody"), Note::Markdown,
                             NoteEditor::LoadPolicy::RecordFormatConversion);
        QVERIFY(editor->isMarkdown());
        QVERIFY(editor->isDirty());
        QVERIFY(editor->canUndo());

        QVERIFY(editor->undo());
        QVERIFY(!editor->isMarkdown());
        QCOMPARE(editor->text(), QStringLiteral("Title\nBody"));
        QVERIFY(editor->redo());
        QVERIFY(editor->isMarkdown());
        QCOMPARE(editor->text(), QStringLiteral("Title\n\nBody"));
    }

    void capturesAndRestoresLogicalEditorAddress()
    {
        QmlNoteEditor editor;
        editor.resize(600, 500);
        editor.load(QStringLiteral("- first\n- second\n\n"
                                   "| A | B |\n| --- | --- |\n| C | D |"),
                    Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 6);

        QObject *secondListItem = nullptr;
        QObject *lastTableCell  = nullptr;
        for (const auto &value : root->property("editors").toList()) {
            auto *candidate = value.value<QObject *>();
            if (!candidate)
                continue;
            if (candidate->property("blockIndex").toInt() == 0 && candidate->property("listItemIndex").toInt() == 1) {
                secondListItem = candidate;
            }
            if (candidate->property("blockIndex").toInt() == 1 && candidate->property("tableCellIndex").toInt() == 3) {
                lastTableCell = candidate;
            }
        }
        QVERIFY(secondListItem);
        QVERIFY(lastTableCell);
        QVERIFY(QMetaObject::invokeMethod(secondListItem, "forceActiveFocus"));
        QVERIFY(QMetaObject::invokeMethod(secondListItem, "select", Q_ARG(int, 1), Q_ARG(int, 4)));
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>(), secondListItem);

        QVariant savedState;
        QVERIFY(QMetaObject::invokeMethod(root, "captureEditorState", Q_RETURN_ARG(QVariant, savedState)));
        QVERIFY(savedState.toMap().value(QStringLiteral("active")).toMap().value(QStringLiteral("field")).toString()
                == QStringLiteral("listItem"));

        const QVariant tableAddress = QVariantMap {
            { QStringLiteral("blockIndex"), 1 },     { QStringLiteral("listItemIndex"), -1 },
            { QStringLiteral("tableCellIndex"), 3 }, { QStringLiteral("field"), QStringLiteral("tableCell") },
            { QStringLiteral("cursorPosition"), 1 },
        };
        QVariant focused;
        QVERIFY(QMetaObject::invokeMethod(root, "focusEditorAddress", Q_RETURN_ARG(QVariant, focused),
                                          Q_ARG(QVariant, tableAddress)));
        QVERIFY(focused.toBool());
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>(), lastTableCell);
        QCOMPARE(lastTableCell->property("cursorPosition").toInt(), 1);

        QVariant restored;
        QVERIFY(QMetaObject::invokeMethod(root, "restoreEditorState", Q_RETURN_ARG(QVariant, restored),
                                          Q_ARG(QVariant, savedState)));
        QVERIFY(restored.toBool());
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>(), secondListItem);
        QCOMPARE(secondListItem->property("selectionStart").toInt(), 1);
        QCOMPARE(secondListItem->property("selectionEnd").toInt(), 4);
    }

    void undoRedoMergesTypingAndRestoresCursor()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QString(), Note::PlainText);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        editor.focusEditor();
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());

        QTest::keyClicks(quick, QStringLiteral("typed"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("typed"));
        QVERIFY(editor.canUndo());

        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QString());
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        QCOMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 0);
        QVERIFY(editor.canRedo());

        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("typed"));
    }

    void undoKeepsInsertionAndDeletionAsSeparateSteps()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QString(), Note::PlainText);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        editor.focusEditor();
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        auto *textEditor = root->property("activeEditor").value<QObject *>();

        QTest::keyClicks(quick, QStringLiteral("ab"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("ab"));
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTRY_COMPARE(editor.contents(), QStringLiteral("a"));

        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("ab"));
        QTRY_COMPARE(textEditor->property("text").toString(), QStringLiteral("ab"));
        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QString());
        QTRY_COMPARE(textEditor->property("text").toString(), QString());
    }

    void repeatedBackspaceIsOneUndoStep()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QString(), Note::PlainText);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        editor.focusEditor();

        QTest::keyClicks(quick, QStringLiteral("abcdef"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("abcdef"));
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTRY_COMPARE(editor.contents(), QStringLiteral("abc"));

        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.contents(), QStringLiteral("abcdef"));
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.contents(), QString());
    }

    void imageFieldsUseScalarUndoCommands()
    {
        QmlNoteEditor editor;
        editor.resize(600, 300);
        editor.load(QStringLiteral("![cat](media://cat)"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        editor.model()->setImageUrl(0, QStringLiteral("media://cat123"));
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::UrlRole).toString(),
                     QStringLiteral("media://cat123"));
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::UrlRole).toString(),
                     QStringLiteral("media://cat"));
        QVERIFY(editor.redo());
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::UrlRole).toString(),
                     QStringLiteral("media://cat123"));
        QVERIFY(editor.undo());
        QVERIFY(!editor.canUndo());

        editor.model()->setImageAlt(0, QStringLiteral("cat123"));
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::AltRole).toString(),
                     QStringLiteral("cat123"));
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::AltRole).toString(),
                     QStringLiteral("cat"));
        QVERIFY(editor.redo());
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::AltRole).toString(),
                     QStringLiteral("cat123"));
    }

    void undoInLinkUrlFieldStaysLocal()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("title\n\nlink"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *text     = root->property("editors").toList().constFirst().value<QObject *>();
        auto *document = text->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        const int linkStart = document->textDocument()->toPlainText().indexOf(QStringLiteral("link"));
        QVERIFY(linkStart > 0);
        QVERIFY(QMetaObject::invokeMethod(text, "forceActiveFocus"));
        text->setProperty("cursorPosition", text->property("length"));
        QTest::keyClicks(quick, QStringLiteral("x"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\nlinkx"));
        QVERIFY(editor.canUndo());

        QVERIFY(QMetaObject::invokeMethod(text, "select", Q_ARG(int, linkStart), Q_ARG(int, linkStart + 4)));
        QTest::keyClick(quick, Qt::Key_K, Qt::ControlModifier);
        auto *urlField = root->findChild<QObject *>(QStringLiteral("noteLinkUrlField"));
        QVERIFY(urlField);
        QTRY_VERIFY(urlField->property("activeFocus").toBool());
        QTest::keyClicks(quick, QStringLiteral("abc"));
        QCOMPARE(urlField->property("text").toString(), QStringLiteral("abc"));

        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(editor.contents(), QStringLiteral("title\n\nlinkx"));
        QTRY_VERIFY(urlField->property("text").toString() != QStringLiteral("abc"));
        QVERIFY(editor.canUndo());

        const QString pastedUrl = QStringLiteral("https://example.org/local-field");
        QVERIFY(QMetaObject::invokeMethod(urlField, "selectAll"));
        QGuiApplication::clipboard()->setText(pastedUrl);
        QTest::keyClick(quick, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(urlField->property("text").toString(), pastedUrl);
        QCOMPARE(editor.contents(), QStringLiteral("title\n\nlinkx"));
        QTest::keyClick(quick, Qt::Key_Escape);
    }

    void undoRedoRestoresCompoundListAndTableOperations()
    {
        QmlNoteEditor editor;
        editor.resize(600, 400);
        editor.load(QStringLiteral("- first"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *listItem = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(listItem);
        listItem->setProperty("cursorPosition", 2);
        QVERIFY(QMetaObject::invokeMethod(listItem, "forceActiveFocus"));

        QTest::keyClick(quick, Qt::Key_Return);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "fi", "rst" }));
        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "first" }));
        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "fi", "rst" }));

        QVERIFY(QMetaObject::invokeMethod(root, "insertTableBlock"));
        QTRY_COMPARE(editor.model()->rowCount(), 2);
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Table));
        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.model()->rowCount(), 1);
        QTest::keyClick(quick, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(editor.model()->rowCount(), 2);
    }

    void undoRedoRestoresExplicitFormatConversion()
    {
        QmlNoteEditor editor;
        editor.load(QStringLiteral("- first\n- second"), Note::Markdown);
        editor.load(editor.contents(), Note::PlainText, NoteEditor::LoadPolicy::RecordFormatConversion);
        QTRY_VERIFY(!editor.isMarkdown());
        QVERIFY(editor.canUndo());

        QVERIFY(editor.undo());
        QTRY_VERIFY(editor.isMarkdown());
        QCOMPARE(editor.contents(), QStringLiteral("- first\n- second"));
        QVERIFY(editor.redo());
        QTRY_VERIFY(!editor.isMarkdown());
    }

    void plainLongListLineStaysOneItemAfterMarkdownConversion()
    {
        const QString item
            = QStringLiteral(
                  "a single long task line that visually wraps in a narrow editor but has no explicit line break ")
                  .repeated(4);
        QmlNoteEditor editor;
        editor.resize(260, 320);
        editor.load(QStringLiteral("- [ ] ") + item, Note::PlainText);
        editor.show();
        QTest::qWait(30);

        editor.load(editor.contents(), Note::Markdown, NoteEditor::LoadPolicy::RecordFormatConversion);
        QTRY_VERIFY(editor.isMarkdown());
        QCOMPARE(editor.model()->rowCount(), 1);
        const QStringList items
            = editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.constFirst().split(QLatin1Char('\n')).join(QLatin1Char(' ')).simplified(), item.simplified());

        const QStringList serializedLines = editor.contents().split(QLatin1Char('\n'));
        QVERIFY(serializedLines.size() > 1);
        QVERIFY(serializedLines.constFirst().startsWith(QStringLiteral("- [ ] ")));
        for (qsizetype index = 1; index < serializedLines.size(); ++index) {
            QVERIFY2(serializedLines.at(index).startsWith(QStringLiteral("      ")),
                     qPrintable(QStringLiteral("unindented continuation: %1").arg(serializedLines.at(index))));
        }
        QVERIFY(!editor.contents().contains(QStringLiteral("\n\n")));
    }

    void sharedToolbarCommandsUseOneFormatConversionTransaction()
    {
        QmlNoteEditor editor;
        editor.load(QString(), Note::PlainText);
        QVERIFY(!editor.isMarkdown());

        editor.beginExternalHistoryTransaction(QStringLiteral("insert-table"));
        editor.load(editor.contents(), Note::Markdown, NoteEditor::LoadPolicy::RecordFormatConversion);
        editor.insertTable();
        editor.endExternalHistoryTransaction();
        QTRY_VERIFY(editor.isMarkdown());
        QTRY_VERIFY(editor.model()->rowCount() > 1);

        QVERIFY(editor.undo());
        QTRY_VERIFY(!editor.isMarkdown());
        QCOMPARE(editor.model()->rowCount(), 1);
        QVERIFY(!editor.canUndo());
    }

    void undoRedoRestoresFormattingAndTableCellText()
    {
        QmlNoteEditor formatting;
        formatting.resize(500, 300);
        formatting.load(QStringLiteral("title\n\nbold text"), Note::Markdown);
        formatting.show();
        QTest::qWait(30);
        auto *formatQuick = formatting.findChild<QQuickWidget *>();
        auto *formatRoot  = formatQuick->rootObject();
        QTRY_COMPARE(formatRoot->property("editors").toList().size(), 1);
        auto *formatEditor   = formatRoot->property("editors").toList().constFirst().value<QObject *>();
        auto *formatDocument = formatEditor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(formatDocument);
        const int boldStart = formatDocument->textDocument()->toPlainText().indexOf(QStringLiteral("bold"));
        QVERIFY(boldStart > 0);
        QVERIFY(QMetaObject::invokeMethod(formatEditor, "forceActiveFocus"));
        QVERIFY(QMetaObject::invokeMethod(formatEditor, "select", Q_ARG(int, boldStart), Q_ARG(int, boldStart + 4)));
        QTest::keyClick(formatQuick, Qt::Key_B, Qt::ControlModifier);
        QTRY_COMPARE(formatting.contents(), QStringLiteral("title\n\n**bold** text"));
        QTest::keyClick(formatQuick, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(formatting.contents(), QStringLiteral("title\n\nbold text"));
        QTest::keyClick(formatQuick, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(formatting.contents(), QStringLiteral("title\n\n**bold** text"));
        // History restores focus asynchronously after delegates have been
        // recreated. Let that finish before opening a second editor window,
        // otherwise the first window can reclaim the application focus.
        QTest::qWait(20);
        formatting.hide();

        QmlNoteEditor table;
        table.resize(600, 400);
        table.load(QStringLiteral("| A | B |\n| --- | --- |\n| C | D |"), Note::Markdown);
        table.show();
        QTest::qWait(30);
        auto *tableQuick = table.findChild<QQuickWidget *>();
        auto *tableRoot  = tableQuick->rootObject();
        QTRY_COMPARE(tableRoot->property("editors").toList().size(), 4);
        QObject *cell = nullptr;
        for (const auto &value : tableRoot->property("editors").toList()) {
            auto *candidate = value.value<QObject *>();
            if (candidate && candidate->property("tableCellIndex").toInt() == 2) {
                cell = candidate;
                break;
            }
        }
        QVERIFY(cell);
        QVERIFY(QMetaObject::invokeMethod(cell, "forceActiveFocus"));
        QTRY_VERIFY(cell->property("activeFocus").toBool());
        cell->setProperty("cursorPosition", cell->property("length"));
        QTest::keyClicks(tableQuick, QStringLiteral("X"));
        QTRY_COMPARE(table.model()
                         ->data(table.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()
                         .value(QStringLiteral("values"))
                         .toStringList()
                         .at(2),
                     QStringLiteral("CX"));
        QTest::keyClick(tableQuick, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(table.model()
                         ->data(table.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()
                         .value(QStringLiteral("values"))
                         .toStringList()
                         .at(2),
                     QStringLiteral("C"));
        QVERIFY(table.redo());
        QTRY_COMPARE(table.model()
                         ->data(table.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()
                         .value(QStringLiteral("values"))
                         .toStringList()
                         .at(2),
                     QStringLiteral("CX"));
    }

    void undoRedoRestoresImageBlockAndMediaManifest()
    {
        QmlNoteEditor editor;
        editor.load(QString(), Note::Markdown);

        MediaReference image;
        image.id           = QUuid::createUuid();
        image.blobId       = QByteArray::fromHex("0123456789abcdef");
        image.originalName = QStringLiteral("cat.png");
        image.portableName = QStringLiteral("cat.png");
        image.mediaType    = QStringLiteral("image/png");

        QList<MediaReference> observedMedia;
        QObject::connect(&editor, &QmlNoteEditor::mediaChanged,
                         [&observedMedia](const QList<MediaReference> &media) { observedMedia = media; });

        editor.beginExternalHistoryTransaction(QStringLiteral("Insert image"));
        editor.setMedia({ image });
        editor.model()->appendImage(image.uri(), image.originalName);
        editor.endExternalHistoryTransaction();
        QCOMPARE(editor.model()->rowCount(), 2);
        QCOMPARE(observedMedia.size(), 1);

        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->rowCount(), 1);
        QCOMPARE(observedMedia.size(), 0);

        QVERIFY(editor.redo());
        QCOMPARE(editor.model()->rowCount(), 2);
        QCOMPARE(observedMedia.size(), 1);
        QCOMPARE(observedMedia.constFirst().id, image.id);
    }

    void tableCellRendersAndPreservesMarkdownLinks()
    {
        QmlNoteEditor editor;
        editor.resize(600, 400);
        editor.load(QStringLiteral("| A | B |\n| --- | --- |\n| before [title](https://link.example) after | D |"),
                    Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 4);
        QObject *cell = nullptr;
        for (const QVariant &value : root->property("editors").toList()) {
            QObject *candidate = value.value<QObject *>();
            if (candidate && candidate->property("tableCellIndex").toInt() == 2) {
                cell = candidate;
                break;
            }
        }
        QVERIFY(cell);
        auto *document = cell->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        const QVariantMap link = backend(editor)->linkInfo(document, 7, 12);
        QVERIFY(link.value(QStringLiteral("valid")).toBool());
        QCOMPARE(link.value(QStringLiteral("href")).toString(), QStringLiteral("https://link.example"));

        QVERIFY(QMetaObject::invokeMethod(cell, "forceActiveFocus"));
        cell->setProperty("cursorPosition", cell->property("length"));
        QTest::keyClicks(quick, QStringLiteral("!"));
        QTRY_COMPARE(editor.model()
                         ->data(editor.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()
                         .value(QStringLiteral("values"))
                         .toStringList()
                         .at(2),
                     QStringLiteral("before [title](https://link.example) after!"));
    }

    void rendersAndPreservesGithubUnderline()
    {
        QmlNoteEditor editor;
        editor.resize(600, 300);
        editor.load(QStringLiteral("before <ins>underlined</ins> after"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        QObject *text = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(text);
        auto *document = text->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("before underlined after"));

        QTextCursor cursor(document->textDocument());
        cursor.setPosition(7);
        cursor.setPosition(17, QTextCursor::KeepAnchor);
        QVERIFY(cursor.charFormat().fontUnderline());
        QCOMPARE(backend(editor)->markdownSelection(document, 7, 17), QStringLiteral("<ins>underlined</ins>"));

        QVERIFY(QMetaObject::invokeMethod(text, "forceActiveFocus"));
        text->setProperty("cursorPosition", text->property("length"));
        QTest::keyClicks(quick, QStringLiteral("!"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("before <ins>underlined</ins> after!"));
    }

    void acceptsUnderlineAliasInTableAndWritesGithubIns()
    {
        QmlNoteEditor editor;
        editor.resize(600, 300);
        editor.load(QStringLiteral("| A | B |\n| --- | --- |\n| <u>underlined</u> | plain |"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 4);
        QObject *cell = nullptr;
        for (const QVariant &value : root->property("editors").toList()) {
            QObject *candidate = value.value<QObject *>();
            if (candidate && candidate->property("tableCellIndex").toInt() == 2) {
                cell = candidate;
                break;
            }
        }
        QVERIFY(cell);
        auto *document = cell->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("underlined"));
        QTextCursor cursor(document->textDocument());
        cursor.setPosition(0);
        cursor.setPosition(10, QTextCursor::KeepAnchor);
        QVERIFY(cursor.charFormat().fontUnderline());

        QVERIFY(QMetaObject::invokeMethod(cell, "forceActiveFocus"));
        cell->setProperty("cursorPosition", cell->property("length"));
        QTest::keyClicks(quick, QStringLiteral("!"));
        QTRY_COMPARE(editor.model()
                         ->data(editor.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()
                         .value(QStringLiteral("values"))
                         .toStringList()
                         .at(2),
                     QStringLiteral("<ins>underlined!</ins>"));
    }

    void serializesUnderlineInsideLink()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("<ins>[underlined](https://example.org)</ins>"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *text = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(text);
        auto *document = text->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        QCOMPARE(backend(editor)->markdownText(document),
                 QStringLiteral("[<ins>underlined</ins>](https://example.org)"));
    }

    void leavesUnderlineTagsLiteralInsideCodeSpan()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("`<ins>literal</ins>`"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *text = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(text);
        auto *document = text->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("<ins>literal</ins>"));
        QTextCursor cursor(document->textDocument());
        cursor.setPosition(0);
        cursor.setPosition(18, QTextCursor::KeepAnchor);
        QVERIFY(!cursor.charFormat().fontUnderline());
        QVERIFY(cursor.charFormat().fontFixedPitch());
        QCOMPARE(backend(editor)->markdownText(document), QStringLiteral("`<ins>literal</ins>`"));
    }

    void tableCellRendersMarkdownHardBreaks()
    {
        QmlNoteEditor editor;
        editor.resize(600, 400);
        editor.load(QStringLiteral("| A | B |\n| --- | --- |\n| one<br>two | D |"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 4);
        QObject *cell = nullptr;
        for (const QVariant &value : root->property("editors").toList()) {
            QObject *candidate = value.value<QObject *>();
            if (candidate && candidate->property("tableCellIndex").toInt() == 2) {
                cell = candidate;
                break;
            }
        }
        QVERIFY(cell);
        auto *document = cell->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("one\ntwo"));
        QCOMPARE(document->textDocument()->blockCount(), 1);
    }

    void tableCellHardBreaksSurviveCrossCellFocusChanges()
    {
        QmlNoteEditor editor;
        editor.resize(600, 400);
        editor.load(QStringLiteral("| A | B |\n| --- | --- |\n| left | first |"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 4);
        QObject *left  = nullptr;
        QObject *right = nullptr;
        for (const QVariant &value : root->property("editors").toList()) {
            QObject *candidate = value.value<QObject *>();
            if (!candidate)
                continue;
            if (candidate->property("tableCellIndex").toInt() == 2)
                left = candidate;
            else if (candidate->property("tableCellIndex").toInt() == 3)
                right = candidate;
        }
        QVERIFY(left);
        QVERIFY(right);
        auto *document = right->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);

        QVERIFY(QMetaObject::invokeMethod(right, "forceActiveFocus"));
        right->setProperty("cursorPosition", right->property("length"));
        QTest::keyClick(quick, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(quick, QStringLiteral("second"));
        QTRY_COMPARE(editor.model()
                         ->data(editor.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()
                         .value(QStringLiteral("values"))
                         .toStringList()
                         .at(3),
                     QStringLiteral("first\nsecond"));

        QVERIFY(QMetaObject::invokeMethod(left, "forceActiveFocus"));
        QTRY_COMPARE(document->textDocument()->toPlainText(), QStringLiteral("first\nsecond"));

        QVERIFY(QMetaObject::invokeMethod(right, "forceActiveFocus"));
        right->setProperty("cursorPosition", right->property("length"));
        QTest::keyClick(quick, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(quick, QStringLiteral("third"));
        QTRY_VERIFY(!right->property("sourceTextPending").toBool());

        auto geometry = [root](int index) {
            QVariant result;
            QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, index));
            return result.toMap();
        };
        const auto leftGeometry  = geometry(2);
        const auto rightGeometry = geometry(3);
        QVERIFY(!leftGeometry.isEmpty());
        QVERIFY(!rightGeometry.isEmpty());
        const QPoint dragStart(
            rightGeometry[QStringLiteral("x")].toInt() + rightGeometry[QStringLiteral("width")].toInt() - 6,
            rightGeometry[QStringLiteral("y")].toInt() + rightGeometry[QStringLiteral("height")].toInt() - 4);
        const QPoint dragEnd(
            leftGeometry[QStringLiteral("x")].toInt() + leftGeometry[QStringLiteral("width")].toInt() - 6,
            leftGeometry[QStringLiteral("y")].toInt() + leftGeometry[QStringLiteral("height")].toInt() / 2);
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, dragStart);
        QTest::mouseMove(quick, dragEnd, 30);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, dragEnd);

        QTRY_VERIFY(root->property("selectionSpansEditors").toBool());
        QTRY_COMPARE(document->textDocument()->toPlainText(), QStringLiteral("first\nsecond\nthird"));
        QCOMPARE(document->textDocument()->blockCount(), 1);
    }

    void backspaceDeletesCrossCellSelectionInOneTableRow()
    {
        QmlNoteEditor editor;
        editor.resize(600, 400);
        editor.load(QStringLiteral("| A | B |\n| --- | --- |\n| left | right |"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 4);
        QObject *left  = nullptr;
        QObject *right = nullptr;
        for (const QVariant &value : root->property("editors").toList()) {
            QObject *candidate = value.value<QObject *>();
            if (!candidate)
                continue;
            if (candidate->property("tableCellIndex").toInt() == 2)
                left = candidate;
            else if (candidate->property("tableCellIndex").toInt() == 3)
                right = candidate;
        }
        QVERIFY(left);
        QVERIFY(right);
        QVERIFY(QMetaObject::invokeMethod(left, "forceActiveFocus"));
        const int leftEnd  = left->property("length").toInt();
        const int rightEnd = right->property("length").toInt();
        QVERIFY(QMetaObject::invokeMethod(left, "select", Q_ARG(int, leftEnd), Q_ARG(int, leftEnd)));
        QVERIFY(QMetaObject::invokeMethod(right, "select", Q_ARG(int, 0), Q_ARG(int, rightEnd)));
        root->setProperty("activeEditor", QVariant::fromValue(left));
        root->setProperty("selectionSpansEditors", true);
        root->setProperty("documentSelectionStartEditor", QVariant::fromValue(left));
        root->setProperty("documentSelectionStartPosition", leftEnd);
        root->setProperty("documentSelectionEndEditor", QVariant::fromValue(right));
        root->setProperty("documentSelectionEndPosition", rightEnd);
        root->setProperty("documentSelectionAvailable", true);

        QTest::keyClick(quick, Qt::Key_Backspace);

        QTRY_COMPARE(editor.model()
                         ->data(editor.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()
                         .value(QStringLiteral("values"))
                         .toStringList(),
                     QStringList({ "A", "B", "left", "" }));
    }

    void imageCanBeDeletedAndCursorCanContinueAfterIt()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("before\n\n![cat](media://cat)"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *before = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(before);
        QVERIFY(QMetaObject::invokeMethod(before, "forceActiveFocus"));
        before->setProperty("cursorPosition", before->property("length"));

        QTest::keyClick(quick, Qt::Key_Delete);
        QTRY_COMPARE(editor.contents(), QStringLiteral("before"));
        QCOMPARE(editor.model()->rowCount(), 1);
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.contents(), QStringLiteral("before\n\n![cat](media://cat)"));

        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        before = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(before, "forceActiveFocus"));
        before->setProperty("cursorPosition", before->property("length"));
        QTest::keyClick(quick, Qt::Key_Down);

        QTRY_COMPARE(editor.model()->rowCount(), 3);
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("blockIndex").toInt(), 2);
        QTest::keyClicks(quick, QStringLiteral("after"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("before\n\n![cat](media://cat)\n\nafter"));
    }

    void backspaceAndDeleteMergeAdjacentTextBlocks()
    {
        const auto prepare = [](QmlNoteEditor &editor) {
            editor.resize(500, 350);
            editor.load(QStringLiteral("first"), Note::Markdown);
            editor.model()->appendTextBlock();
            editor.model()->setBlockText(1, QStringLiteral("second"));
            editor.show();
            QTest::qWait(30);
        };
        const auto editorForBlock = [](QObject *root, int blockIndex) -> QObject * {
            for (const QVariant &value : root->property("editors").toList()) {
                QObject *candidate = value.value<QObject *>();
                if (candidate && candidate->property("blockIndex").toInt() == blockIndex)
                    return candidate;
            }
            return nullptr;
        };

        QmlNoteEditor backspaceEditor;
        prepare(backspaceEditor);
        auto *backspaceQuick = backspaceEditor.findChild<QQuickWidget *>();
        auto *backspaceRoot  = backspaceQuick->rootObject();
        QTRY_COMPARE(backspaceRoot->property("editors").toList().size(), 2);
        auto *second = editorForBlock(backspaceRoot, 1);
        QVERIFY(second);
        QVERIFY(QMetaObject::invokeMethod(second, "forceActiveFocus"));
        second->setProperty("cursorPosition", 0);
        QTest::keyClick(backspaceQuick, Qt::Key_Backspace);
        QTRY_COMPARE(backspaceEditor.contents(), QStringLiteral("firstsecond"));
        QCOMPARE(backspaceEditor.model()->rowCount(), 1);
        QTRY_COMPARE(backspaceRoot->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 5);

        QmlNoteEditor deleteEditor;
        prepare(deleteEditor);
        auto *deleteQuick = deleteEditor.findChild<QQuickWidget *>();
        auto *deleteRoot  = deleteQuick->rootObject();
        QTRY_COMPARE(deleteRoot->property("editors").toList().size(), 2);
        auto *first = editorForBlock(deleteRoot, 0);
        QVERIFY(first);
        QVERIFY(QMetaObject::invokeMethod(first, "forceActiveFocus"));
        first->setProperty("cursorPosition", first->property("length"));
        QTest::keyClick(deleteQuick, Qt::Key_Delete);
        QTRY_COMPARE(deleteEditor.contents(), QStringLiteral("firstsecond"));
        QCOMPARE(deleteEditor.model()->rowCount(), 1);
        QTRY_COMPARE(deleteRoot->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 5);
    }

    void keyboardCursorScrollsOuterStructuredEditor()
    {
        QmlNoteEditor editor;
        editor.resize(500, 250);
        editor.load(QStringLiteral("a long line ").repeated(800), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        auto *active = root->property("activeEditor").value<QObject *>();

        QTest::keyClick(quick, Qt::Key_End, Qt::ControlModifier);
        QTRY_COMPARE(active->property("cursorPosition").toInt(), active->property("length").toInt());
        QTRY_VERIFY(root->property("contentY").toReal() > root->property("originY").toReal() + 1.0);
    }

    void routesClipboardImages()
    {
        QmlNoteEditor editor;
        auto          quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QSignalSpy spy(&editor, &QmlNoteEditor::imagePasteRequested);
        QImage     image(3, 2, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        QGuiApplication::clipboard()->setImage(image);

        QTest::keyClick(quick, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(qvariant_cast<QImage>(spy.first().first()).size(), QSize(3, 2));
    }

    void acceptsBitmapAndLocalImageDrops()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("first\n\nsecond"), Note::Markdown);
        editor.setImageInsertionEnabled(true);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        const int    initialRows = editor.model()->rowCount();
        const QPoint dropPoint(quick->width() / 2, quick->height() - 12);
        QVariant     expectedRowValue;
        QVERIFY(QMetaObject::invokeMethod(root, "insertionRowAtPoint", Q_RETURN_ARG(QVariant, expectedRowValue),
                                          Q_ARG(QVariant, dropPoint.x()), Q_ARG(QVariant, dropPoint.y())));
        const int expectedRow = expectedRowValue.toInt();

        QImage bitmap(4, 3, QImage::Format_ARGB32_Premultiplied);
        bitmap.fill(Qt::red);
        QMimeData bitmapMime;
        bitmapMime.setImageData(bitmap);
        QSignalSpy      bitmapDrop(&editor, &QmlNoteEditor::imageDropRequested);
        QDragEnterEvent bitmapEnter(dropPoint, Qt::CopyAction, &bitmapMime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(quick, &bitmapEnter);
        QVERIFY(bitmapEnter.isAccepted());
        QDropEvent bitmapEvent(QPointF(dropPoint), Qt::CopyAction, &bitmapMime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(quick, &bitmapEvent);
        QVERIFY(bitmapEvent.isAccepted());
        QCOMPARE(bitmapDrop.size(), 1);
        QCOMPARE(qvariant_cast<QImage>(bitmapDrop.constFirst().at(0)).size(), QSize(4, 3));
        QCOMPARE(bitmapDrop.constFirst().at(1).toInt(), expectedRow);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString fileName = directory.filePath(QStringLiteral("dropped.png"));
        QVERIFY(bitmap.save(fileName));
        QMimeData fileMime;
        fileMime.setUrls({ QUrl::fromLocalFile(fileName) });
        QSignalSpy      fileDrop(&editor, &QmlNoteEditor::imageFilesDropRequested);
        QDragEnterEvent fileEnter(dropPoint, Qt::CopyAction, &fileMime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(quick, &fileEnter);
        QVERIFY(fileEnter.isAccepted());
        QDropEvent fileEvent(QPointF(dropPoint), Qt::CopyAction, &fileMime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(quick, &fileEvent);
        QVERIFY(fileEvent.isAccepted());
        QCOMPARE(fileDrop.size(), 1);
        QCOMPARE(fileDrop.constFirst().at(0).toStringList(), QStringList({ fileName }));
        QCOMPARE(fileDrop.constFirst().at(1).toInt(), expectedRow);

        NoteFragment fragment;
        fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
        NoteFragmentBlock imageBlock;
        imageBlock.type            = NoteFragmentBlockType::Image;
        imageBlock.image.alt       = QStringLiteral("dragged");
        imageBlock.image.sourceUri = QUrl::fromLocalFile(fileName).toString();
        fragment.blocks.append(imageBlock);
        NoteTransferController controller;
        auto                   exported = controller.createMimeData(fragment);
        QVERIFY2(exported, qPrintable(exported.error));
        QDragEnterEvent internalEnter(dropPoint, Qt::CopyAction, exported.mimeData.get(), Qt::LeftButton,
                                      Qt::NoModifier);
        QCoreApplication::sendEvent(quick, &internalEnter);
        QVERIFY(internalEnter.isAccepted());
        QDropEvent internalEvent(QPointF(dropPoint), Qt::CopyAction, exported.mimeData.get(), Qt::LeftButton,
                                 Qt::NoModifier);
        QCoreApplication::sendEvent(quick, &internalEvent);
        QVERIFY(internalEvent.isAccepted());
        QTRY_COMPARE(editor.model()->rowCount(), initialRows + 1);
        QCOMPARE(editor.model()->blockTypeAt(expectedRow), int(NoteBlockModel::Image));
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.model()->rowCount(), initialRows);
    }

    void acceptsKeyboardInput()
    {
        QmlNoteEditor editor;
        editor.resize(400, 300);
        editor.load(QString(), Note::PlainText);
        editor.show();
        QTest::qWait(30);
        auto quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QVERIFY(quick->rootObject());
        QVERIFY(quick->rootObject()->width() > 0);
        QVERIFY(quick->rootObject()->height() > 0);
        editor.setFocus(Qt::OtherFocusReason);
        QTest::qWait(20);
        auto activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        QVERIFY(activeEditor);
        QVERIFY(activeEditor->property("activeFocus").toBool());

        // The final text block fills the viewport, so an empty note behaves as
        // one continuous editor even though it is structurally a block list.
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, QPoint(30, 250));
        QTest::keyClicks(quick, QStringLiteral("typed"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("typed"));
    }

    void clickBelowLastBlockFocusesDocumentEnd()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("short text"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        auto *active = root->property("activeEditor").value<QObject *>();
        active->setProperty("cursorPosition", 0);

        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, QPoint(quick->width() / 2, quick->height() - 10));

        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(),
                     active->property("length").toInt());
    }

    void invokesQmlDocumentHighlighters()
    {
        QmlNoteEditor editor;
        editor.resize(400, 300);
        editor.show();
        editor.load(QStringLiteral("misspelled"), Note::PlainText);
        auto extension = std::make_shared<CountingHighlighter>();
        editor.addHighlightExtension(extension, int(NoteHighlighter::SpellCheck));
        editor.rehighlight();
        editor.focusEditor();
        QTRY_VERIFY(extension->calls > 0);
        auto quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());
        auto activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        auto document     = activeEditor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        const int callsBeforeRanges = extension->calls;
        QCOMPARE(editor.spellCheckRanges(document).size(), 1);
        QCOMPARE(extension->calls, callsBeforeRanges);
        editor.setSpellCheckEnabled(false);
        QVERIFY(editor.spellCheckRanges(document).isEmpty());
        editor.setSpellCheckEnabled(true);
        QCOMPARE(editor.spellCheckRanges(document).size(), 1);
        QCOMPARE(editor.spellingSuggestions(QStringLiteral("wrong")), QStringList { QStringLiteral("wrong-fixed") });
        editor.addToSpellingDictionary(QStringLiteral("custom"));
        QCOMPARE(extension->addedWord, QStringLiteral("custom"));
    }

    void selectsCopiesAndCutsWholeBlockDocument()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("first paragraph\n\nsecond paragraph"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);

        QVERIFY(QMetaObject::invokeMethod(root, "selectAllDocument"));
        QVariant selected;
        QVERIFY(QMetaObject::invokeMethod(root, "selectedDocumentText", Q_RETURN_ARG(QVariant, selected)));
        QCOMPARE(selected.toString(), editor.contents());

        QVERIFY(QMetaObject::invokeMethod(root, "copyDocumentSelection"));
        QCOMPARE(QGuiApplication::clipboard()->text(), editor.contents());
        QVERIFY(QGuiApplication::clipboard()->mimeData()->hasFormat(
            QString::fromLatin1(NoteTransferController::FragmentMimeType)));

        QVERIFY(QMetaObject::invokeMethod(root, "cutDocumentSelection"));
        QTRY_COMPARE(editor.contents(), QString());
    }

    void deleteRemovesWholeSelectedDocumentIncludingTable()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("before\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\nafter"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QVERIFY(QMetaObject::invokeMethod(root, "selectAllDocument"));

        QTest::keyClick(quick, Qt::Key_Delete);

        QTRY_COMPARE(editor.contents(), QString());
        QCOMPARE(editor.model()->rowCount(), 1);
        QCOMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Text));
    }

    void partialCopyPreservesMarkdownFormatting()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("**bold** text"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());
        auto *activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        auto *document     = activeEditor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);

        const QString markdown = backend(editor)->markdownSelection(document, 0, 4);
        QCOMPARE(markdown, QStringLiteral("**bold**"));
        backend(editor)->copyMarkdownToClipboard(markdown);
        QCOMPARE(QString::fromUtf8(QGuiApplication::clipboard()->mimeData()->data(
                     QString::fromLatin1(NoteTransferController::MarkdownMimeType))),
                 QStringLiteral("**bold**"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("**bold**"));
    }

    void keyboardCopyUsesLiveSelectionState()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("**bold**"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        auto *activeEditor = root->property("activeEditor").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(activeEditor, "select", Q_ARG(int, 0),
                                          Q_ARG(int, activeEditor->property("length").toInt())));
        // Reproduce Ctrl+C arriving before selectionStateRefresh's zero-delay
        // timer has updated the cached property.
        root->setProperty("documentSelectionAvailable", false);
        QTest::keyClick(quick, Qt::Key_C, Qt::ControlModifier);

        const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mime->hasFormat(QString::fromLatin1(NoteTransferController::FragmentMimeType)));
        QCOMPARE(QString::fromUtf8(mime->data(QString::fromLatin1(NoteTransferController::MarkdownMimeType))),
                 QStringLiteral("**bold**"));
    }

    void copyingOneListItemPublishesSerializedMarkdown()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("- [x] completed task"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *listItem = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(listItem);
        QVERIFY(QMetaObject::invokeMethod(listItem, "select", Q_ARG(int, 0),
                                          Q_ARG(int, listItem->property("length").toInt())));
        QTest::keyClick(quick, Qt::Key_C, Qt::ControlModifier);

        const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mime);
        QVERIFY(mime->hasFormat(QString::fromLatin1(NoteTransferController::MarkdownMimeType)));
        QCOMPARE(QString::fromUtf8(mime->data(QString::fromLatin1(NoteTransferController::MarkdownMimeType))),
                 QStringLiteral("- [x] completed task"));
        QCOMPARE(mime->text(), QStringLiteral("- [x] completed task"));
    }

    void listMarkersShareOneColumnAndAlignWithFirstLine()
    {
        QmlNoteEditor editor;
        editor.resize(280, 400);
        editor.load(QStringLiteral("- a bullet item long enough to wrap over several visual lines in this narrow editor"
                                   "\n\n- [ ] task\n\n1. numbered"),
                    Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *rootObject = quick->rootObject();
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 3);

        const QVariantList  editors = rootObject->property("editors").toList();
        QList<QQuickItem *> markers;
        for (const QVariant &value : editors) {
            auto *textItem = qobject_cast<QQuickItem *>(value.value<QObject *>());
            QVERIFY(textItem);
            QQuickItem *marker = listMarker(rootObject, textItem);
            QVERIFY(marker);
            markers.append(marker);
        }
        const qreal markerX = markers.constFirst()->mapToItem(root, QPointF()).x();
        for (QQuickItem *marker : markers) {
            QCOMPARE(marker->mapToItem(root, QPointF()).x(), markerX);
            QCOMPARE(marker->width(), markers.constFirst()->width());
        }

        for (int block = 0; block < editors.size(); ++block) {
            auto *textItem = qobject_cast<QQuickItem *>(editors.at(block).value<QObject *>());
            QVERIFY(textItem);
            QCOMPARE(markers.at(block)->mapToItem(root, QPointF()).y(), textItem->mapToItem(root, QPointF()).y());
        }
        auto *wrappedText = qobject_cast<QQuickItem *>(editors.constFirst().value<QObject *>());
        QVERIFY(wrappedText);
        QVERIFY(wrappedText->height() > markers.constFirst()->height());

        auto *bulletGlyph = quickItemByName(root, QStringLiteral("listGlyph-0-0"));
        auto *taskControl = quickItemByName(root, QStringLiteral("taskMarker-1-0"));
        auto *numberGlyph = quickItemByName(root, QStringLiteral("listGlyph-2-0"));
        QVERIFY(bulletGlyph);
        QVERIFY(taskControl);
        QVERIFY(numberGlyph);
        auto *taskIndicator = qobject_cast<QQuickItem *>(taskControl->property("indicator").value<QObject *>());
        QVERIFY(taskIndicator);
        const qreal indicatorCenter = taskIndicator->mapToItem(root, QPointF(taskIndicator->width() / 2, 0)).x();
        const qreal bulletCenter    = bulletGlyph->mapToItem(root, QPointF(bulletGlyph->width() / 2, 0)).x();
        const qreal numberCenter    = numberGlyph->mapToItem(root, QPointF(numberGlyph->width() / 2, 0)).x();
        QVERIFY(qAbs(bulletCenter - indicatorCenter) <= 1);
        QVERIFY(qAbs(numberCenter - indicatorCenter) <= 1);

        auto *taskText = qobject_cast<QQuickItem *>(editors.at(1).value<QObject *>());
        QVERIFY(taskText);
        const qreal indicatorLeft   = taskIndicator->mapToItem(root, QPointF(0, 0)).x();
        const qreal indicatorRight  = taskIndicator->mapToItem(root, QPointF(taskIndicator->width(), 0)).x();
        const qreal visibleTextLeft = taskText->mapToItem(root, QPointF(taskText->property("leftPadding").toReal(), 0)).x();
        const qreal averageCharacterWidth = rootObject->property("editorFontAverageCharacterWidth").toReal();
        const qreal markerGap             = visibleTextLeft - indicatorRight;
        QVERIFY(markerGap >= 0);
        QVERIFY(markerGap <= averageCharacterWidth * 2);
    }

    void listItemsKeepReadableVerticalSpacing()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] first\n- [ ] second"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick ? qobject_cast<QQuickItem *>(quick->rootObject()) : nullptr;
        QVERIFY(root);
        QTRY_COMPARE(quick->rootObject()->property("editors").toList().size(), 2);

        auto *first  = quickItemByName(root, QStringLiteral("listMarker-0-0"));
        auto *second = quickItemByName(root, QStringLiteral("listMarker-0-1"));
        QVERIFY(first);
        QVERIFY(second);
        const qreal firstBottom = first->mapToItem(root, QPointF(0, first->height())).y();
        const qreal secondTop   = second->mapToItem(root, QPointF(0, 0)).y();
        QVERIFY2(secondTop - firstBottom >= 5,
                 qPrintable(QStringLiteral("list item gap is only %1 px").arg(secondTop - firstBottom)));
    }

    void draggingListMarkerReordersItemsAsOneHistoryOperation()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("- first\n- second\n- third"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 3);

        const QVariantList editors     = rootObject->property("editors").toList();
        auto              *firstMarker = listMarker(rootObject, editors.at(0).value<QObject *>());
        auto              *lastMarker  = listMarker(rootObject, editors.at(2).value<QObject *>());
        QVERIFY(firstMarker);
        QVERIFY(lastMarker);

        const QPointF startPoint = firstMarker->mapToItem(root, QPointF(firstMarker->width() / 2, firstMarker->height() / 2));
        const QPointF endPoint   = lastMarker->mapToItem(root, QPointF(lastMarker->width() / 2, lastMarker->height()));
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, startPoint.toPoint());
        for (int step = 1; step <= 8; ++step) {
            const QPointF point = startPoint + (endPoint - startPoint) * (qreal(step) / 8);
            QTest::mouseMove(quick, point.toPoint(), 15);
        }
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, endPoint.toPoint());

        QTRY_COMPARE(editor.contents(), QStringLiteral("- second\n- third\n- first"));
        auto *droppedMarker = quickItemByName(root, QStringLiteral("listMarker-0-2"));
        QVERIFY(droppedMarker);
        for (int index = 0; index < 3; ++index) {
            auto *row = quickItemByName(root, QStringLiteral("listRow-0-%1").arg(index));
            QVERIFY(row);
            QVERIFY(qAbs(row->property("collapseSpace").toReal()) < 0.01);
            QVERIFY(qAbs(row->property("dropSpace").toReal()) < 0.01);
        }
        const qreal droppedY = droppedMarker->mapToItem(root, QPointF()).y();
        QTest::qWait(220);
        QVERIFY(qAbs(droppedMarker->mapToItem(root, QPointF()).y() - droppedY) < 0.5);
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.contents(), QStringLiteral("- first\n- second\n- third"));
    }

    void repeatedlyDraggingFocusedTaskItemKeepsVisibleTextsInSync()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("test\n\n- [x] 1\n- [x] 2\n- [ ] 3"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 4);

        const auto visibleListTexts = [root]() {
            QStringList texts;
            for (int index = 0; index < 3; ++index) {
                auto *row = quickItemByName(root, QStringLiteral("listRow-1-%1").arg(index));
                if (!row)
                    return QStringList {};
                auto *cell = row->property("listEditor").value<QObject *>();
                if (!cell)
                    return QStringList {};
                texts.append(cell->property("text").toString().trimmed());
            }
            return texts;
        };

        const QList<QStringList> expectedTexts {
            { QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("1") },
            { QStringLiteral("3"), QStringLiteral("1"), QStringLiteral("2") },
            { QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3") },
        };
        const QStringList expectedContents {
            QStringLiteral("test\n\n- [x] 2\n- [ ] 3\n- [x] 1"),
            QStringLiteral("test\n\n- [ ] 3\n- [x] 1\n- [x] 2"),
            QStringLiteral("test\n\n- [x] 1\n- [x] 2\n- [ ] 3"),
        };

        for (int iteration = 0; iteration < expectedTexts.size(); ++iteration) {
            auto *firstRow = quickItemByName(root, QStringLiteral("listRow-1-0"));
            auto *first    = quickItemByName(root, QStringLiteral("listMarker-1-0"));
            auto *last     = quickItemByName(root, QStringLiteral("listMarker-1-2"));
            QVERIFY(firstRow);
            QVERIFY(first);
            QVERIFY(last);
            auto *focusedCell = firstRow->property("listEditor").value<QObject *>();
            QVERIFY(focusedCell);
            QVERIFY(QMetaObject::invokeMethod(focusedCell, "forceActiveFocus"));

            const QPointF start = first->mapToItem(root, QPointF(first->width() / 2, first->height() / 2));
            const QPointF end   = last->mapToItem(root, QPointF(last->width() / 2, last->height()));
            QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());
            for (int step = 1; step <= 8; ++step)
                QTest::mouseMove(quick, (start + (end - start) * (qreal(step) / 8)).toPoint(), 15);
            QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, end.toPoint());

            QTRY_COMPARE(editor.contents(), expectedContents.at(iteration));
            QTRY_COMPARE(visibleListTexts(), expectedTexts.at(iteration));
            QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("listItemIndex").toInt(), 2);
        }
    }

    void droppingListItemKeepsDisplacedRowsStationary()
    {
        QmlNoteEditor editor;
        editor.resize(500, 420);
        editor.load(QStringLiteral("- first\n- second\n- third\n- fourth"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 4);

        auto *sourceMarker = quickItemByName(root, QStringLiteral("listMarker-0-0"));
        auto *thirdMarker  = quickItemByName(root, QStringLiteral("listMarker-0-2"));
        auto *fourthMarker = quickItemByName(root, QStringLiteral("listMarker-0-3"));
        auto *controller   = quickItemByName(root, QStringLiteral("editorReorderController"));
        QVERIFY(sourceMarker);
        QVERIFY(thirdMarker);
        QVERIFY(fourthMarker);
        QVERIFY(controller);

        const QPointF start  = sourceMarker->mapToItem(root, QPointF(sourceMarker->width() / 2, sourceMarker->height() / 2));
        const QPointF target = thirdMarker->mapToItem(root, QPointF(thirdMarker->width() / 2, 1));
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(quick, (start + (target - start) * (qreal(step) / 8)).toPoint(), 15);
        QTRY_VERIFY(controller->property("dragging").toBool());
        QTRY_COMPARE(controller->property("targetItem").toInt(), 2);

        const qreal fourthYBeforeDrop = fourthMarker->mapToItem(root, QPointF()).y();
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, target.toPoint());
        QTRY_COMPARE(editor.contents(), QStringLiteral("- second\n- third\n- first\n- fourth"));

        auto *fourthAfterDrop = quickItemByName(root, QStringLiteral("listMarker-0-3"));
        QVERIFY(fourthAfterDrop);
        const qreal fourthYAfterDrop = fourthAfterDrop->mapToItem(root, QPointF()).y();
        QVERIFY2(qAbs(fourthYAfterDrop - fourthYBeforeDrop) < 0.5,
                 qPrintable(QStringLiteral("fourth item moved on drop: before=%1 after=%2")
                                .arg(fourthYBeforeDrop)
                                .arg(fourthYAfterDrop)));
        QTest::qWait(220);
        QVERIFY(qAbs(fourthAfterDrop->mapToItem(root, QPointF()).y() - fourthYAfterDrop) < 0.5);
    }

    void draggingParentListItemMovesItsDescendantsTogether()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("- parent\n    - child\n- middle\n- tail"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 4);

        const QVariantList editors      = rootObject->property("editors").toList();
        auto              *parentMarker = listMarker(rootObject, editors.at(0).value<QObject *>());
        auto              *middleMarker = listMarker(rootObject, editors.at(2).value<QObject *>());
        auto              *tailMarker   = listMarker(rootObject, editors.at(3).value<QObject *>());
        auto              *middleRow    = quickItemByName(root, QStringLiteral("listRow-0-2"));
        auto              *controller   = quickItemByName(root, QStringLiteral("editorReorderController"));
        QVERIFY(parentMarker);
        QVERIFY(middleMarker);
        QVERIFY(tailMarker);
        QVERIFY(middleRow);
        QVERIFY(controller);

        const QPointF startPoint = parentMarker->mapToItem(root, QPointF(parentMarker->width() / 2, parentMarker->height() / 2));
        const QPointF endPoint   = tailMarker->mapToItem(root, QPointF(tailMarker->width() / 2, tailMarker->height()));
        const qreal   middleY    = middleMarker->mapToItem(root, QPointF()).y();
        const qreal   middleHeight = middleRow->property("naturalHeight").toReal();
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, startPoint.toPoint());
        QTest::mouseMove(quick, (startPoint + QPointF(12, 0)).toPoint(), 15);
        QTRY_VERIFY(controller->property("dragging").toBool());
        QVERIFY(controller->property("targetByDraggedTop").toBool());
        QVERIFY(controller->property("startDraggedTopY").toReal() < controller->property("startPointerY").toReal());

        QTest::mouseMove(quick, (startPoint + QPointF(0, middleHeight * 0.4)).toPoint(), 15);
        QTRY_COMPARE(controller->property("targetItem").toInt(), 0);
        QTest::qWait(220);
        QVERIFY(qAbs(middleMarker->mapToItem(root, QPointF()).y() - middleY) < 0.5);

        QTest::mouseMove(quick, (startPoint + QPointF(0, middleHeight * 0.6)).toPoint(), 15);
        QTRY_COMPARE(controller->property("targetItem").toInt(), 1);
        QTRY_VERIFY(middleMarker->mapToItem(root, QPointF()).y() < middleY - 1);
        for (int step = 3; step <= 10; ++step) {
            const QPointF point = startPoint + (endPoint - startPoint) * (qreal(step) / 10);
            QTest::mouseMove(quick, point.toPoint(), 15);
        }
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, endPoint.toPoint());

        QTRY_COMPARE(editor.contents(), QStringLiteral("- middle\n- tail\n- parent\n    - child"));
    }

    void draggingListItemAcrossBlocksTracksPointerAndUsesOneHistoryStep()
    {
        QmlNoteEditor editor;
        editor.resize(500, 500);
        editor.load(QStringLiteral("- source\n\nbetween\n\n- target\n- tail"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 4);

        auto *sourceMarker = quickItemByName(root, QStringLiteral("listMarker-0-0"));
        auto *targetMarker = quickItemByName(root, QStringLiteral("listMarker-2-0"));
        auto *controller   = quickItemByName(root, QStringLiteral("editorReorderController"));
        QVERIFY(sourceMarker);
        QVERIFY(targetMarker);
        QVERIFY(controller);

        auto *sourceRow = quickItemByName(root, QStringLiteral("listRow-0-0"));
        QVERIFY(sourceRow);
        auto *sourceContent = qobject_cast<QQuickItem *>(sourceRow->property("dragContent").value<QObject *>());
        QVERIFY(sourceContent);
        const QPointF sourceContentOrigin = sourceContent->mapToItem(root, QPointF());
        const QPointF start = sourceMarker->mapToItem(root, QPointF(sourceMarker->width() / 2, sourceMarker->height() / 2));
        const QPointF initialTarget = targetMarker->mapToItem(root, QPointF(targetMarker->width() / 2, 1));
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        for (int step = 1; step <= 12; ++step)
            QTest::mouseMove(quick, (start + (initialTarget - start) * (qreal(step) / 12)).toPoint(), 15);
        QTest::qWait(180);
        const QPointF target = targetMarker->mapToItem(root, QPointF(targetMarker->width() / 2, 1));
        QTest::mouseMove(quick, target.toPoint(), 15);

        auto *preview = quickItemByName(root, QStringLiteral("listDragPreview-0"));
        QTRY_VERIFY(preview);
        QVERIFY(preview->isVisible());
        QVERIFY(preview->width() > 0);
        QVERIFY(preview->height() > 0);
        const QPointF expectedPreviewOrigin = sourceContentOrigin + target - start;
        QTRY_VERIFY((preview->mapToItem(root, QPointF()) - expectedPreviewOrigin).manhattanLength() < 3);
        auto *targetBlock = controller->property("targetBlock").value<QObject *>();
        QVERIFY(targetBlock);
        QCOMPARE(targetBlock->property("blockIndex").toInt(), 2);
        QCOMPARE(controller->property("targetItem").toInt(), 0);
        auto *targetRow = quickItemByName(root, QStringLiteral("listRow-2-0"));
        QVERIFY(targetRow);
        QTRY_VERIFY(targetRow->property("dropSpace").toReal() > 0);
        QTRY_VERIFY(qAbs(targetRow->property("dropSpace").toReal() - controller->property("draggedHeight").toReal())
                    < 0.5);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, target.toPoint());

        QTRY_COMPARE(editor.contents(), QStringLiteral("between\n\n- source\n- target\n- tail"));
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.contents(), QStringLiteral("- source\n\nbetween\n\n- target\n- tail"));
    }

    void scrollingWhileDraggingKeepsPreviewUnderPointer()
    {
        QStringList lines;
        for (int index = 0; index < 18; ++index)
            lines.append(QStringLiteral("- item %1").arg(index));

        QmlNoteEditor editor;
        editor.resize(400, 240);
        editor.load(lines.join(QLatin1Char('\n')), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), lines.size());
        QTRY_VERIFY(rootObject->property("contentHeight").toReal() > root->height());

        rootObject->setProperty("contentY", 70.0);
        QTRY_COMPARE(rootObject->property("contentY").toReal(), 70.0);
        auto *sourceMarker = quickItemByName(root, QStringLiteral("listMarker-0-4"));
        auto *sourceRow    = quickItemByName(root, QStringLiteral("listRow-0-4"));
        auto *controller   = quickItemByName(root, QStringLiteral("editorReorderController"));
        QVERIFY(sourceMarker);
        QVERIFY(sourceRow);
        QVERIFY(controller);
        auto *sourceContent = qobject_cast<QQuickItem *>(sourceRow->property("dragContent").value<QObject *>());
        QVERIFY(sourceContent);

        const QPointF start   = sourceMarker->mapToItem(root, QPointF(sourceMarker->width() / 2, sourceMarker->height() / 2));
        const QPointF pointer = start + QPointF(0, 16);
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        QTest::mouseMove(quick, pointer.toPoint(), 15);
        QTRY_VERIFY(controller->property("dragging").toBool());

        auto *preview = quickItemByName(root, QStringLiteral("listDragPreview-0"));
        QTRY_VERIFY(preview);
        const QPointF markerInContent
            = sourceMarker->mapToItem(sourceContent, QPointF(sourceMarker->width() / 2, sourceMarker->height() / 2));
        const QPointF previewMarkerBeforeScroll = preview->mapToItem(root, markerInContent);
        const int     targetBeforeScroll        = controller->property("targetItem").toInt();

        rootObject->setProperty("contentY", 130.0);
        QTRY_COMPARE(rootObject->property("contentY").toReal(), 130.0);
        QTRY_VERIFY((preview->mapToItem(root, markerInContent) - previewMarkerBeforeScroll).manhattanLength() < 1);
        QTRY_VERIFY(controller->property("targetItem").toInt() > targetBeforeScroll);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, pointer.toPoint());
    }

    void slowlyDraggingFirstItemDownNeverMovesTargetBackward()
    {
        QmlNoteEditor editor;
        editor.resize(400, 320);
        editor.load(QStringLiteral("- first\n- second\n- third\n- fourth\n- fifth"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 5);

        auto *source     = quickItemByName(root, QStringLiteral("listMarker-0-0"));
        auto *sourceRow  = quickItemByName(root, QStringLiteral("listRow-0-0"));
        auto *controller = quickItemByName(root, QStringLiteral("editorReorderController"));
        QVERIFY(source);
        QVERIFY(sourceRow);
        QVERIFY(controller);
        const QPointF start = source->mapToItem(root, QPointF(source->width() / 2, source->height() / 2));
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());

        int previousTarget       = 0;
        int firstAdvanceDistance = -1;
        for (int distance = 12; distance <= 80; ++distance) {
            const QPointF pointer = start + QPointF(0, distance);
            QTest::mouseMove(quick, pointer.toPoint(), 4);
            if (!controller->property("dragging").toBool())
                continue;
            const int target = controller->property("targetItem").toInt();
            if (firstAdvanceDistance < 0 && target > 0)
                firstAdvanceDistance = distance;
            QVERIFY2(target >= previousTarget,
                     qPrintable(QStringLiteral("target moved backward from %1 to %2 at y=%3")
                                    .arg(previousTarget)
                                    .arg(target)
                                    .arg(distance)));
            previousTarget = target;
        }
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, (start + QPointF(0, 80)).toPoint());
        QVERIFY(firstAdvanceDistance > 0);
        QVERIFY(firstAdvanceDistance <= qCeil(sourceRow->property("naturalHeight").toReal() * 0.75));
    }

    void horizontalListDragChangesIndentation()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("- one\n- two\n- three"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 3);

        auto *source     = quickItemByName(root, QStringLiteral("listMarker-0-2"));
        auto *before     = quickItemByName(root, QStringLiteral("listMarker-0-1"));
        auto *controller = quickItemByName(root, QStringLiteral("editorReorderController"));
        QVERIFY(source);
        QVERIFY(before);
        QVERIFY(controller);
        const qreal   indent = rootObject->property("listIndent").toReal();
        const QPointF start  = source->mapToItem(root, QPointF(source->width() / 2, source->height() / 2));
        const QPointF target = before->mapToItem(root, QPointF(before->width() / 2 + indent, 1));

        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        for (int step = 1; step <= 10; ++step)
            QTest::mouseMove(quick, (start + (target - start) * (qreal(step) / 10)).toPoint(), 15);
        QTRY_COMPARE(controller->property("targetItem").toInt(), 1);
        QTRY_COMPARE(controller->property("targetIndent").toInt(), 1);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, target.toPoint());

        QTRY_COMPARE(editor.contents(), QStringLiteral("- one\n    - three\n- two"));
    }

    void horizontalListDragCanOutdent()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("- parent\n    - child\n- tail"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 3);

        auto *source = quickItemByName(root, QStringLiteral("listMarker-0-1"));
        auto *tail   = quickItemByName(root, QStringLiteral("listMarker-0-2"));
        QVERIFY(source);
        QVERIFY(tail);
        const QPointF start  = source->mapToItem(root, QPointF(source->width() / 2, source->height() / 2));
        const QPointF target = tail->mapToItem(root, QPointF(tail->width() / 2, tail->height()));

        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        for (int step = 1; step <= 10; ++step)
            QTest::mouseMove(quick, (start + (target - start) * (qreal(step) / 10)).toPoint(), 15);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, target.toPoint());

        QTRY_COMPARE(editor.contents(), QStringLiteral("- parent\n- tail\n- child"));
    }

    void draggingListLevelHandleMovesCurrentSublistWithoutParent()
    {
        QmlNoteEditor editor;
        editor.resize(520, 380);
        editor.load(QStringLiteral("- parent\n    - one\n    - two\n- tail"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 4);

        auto *firstChildEditor = rootObject->property("editors").toList().at(1).value<QObject *>();
        QVERIFY(firstChildEditor);
        firstChildEditor->setProperty("cursorPosition", 1);
        QVERIFY(QMetaObject::invokeMethod(firstChildEditor, "forceActiveFocus"));

        auto *levelHandle = quickItemByName(root, QStringLiteral("listLevelReorderHandle-0-1-1"));
        auto *childMarker = quickItemByName(root, QStringLiteral("listMarker-0-1"));
        auto *tailMarker  = quickItemByName(root, QStringLiteral("listMarker-0-3"));
        QVERIFY(levelHandle);
        QVERIFY(childMarker);
        QVERIFY(tailMarker);
        QTRY_VERIFY(levelHandle->isVisible());
        QVERIFY(levelHandle->mapToItem(root, QPointF(levelHandle->width(), 0)).x() < childMarker->mapToItem(root, QPointF(0, 0)).x());
        auto *secondChildMarker = quickItemByName(root, QStringLiteral("listMarker-0-2"));
        QVERIFY(secondChildMarker);
        QVERIFY(levelHandle->height() >= secondChildMarker->mapToItem(levelHandle, QPointF(0, secondChildMarker->height())).y());

        const QPointF start      = levelHandle->mapToItem(root, QPointF(levelHandle->width() / 2, levelHandle->height() / 2));
        const QPointF tailBottom = tailMarker->mapToItem(root, QPointF(tailMarker->width() / 2, tailMarker->height()));
        const QPointF target(start.x(), tailBottom.y());
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        for (int step = 1; step <= 10; ++step)
            QTest::mouseMove(quick, (start + (target - start) * (qreal(step) / 10)).toPoint(), 15);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, target.toPoint());

        QTRY_COMPARE(editor.contents(), QStringLiteral("- parent\n- tail\n    - one\n    - two"));
    }

    void hoveringNestedListTextDoesNotShowLevelHandle()
    {
        QmlNoteEditor editor;
        editor.resize(520, 380);
        editor.load(QStringLiteral("- parent\n    - child\n- tail"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 3);

        auto *parentEditor = rootObject->property("editors").toList().at(0).value<QObject *>();
        QVERIFY(parentEditor);
        parentEditor->setProperty("cursorPosition", 1);
        QVERIFY(QMetaObject::invokeMethod(parentEditor, "forceActiveFocus"));

        auto *parentHandle = quickItemByName(root, QStringLiteral("listLevelReorderHandle-0-0-0"));
        auto *childMarker  = quickItemByName(root, QStringLiteral("listMarker-0-1"));
        QVERIFY(parentHandle);
        QVERIFY(childMarker);
        QTRY_VERIFY(parentHandle->isVisible());
        QCOMPARE(parentHandle->opacity(), qreal(0));

        const QPointF childHoverPoint
            = childMarker->mapToItem(root, QPointF(childMarker->width() / 2, childMarker->height() / 2));
        QTest::mouseMove(quick, childHoverPoint.toPoint());

        auto *childHandle = quickItemByName(root, QStringLiteral("listLevelReorderHandle-0-1-1"));
        QTRY_VERIFY(childHandle);
        QTRY_VERIFY(childHandle->isVisible());
        QCOMPARE(parentHandle->opacity(), qreal(0));
        QCOMPARE(childHandle->opacity(), qreal(0));

        const QPointF handleHoverPoint
            = childHandle->mapToItem(root, QPointF(childHandle->width() / 2, childHandle->height() / 2));
        QTest::mouseMove(quick, handleHoverPoint.toPoint());
        QTRY_VERIFY(childHandle->opacity() > 0);
        QVERIFY(childHandle->mapToItem(root, QPointF(childHandle->width(), 0)).x() < childMarker->mapToItem(root, QPointF(0, 0)).x());
    }

    void clickingTaskMarkerStillTogglesWithoutStartingDrag()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] task\n- [ ] second"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick      = editor.findChild<QQuickWidget *>();
        auto *rootObject = quick ? quick->rootObject() : nullptr;
        auto *root       = qobject_cast<QQuickItem *>(rootObject);
        QVERIFY(root);
        QTRY_COMPARE(rootObject->property("editors").toList().size(), 2);

        auto *textItem
            = qobject_cast<QQuickItem *>(rootObject->property("editors").toList().constFirst().value<QObject *>());
        QVERIFY(textItem);
        auto *checkbox = quickItemByName(root, QStringLiteral("taskMarker-0-0"));
        QVERIFY(checkbox);
        const QPointF clickPoint = checkbox->mapToItem(root, QPointF(checkbox->width() / 2, checkbox->height() / 2));
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, clickPoint.toPoint());
        QTRY_COMPARE(editor.contents(), QStringLiteral("- [x] task\n- [ ] second"));
    }

    void structuredPasteSplitsMarkdownTextBlock()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("before after"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());
        auto *activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        auto *document     = activeEditor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);

        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(NoteTransferController::MarkdownMimeType),
                      QByteArrayLiteral("- first\n- second"));
        QGuiApplication::clipboard()->setMimeData(mime);
        const QVariantMap result = backend(editor)->pasteStructuredFromClipboard(document, 0, 7, 7);
        QVERIFY(result.value(QStringLiteral("handled")).toBool());
        QCOMPARE(result.value(QStringLiteral("focusRow")).toInt(), 1);
        QCOMPARE(editor.contents(), QStringLiteral("before\n\n- first\n- second\n\nafter"));
    }

    void keyboardPasteUsesStructuredMarkdownPath()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("before after"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());
        auto *activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        activeEditor->setProperty("cursorPosition", 7);
        activeEditor->setProperty("selectionStart", 7);
        activeEditor->setProperty("selectionEnd", 7);

        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(NoteTransferController::MarkdownMimeType), QByteArrayLiteral("## inserted"));
        QGuiApplication::clipboard()->setMimeData(mime);
        QTest::keyClick(quick, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("before\n\n## inserted\n\nafter"));
    }

    void keyboardPastePreservesWholeQtNoteFragment()
    {
        QmlNoteEditor source;
        source.resize(500, 400);
        source.load(QStringLiteral("## Header\n\n- [x] task\n\n| A | B |\n| --- | --- |\n| 1 | 2 |"), Note::Markdown);
        source.show();
        QTest::qWait(30);
        auto *sourceQuick = source.findChild<QQuickWidget *>();
        QVERIFY(sourceQuick);
        QTRY_VERIFY(sourceQuick->rootObject()->property("activeEditor").value<QObject *>());
        QTest::keyClick(sourceQuick, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(sourceQuick, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(QGuiApplication::clipboard()->mimeData()->hasFormat(
            QString::fromLatin1(NoteTransferController::FragmentMimeType)));

        QmlNoteEditor target;
        target.resize(500, 400);
        target.load(QStringLiteral("before"), Note::Markdown);
        target.show();
        QTest::qWait(30);
        auto *quick = target.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());
        auto *activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        activeEditor->setProperty("cursorPosition", 0);
        QTest::keyClick(quick, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(target.contents(),
                     QStringLiteral("## Header\n\n- [x] task\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\nbefore"));
        QCOMPARE(target.model()->rowCount(), 4);
        QCOMPARE(target.model()->data(target.model()->index(1), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::CheckList));
        QCOMPARE(target.model()->data(target.model()->index(2), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Table));
    }

    void copyMarkdownClipboardKeepsStructuralBlocks()
    {
        const QString  markdown = QStringLiteral("## Header\n\n- [ ] task\n\n| A | B |\n| --- | --- |\n| 1 | 2 |");
        NoteBlockModel parsed;
        parsed.load(markdown, true);
        QCOMPARE(parsed.rowCount(), 3);
        QCOMPARE(parsed.data(parsed.index(2), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Table));

        QmlNoteEditor source;
        backend(source)->copyMarkdownToClipboard(markdown);

        NoteTransferController controller;
        const auto             imported = controller.importMimeData(QGuiApplication::clipboard()->mimeData());
        QVERIFY(imported);
        QCOMPARE(imported.fragment.blocks.size(), 3);
        QCOMPARE(imported.fragment.blocks.at(0).type, NoteFragmentBlockType::Heading);
        QCOMPARE(imported.fragment.blocks.at(1).type, NoteFragmentBlockType::List);
        QCOMPARE(imported.fragment.blocks.at(2).type, NoteFragmentBlockType::Table);

        QmlNoteEditor target;
        target.resize(500, 400);
        target.load(QStringLiteral("before"), Note::Markdown);
        target.show();
        QTest::qWait(30);
        auto *quick = target.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());
        auto *activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        auto *document     = activeEditor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        const QVariantMap result = backend(target)->pasteStructuredFromClipboard(document, 0, 0, 0);
        QVERIFY(result.value(QStringLiteral("handled")).toBool());
        QCOMPARE(target.contents(),
                 QStringLiteral("## Header\n\n- [ ] task\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\nbefore"));
    }

    void partialCrossBlockCopyPreservesListAndTable()
    {
        QmlNoteEditor source;
        source.resize(700, 700);
        source.load(QStringLiteral("prefix\n\n- [ ] one\n- [x] two\n\n"
                                   "| A | B |\n| --- | --- |\n| 1 | 2 |\n\nsuffix"),
                    Note::Markdown);
        source.show();
        QTest::qWait(30);
        auto *sourceQuick = source.findChild<QQuickWidget *>();
        auto *sourceRoot  = sourceQuick->rootObject();
        QTRY_COMPARE(sourceRoot->property("editors").toList().size(), 8);
        const auto editors = sourceRoot->property("editors").toList();
        auto      *first   = editors.constFirst().value<QObject *>();
        auto      *last    = editors.constLast().value<QObject *>();
        QVERIFY(first && last);
        QVERIFY(QMetaObject::invokeMethod(sourceRoot, "applyDocumentSelection",
                                          Q_ARG(QVariant, QVariant::fromValue(first)), Q_ARG(QVariant, 3),
                                          Q_ARG(QVariant, QVariant::fromValue(last)), Q_ARG(QVariant, 3)));
        QTest::keyClick(sourceQuick, Qt::Key_C, Qt::ControlModifier);

        NoteTransferController controller;
        const auto             imported = controller.importMimeData(QGuiApplication::clipboard()->mimeData());
        QVERIFY(imported);
        QCOMPARE(imported.fragment.blocks.size(), 4);
        QCOMPARE(imported.fragment.blocks.at(0).type, NoteFragmentBlockType::Text);
        QCOMPARE(imported.fragment.blocks.at(1).type, NoteFragmentBlockType::List);
        QCOMPARE(imported.fragment.blocks.at(2).type, NoteFragmentBlockType::Table);
        QCOMPARE(imported.fragment.blocks.at(3).type, NoteFragmentBlockType::Text);

        QmlNoteEditor target;
        target.resize(700, 700);
        target.load(QStringLiteral("target"), Note::Markdown);
        target.show();
        QTest::qWait(30);
        auto *targetQuick = target.findChild<QQuickWidget *>();
        QTRY_VERIFY(targetQuick->rootObject()->property("activeEditor").value<QObject *>());
        QTest::keyClick(targetQuick, Qt::Key_V, Qt::ControlModifier);

        QTRY_COMPARE(target.contents(),
                     QStringLiteral("fix\n\n- [ ] one\n- [x] two\n\n"
                                    "| A | B |\n| --- | --- |\n| 1 | 2 |\n\nsuf\n\ntarget"));
        QCOMPARE(target.model()->data(target.model()->index(1), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::CheckList));
        QCOMPARE(target.model()->data(target.model()->index(2), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Table));
    }

    void partialCrossBlockDeleteRemovesListAndTable()
    {
        QmlNoteEditor editor;
        editor.resize(700, 700);
        editor.load(QStringLiteral("beforeXSELECT\n\n- [ ] one\n- [x] two\n\n"
                                   "| A | B |\n| --- | --- |\n| 1 | 2 |\n\nREMOVEafter"),
                    Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 8);
        const auto editors = root->property("editors").toList();
        auto      *first   = editors.constFirst().value<QObject *>();
        auto      *last    = editors.constLast().value<QObject *>();
        QVERIFY(first && last);
        QVERIFY(QMetaObject::invokeMethod(root, "applyDocumentSelection", Q_ARG(QVariant, QVariant::fromValue(first)),
                                          Q_ARG(QVariant, 7), Q_ARG(QVariant, QVariant::fromValue(last)),
                                          Q_ARG(QVariant, 6)));

        QTest::keyClick(quick, Qt::Key_Delete);

        QTRY_COMPARE(editor.contents(), QStringLiteral("beforeXafter"));
        QCOMPARE(editor.model()->rowCount(), 1);
        QCOMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Text));
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 7);
    }

    void pastePrefersTsvOverOfficeImagePreview()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("before"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());
        auto *activeEditor = quick->rootObject()->property("activeEditor").value<QObject *>();
        activeEditor->setProperty("cursorPosition", 0);

        QImage preview(1, 1, QImage::Format_ARGB32_Premultiplied);
        preview.fill(Qt::red);
        auto *mime = new QMimeData;
        mime->setImageData(preview);
        mime->setData(QString::fromLatin1(NoteTransferController::TsvMimeType), QByteArrayLiteral("A\tB\n1\t2"));
        QGuiApplication::clipboard()->setMimeData(mime);
        QTest::keyClick(quick, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("| A | B |\n| --- | --- |\n| 1 | 2 |\n\nbefore"));
    }

    void pasteImportsHtmlTableInsteadOfImagePreview()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("before"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        QTRY_VERIFY(quick->rootObject()->property("activeEditor").value<QObject *>());

        QImage preview(1, 1, QImage::Format_ARGB32_Premultiplied);
        preview.fill(Qt::red);
        auto *mime = new QMimeData;
        mime->setImageData(preview);
        mime->setHtml(QStringLiteral("<table><tr><td>A</td><td>B</td></tr>"
                                     "<tr><td>1</td><td>2</td></tr></table>"));
        QGuiApplication::clipboard()->setMimeData(mime);
        QTest::keyClick(quick, Qt::Key_V, Qt::ControlModifier);

        QTRY_COMPARE(editor.model()->rowCount(), 2);
        QCOMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Table));
        QCOMPARE(editor.contents(), QStringLiteral("| A | B |\n| --- | --- |\n| 1 | 2 |\n\nbefore"));
    }

    void tablePasteImportsTsvRectangle()
    {
        QmlNoteEditor editor;
        editor.load(QStringLiteral("text"), Note::Markdown);
        editor.model()->insertTable(1);
        editor.model()->setTableCell(1, 0, QStringLiteral("A"));
        editor.model()->setTableCell(1, 1, QStringLiteral("B"));
        editor.model()->setTableCell(1, 2, QStringLiteral("1"));
        editor.model()->setTableCell(1, 3, QStringLiteral("2"));
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(NoteTransferController::TsvMimeType), QByteArrayLiteral("X\tY\nZ\tW"));
        QGuiApplication::clipboard()->setMimeData(mime);

        const QVariantMap result = backend(editor)->pasteTableFromClipboard(1, 3);
        QVERIFY(result.value(QStringLiteral("handled")).toBool());
        const auto table = editor.model()->data(editor.model()->index(1), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table.value(QStringLiteral("columns")).toInt(), 3);
        QCOMPARE(table.value(QStringLiteral("values")).toStringList(),
                 QStringList({ "A", "B", "", "1", "X", "Y", "", "Z", "W" }));
    }

    void listPastePreservesNestedFragmentStructure()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("- before selected after\n- tail"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        const QJSValue editors = root->property("editors").value<QJSValue>();
        QCOMPARE(editors.property(QStringLiteral("length")).toInt(), 2);
        QObject *listCell = editors.property(0).toQObject();
        QVERIFY(listCell);
        auto *document = listCell->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(NoteTransferController::MarkdownMimeType),
                      QByteArrayLiteral("- [x] task\n    1. nested"));
        QGuiApplication::clipboard()->setMimeData(mime);

        const QVariantMap result = backend(editor)->pasteListFromClipboard(document, 0, 0, 7, 15);
        QVERIFY(result.value(QStringLiteral("handled")).toBool());
        QCOMPARE(result.value(QStringLiteral("focusItem")).toInt(), 1);
        QCOMPARE(editor.contents(), QStringLiteral("- before\n- [x] task\n    1. nested\n- after\n- tail"));
    }

    void adjacentParagraphsUseOneTextEditor()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("first paragraph\n\nsecond paragraph"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        auto geometry = [root](int index) {
            QVariant result;
            QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, index));
            return result.toMap();
        };
        QTRY_VERIFY(!geometry(0).isEmpty());
        QVERIFY(geometry(1).isEmpty());
    }

    void editingTableCellKeepsTextDocuments()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("| Name | Value |\n| --- | --- |\n| one | two |"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        auto geometry = [root](int index) {
            QVariant result;
            QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, index));
            return result.toMap();
        };
        QTRY_VERIFY(!geometry(3).isEmpty());
        const int registrations = root->property("editorRegistrations").toInt();
        QCOMPARE(registrations, 4);

        const auto   first = geometry(0);
        const QPoint position(first[QStringLiteral("x")].toInt() + first[QStringLiteral("width")].toInt() / 2,
                              first[QStringLiteral("y")].toInt() + first[QStringLiteral("height")].toInt() / 2);
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, position);
        QTest::keyClicks(quick, QStringLiteral("x"));
        QTRY_VERIFY(editor.contents().contains(QLatin1Char('x')));
        QCOMPARE(root->property("editorRegistrations").toInt(), registrations);

        const auto   last = geometry(3);
        const QPoint lastPosition(last[QStringLiteral("x")].toInt() + last[QStringLiteral("width")].toInt() / 2,
                                  last[QStringLiteral("y")].toInt() + last[QStringLiteral("height")].toInt() / 2);
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, lastPosition);
        QTest::keyClick(quick, Qt::Key_End);
        QTest::keyClick(quick, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(geometry(2)[QStringLiteral("height")].toInt(), geometry(3)[QStringLiteral("height")].toInt());
        QCOMPARE(root->property("editorRegistrations").toInt(), registrations);
        QTRY_VERIFY(editor.contents().contains(QStringLiteral("<br>")));
        QTest::keyClicks(quick, QStringLiteral("three"));
        QTRY_VERIFY(editor.contents().contains(QStringLiteral("two<br>three")));

        QTest::keyClick(quick, Qt::Key_End);
        QTest::keyClick(quick, Qt::Key_Return);
        QTRY_VERIFY(!geometry(5).isEmpty());
        const int registrationsAfterInsert = root->property("editorRegistrations").toInt();
        editor.model()->removeTableRow(0, 1);
        QTRY_VERIFY(geometry(4).isEmpty());
        QCOMPARE(root->property("editorRegistrations").toInt(), registrationsAfterInsert);
    }

    void editingChecklistItemKeepsTextDocumentsAndFocus()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("- [ ] first item\n- [x] second item"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 0)));
        const auto geometry = geometryValue.toMap();
        QVERIFY(!geometry.isEmpty());
        const int registrations = root->property("editorRegistrations").toInt();
        QCOMPARE(registrations, 2);
        const QPoint position(geometry[QStringLiteral("x")].toInt() + geometry[QStringLiteral("width")].toInt() / 2,
                              geometry[QStringLiteral("y")].toInt() + geometry[QStringLiteral("height")].toInt() / 2);

        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, position);
        auto activeIndex = [root]() {
            QVariant result;
            QMetaObject::invokeMethod(root, "activeEditorIndex", Q_RETURN_ARG(QVariant, result));
            return result.toInt();
        };
        QTest::keyClick(quick, Qt::Key_Home);
        QTest::keyClick(quick, Qt::Key_Down);
        QTRY_COMPARE(activeIndex(), 1);
        QTest::keyClick(quick, Qt::Key_Up);
        QTRY_COMPARE(activeIndex(), 0);
        QTest::keyClicks(quick, QStringLiteral("xyz"));
        QTRY_VERIFY(editor.contents().contains(QStringLiteral("xyz")));
        QVERIFY(!editor.contents().contains(QStringLiteral("<br>")));
        QCOMPARE(root->property("editorRegistrations").toInt(), registrations);
        QVERIFY(root->property("activeEditor").value<QObject *>()->property("activeFocus").toBool());

        QTest::keyClick(quick, Qt::Key_Return);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList().size(),
                     3);
        QTRY_COMPARE(root->property("editorRegistrations").toInt(), registrations + 1);
        QVERIFY(root->property("activeEditor").value<QObject *>()->property("activeFocus").toBool());

        QTest::keyClick(quick, Qt::Key_Home);
        QTest::keyClick(quick, Qt::Key_End, Qt::ShiftModifier);
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList().size(),
                     2);
        QVERIFY(root->property("activeEditor").value<QObject *>()->property("activeFocus").toBool());
    }

    void enterAtEndOfLinkedListItemDoesNotSplitUrl()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("1. hello [dsbb](https://ya.ru) world"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *item = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(item);
        QVERIFY(QMetaObject::invokeMethod(item, "forceActiveFocus"));
        item->setProperty("cursorPosition", item->property("length"));

        QTest::keyClick(quick, Qt::Key_Return);

        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "hello [dsbb](https://ya.ru) world", "" }));
        QCOMPARE(editor.contents(), QStringLiteral("1. hello [dsbb](https://ya.ru) world\n2. "));

        QmlNoteEditor splitInsideLink;
        splitInsideLink.resize(500, 350);
        splitInsideLink.load(QStringLiteral("- hello [dsbb](https://ya.ru) world"), Note::Markdown);
        splitInsideLink.show();
        QTest::qWait(30);
        auto *splitQuick = splitInsideLink.findChild<QQuickWidget *>();
        auto *splitRoot  = splitQuick->rootObject();
        QTRY_COMPARE(splitRoot->property("editors").toList().size(), 1);
        auto *splitItem = splitRoot->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(splitItem, "forceActiveFocus"));
        splitItem->setProperty("cursorPosition", 8); // Between "ds" and "bb".
        QTest::keyClick(splitQuick, Qt::Key_Return);
        QTRY_COMPARE(
            splitInsideLink.model()->data(splitInsideLink.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
            QStringList({ "hello [ds](https://ya.ru)", "[bb](https://ya.ru) world" }));
    }

    void backspaceConvertsLastEmptyChecklistItemToText()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] "), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 0)));
        const auto geometry = geometryValue.toMap();
        QTRY_VERIFY(!geometry.isEmpty());
        const QPoint position(geometry[QStringLiteral("x")].toInt() + 8,
                              geometry[QStringLiteral("y")].toInt() + geometry[QStringLiteral("height")].toInt() / 2);
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, position);
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        QTest::keyClick(quick, Qt::Key_Backspace);

        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::TypeRole).toInt(),
                     int(NoteBlockModel::Text));
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        QVERIFY(root->property("activeEditor").value<QObject *>()->property("activeFocus").toBool());
        QTest::keyClicks(quick, QStringLiteral("plain"));
        QTRY_COMPARE(editor.contents(), QStringLiteral("plain"));
    }

    void deleteAtEndMergesListItems()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] first\n- [x] second\n- [ ] third"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 3);
        auto *first = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(first);
        QVERIFY(first->setProperty("cursorPosition", first->property("length")));
        QVERIFY(QMetaObject::invokeMethod(first, "forceActiveFocus"));

        QTest::keyClick(quick, Qt::Key_Delete);

        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "firstsecond", "third" }));
        QCOMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::CheckedRole).toList(),
                 QVariantList({ false, false }));
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 5);
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("sourceText").toString(),
                     QStringLiteral("firstsecond"));
        QTRY_VERIFY(!root->property("activeEditor").value<QObject *>()->property("sourceTextPending").toBool());
    }

    void deleteFromEmptyTaskItemPreservesFollowingCheckState()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] first\n- [x] second"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick ? quick->rootObject() : nullptr;
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 2);
        auto *first = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(first);
        QVERIFY(QMetaObject::invokeMethod(first, "forceActiveFocus"));
        QVERIFY(
            QMetaObject::invokeMethod(first, "select", Q_ARG(int, 0), Q_ARG(int, first->property("length").toInt())));
        QTest::keyClick(quick, Qt::Key_Delete);
        QTRY_COMPARE(first->property("length").toInt(), 0);

        QTest::keyClick(quick, Qt::Key_Delete);

        QTRY_COMPARE(editor.contents(), QStringLiteral("- [x] second"));
        auto *checkbox = quickItemByName(root, QStringLiteral("taskMarker-0-0"));
        QVERIFY(checkbox);
        QTRY_COMPARE(checkbox->property("checked").toBool(), true);
    }

    void deleteAfterSplittingListItemKeepsFollowingItem()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- first\n- middle\n- next"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick ? quick->rootObject() : nullptr;
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 3);
        auto *middle = root->property("editors").toList().at(1).value<QObject *>();
        QVERIFY(middle);
        QVERIFY(middle->setProperty("cursorPosition", middle->property("length")));
        QVERIFY(QMetaObject::invokeMethod(middle, "forceActiveFocus"));

        QTest::keyClick(quick, Qt::Key_Return);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "first", "middle", "", "next" }));
        QTRY_COMPARE(root->property("editors").toList().size(), 4);
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("listItemIndex").toInt(), 2);

        QTest::keyClick(quick, Qt::Key_Delete);

        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "first", "middle", "next" }));
        QTRY_COMPARE(root->property("editors").toList().size(), 3);
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("listItemIndex").toInt(), 2);
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("sourceText").toString(),
                     QStringLiteral("next"));
        QTRY_VERIFY(!root->property("activeEditor").value<QObject *>()->property("sourceTextPending").toBool());
        QVERIFY(root->property("activeEditor")
                    .value<QObject *>()
                    ->property("text")
                    .toString()
                    .startsWith(QStringLiteral("next")));
    }

    void deleteAtEndOfLastListItemMergesFollowingBlock()
    {
        const auto verify = [](const QString &source, const QString &expected) {
            QmlNoteEditor editor;
            editor.resize(500, 300);
            editor.load(source, Note::Markdown);
            editor.show();
            QTest::qWait(30);

            auto *quick = editor.findChild<QQuickWidget *>();
            auto *root  = quick ? quick->rootObject() : nullptr;
            QVERIFY(root);
            QTRY_VERIFY(!root->property("editors").toList().isEmpty());
            auto *first = root->property("editors").toList().constFirst().value<QObject *>();
            QVERIFY(first);
            first->setProperty("cursorPosition", first->property("length"));
            QVERIFY(QMetaObject::invokeMethod(first, "forceActiveFocus"));

            QTest::keyClick(quick, Qt::Key_Delete);

            QTRY_COMPARE(editor.contents(), expected);
            QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("listItemIndex").toInt(), 0);
            QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 5);
        };

        verify(QStringLiteral("- first\n\nfollowing"), QStringLiteral("- firstfollowing"));
        verify(QStringLiteral("- first\n\n- [x] second\n- [ ] third"), QStringLiteral("- firstsecond\n- [ ] third"));
    }

    void backspaceRemovesTheEmptyFocusedTaskItem()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] first\n- [x] second"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick ? quick->rootObject() : nullptr;
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 2);
        auto *first = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(first);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::CheckedRole).toList(),
                     QVariantList({ false, true }));
        QVERIFY(QMetaObject::invokeMethod(first, "forceActiveFocus"));
        QVERIFY(
            QMetaObject::invokeMethod(first, "select", Q_ARG(int, 0), Q_ARG(int, first->property("length").toInt())));
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTRY_COMPARE(first->property("length").toInt(), 0);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::CheckedRole).toList(),
                     QVariantList({ false, true }));

        QTest::keyClick(quick, Qt::Key_Backspace);

        QTRY_COMPARE(editor.contents(), QStringLiteral("- [x] second"));
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *remaining = root->property("activeEditor").value<QObject *>();
        QVERIFY(remaining);
        QTRY_COMPARE(remaining->property("sourceText").toString(), QStringLiteral("second"));
        QCOMPARE(remaining->property("listItemIndex").toInt(), 0);
        QCOMPARE(remaining->property("cursorPosition").toInt(), 0);
        auto *checkbox = quickItemByName(root, QStringLiteral("taskMarker-0-0"));
        QVERIFY(checkbox);
        QTRY_COMPARE(checkbox->property("checked").toBool(), true);
    }

    void backspaceRemovingLoadedListItemKeepsViewport()
    {
        QStringList items;
        for (int index = 0; index < 30; ++index)
            items.append(QStringLiteral("- [ ] item %1").arg(index));

        QmlNoteEditor editor;
        editor.resize(500, 260);
        editor.load(items.join(QLatin1Char('\n')), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick ? quick->rootObject() : nullptr;
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), items.size());
        auto *target = root->property("editors").toList().at(12).value<QObject *>();
        QVERIFY(target);
        QVERIFY(QMetaObject::invokeMethod(target, "forceActiveFocus"));
        QVERIFY(
            QMetaObject::invokeMethod(target, "select", Q_ARG(int, 0), Q_ARG(int, target->property("length").toInt())));
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTRY_COMPARE(target->property("length").toInt(), 0);

        root->setProperty("contentY", 180.0);
        QTRY_COMPARE(root->property("contentY").toReal(), 180.0);
        const qreal viewportBefore = root->property("contentY").toReal();
        QTest::keyClick(quick, Qt::Key_Backspace);

        QTRY_COMPARE(editor.contents().contains(QStringLiteral("item 12")), false);
        QTest::qWait(80);
        QVERIFY(qAbs(root->property("contentY").toReal() - viewportBefore) < 1.0);
    }

    void tabsSelectedChecklistItems()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] one\n- [ ] two"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QVERIFY(root);
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 0)));
        const auto geometry = geometryValue.toMap();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + 8, geometry["y"].toInt() + geometry["height"].toInt() / 2));
        QVERIFY(QMetaObject::invokeMethod(root, "selectAllDocument"));
        QTest::keyClick(quick, Qt::Key_Tab);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::IndentsRole).toList(),
                     QVariantList({ 0, 1 }));
        QTest::keyClick(quick, Qt::Key_Tab, Qt::ShiftModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::IndentsRole).toList(),
                     QVariantList({ 0, 0 }));
    }

    void listToolbarActionConvertsActiveList()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("- [ ] one\n  - [ ] two"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto    *quick = editor.findChild<QQuickWidget *>();
        auto    *root  = quick->rootObject();
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 1)));
        const auto geometry = geometryValue.toMap();
        QTRY_VERIFY(!geometry.isEmpty());
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + 8, geometry["y"].toInt() + geometry["height"].toInt() / 2));
        QTRY_VERIFY(root->property("activeEditor").value<QObject *>());
        auto *activeBefore = root->property("activeEditor").value<QObject *>();
        activeBefore->setProperty("cursorPosition", 1);

        editor.insertList(NoteBlockModel::NumberedList);
        QTRY_COMPARE(editor.model()->rowCount(), 1);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemTypesRole).toList(),
                     QVariantList({ int(NoteBlockModel::CheckList), int(NoteBlockModel::NumberedList) }));
        const auto items = editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList();
        QCOMPARE(items.size(), 2);
        QVERIFY(items[0].startsWith(QStringLiteral("one")));
        QVERIFY(items[1].startsWith(QStringLiteral("two")));
        QCOMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::IndentsRole).toList(),
                 QVariantList({ 0, 1 }));
        QCOMPARE(root->property("activeEditor").value<QObject *>(), activeBefore);
        QCOMPARE(activeBefore->property("cursorPosition").toInt(), 1);
    }

    void listKeyboardShortcutsConvertActiveLevel()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("1. one\n2. two"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 2);
        auto *first = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(first);
        QVERIFY(QMetaObject::invokeMethod(first, "forceActiveFocus"));

        QTest::keyClick(quick, Qt::Key_8, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemTypesRole).toList(),
                     QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::BulletList) }));
        QTest::keyClick(quick, Qt::Key_9, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemTypesRole).toList(),
                     QVariantList({ int(NoteBlockModel::CheckList), int(NoteBlockModel::CheckList) }));
        QTest::keyClick(quick, Qt::Key_7, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemTypesRole).toList(),
                     QVariantList({ int(NoteBlockModel::NumberedList), int(NoteBlockModel::NumberedList) }));
    }

    void headingKeyboardShortcutsConvertTextBlock()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("title\n\nheading"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *text = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(text);
        auto *document = text->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        const int headingPosition = document->textDocument()->toPlainText().indexOf(QStringLiteral("heading"));
        QVERIFY(headingPosition > 0);
        text->setProperty("cursorPosition", headingPosition);
        QVERIFY(QMetaObject::invokeMethod(text, "forceActiveFocus"));

        QTest::keyClick(quick, Qt::Key_2, Qt::ControlModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TypeRole).toInt(),
                     int(NoteBlockModel::Heading));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::HeadingLevelRole).toInt(), 2);
        QCOMPARE(editor.contents(), QStringLiteral("title\n\n## heading"));

        QTRY_COMPARE(root->property("editors").toList().size(), 2);
        auto *heading = root->property("editors").toList().at(1).value<QObject *>();
        QVERIFY(heading);
        QVERIFY(QMetaObject::invokeMethod(heading, "forceActiveFocus"));
        QTest::keyClick(quick, Qt::Key_0, Qt::ControlModifier);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TypeRole).toInt(),
                     int(NoteBlockModel::Text));
        QCOMPARE(editor.contents(), QStringLiteral("title\n\nheading"));
    }

    void titleEditorRejectsMarkdownFormattingCommands()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("title"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick ? quick->rootObject() : nullptr;
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *title = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(title);
        QVERIFY(title->property("titleDocument").toBool());
        QVERIFY(QMetaObject::invokeMethod(title, "forceActiveFocus"));
        QVERIFY(QMetaObject::invokeMethod(title, "select", Q_ARG(int, 0), Q_ARG(int, 5)));

        QTest::keyClick(quick, Qt::Key_B, Qt::ControlModifier);
        QTest::keyClick(quick, Qt::Key_2, Qt::ControlModifier);
        QTest::keyClick(quick, Qt::Key_K, Qt::ControlModifier);

        QCOMPARE(editor.contents(), QStringLiteral("title"));
        QCOMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Text));
        auto *urlField = root->findChild<QObject *>(QStringLiteral("noteLinkUrlField"));
        QVERIFY(urlField);
        QVERIFY(!urlField->property("activeFocus").toBool());
    }

    void inlineFormattingShortcutsWrapSelection()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("title\n\nbold text"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *text = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(text);
        QVERIFY(QMetaObject::invokeMethod(text, "forceActiveFocus"));
        auto *document = text->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        const int bodyStart = document->textDocument()->toPlainText().indexOf(QStringLiteral("bold"));
        QVERIFY(bodyStart > 0);
        QVERIFY(QMetaObject::invokeMethod(text, "select", Q_ARG(int, bodyStart), Q_ARG(int, bodyStart + 4)));

        QTest::keyClick(quick, Qt::Key_B, Qt::ControlModifier);

        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\n**bold** text"));
        QTest::keyClick(quick, Qt::Key_B, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\nbold text"));

        QMetaObject::invokeMethod(text, "select", Q_ARG(int, bodyStart), Q_ARG(int, bodyStart + 4));
        QTest::keyClick(quick, Qt::Key_U, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\n<ins>bold</ins> text"));
        QTest::keyClick(quick, Qt::Key_U, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\nbold text"));

        QMetaObject::invokeMethod(text, "select", Q_ARG(int, bodyStart), Q_ARG(int, bodyStart + 4));
        QTest::keyClick(quick, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\n`bold` text"));
        QTest::keyClick(quick, Qt::Key_U, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\n<ins>`bold`</ins> text"));
        QTest::keyClick(quick, Qt::Key_U, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\n`bold` text"));
        QTest::keyClick(quick, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\nbold text"));

        QMetaObject::invokeMethod(text, "select", Q_ARG(int, bodyStart), Q_ARG(int, bodyStart + 4));
        QTest::keyClick(quick, Qt::Key_B, Qt::ControlModifier);
        QMetaObject::invokeMethod(text, "select", Q_ARG(int, bodyStart + 5), Q_ARG(int, bodyStart + 9));
        QTest::keyClick(quick, Qt::Key_I, Qt::ControlModifier);
        QMetaObject::invokeMethod(text, "select", Q_ARG(int, bodyStart), Q_ARG(int, bodyStart + 9));
        auto *urlField = root->findChild<QObject *>(QStringLiteral("noteLinkUrlField"));
        QVERIFY(urlField);
        QTest::keyClick(quick, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(urlField->property("activeFocus").toBool());
        QTest::keyClicks(quick, QStringLiteral("url"));
        QTest::keyClick(quick, Qt::Key_Return);
        QTRY_COMPARE(editor.contents(), QStringLiteral("title\n\n[**bold** *text*](url)"));
    }

    void inlineLinkStaysWithinParagraphAndListAcrossModeSwitch()
    {
        const auto applyLink = [](QmlNoteEditor &editor, int selectionStart, int selectionEnd) {
            editor.resize(500, 300);
            editor.show();
            QTest::qWait(30);
            auto *quick = editor.findChild<QQuickWidget *>();
            auto *root  = quick->rootObject();
            QTRY_VERIFY(!root->property("editors").toList().isEmpty());
            auto *text = root->property("editors").toList().constLast().value<QObject *>();
            QVERIFY(text);
            auto *document = text->property("textDocument").value<QQuickTextDocument *>();
            QVERIFY(document);
            const int contentStart = document->textDocument()->toPlainText().indexOf(QStringLiteral("before"));
            QVERIFY(contentStart >= 0);
            QVERIFY(QMetaObject::invokeMethod(text, "forceActiveFocus"));
            QVERIFY(QMetaObject::invokeMethod(text, "select", Q_ARG(int, contentStart + selectionStart),
                                              Q_ARG(int, contentStart + selectionEnd)));
            QTest::keyClick(quick, Qt::Key_K, Qt::ControlModifier);
            auto *urlField = root->findChild<QObject *>(QStringLiteral("noteLinkUrlField"));
            QVERIFY(urlField);
            QTRY_VERIFY(urlField->property("activeFocus").toBool());
            QTest::keyClicks(quick, QStringLiteral("url"));
            QTest::keyClick(quick, Qt::Key_Return);
        };

        QmlNoteEditor paragraph;
        paragraph.load(QStringLiteral("title\n\nbefore link after"), Note::Markdown);
        applyLink(paragraph, 7, 11);
        QTRY_COMPARE(paragraph.contents(), QStringLiteral("title\n\nbefore [link](url) after"));

        paragraph.load(paragraph.contents(), Note::PlainText);
        QCOMPARE(paragraph.contents(), QStringLiteral("title\n\nbefore [link](url) after"));
        paragraph.load(paragraph.contents(), Note::Markdown);
        QCOMPARE(paragraph.contents(), QStringLiteral("title\n\nbefore [link](url) after"));
        paragraph.hide();

        QmlNoteEditor inserted;
        inserted.load(QStringLiteral("title\n\nbefore  after"), Note::Markdown);
        applyLink(inserted, 7, 7);
        QTRY_COMPARE(inserted.contents(), QStringLiteral("title\n\nbefore [link](url) after"));
        inserted.hide();

        QmlNoteEditor list;
        list.load(QStringLiteral("- before link after"), Note::Markdown);
        applyLink(list, 7, 11);
        QTRY_COMPARE(list.contents(), QStringLiteral("- before [link](url) after"));
        list.hide();

        const QString longUrl = QStringLiteral("https://example.org/a/very/long/path/that/exceeds/the/markdown/"
                                               "writers/usual/wrapping/column?with=query&and=value");
        QmlNoteEditor longLink;
        longLink.resize(500, 300);
        longLink.load(QStringLiteral("before link after"), Note::Markdown);
        longLink.show();
        QTest::qWait(30);
        auto *longRoot = longLink.findChild<QQuickWidget *>()->rootObject();
        QTRY_COMPARE(longRoot->property("editors").toList().size(), 1);
        auto *longText = longRoot->property("editors").toList().constFirst().value<QObject *>();
        auto *document = longText->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        QCOMPARE(backend(longLink)->setLink(document, 7, 11, longUrl), 11);
        longLink.model()->setBlockText(0, backend(longLink)->markdownText(document));
        const QString inlineLongLink = QStringLiteral("before [link](%1) after").arg(longUrl);
        QCOMPARE(longLink.contents(), inlineLongLink);
        longLink.load(longLink.contents(), Note::PlainText);
        QCOMPARE(longLink.contents(), inlineLongLink);
        longLink.load(longLink.contents(), Note::Markdown);
        QCOMPARE(longLink.contents(), inlineLongLink);
    }

    void downLeavesHeadingForFollowingTextBlock()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("## heading\n\nfollowing"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 2);
        auto *heading = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(heading);
        QVERIFY(QMetaObject::invokeMethod(heading, "forceActiveFocus"));
        heading->setProperty("cursorPosition", heading->property("length"));

        QTest::keyClick(quick, Qt::Key_Down);

        QTRY_COMPARE(root->property("activeEditor").value<QObject *>()->property("blockIndex").toInt(), 1);
        QCOMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 0);
    }

    void downFromLastHeadingDoesNotCreateTextBlock()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("## heading"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *heading = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(heading);
        QVERIFY(QMetaObject::invokeMethod(heading, "forceActiveFocus"));
        heading->setProperty("cursorPosition", heading->property("length"));

        QTest::keyClick(quick, Qt::Key_Down);

        QTest::qWait(20);
        QCOMPARE(editor.model()->rowCount(), 1);
        QCOMPARE(root->property("activeEditor").value<QObject *>()->property("blockIndex").toInt(), 0);
    }

    void downAtDocumentEndDoesNotAppendBlocks()
    {
        QmlNoteEditor editor;
        editor.resize(500, 300);
        editor.load(QStringLiteral("last"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 1);
        auto *text = root->property("editors").toList().constFirst().value<QObject *>();
        QVERIFY(text);
        QVERIFY(QMetaObject::invokeMethod(text, "forceActiveFocus"));
        text->setProperty("cursorPosition", text->property("length"));

        for (int press = 0; press < 6; ++press)
            QTest::keyClick(quick, Qt::Key_Down);
        QTest::qWait(20);
        QCOMPARE(editor.model()->rowCount(), 1);
    }

    void shiftSelectsAcrossEditors()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("first\n\n- [ ] middle\n\nlast"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QVERIFY(root);
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 0)));
        const auto geometry = geometryValue.toMap();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + geometry["width"].toInt() - 8,
                                 geometry["y"].toInt() + geometry["height"].toInt() / 2));
        QTest::keyClick(quick, Qt::Key_End);
        QTest::keyClick(quick, Qt::Key_Left);
        QTest::keyClick(quick, Qt::Key_Right, Qt::ShiftModifier);
        QTest::keyClick(quick, Qt::Key_Right, Qt::ShiftModifier);
        QTest::keyClick(quick, Qt::Key_Right, Qt::ShiftModifier);
        auto selectedCount = [root]() {
            QVariant result;
            QMetaObject::invokeMethod(root, "selectedEditorCount", Q_RETURN_ARG(QVariant, result));
            return result.toInt();
        };
        QTRY_COMPARE(selectedCount(), 2);
    }

    void navigationWithoutShiftClearsCrossEditorSelection()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("title\n\n## middle\n\n- last"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick ? quick->rootObject() : nullptr;
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 3);
        const auto editors = root->property("editors").toList();
        auto      *first   = editors.constFirst().value<QObject *>();
        auto      *last    = editors.constLast().value<QObject *>();
        QVERIFY(first);
        QVERIFY(last);
        QVERIFY(QMetaObject::invokeMethod(root, "applyDocumentSelection", Q_ARG(QVariant, QVariant::fromValue(first)),
                                          Q_ARG(QVariant, 1), Q_ARG(QVariant, QVariant::fromValue(last)),
                                          Q_ARG(QVariant, 2)));
        QTRY_VERIFY(root->property("selectionSpansEditors").toBool());

        QTest::keyClick(quick, Qt::Key_Left);

        QTRY_VERIFY(!root->property("selectionSpansEditors").toBool());
        QTRY_COMPARE(root->property("activeEditor").value<QObject *>(), first);
        QCOMPARE(first->property("cursorPosition").toInt(), 1);
        for (const QVariant &value : root->property("editors").toList()) {
            auto *candidate = value.value<QObject *>();
            QVERIFY(candidate);
            QCOMPARE(candidate->property("selectionStart").toInt(), candidate->property("selectionEnd").toInt());
        }
    }

    void deleteRemovesSelectedListItems()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("1. one\n2. two\n3. three\n4. four"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QTRY_COMPARE(root->property("editors").toList().size(), 4);
        const auto editors = root->property("editors").toList();
        auto      *second  = editors[1].value<QObject *>();
        auto      *third   = editors[2].value<QObject *>();
        QVERIFY(second && third);
        const bool selectionApplied = QMetaObject::invokeMethod(
            root, "applyDocumentSelection", Q_ARG(QVariant, QVariant::fromValue(third)), Q_ARG(QVariant, 1),
            Q_ARG(QVariant, QVariant::fromValue(second)), Q_ARG(QVariant, 1));
        QVERIFY(selectionApplied);
        QTest::keyClick(quick, Qt::Key_Delete);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "one", "four" }));

        QTRY_COMPARE(root->property("editors").toList().size(), 2);
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 1)));
        const auto geometry = geometryValue.toMap();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + 8, geometry["y"].toInt() + geometry["height"].toInt() / 2));
        QTest::qWait(30);
        QCOMPARE(editor.model()->data(editor.model()->index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "one", "four" }));
    }

    void reverseSelectionFromListItemBoundaryDoesNotResurrectDeletedText()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("xx\n1. a\n2. b"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        auto *root  = quick->rootObject();
        QVERIFY(root);
        QTRY_COMPARE(root->property("editors").toList().size(), 3);
        const auto editors = root->property("editors").toList();
        auto      *text    = editors[0].value<QObject *>();
        auto      *first   = editors[1].value<QObject *>();
        auto      *second  = editors[2].value<QObject *>();
        QVERIFY(text && first && second);

        QVERIFY(QMetaObject::invokeMethod(root, "applyDocumentSelection", Q_ARG(QVariant, QVariant::fromValue(second)),
                                          Q_ARG(QVariant, second->property("length")),
                                          Q_ARG(QVariant, QVariant::fromValue(first)),
                                          Q_ARG(QVariant, first->property("length"))));
        QTest::keyClick(quick, Qt::Key_Delete);

        QTRY_COMPARE(editor.model()->rowCount(), 2);
        QTRY_COMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ItemsRole).toStringList(),
                     QStringList({ "a" }));
        QTRY_COMPARE(root->property("editors").toList().size(), 2);

        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 0)));
        const auto geometry = geometryValue.toMap();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + geometry["width"].toInt() - 8,
                                 geometry["y"].toInt() + geometry["height"].toInt() / 2));
        QTest::qWait(30);

        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "a" }));
        QCOMPARE(editor.contents(), QStringLiteral("xx\n\n1. a"));
    }

    void downLeavesLastStructuredBlock()
    {
        QmlNoteEditor listEditor;
        listEditor.resize(500, 350);
        listEditor.load(QStringLiteral("- one\n- two"), Note::Markdown);
        listEditor.show();
        QTest::qWait(30);
        auto    *listQuick = listEditor.findChild<QQuickWidget *>();
        auto    *listRoot  = listQuick->rootObject();
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(listRoot, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 1)));
        const auto geometry = geometryValue.toMap();
        QTest::mouseClick(listQuick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + 8, geometry["y"].toInt() + geometry["height"].toInt() / 2));
        QTest::keyClick(listQuick, Qt::Key_End);
        QTest::keyClick(listQuick, Qt::Key_Down);
        QTRY_COMPARE(listEditor.model()->rowCount(), 2);
        QTRY_COMPARE(listRoot->property("activeEditor").value<QObject *>()->property("blockIndex").toInt(), 1);
        QCOMPARE(listEditor.model()->data(listEditor.model()->index(1), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Text));
        auto *listActive = listRoot->property("activeEditor").value<QObject *>();
        QVERIFY(listActive);
        QTRY_VERIFY(listActive->property("activeFocus").toBool());
        QTest::keyClick(listQuick, Qt::Key_Up);
        QTRY_COMPARE(listRoot->property("activeEditor").value<QObject *>()->property("blockIndex").toInt(), 0);
    }

    void downLeavesLastTableRow()
    {
        QmlNoteEditor tableEditor;
        tableEditor.resize(500, 350);
        tableEditor.load(QStringLiteral("| Aaa | Bbb |\n| --- | --- |\n| Ccc | Ddd |"), Note::Markdown);
        tableEditor.show();
        QTest::qWait(30);
        auto *tableQuick        = tableEditor.findChild<QQuickWidget *>();
        auto *tableRoot         = tableQuick->rootObject();
        auto  tableCellGeometry = [tableRoot](int index) {
            QVariant result;
            QMetaObject::invokeMethod(tableRoot, "editorGeometry", Q_RETURN_ARG(QVariant, result),
                                       Q_ARG(QVariant, index));
            return result.toMap();
        };
        QTRY_VERIFY(!tableCellGeometry(3).isEmpty());
        const auto tableGeometry = tableCellGeometry(3);
        QTest::mouseClick(tableQuick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(tableGeometry["x"].toInt() + tableGeometry["width"].toInt() / 2,
                                 tableGeometry["y"].toInt() + tableGeometry["height"].toInt() / 2));
        auto activeIndex = [tableRoot]() {
            QVariant result;
            QMetaObject::invokeMethod(tableRoot, "activeEditorIndex", Q_RETURN_ARG(QVariant, result));
            return result.toInt();
        };
        QTRY_COMPARE(activeIndex(), 3);
        QTest::keyClick(tableQuick, Qt::Key_End);
        QTest::keyClick(tableQuick, Qt::Key_Down);
        QTRY_COMPARE(tableRoot->property("activeEditor").value<QObject *>()->property("blockIndex").toInt(), 1);
        QTRY_COMPARE(tableEditor.model()->rowCount(), 2);
        auto *tableActive = tableRoot->property("activeEditor").value<QObject *>();
        QVERIFY(tableActive);
        QTRY_VERIFY(tableActive->property("activeFocus").toBool());
        QTest::keyClick(tableQuick, Qt::Key_Up);
        QTRY_COMPARE(tableRoot->property("activeEditor").value<QObject *>()->property("blockIndex").toInt(), 0);
    }

    void backspaceRemovesEmptyTableRow()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |"), Note::Markdown);
        editor.model()->insertTableRow(0, 2);
        editor.show();
        QTest::qWait(30);
        auto    *quick = editor.findChild<QQuickWidget *>();
        auto    *root  = quick->rootObject();
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 4)));
        const auto geometry = geometryValue.toMap();
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + 8, geometry["y"].toInt() + geometry["height"].toInt() / 2));
        QTest::keyClick(quick, Qt::Key_Backspace);
        QTRY_COMPARE(editor.model()
                         ->data(editor.model()->index(0), NoteBlockModel::CellsRole)
                         .toMap()[QStringLiteral("values")]
                         .toStringList()
                         .size(),
                     4);
        const auto activeIndex = [root] {
            QVariant value;
            QMetaObject::invokeMethod(root, "activeEditorIndex", Q_RETURN_ARG(QVariant, value));
            return value.toInt();
        };
        QTRY_COMPARE(activeIndex(), 3);
        QCOMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 3);
    }

    void backspaceInEmptyTableCellMovesToPreviousCell()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("| A | B |\n| --- | --- |\n| C | |"), Note::Markdown);
        editor.show();
        QTest::qWait(30);
        auto    *quick = editor.findChild<QQuickWidget *>();
        auto    *root  = quick->rootObject();
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 3)));
        const auto geometry = geometryValue.toMap();
        QVERIFY(!geometry.isEmpty());
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry["x"].toInt() + 8, geometry["y"].toInt() + geometry["height"].toInt() / 2));

        QTest::keyClick(quick, Qt::Key_Backspace);

        const auto activeIndex = [root] {
            QVariant value;
            QMetaObject::invokeMethod(root, "activeEditorIndex", Q_RETURN_ARG(QVariant, value));
            return value.toInt();
        };
        QTRY_COMPARE(activeIndex(), 2);
        QCOMPARE(root->property("activeEditor").value<QObject *>()->property("cursorPosition").toInt(), 1);
        QCOMPARE(editor.model()
                     ->data(editor.model()->index(0), NoteBlockModel::CellsRole)
                     .toMap()[QStringLiteral("values")]
                     .toStringList(),
                 QStringList({ "A", "B", "C", "" }));
    }

    void boundaryDeleteRemovesCompletelyEmptyTable()
    {
        const auto makeEmptyTable = [](QmlNoteEditor &editor) {
            editor.load(QStringLiteral("placeholder"), Note::Markdown);
            editor.model()->removeBlock(0);
            editor.model()->insertTable(0);
            editor.resize(500, 350);
            editor.show();
            QTest::qWait(30);
        };
        const auto clickCell = [](QmlNoteEditor &editor, int cell) {
            auto    *quick = editor.findChild<QQuickWidget *>();
            auto    *root  = quick->rootObject();
            QVariant geometryValue;
            QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                      Q_ARG(QVariant, cell));
            const auto geometry = geometryValue.toMap();
            QTest::mouseClick(
                quick, Qt::LeftButton, Qt::NoModifier,
                QPoint(geometry["x"].toInt() + 8, geometry["y"].toInt() + geometry["height"].toInt() / 2));
            return quick;
        };

        QmlNoteEditor backspaceEditor;
        makeEmptyTable(backspaceEditor);
        auto *backspaceQuick = clickCell(backspaceEditor, 0);
        QTest::keyClick(backspaceQuick, Qt::Key_Backspace);
        QTRY_COMPARE(backspaceEditor.model()->rowCount(), 1);
        QCOMPARE(backspaceEditor.model()->data(backspaceEditor.model()->index(0), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Text));
        QCOMPARE(backspaceEditor.contents(), QString());

        QmlNoteEditor deleteEditor;
        makeEmptyTable(deleteEditor);
        auto *deleteQuick = clickCell(deleteEditor, 3);
        QTest::keyClick(deleteQuick, Qt::Key_Delete);
        QTRY_COMPARE(deleteEditor.model()->rowCount(), 1);
        QCOMPARE(deleteEditor.model()->data(deleteEditor.model()->index(0), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Text));
        QCOMPARE(deleteEditor.contents(), QString());
    }

    void insertsTableAndNumberedListBlocks()
    {
        QmlNoteEditor editor;
        editor.resize(500, 350);
        editor.load(QStringLiteral("text"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        editor.insertTable();
        QTRY_COMPARE(editor.model()->rowCount(), 2);
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::TypeRole).toInt(),
                 int(NoteBlockModel::Table));
        QCOMPARE(editor.model()
                     ->data(editor.model()->index(1), NoteBlockModel::CellsRole)
                     .toMap()[QStringLiteral("values")]
                     .toStringList()
                     .size(),
                 4);

        QTest::qWait(30);
        editor.insertList(NoteBlockModel::NumberedList);
        QTRY_COMPARE(editor.model()->rowCount(), 3);
        QList<int> insertedTypes;
        insertedTypes << editor.model()->data(editor.model()->index(1), NoteBlockModel::TypeRole).toInt()
                      << editor.model()->data(editor.model()->index(2), NoteBlockModel::TypeRole).toInt();
        QVERIFY(insertedTypes.contains(int(NoteBlockModel::Table)));
        QVERIFY(insertedTypes.contains(int(NoteBlockModel::NumberedList)));
    }

    void selectingInsideTableCellAvoidsFullEditorScan()
    {
        QmlNoteEditor editor;
        editor.resize(600, 400);
        editor.load(QStringLiteral("| Name | Value |\n| --- | --- |\n| one | a fairly long value to select |"),
                    Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 3)));
        const auto geometry = geometryValue.toMap();
        QVERIFY(!geometry.isEmpty());
        const QPoint start(geometry[QStringLiteral("x")].toInt() + 8,
                           geometry[QStringLiteral("y")].toInt() + geometry[QStringLiteral("height")].toInt() / 2);
        const QPoint end(geometry[QStringLiteral("x")].toInt() + geometry[QStringLiteral("width")].toInt() - 8,
                         start.y());

        for (int iteration = 0; iteration < 20; ++iteration) {
            QVERIFY(QMetaObject::invokeMethod(root, "selectAllDocument"));
            QVERIFY(QMetaObject::invokeMethod(root, "clearDocumentSelection"));
        }

        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, start);
        const int fullSelectionPasses = root->property("fullSelectionPasses").toInt();
        QTest::mouseMove(quick, end, 30);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, end);

        QCOMPARE(root->property("fullSelectionPasses").toInt(), fullSelectionPasses);
    }

    void navigatesTableCellsWithArrowKeys()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("| Aaa | Bbb |\n| --- | --- |\n| Ccc | Ddd |"), Note::Markdown);
        editor.show();
        QTest::qWait(30);

        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);
        auto isBold = [root](int index) {
            QVariant result;
            QMetaObject::invokeMethod(root, "editorIsBold", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, index));
            return result.toBool();
        };
        QTRY_VERIFY(isBold(0));
        QVERIFY(isBold(1));
        QVERIFY(!isBold(2));
        QVERIFY(!isBold(3));
        auto activeIndex = [root]() {
            QVariant result;
            QMetaObject::invokeMethod(root, "activeEditorIndex", Q_RETURN_ARG(QVariant, result));
            return result.toInt();
        };
        QVariant geometryValue;
        QVERIFY(QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, geometryValue),
                                          Q_ARG(QVariant, 0)));
        const auto   geometry = geometryValue.toMap();
        const QPoint cellCenter(geometry[QStringLiteral("x")].toInt() + geometry[QStringLiteral("width")].toInt() / 2,
                                geometry[QStringLiteral("y")].toInt() + geometry[QStringLiteral("height")].toInt() / 2);
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, cellCenter);
        QTRY_COMPARE(activeIndex(), 0);

        QTest::keyClick(quick, Qt::Key_Home);
        QTest::keyClick(quick, Qt::Key_Left);
        QTRY_COMPARE(activeIndex(), 0);
        QTest::keyClick(quick, Qt::Key_Right);
        QTRY_COMPARE(activeIndex(), 0);

        QTest::keyClick(quick, Qt::Key_End);
        QTest::keyClick(quick, Qt::Key_Right);
        QTRY_COMPARE(activeIndex(), 1);
        QTest::keyClick(quick, Qt::Key_Down);
        QTRY_COMPARE(activeIndex(), 3);
        QTest::keyClick(quick, Qt::Key_Home);
        QTest::keyClick(quick, Qt::Key_Left);
        QTRY_COMPARE(activeIndex(), 2);
        QTest::keyClick(quick, Qt::Key_Up);
        QTRY_COMPARE(activeIndex(), 0);
    }

    void dragsSelectionAcrossTextBlocks()
    {
        QmlNoteEditor editor;
        editor.resize(500, 400);
        editor.load(QStringLiteral("first paragraph\n\n- middle\n\nsecond paragraph"), Note::Markdown);
        editor.show();
        QTest::qWait(40);
        auto *quick = editor.findChild<QQuickWidget *>();
        QVERIFY(quick);
        auto *root = quick->rootObject();
        QVERIFY(root);

        auto geometry = [root](int index) {
            QVariant result;
            QMetaObject::invokeMethod(root, "editorGeometry", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, index));
            return result.toMap();
        };
        QTRY_VERIFY(!geometry(2).isEmpty());
        const auto   firstGeometry  = geometry(0);
        const auto   secondGeometry = geometry(2);
        const QPoint first(firstGeometry[QStringLiteral("x")].toInt() + 8,
                           firstGeometry[QStringLiteral("y")].toInt()
                               + firstGeometry[QStringLiteral("height")].toInt() / 2);
        const QPoint second(
            secondGeometry[QStringLiteral("x")].toInt() + secondGeometry[QStringLiteral("width")].toInt() - 8,
            secondGeometry[QStringLiteral("y")].toInt() + secondGeometry[QStringLiteral("height")].toInt() / 2);

        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, first);
        QTest::mouseMove(quick, second, 30);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, second);

        auto selectedEditorCount = [root]() {
            QVariant result;
            QMetaObject::invokeMethod(root, "selectedEditorCount", Q_RETURN_ARG(QVariant, result));
            return result.toInt();
        };
        QTRY_COMPARE(selectedEditorCount(), 3);
    }
};

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND"))
        qputenv("QSG_RHI_BACKEND", QByteArrayLiteral("software"));

    QApplication      application(argc, argv);
    QmlNoteEditorTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "qmlnoteeditor_test.moc"
