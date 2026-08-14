#include "notetransfercontroller.h"

#include <QBuffer>
#include <QMimeData>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>

using namespace AnyKeep;

class NoteTransferControllerTest : public QObject {
    Q_OBJECT

private slots:
    void exportsMultipleFormatsAndRestoresPrivateFragment();
    void exportsPlainTextWithoutRichFormats();
    void preservesMarkdownHardBreaksInPlainText();
    void roundTripsCodeBlockWithoutFormatting();
    void importsMarkdownBeforeHtmlAndPlainText();
    void importsTsvAsTable();
    void importsHtmlTableAsTable();
    void exportsSingleTableCellAsCompactPlainText();
    void importsInlineHtmlLinkWithinParagraph();
    void importsHtmlBoldAsBold();
    void splitsInlineFormattingAtHtmlParagraphBoundaries();
    void importsHtmlUnderlineAsGithubIns();
    void exportsGithubUnderlineAsHtmlUnderline();
    void roundTripsSingleImageAsPng();
    void malformedPrivateFormatFallsBackToPlainText();
    void convertsPlainTextNotesToMarkdownWithoutChangingTheirText();
};

void NoteTransferControllerTest::exportsMultipleFormatsAndRestoresPrivateFragment()
{
    NoteFragment      fragment;
    NoteFragmentBlock list;
    list.type      = NoteFragmentBlockType::List;
    list.listItems = { { QStringLiteral("**done**"), 0, NoteFragmentListKind::Check, true },
                       { QStringLiteral("nested"), 1, NoteFragmentListKind::Numbered, false } };
    fragment.blocks.append(list);

    NoteTransferController controller;
    const auto             exported = controller.createMimeData(fragment);
    QVERIFY2(exported, qPrintable(exported.error));
    QVERIFY(exported.mimeData->hasFormat(QString::fromLatin1(NoteTransferController::FragmentMimeType)));
    QVERIFY(exported.mimeData->hasFormat(QString::fromLatin1(NoteTransferController::MarkdownMimeType)));
    QVERIFY(exported.mimeData->hasHtml());
    QVERIFY(exported.mimeData->hasText());
    QCOMPARE(QString::fromUtf8(exported.mimeData->data(QString::fromLatin1(NoteTransferController::MarkdownMimeType))),
             QStringLiteral("- [x] **done**\n    1. nested"));

    const auto imported = controller.importMimeData(exported.mimeData.get());
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.sourceMimeType, QString::fromLatin1(NoteTransferController::FragmentMimeType));
    QCOMPARE(imported.fragment.blocks.at(0).listItems.at(1).kind, NoteFragmentListKind::Numbered);
}

void NoteTransferControllerTest::exportsPlainTextWithoutRichFormats()
{
    NoteFragment fragment;
    fragment.sourceFormat = NoteFragmentSourceFormat::PlainText;
    NoteFragmentBlock block;
    block.type     = NoteFragmentBlockType::Text;
    block.markdown = QStringLiteral("first\r\nsecond\u2028third\u2029fourth");
    fragment.blocks.append(block);

    NoteTransferController controller;
    const auto             exported = controller.createMimeData(fragment);
    QVERIFY2(exported, qPrintable(exported.error));
    QVERIFY(exported.mimeData->hasFormat(QString::fromLatin1(NoteTransferController::FragmentMimeType)));
    QVERIFY(exported.mimeData->hasText());
    QVERIFY(!exported.mimeData->hasFormat(QString::fromLatin1(NoteTransferController::MarkdownMimeType)));
    QVERIFY(!exported.mimeData->hasHtml());
    QCOMPARE(exported.mimeData->text(), QStringLiteral("first\nsecond\nthird\nfourth"));
}

void NoteTransferControllerTest::preservesMarkdownHardBreaksInPlainText()
{
    NoteFragment fragment;
    fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
    NoteFragmentBlock block;
    block.type     = NoteFragmentBlockType::Text;
    block.markdown = QStringLiteral("first line  \nsecond line\n\nthird paragraph");
    fragment.blocks.append(block);

    NoteTransferController controller;
    const auto             exported = controller.createMimeData(fragment);
    QVERIFY2(exported, qPrintable(exported.error));
    QVERIFY(exported.mimeData->hasFormat(QString::fromLatin1(NoteTransferController::MarkdownMimeType)));
    QVERIFY(exported.mimeData->hasHtml());
    QCOMPARE(exported.mimeData->text(), QStringLiteral("first line  \nsecond line\nthird paragraph"));
}

void NoteTransferControllerTest::exportsSingleTableCellAsCompactPlainText()
{
    NoteFragment fragment;
    fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
    NoteFragmentBlock table;
    table.type                = NoteFragmentBlockType::Table;
    table.table.rows          = 1;
    table.table.columns       = 1;
    table.table.headerRows    = 1;
    table.table.markdownCells = { QStringLiteral("copied cell") };
    fragment.blocks.append(table);

    NoteTransferController controller;
    const auto             exported = controller.createMimeData(fragment);
    QVERIFY2(exported, qPrintable(exported.error));
    QCOMPARE(exported.mimeData->text(), QStringLiteral("copied cell"));
    QVERIFY(exported.mimeData->hasFormat(QString::fromLatin1(NoteTransferController::FragmentMimeType)));
    QVERIFY(exported.mimeData->hasFormat(QString::fromLatin1(NoteTransferController::TsvMimeType)));

    table.table.rows          = 2;
    table.table.columns       = 2;
    table.table.markdownCells = { QStringLiteral("**Name**"), QStringLiteral("[Value](https://example.org)"),
                                  QStringLiteral("first"), QStringLiteral("second") };
    fragment.blocks           = { table };
    const auto tableExport    = controller.createMimeData(fragment);
    QVERIFY2(tableExport, qPrintable(tableExport.error));
    QCOMPARE(tableExport.mimeData->text(), QStringLiteral("Name\tValue\nfirst\tsecond"));
}

void NoteTransferControllerTest::roundTripsCodeBlockWithoutFormatting()
{
    NoteFragment fragment;
    fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
    NoteFragmentBlock code;
    code.type     = NoteFragmentBlockType::CodeBlock;
    code.language = QStringLiteral("python");
    code.markdown = QStringLiteral("value = '**not bold**'\n\n# not a heading\n");
    fragment.blocks.append(code);

    NoteTransferController controller;
    const auto             exported = controller.createMimeData(fragment);
    QVERIFY2(exported, qPrintable(exported.error));
    QCOMPARE(QString::fromUtf8(exported.mimeData->data(QString::fromLatin1(NoteTransferController::MarkdownMimeType))),
             QStringLiteral("```python\nvalue = '**not bold**'\n\n# not a heading\n\n```"));
    QCOMPARE(exported.mimeData->text(), QStringLiteral("value = '**not bold**'\n\n# not a heading"));

    const auto imported = controller.importMimeData(exported.mimeData.get());
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.fragment.blocks.size(), 1);
    QCOMPARE(imported.fragment.blocks.constFirst().type, NoteFragmentBlockType::CodeBlock);
    QCOMPARE(imported.fragment.blocks.constFirst().language, QStringLiteral("python"));
    QCOMPARE(imported.fragment.blocks.constFirst().markdown, code.markdown);
}

void NoteTransferControllerTest::convertsPlainTextNotesToMarkdownWithoutChangingTheirText()
{
    const QString plain    = QStringLiteral("# literal heading\n"
                                               "* literal emphasis *\n"
                                               "> literal quote\n"
                                               "1. literal list item\n"
                                               "inline *emphasis*, `code`, and [link](https://example.org)\n"
                                               "literal \\ slash\n"
                                               "ordinary text");
    const QString markdown = NoteTransferController::convertTextFormat(plain, Note::PlainText, Note::Markdown);

    QVERIFY(markdown != plain);
    QCOMPARE(NoteTransferController::convertTextFormat(markdown, Note::Markdown, Note::PlainText), plain);
}

void NoteTransferControllerTest::importsMarkdownBeforeHtmlAndPlainText()
{
    QMimeData mime;
    mime.setText(QStringLiteral("plain"));
    mime.setHtml(QStringLiteral("<p>html</p>"));
    mime.setData(QString::fromLatin1(NoteTransferController::MarkdownMimeType), QByteArrayLiteral("## markdown"));

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(&mime);
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.sourceMimeType, QString::fromLatin1(NoteTransferController::MarkdownMimeType));
    QCOMPARE(imported.fragment.blocks.at(0).type, NoteFragmentBlockType::Heading);
    QCOMPARE(imported.fragment.blocks.at(0).markdown, QStringLiteral("markdown"));
}

void NoteTransferControllerTest::importsTsvAsTable()
{
    QMimeData mime;
    mime.setData(QString::fromLatin1(NoteTransferController::TsvMimeType), QByteArrayLiteral("A\tB\n1\t2"));

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(&mime);
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.sourceMimeType, QString::fromLatin1(NoteTransferController::TsvMimeType));
    QCOMPARE(imported.fragment.blocks.size(), 1);
    QCOMPARE(imported.fragment.blocks.at(0).table.rows, 2);
    QCOMPARE(imported.fragment.blocks.at(0).table.columns, 2);
    QCOMPARE(imported.fragment.blocks.at(0).table.markdownCells, QStringList({ "A", "B", "1", "2" }));
}

void NoteTransferControllerTest::importsHtmlTableAsTable()
{
    QMimeData mime;
    mime.setHtml(QStringLiteral("<html><body><table><tr><td>A</td><td>B</td></tr>"
                                "<tr><td>1</td><td>2</td></tr></table></body></html>"));
    QImage preview(1, 1, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::red);
    mime.setImageData(preview);

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(&mime);
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.sourceMimeType, QStringLiteral("text/html"));
    QCOMPARE(imported.fragment.blocks.size(), 1);
    QCOMPARE(imported.fragment.blocks.at(0).type, NoteFragmentBlockType::Table);
    QCOMPARE(imported.fragment.blocks.at(0).table.rows, 2);
    QCOMPARE(imported.fragment.blocks.at(0).table.columns, 2);
    QCOMPARE(imported.fragment.blocks.at(0).table.markdownCells, QStringList({ "A", "B", "1", "2" }));
}

void NoteTransferControllerTest::importsInlineHtmlLinkWithinParagraph()
{
    QMimeData mime;
    mime.setHtml(QStringLiteral("<p>before <a href=\"https://example.org\">link</a> after</p>"));

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(&mime);
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.fragment.blocks.size(), 1);
    QString error;
    QCOMPARE(controller.markdownForFragment(imported.fragment, &error),
             QStringLiteral("before [link](https://example.org) after"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void NoteTransferControllerTest::importsHtmlBoldAsBold()
{
    const QStringList samples {
        QStringLiteral("<p><b>bold</b></p>"),
        QStringLiteral("<p><strong>bold</strong></p>"),
        QStringLiteral("<p><span style=\"font-weight:bold\">bold</span></p>"),
        QStringLiteral("<p><span style=\"font-weight:600\">bold</span></p>"),
        QStringLiteral("<p><span style=\"font-weight:700\">bold</span></p>"),
    };
    NoteTransferController controller;
    for (const QString &html : samples) {
        QMimeData mime;
        mime.setHtml(html);
        const auto imported = controller.importMimeData(&mime);
        QVERIFY2(imported, qPrintable(imported.error));
        QString error;
        QCOMPARE(controller.markdownForFragment(imported.fragment, &error), QStringLiteral("**bold**"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
}

void NoteTransferControllerTest::splitsInlineFormattingAtHtmlParagraphBoundaries()
{
    QMimeData sourceMime;
    sourceMime.setHtml(QStringLiteral(
        "<p>Turn your text into bold cursive — <em>perfect for standout usernames, titles, and</em></p>"
        "<p><em>short phrases</em>. Choose from Bold Script, Mathematical Bold, Bold Italic, and more.</p>"));

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(&sourceMime);
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.fragment.blocks.size(), 2);
    QCOMPARE(imported.fragment.blocks.at(0).markdown,
             QStringLiteral("Turn your text into bold cursive — *perfect for standout usernames, titles, and*"));
    QCOMPARE(imported.fragment.blocks.at(1).markdown,
             QStringLiteral("*short phrases*. Choose from Bold Script, Mathematical Bold, Bold Italic, and more."));
    const auto exported = controller.createMimeData(imported.fragment);
    QVERIFY2(exported, qPrintable(exported.error));

    const QString markdown
        = QStringLiteral("Turn your text into bold cursive — *perfect for standout usernames, titles, and*\n\n"
                         "*short phrases*. Choose from Bold Script, Mathematical Bold, Bold Italic, and more.");
    QCOMPARE(QString::fromUtf8(exported.mimeData->data(QString::fromLatin1(NoteTransferController::MarkdownMimeType))),
             markdown);
    QCOMPARE(exported.mimeData->text(),
             QStringLiteral("Turn your text into bold cursive — perfect for standout usernames, titles, and\n"
                            "short phrases. Choose from Bold Script, Mathematical Bold, Bold Italic, and more."));
    QVERIFY(exported.mimeData->hasHtml());
}

void NoteTransferControllerTest::importsHtmlUnderlineAsGithubIns()
{
    QMimeData mime;
    mime.setHtml(QStringLiteral("<table><tr><td>plain <u>underlined</u></td></tr></table>"));

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(&mime);
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.fragment.blocks.size(), 1);
    QCOMPARE(imported.fragment.blocks.constFirst().type, NoteFragmentBlockType::Table);
    QCOMPARE(imported.fragment.blocks.constFirst().table.markdownCells,
             QStringList({ QStringLiteral("plain <ins>underlined</ins>") }));
}

void NoteTransferControllerTest::exportsGithubUnderlineAsHtmlUnderline()
{
    NoteFragment fragment;
    fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
    NoteFragmentBlock block;
    block.type     = NoteFragmentBlockType::Text;
    block.markdown = QStringLiteral("plain <ins>underlined</ins>");
    fragment.blocks.append(block);

    NoteTransferController controller;
    QString                error;
    QTextDocument          document;
    document.setHtml(controller.htmlForFragment(fragment, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(document.toPlainText(), QStringLiteral("plain underlined"));
    QTextCursor cursor(&document);
    cursor.setPosition(6);
    cursor.setPosition(16, QTextCursor::KeepAnchor);
    QVERIFY(cursor.charFormat().fontUnderline());

    block.markdown  = QStringLiteral("`<ins>literal</ins>`");
    fragment.blocks = { block };
    document.setHtml(controller.htmlForFragment(fragment, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(document.toPlainText(), QStringLiteral("<ins>literal</ins>"));
    cursor = QTextCursor(&document);
    cursor.setPosition(0);
    cursor.setPosition(18, QTextCursor::KeepAnchor);
    QVERIFY(!cursor.charFormat().fontUnderline());
}

void NoteTransferControllerTest::roundTripsSingleImageAsPng()
{
    QImage image(2, 2, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));

    NoteFragment      fragment;
    NoteFragmentBlock block;
    block.type            = NoteFragmentBlockType::Image;
    block.image.sourceUri = QStringLiteral("anykeep-media:/11111111-1111-1111-1111-111111111111/image.png");
    fragment.blocks.append(block);
    NoteFragmentMedia media;
    media.sourceUri              = block.image.sourceUri;
    media.reference.id           = QUuid(QStringLiteral("{11111111-1111-1111-1111-111111111111}"));
    media.reference.blobId       = QByteArray::fromHex("abcd");
    media.reference.portableName = QStringLiteral("image.png");
    media.reference.originalName = QStringLiteral("image.png");
    media.reference.mediaType    = QStringLiteral("image/png");
    media.reference.size         = buffer.data().size();
    media.data                   = buffer.data();
    fragment.media.append(media);

    // An unrelated entry must not become the copied image merely because it
    // appears first in the payload.
    NoteFragmentMedia unrelated = media;
    unrelated.sourceUri         = QStringLiteral("anykeep-media:/22222222-2222-2222-2222-222222222222/other.png");
    unrelated.reference.id      = QUuid(QStringLiteral("{22222222-2222-2222-2222-222222222222}"));
    unrelated.data              = QByteArrayLiteral("not an image");
    fragment.media.prepend(unrelated);

    NoteTransferController controller;
    const auto             exported = controller.createMimeData(fragment);
    QVERIFY2(exported, qPrintable(exported.error));
    QVERIFY(exported.mimeData->hasImage());
    QVERIFY(exported.mimeData->hasFormat(QStringLiteral("image/png")));
    const auto imported = controller.importMimeData(exported.mimeData.get());
    QVERIFY2(imported, qPrintable(imported.error));
    // The private AnyKeep representation remains the preferred round-trip.
    QCOMPARE(imported.sourceMimeType, QString::fromLatin1(NoteTransferController::FragmentMimeType));
}

void NoteTransferControllerTest::malformedPrivateFormatFallsBackToPlainText()
{
    QMimeData mime;
    mime.setData(QString::fromLatin1(NoteTransferController::FragmentMimeType), QByteArrayLiteral("bad"));
    mime.setText(QStringLiteral("fallback"));

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(&mime);
    QVERIFY2(imported, qPrintable(imported.error));
    QCOMPARE(imported.sourceMimeType, QStringLiteral("text/plain"));
    QCOMPARE(imported.fragment.blocks.at(0).markdown, QStringLiteral("fallback"));
}

QTEST_MAIN(NoteTransferControllerTest)

#include "notetransfercontroller_test.moc"
