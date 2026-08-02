#include <QtTest>

#include "noteblockmodel.h"
#include "notedata.h"
#include "notetagline.h"

using namespace QtNote;

class NoteBlockModelTest : public QObject {
    Q_OBJECT

private slots:
    void titleIsAlwaysASeparateTextBlock()
    {
        NoteBlockModel plain;
        plain.load(QStringLiteral("title\nbody\ncontinued"), false);
        QCOMPARE(plain.rowCount(), 2);
        QCOMPARE(plain.data(plain.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("title"));
        QCOMPARE(plain.data(plain.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("body\ncontinued"));
        QCOMPARE(plain.contents(), QStringLiteral("title\nbody\ncontinued"));
        QVERIFY(plain.mergeTextBlockWithNext(0));
        QCOMPARE(plain.rowCount(), 2);
        QCOMPARE(plain.data(plain.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("titlebody"));
        QCOMPARE(plain.data(plain.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("continued"));
        QCOMPARE(plain.contents(), QStringLiteral("titlebody\ncontinued"));

        NoteBlockModel markdown;
        markdown.load(QStringLiteral("Title\n\nBody paragraph"), true);
        QCOMPARE(markdown.rowCount(), 2);
        QCOMPARE(markdown.data(markdown.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("Title"));
        QCOMPARE(markdown.data(markdown.index(1), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("Body paragraph"));
        QCOMPARE(markdown.contents(), QStringLiteral("Title\n\nBody paragraph"));
        QVERIFY(markdown.mergeTextBlockWithNext(0));
        QCOMPARE(markdown.rowCount(), 1);
        QCOMPARE(markdown.contents(), QStringLiteral("TitleBody paragraph"));

        NoteBlockModel split;
        split.load(QStringLiteral("Title"), true);
        QVERIFY(split.splitTitleBlock(QStringLiteral("Ti"), QStringLiteral("tle")));
        QCOMPARE(split.rowCount(), 2);
        QCOMPARE(split.data(split.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("Ti"));
        QCOMPARE(split.data(split.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("tle"));
        QCOMPARE(split.contents(), QStringLiteral("Ti\n\ntle"));
    }

    void structuredPasteIntoTitleKeepsFollowingBlocks()
    {
        NoteBlockModel model;
        model.load(QString(), true);

        NoteFragment      fragment;
        NoteFragmentBlock title;
        title.type     = NoteFragmentBlockType::Text;
        title.markdown = QStringLiteral("Copied title");
        fragment.blocks.append(title);

        NoteFragmentBlock list;
        list.type      = NoteFragmentBlockType::List;
        list.listItems = { { QStringLiteral("first"), 0, NoteFragmentListKind::Check, false },
                           { QStringLiteral("nested"), 1, NoteFragmentListKind::Numbered, false } };
        fragment.blocks.append(list);

        QString error;
        QCOMPARE(model.replaceTextBlockRangeWithFragment(0, QString(), QString(), fragment, &error), 0);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.blockTypeAt(0), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("Copied title"));
        QCOMPARE(model.blockTypeAt(1), int(NoteBlockModel::CheckList));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::CheckList), int(NoteBlockModel::NumberedList) }));
        QCOMPARE(model.contents(), QStringLiteral("Copied title\n\n- [ ] first\n    1. nested"));

        NoteFragment listOnly;
        listOnly.blocks.append(list);
        QCOMPARE(model.replaceTextBlockRangeWithFragment(0, QString(), QString(), listOnly, &error), -1);
        QCOMPARE(model.blockTypeAt(0), int(NoteBlockModel::Text));
    }

    void movingWholeListBlockRestoresTagLineAtBodyBoundary()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Title\n\n*tb\n\n- [ ] item"), true);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.blockTypeAt(1), int(NoteBlockModel::TagLine));
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::CheckList));

        // A one-item list is dragged through the list-item reorder path, not
        // NoteBlockModel::moveBlock(). Moving it above the title demotes the
        // TagLine; moving it back must promote the first body line again.
        QCOMPARE(model.moveListRangeToBlock(2, 0, 0, 0), 0);
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::Text));

        QCOMPARE(model.moveListRangeToBlock(0, 0, 0, model.rowCount()), 2);
        QCOMPARE(model.blockTypeAt(0), int(NoteBlockModel::Text));
        QCOMPARE(model.blockTypeAt(1), int(NoteBlockModel::TagLine));
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::CheckList));
        QCOMPARE(model.contents(), QStringLiteral("Title\n\n*tb\n\n- [ ] item"));
    }

    void inlineCodeListItemKeepsLeadingDashes()
    {
        const QString  source = QStringLiteral("Title\n\n- `--pytest-opts=\"`--maxfail=0`\"` # comment");
        NoteBlockModel model;
        model.load(source, true);
        QCOMPARE(model.contents(), source);
    }

    void parsesPromotesAndSerializesTagLine()
    {
        const QString  markdown = QStringLiteral("Title\n\n*tb *interview\n\nhello tom boy");
        NoteBlockModel model;
        model.load(markdown, true);

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::TagLine));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TagsRole).toStringList(),
                 QStringList({ QStringLiteral("tb"), QStringLiteral("interview") }));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("*tb *interview"));
        QCOMPARE(model.contents(), markdown);

        const QString body = model.contents().section(QStringLiteral("\n\n"), 1);
        QCOMPARE(NoteData::tagsFromText(body), QStringList({ QStringLiteral("tb"), QStringLiteral("interview") }));

        NoteBlockModel promoted;
        promoted.load(QStringLiteral("Title"), true);
        const QVariantMap result = promoted.promoteTagLineFromText(0, QStringLiteral("Title\n\n*tb *work "),
                                                                   QStringLiteral("Title\n\n\\*tb \\*work"), 18, false);
        QVERIFY(result.value(QStringLiteral("handled")).toBool());
        QCOMPARE(promoted.rowCount(), 2);
        QCOMPARE(promoted.data(promoted.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::TagLine));
        QCOMPARE(promoted.contents(), QStringLiteral("Title\n\n*tb *work"));

        NoteBlockModel separate;
        separate.load(QStringLiteral("Title"), true);
        separate.insertTextBlock(1);
        const QVariantMap separateResult
            = separate.promoteTagLineFromText(1, QStringLiteral("*tb "), QStringLiteral("\\*tb"), 4, false);
        QVERIFY(separateResult.value(QStringLiteral("handled")).toBool());
        QCOMPARE(separate.data(separate.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::TagLine));
        QCOMPARE(separate.contents(), QStringLiteral("Title\n\n*tb"));
    }

    void tagLineUsesFormatSpecificBodyBoundary()
    {
        const auto plain = NoteTagLine::findPlainTextDocumentTagLine(QStringLiteral("Title\n*tb *work"));
        QVERIFY(plain);
        QCOMPARE(plain->tags, QStringList({ QStringLiteral("tb"), QStringLiteral("work") }));
        QVERIFY(!NoteTagLine::findPlainTextDocumentTagLine(QStringLiteral("Title\n\n*tb")));

        const auto markdown = NoteTagLine::findMarkdownDocumentTagLine(QStringLiteral("Title\n\n*tb *work\n\nBody"));
        QVERIFY(markdown);
        QCOMPARE(markdown->tags, QStringList({ QStringLiteral("tb"), QStringLiteral("work") }));
        QVERIFY(!NoteTagLine::findMarkdownDocumentTagLine(QStringLiteral("Title\n*tb")));

        const QChar    paragraphSeparator(QChar::ParagraphSeparator);
        NoteBlockModel projected;
        projected.load(QStringLiteral("Title"), true);
        const QVariantMap result
            = projected.promoteTagLineFromText(0, QStringLiteral("Title") + paragraphSeparator + QStringLiteral("*tb "),
                                               QStringLiteral("Title\n\n\\*tb"), 10, false);
        QVERIFY(result.value(QStringLiteral("handled")).toBool());
        QCOMPARE(projected.data(projected.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::TagLine));
        QCOMPARE(projected.contents(), QStringLiteral("Title\n\n*tb"));

        QCOMPARE(NoteData::tagsFromText(QStringLiteral("*plain *second\nbody")),
                 QStringList({ QStringLiteral("plain"), QStringLiteral("second") }));
        QCOMPARE(NoteData::tagsFromText(QStringLiteral("*plain") + paragraphSeparator + QStringLiteral("body")),
                 QStringList({ QStringLiteral("plain") }));
    }

    void escapedOrMixedTagLinesRemainOrdinaryText()
    {
        NoteBlockModel escaped;
        escaped.load(QStringLiteral("Title\n\n\\*tb\n\nhello"), true);
        QCOMPARE(escaped.rowCount(), 2);
        QCOMPARE(escaped.data(escaped.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(escaped.data(escaped.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QVERIFY(NoteData::tagsFromText(QStringLiteral("\\*tb\nhello")).isEmpty());

        NoteBlockModel mixed;
        mixed.load(QStringLiteral("Title\n\n*tb extra\n\nhello"), true);
        QCOMPARE(mixed.rowCount(), 2);
        QCOMPARE(mixed.data(mixed.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(mixed.data(mixed.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QVERIFY(NoteData::tagsFromText(QStringLiteral("*tb extra\nhello")).isEmpty());
    }

    void invalidTagEditRestoresOrdinaryTextAndCursor()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Title\n\n*tb *work\n\nhello"), true);

        const QVariantMap result = model.setTagLineTag(1, 0, QStringLiteral("*tb?"), 3);
        QVERIFY(result.value(QStringLiteral("handled")).toBool());
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("*tb? *work"));
        QCOMPARE(result.value(QStringLiteral("cursorPosition")).toInt(), 3);
    }

    void tagLineRoundTripsThroughBlockFragment()
    {
        NoteBlockModel source;
        source.load(QStringLiteral("Title\n\n*tb *work\n\nbody"), true);
        const NoteFragment fragment = source.extractBlockFragment(1, 1);
        QCOMPARE(fragment.blocks.size(), 1);
        QCOMPARE(fragment.blocks.constFirst().type, NoteFragmentBlockType::TagLine);
        QCOMPARE(fragment.blocks.constFirst().tags, QStringList({ QStringLiteral("tb"), QStringLiteral("work") }));

        NoteBlockModel destination;
        destination.load(QStringLiteral("Other"), true);
        QString error;
        QVERIFY2(destination.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(destination.contents(), QStringLiteral("Other\n\n*tb *work"));
    }

    void structuredPastePromotesOnlyTheFirstBodyTagLine()
    {
        NoteFragment      textFragment;
        NoteFragmentBlock text;
        text.type     = NoteFragmentBlockType::Text;
        text.markdown = QStringLiteral("*tb *work");
        textFragment.blocks.append(text);

        NoteBlockModel firstBody;
        firstBody.load(QStringLiteral("Title"), true);
        QString error;
        QVERIFY2(firstBody.insertBlockFragment(1, textFragment, &error), qPrintable(error));
        QCOMPARE(firstBody.data(firstBody.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::TagLine));
        QCOMPARE(firstBody.contents(), QStringLiteral("Title\n\n*tb *work"));

        NoteFragment      tagFragment;
        NoteFragmentBlock tags;
        tags.type = NoteFragmentBlockType::TagLine;
        tags.tags = { QStringLiteral("tb") };
        tagFragment.blocks.append(tags);

        NoteBlockModel afterBody;
        afterBody.load(QStringLiteral("Title\n\nbody"), true);
        QVERIFY2(afterBody.insertBlockFragment(2, tagFragment, &error), qPrintable(error));
        QCOMPARE(afterBody.data(afterBody.index(2), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(afterBody.data(afterBody.index(2), NoteBlockModel::TextRole).toString(), QStringLiteral("*tb"));
        QVERIFY(NoteData::tagsFromText(afterBody.contents().section(QStringLiteral("\n\n"), 1)).isEmpty());
    }

    void tagLineBecomesOrdinaryTextWhenContentMovesBeforeIt()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Title\n\n*tb\n\nbody"), true);

        model.insertImage(1, QStringLiteral("media://image"), QStringLiteral("image"));
        QCOMPARE(model.data(model.index(2), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(2), NoteBlockModel::TextRole).toString(), QStringLiteral("*tb"));
        QVERIFY(NoteData::tagsFromText(model.contents().section(QStringLiteral("\n\n"), 1)).isEmpty());
    }

    void parsesAndWritesGithubBlocks()
    {
        const QString  markdown = QStringLiteral("A [link](https://example.org)\n\n"
                                                  "- one\n- two\n\n"
                                                  "- [ ] todo\n- [x] done\n\n"
                                                  "| Name | Value |\n| --- | --- |\n| a | b |\n\n"
                                                  "![cat](media://cat)");
        NoteBlockModel model;
        model.load(markdown, true);
        QCOMPARE(model.rowCount(), 5);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BulletList));
        QCOMPARE(model.data(model.index(2), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::CheckList));
        QCOMPARE(model.data(model.index(3), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Table));
        QCOMPARE(model.data(model.index(4), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Image));
        QVERIFY(model.contents().contains(QStringLiteral("- [x] done")));
        QVERIFY(model.contents().contains(QStringLiteral("| Name | Value |")));
        QVERIFY(model.contents().contains(QStringLiteral("[link](https://example.org)")));
    }

    void serializesAndParsesImagePresentation()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("![A & B](media://image?x=1&y=2)"), true);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ImageWidthRole).toInt(), 0);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ImageAlignmentRole).toString(), QStringLiteral("center"));

        model.setImageWidth(0, 320);
        model.setImageAlignment(0, QStringLiteral("right"));
        const QString html = QStringLiteral(
            "<p align=\"right\"><img src=\"media://image?x=1&amp;y=2\" alt=\"A &amp; B\" width=\"320\" /></p>");
        QCOMPARE(model.contents(), html);

        NoteBlockModel restored;
        restored.load(html, true);
        QCOMPARE(restored.rowCount(), 1);
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Image));
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::UrlRole).toString(),
                 QStringLiteral("media://image?x=1&y=2"));
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::AltRole).toString(), QStringLiteral("A & B"));
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ImageWidthRole).toInt(), 320);
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ImageAlignmentRole).toString(),
                 QStringLiteral("right"));
        QCOMPARE(restored.contents(), html);

        const NoteFragment fragment = restored.extractBlockFragment(0, 0);
        QCOMPARE(fragment.blocks.constFirst().image.width, 320);
        QCOMPARE(fragment.blocks.constFirst().image.alignment, QStringLiteral("right"));
        NoteBlockModel transferred;
        transferred.load(QStringLiteral("before"), true);
        QString error;
        QVERIFY2(transferred.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(transferred.contents(), QStringLiteral("before\n\n") + html);

        restored.setImageWidth(0, 0);
        restored.setImageAlignment(0, QStringLiteral("center"));
        QCOMPARE(restored.contents(), QStringLiteral("![A & B](media://image?x=1&y=2)"));

        restored.setImageAlignment(0, QStringLiteral("left"));
        QCOMPARE(restored.contents(),
                 QStringLiteral("<p align=\"left\"><img src=\"media://image?x=1&amp;y=2\" "
                                "alt=\"A &amp; B\" /></p>"));
        restored.setImageAlignment(0, QStringLiteral("unsupported"));
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ImageAlignmentRole).toString(),
                 QStringLiteral("center"));

        const QString spacedHtml
            = QStringLiteral("<p align=\"left\"><img src=\"media://spaced\" alt=\"A  &quot;B&quot;\" /></p>");
        NoteBlockModel spaced;
        spaced.load(spacedHtml, true);
        QCOMPARE(spaced.data(spaced.index(0), NoteBlockModel::AltRole).toString(), QStringLiteral("A  \"B\""));
        QCOMPARE(spaced.contents(), spacedHtml);
    }

    void serializesParsesAndTransfersAudioBlocks()
    {
        const QString uri = QStringLiteral("qtnote-media:/11111111-1111-1111-1111-111111111111/recording.m4a");
        const QString html
            = QStringLiteral(
                  "<audio controls src=\"%1\" title=\"Meeting &amp; notes\" "
                  "data-qtnote-duration-ms=\"91234\"></audio>\n"
                  "<div data-qtnote-audio-transcript=\"1\">First line&lt;br&gt;<br />Second &amp; final</div>")
                  .arg(uri);
        NoteBlockModel model;
        model.load(html, true);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Audio));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::UrlRole).toString(), uri);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::AltRole).toString(), QStringLiteral("Meeting & notes"));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::AudioDurationRole).toLongLong(), qint64(91234));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::AudioTranscriptRole).toString(),
                 QStringLiteral("First line<br>\nSecond & final"));
        QCOMPARE(model.contents(), html);

        const NoteFragment fragment = model.extractBlockFragment(0, 0);
        QCOMPARE(fragment.blocks.size(), 1);
        QCOMPARE(fragment.blocks.constFirst().type, NoteFragmentBlockType::Audio);
        QCOMPARE(fragment.blocks.constFirst().audio.sourceUri, uri);
        QCOMPARE(fragment.blocks.constFirst().audio.title, QStringLiteral("Meeting & notes"));
        QCOMPARE(fragment.blocks.constFirst().audio.durationMs, qint64(91234));
        QCOMPARE(fragment.blocks.constFirst().audio.transcript, QStringLiteral("First line<br>\nSecond & final"));

        NoteBlockModel transferred;
        transferred.load(QStringLiteral("before"), true);
        QString error;
        QVERIFY2(transferred.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(transferred.contents(), QStringLiteral("before\n\n") + html);

        NoteBlockModel inserted;
        inserted.load(QStringLiteral("title"), true);
        inserted.insertAudio(1, uri, QStringLiteral("Voice memo"), 2500);
        QCOMPARE(inserted.data(inserted.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Audio));
        QVERIFY(inserted.contents().contains(QStringLiteral("title=\"Voice memo\"")));
        QVERIFY(inserted.contents().contains(QStringLiteral("data-qtnote-duration-ms=\"2500\"")));
    }

    void serializesParsesAndTransfersAttachments()
    {
        const QString uri  = QStringLiteral("qtnote-media:/22222222-2222-2222-2222-222222222222/spec.pdf");
        const QString html = QStringLiteral("<a href=\"%1\" data-qtnote-attachment=\"1\" "
                                            "data-qtnote-media-type=\"application/pdf\" data-qtnote-size=\"123456\">"
                                            "Spec &amp; notes.pdf</a>")
                                 .arg(uri);
        NoteBlockModel model;
        model.load(html, true);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Attachment));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::UrlRole).toString(), uri);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::AltRole).toString(), QStringLiteral("Spec & notes.pdf"));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::AttachmentMediaTypeRole).toString(),
                 QStringLiteral("application/pdf"));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::AttachmentSizeRole).toLongLong(), qint64(123456));
        QCOMPARE(model.contents(), html);

        const NoteFragment fragment = model.extractBlockFragment(0, 0);
        QCOMPARE(fragment.blocks.size(), 1);
        QCOMPARE(fragment.blocks.constFirst().type, NoteFragmentBlockType::Attachment);
        QCOMPARE(fragment.blocks.constFirst().attachment.sourceUri, uri);
        QCOMPARE(fragment.blocks.constFirst().attachment.fileName, QStringLiteral("Spec & notes.pdf"));
        QCOMPARE(fragment.blocks.constFirst().attachment.mediaType, QStringLiteral("application/pdf"));
        QCOMPARE(fragment.blocks.constFirst().attachment.size, qint64(123456));

        NoteBlockModel transferred;
        transferred.load(QStringLiteral("before"), true);
        QString error;
        QVERIFY2(transferred.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(transferred.contents(), QStringLiteral("before\n\n") + html);
    }

    void parsesSerializesAndTransfersBlockQuotes()
    {
        const QString  markdown = QStringLiteral("title\n\n"
                                                  "> quoted **text**\n"
                                                  ">\n"
                                                  "> second paragraph\n\n"
                                                  "after");
        NoteBlockModel model;
        model.load(markdown, true);

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BlockQuote));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("quoted **text**\n\nsecond paragraph"));
        QCOMPARE(model.contents(), markdown);

        const NoteFragment fragment = model.extractBlockFragment(1, 1);
        QCOMPARE(fragment.blocks.size(), 1);
        QCOMPARE(fragment.blocks.constFirst().type, NoteFragmentBlockType::BlockQuote);

        NoteBlockModel destination;
        destination.load(QStringLiteral("before"), true);
        QString error;
        QVERIFY2(destination.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(destination.contents(), QStringLiteral("before\n\n> quoted **text**\n>\n> second paragraph"));
    }

    void convertsBetweenBlockQuotesAndHeadings()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("> quoted text"), true);

        QCOMPARE(model.convertTextBlockToHeading(0, 0, 2), 0);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Heading));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::HeadingLevelRole).toInt(), 2);
        QCOMPARE(model.contents(), QStringLiteral("## quoted text"));

        QCOMPARE(model.convertTextBlockToQuote(0, 0, true), 0);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BlockQuote));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::HeadingLevelRole).toInt(), 0);
        QCOMPARE(model.contents(), QStringLiteral("> quoted text"));

        QCOMPARE(model.convertTextBlockToHeading(0, 0, 0), 0);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.contents(), QStringLiteral("quoted text"));
    }

    void splitsStructuredBlockIntoFollowingParagraph()
    {
        NoteBlockModel heading;
        heading.load(QStringLiteral("## heading text"), true);

        QVERIFY(heading.splitStructuredBlockToText(0, QStringLiteral("heading"), QStringLiteral("text")));
        QCOMPARE(heading.rowCount(), 2);
        QCOMPARE(heading.data(heading.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Heading));
        QCOMPARE(heading.data(heading.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(heading.contents(), QStringLiteral("## heading\n\ntext"));

        NoteBlockModel quote;
        quote.load(QStringLiteral("> quoted"), true);
        QVERIFY(quote.splitStructuredBlockToText(0, QStringLiteral("quoted"), QString()));
        QCOMPARE(quote.contents(), QStringLiteral("> quoted"));
        QCOMPARE(quote.rowCount(), 2);
        QVERIFY(quote.isExplicitEmptyTextBlock(1));

        NoteBlockModel paragraph;
        paragraph.load(QStringLiteral("plain"), true);
        QVERIFY(!paragraph.splitStructuredBlockToText(0, QStringLiteral("plain"), QString()));
    }

    void leadingTablePipeDoesNotCreatePhantomColumn()
    {
        const QString  markdown = QStringLiteral("|  Header 1 | Header 2 |\n"
                                                  "| --- | --- |\n"
                                                  "| Content 1 | content 2 |");
        NoteBlockModel model;
        model.load(markdown, true);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Table));
        const QVariantMap table = model.data(model.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table.value(QStringLiteral("columns")).toInt(), 2);
        QCOMPARE(table.value(QStringLiteral("values")).toStringList(),
                 QStringList({ "Header 1", "Header 2", "Content 1", "content 2" }));
        QCOMPARE(model.contents(),
                 QStringLiteral("| Header 1 | Header 2 |\n"
                                "| --- | --- |\n"
                                "| Content 1 | content 2 |"));
    }

    void preservesGithubUnderlineMarkup()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("before <ins>one</ins> after\n\n"
                                  "| A | B |\n| --- | --- |\n| <u>two</u> | plain |"),
                   true);

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("before <ins>one</ins> after"));
        const auto table = model.data(model.index(1), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table.value(QStringLiteral("values")).toStringList(),
                 QStringList({ "A", "B", "<u>two</u>", "plain" }));
        QCOMPARE(model.contents(),
                 QStringLiteral("before <ins>one</ins> after\n\n"
                                "| A | B |\n| --- | --- |\n| <u>two</u> | plain |"));
    }

    void extractsAndInsertsWholeBlockFragments()
    {
        NoteBlockModel source;
        source.load(QStringLiteral("# Title\n\n- [x] done"), true);

        const NoteFragment fragment = source.extractBlockFragment(0, 2);
        QCOMPARE(fragment.kind, NoteFragmentKind::BlockSequence);
        QCOMPARE(fragment.sourceFormat, NoteFragmentSourceFormat::Markdown);
        QCOMPARE(fragment.blocks.size(), 2);
        QCOMPARE(fragment.blocks.at(0).type, NoteFragmentBlockType::Heading);
        QCOMPARE(fragment.blocks.at(1).listItems.at(0).kind, NoteFragmentListKind::Check);

        NoteBlockModel destination;
        destination.load(QStringLiteral("before"), true);
        QString error;
        QVERIFY2(destination.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(destination.rowCount(), 3);
        QCOMPARE(destination.contents(), QStringLiteral("before\n\n# Title\n\n- [x] done"));

        NoteBlockModel tableSource;
        tableSource.load(QStringLiteral("A [link](https://example.org)\n\n"
                                        "- one\n- two\n\n"
                                        "- [ ] todo\n- [x] done\n\n"
                                        "| Name | Value |\n| --- | --- |\n| a | b |\n\n"
                                        "![cat](media://cat)"),
                         true);
        const NoteFragment tableFragment = tableSource.extractBlockFragment(3, 3);
        QCOMPARE(tableFragment.blocks.at(0).table.markdownCells, QStringList({ "Name", "Value", "a", "b" }));
        QVERIFY2(destination.insertBlockFragment(3, tableFragment, &error), qPrintable(error));
        QCOMPARE(destination.contents(),
                 QStringLiteral("before\n\n# Title\n\n- [x] done\n\n| Name | Value |\n| --- | --- |\n| a | b |"));
    }

    void extractsCrossBlockSelectionStructurally()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("prefix\n\n- [ ] one\n- [x] two\n\n"
                                  "| A | B |\n| --- | --- |\n| 1 | 2 |\n\nsuffix"),
                   true);
        const QList<NoteBlockSelectionRange> ranges {
            { 0, -1, -1, QStringLiteral("fix"), false }, { 1, 0, -1, QStringLiteral("one"), true },
            { 1, 1, -1, QStringLiteral("two"), true },   { 2, -1, 0, QStringLiteral("A"), true },
            { 2, -1, 1, QStringLiteral("B"), true },     { 2, -1, 2, QStringLiteral("1"), true },
            { 2, -1, 3, QStringLiteral("2"), true },     { 3, -1, -1, QStringLiteral("suf"), false },
        };

        const NoteFragment fragment = model.extractSelectionFragment(ranges);
        QCOMPARE(fragment.blocks.size(), 4);
        QCOMPARE(fragment.blocks.at(0).type, NoteFragmentBlockType::Text);
        QCOMPARE(fragment.blocks.at(0).markdown, QStringLiteral("fix"));
        QCOMPARE(fragment.blocks.at(1).type, NoteFragmentBlockType::List);
        QCOMPARE(fragment.blocks.at(1).listItems.size(), 2);
        QCOMPARE(fragment.blocks.at(1).listItems.at(1).checked, true);
        QCOMPARE(fragment.blocks.at(2).type, NoteFragmentBlockType::Table);
        QCOMPARE(fragment.blocks.at(2).table.markdownCells, QStringList({ "A", "B", "1", "2" }));
        QCOMPARE(fragment.blocks.at(3).markdown, QStringLiteral("suf"));
    }

    void includesUnrangedBlocksCrossedBySelection()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("before text\n\n![diagram](media://diagram)\n\nafter text"), true);

        // Images do not own a TextArea, so a mouse selection whose endpoints
        // are in the surrounding paragraphs has no explicit range for row 1.
        const QList<NoteBlockSelectionRange> ranges {
            { 0, -1, -1, QStringLiteral("text"), false },
            { 2, -1, -1, QStringLiteral("after"), false },
        };

        const NoteFragment fragment = model.extractSelectionFragment(ranges);
        QCOMPARE(fragment.blocks.size(), 3);
        QCOMPARE(fragment.blocks.at(0).type, NoteFragmentBlockType::Text);
        QCOMPARE(fragment.blocks.at(0).markdown, QStringLiteral("text"));
        QCOMPARE(fragment.blocks.at(1).type, NoteFragmentBlockType::Image);
        QCOMPARE(fragment.blocks.at(1).image.sourceUri, QStringLiteral("media://diagram"));
        QCOMPARE(fragment.blocks.at(1).image.alt, QStringLiteral("diagram"));
        QCOMPARE(fragment.blocks.at(2).type, NoteFragmentBlockType::Text);
        QCOMPARE(fragment.blocks.at(2).markdown, QStringLiteral("after"));

        NoteBlockModel destination;
        destination.load(QStringLiteral("destination"), true);
        QString error;
        QVERIFY2(destination.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(destination.contents(), QStringLiteral("destination\n\ntext\n\n![diagram](media://diagram)\n\nafter"));
    }

    void removesCrossBlockSelectionIncludingListAndTable()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("beforeXSELECT\n\n- [ ] one\n- [x] two\n\n"
                                  "| A | B |\n| --- | --- |\n| 1 | 2 |\n\nREMOVEafter"),
                   true);
        const QList<NoteBlockSelectionRange> ranges {
            { 0, -1, -1, QStringLiteral("SELECT"), false, QStringLiteral("beforeX"), QString() },
            { 1, 0, -1, QStringLiteral("one"), true, QString(), QString() },
            { 1, 1, -1, QStringLiteral("two"), true, QString(), QString() },
            { 2, -1, 0, QStringLiteral("A"), true, QString(), QString() },
            { 2, -1, 1, QStringLiteral("B"), true, QString(), QString() },
            { 2, -1, 2, QStringLiteral("1"), true, QString(), QString() },
            { 2, -1, 3, QStringLiteral("2"), true, QString(), QString() },
            { 3, -1, -1, QStringLiteral("REMOVE"), false, QString(), QStringLiteral("after") },
        };

        QCOMPARE(model.removeSelectionRanges(ranges), 0);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.contents(), QStringLiteral("beforeXafter"));
    }

    void removesTrailingImageCrossedFromDocumentBoundary()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("beforeSELECT\n\n![diagram](media://diagram)"), true);
        const QList<NoteBlockSelectionRange> ranges {
            { 0, -1, -1, QStringLiteral("SELECT"), false, QStringLiteral("before"), QString() },
            { 1, -1, -1, QString(), true, QString(), QString() },
        };

        QCOMPARE(model.removeSelectionRanges(ranges), 0);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.contents(), QStringLiteral("before"));
    }

    void rejectsNonBlockFragmentInsertion()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("unchanged"), true);
        NoteFragment inlineFragment;
        inlineFragment.kind = NoteFragmentKind::Inline;
        QString error;
        QVERIFY(!model.insertBlockFragment(0, inlineFragment, &error));
        QCOMPARE(error, QStringLiteral("fragment is not a block sequence"));
        QCOMPARE(model.contents(), QStringLiteral("unchanged"));
    }

    void replacesTextRangeWithStructuredFragmentAtomically()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("before selected after"), true);
        NoteFragment      fragment;
        NoteFragmentBlock list;
        list.type      = NoteFragmentBlockType::List;
        list.listItems = { { QStringLiteral("first"), 0, NoteFragmentListKind::Bullet, false },
                           { QStringLiteral("second"), 0, NoteFragmentListKind::Bullet, false } };
        fragment.blocks.append(list);

        QString error;
        QCOMPARE(model.replaceTextBlockRangeWithFragment(0, QStringLiteral("before "), QStringLiteral(" after"),
                                                         fragment, &error),
                 1);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.contents(), QStringLiteral("before\n\n- first\n- second\n\nafter"));
    }

    void replacesTableRectangleAndExpandsTable()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("text"), true);
        model.insertTable(1);
        model.setTableCell(1, 0, QStringLiteral("A"));
        model.setTableCell(1, 1, QStringLiteral("B"));
        model.setTableCell(1, 2, QStringLiteral("1"));
        model.setTableCell(1, 3, QStringLiteral("2"));
        NoteFragment      fragment;
        NoteFragmentBlock table;
        table.type             = NoteFragmentBlockType::Table;
        table.table.rows       = 2;
        table.table.columns    = 2;
        table.table.headerRows = 1;
        table.table.markdownCells
            = { QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"), QStringLiteral("W") };
        fragment.blocks.append(table);

        QString error;
        QVERIFY2(model.replaceTableCellsWithFragment(1, 3, fragment, &error), qPrintable(error));
        const auto cells = model.data(model.index(1), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(cells.value(QStringLiteral("columns")).toInt(), 3);
        QCOMPARE(cells.value(QStringLiteral("values")).toStringList(),
                 QStringList({ "A", "B", "", "1", "X", "Y", "", "Z", "W" }));
    }

    void replacesFlatListItemWithMixedListFragment()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- before selected after\n- tail"), true);
        NoteFragment      fragment;
        NoteFragmentBlock list;
        list.type      = NoteFragmentBlockType::List;
        list.listItems = { { QStringLiteral("task"), 0, NoteFragmentListKind::Check, true },
                           { QStringLiteral("nested"), 1, NoteFragmentListKind::Numbered, false } };
        fragment.blocks.append(list);

        QString error;
        QCOMPARE(model.replaceListItemRangeWithFragment(0, 0, QStringLiteral("before "), QStringLiteral(" after"),
                                                        fragment, &error),
                 1);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(model.contents(), QStringLiteral("- before\n- [x] task\n    1. nested\n- after\n- tail"));
    }

    void rejectsListPasteIntoItemWithDescendants()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- parent\n    - child\n- tail"), true);
        NoteFragment      fragment;
        NoteFragmentBlock list;
        list.type      = NoteFragmentBlockType::List;
        list.listItems = { { QStringLiteral("pasted"), 0, NoteFragmentListKind::Bullet, false } };
        fragment.blocks.append(list);
        QString error;
        QCOMPARE(model.replaceListItemRangeWithFragment(0, 0, QString(), QString(), fragment, &error), -1);
        QCOMPARE(error, QStringLiteral("target list item has nested descendants"));
    }

    void mergesAdjacentMarkdownParagraphs()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Title\n\nfirst paragraph\n\nsecond paragraph"), true);

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("Title"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("first paragraph\n\nsecond paragraph"));
        QCOMPARE(model.contents(), QStringLiteral("Title\n\nfirst paragraph\n\nsecond paragraph"));
    }

    void keepsStructuralBoundariesBetweenTextSections()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("before one\n\nbefore two\n\n- item\n\nafter one\n\nafter two"), true);

        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("before one"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("before two"));
        QCOMPARE(model.data(model.index(2), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BulletList));
        QCOMPARE(model.data(model.index(3), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("after one\n\nafter two"));
    }

    void backspaceAtListItemStartUnlistsOrOutdents()
    {
        NoteBlockModel topLevel;
        topLevel.load(QStringLiteral("- one\n- two\n    - child\n- three"), true);
        QCOMPARE(topLevel.unlistListItem(0, 1), 1);
        QCOMPARE(topLevel.rowCount(), 3);
        QCOMPARE(topLevel.data(topLevel.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList { QStringLiteral("one") });
        QCOMPARE(topLevel.data(topLevel.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(topLevel.data(topLevel.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("two"));
        QCOMPARE(topLevel.data(topLevel.index(2), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("child"), QStringLiteral("three") }));
        QCOMPARE(topLevel.data(topLevel.index(2), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 0 }));
        QCOMPARE(topLevel.contents(), QStringLiteral("- one\n\ntwo\n\n- child\n- three"));

        NoteBlockModel nested;
        nested.load(QStringLiteral("- parent\n    - child\n        - grandchild\n- sibling"), true);
        QCOMPARE(nested.unlistListItem(0, 1), 0);
        QCOMPARE(nested.rowCount(), 1);
        QCOMPARE(nested.data(nested.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 0, 1, 0 }));
        QCOMPARE(nested.contents(), QStringLiteral("- parent\n- child\n    - grandchild\n- sibling"));
    }

    void textAfterBlankLineDoesNotJoinChecklistItem()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] task\n\nplain text"), true);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::CheckList));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(), QStringList { "task" });
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("plain text"));

        model.load(QStringLiteral("- [ ] task<br><br>\n\nplain text"), true);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(), QStringList { "task" });
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("plain text"));
    }

    void editsAreSerialized()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] first"), true);
        model.setChecked(0, 0, true);
        model.setListItem(0, 0, QStringLiteral("changed"));
        QCOMPARE(model.contents(), QStringLiteral("- [x] changed"));
    }

    void nestedTaskChecksPropagateBetweenParentsAndDescendants()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] parent\n"
                                  "    - [ ] first\n"
                                  "    - [ ] second\n"
                                  "        - [ ] grandchild\n"
                                  "- [ ] unrelated"),
                   true);

        model.setChecked(0, 1, true);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::CheckedRole).toList(),
                 QVariantList({ false, true, false, false, false }));

        model.setChecked(0, 2, true);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::CheckedRole).toList(),
                 QVariantList({ true, true, true, true, false }));

        model.setChecked(0, 3, false);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::CheckedRole).toList(),
                 QVariantList({ false, true, false, false, false }));

        model.setChecked(0, 0, true);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::CheckedRole).toList(),
                 QVariantList({ true, true, true, true, false }));

        model.setChecked(0, 0, false);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::CheckedRole).toList(),
                 QVariantList({ false, false, false, false, false }));
    }

    void mergesLastListItemWithFollowingTextOrListItem()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- first\n- second\n\nfollowing text"), true);
        QVERIFY(model.mergeListItemWithFollowingBlock(0, 1));
        QCOMPARE(model.contents(), QStringLiteral("- first\n- secondfollowing text"));
        QCOMPARE(model.rowCount(), 1);

        model.load(QStringLiteral("- first\n\n- [x] second\n- [ ] third"), true);
        QVERIFY(model.mergeListItemWithFollowingBlock(0, 0));
        QCOMPARE(model.contents(), QStringLiteral("- firstsecond\n- [ ] third"));
        QCOMPARE(model.rowCount(), 1);

        model.load(QStringLiteral("- first\n\n- [x] second\n  - nested\n- third"), true);
        QCOMPARE(model.rowCount(), 2);
        QVERIFY(model.mergeListItemWithFollowingBlock(0, 0));
        QCOMPARE(model.contents(), QStringLiteral("- firstsecond\n    - nested\n- third"));
        QCOMPARE(model.rowCount(), 1);
    }

    void equalScalarWritesAreNoOps()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] first"), true);
        QSignalSpy changed(&model, &NoteBlockModel::contentsChanged);

        model.setListItem(0, 0, QStringLiteral("first"));
        model.setChecked(0, 0, false);
        QVERIFY(!model.setData(model.index(0), QStringList { QStringLiteral("first") }, NoteBlockModel::ItemsRole));
        QCOMPARE(changed.size(), 0);

        model.load(QStringLiteral("| A | B |\n| --- | --- |\n| C | D |"), true);
        changed.clear();
        model.setTableCell(0, 2, QStringLiteral("C"));
        QCOMPARE(changed.size(), 0);

        model.load(QStringLiteral("![cat](media://cat)"), true);
        changed.clear();
        const auto image = model.index(0);
        QVERIFY(!model.setData(image, model.data(image, NoteBlockModel::UrlRole), NoteBlockModel::UrlRole));
        QVERIFY(!model.setData(image, model.data(image, NoteBlockModel::AltRole), NoteBlockModel::AltRole));
        QVERIFY(
            !model.setData(image, model.data(image, NoteBlockModel::ImageWidthRole), NoteBlockModel::ImageWidthRole));
        QVERIFY(!model.setData(image, model.data(image, NoteBlockModel::ImageAlignmentRole),
                               NoteBlockModel::ImageAlignmentRole));
        QCOMPARE(changed.size(), 0);
    }

    void reportsScalarEditsWithoutExposingInternalState()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("text\n\n- first\n\n"
                                  "| A | B |\n| --- | --- |\n| C | D |\n\n"
                                  "![cat](media://cat)"),
                   true);
        QSignalSpy edits(&model, &NoteBlockModel::scalarEdited);

        model.setBlockText(0, QStringLiteral("changed"));
        model.setListItem(1, 0, QStringLiteral("second"));
        model.setTableCell(2, 2, QStringLiteral("cell"));
        model.setImageUrl(3, QStringLiteral("media://other"));
        model.setImageAlt(3, QStringLiteral("description"));
        model.setImageWidth(3, 240);
        model.setImageAlignment(3, QStringLiteral("left"));

        QCOMPARE(edits.size(), 7);
        QCOMPARE(edits.at(0).at(1).toInt(), int(NoteBlockModel::TextRole));
        QCOMPARE(edits.at(1).at(1).toInt(), int(NoteBlockModel::ItemsRole));
        QCOMPARE(edits.at(1).at(2).toInt(), 0);
        QCOMPARE(edits.at(2).at(1).toInt(), int(NoteBlockModel::CellsRole));
        QCOMPARE(edits.at(2).at(2).toInt(), 2);
        QCOMPARE(edits.at(3).at(1).toInt(), int(NoteBlockModel::UrlRole));
        QCOMPARE(edits.at(4).at(1).toInt(), int(NoteBlockModel::AltRole));
        QCOMPARE(edits.at(5).at(1).toInt(), int(NoteBlockModel::ImageWidthRole));
        QCOMPARE(edits.at(6).at(1).toInt(), int(NoteBlockModel::ImageAlignmentRole));
    }

    void coalescesAdjacentLinksCreatedAcrossFormatRuns()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("text"), true);
        model.setBlockText(0, QStringLiteral("[Надежду ](url)[*Л*](url)[ебедев](url)[*у*](url)"));
        QCOMPARE(model.contents(), QStringLiteral("[Надежду *Л*ебедев*у*](url)"));
    }

    void keepsLongInlineLinksOnTheirSourceLine()
    {
        const QString  url       = QStringLiteral("https://example.org/a/very/long/path/that/exceeds/the/markdown/"
                                                         "writers/usual/wrapping/column?with=query&and=value");
        const QString  paragraph = QStringLiteral("before [%1](%1) after").arg(url);
        NoteBlockModel model;
        model.load(paragraph, true);
        QCOMPARE(model.contents(), paragraph);

        const QString listSource = QStringLiteral("- before [%1](%1) after").arg(url);
        model.load(listSource, true);
        QCOMPARE(model.contents(), listSource);
    }

    void editsTableStructure()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |"), true);
        QCOMPARE(model.rowCount(), 1);

        model.insertTableRow(0, 1);
        auto table = model.data(model.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table[QStringLiteral("columns")].toInt(), 2);
        QCOMPARE(table[QStringLiteral("values")].toStringList().size(), 6);

        model.insertTableColumn(0, 1);
        table = model.data(model.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table[QStringLiteral("columns")].toInt(), 3);
        QCOMPARE(table[QStringLiteral("values")].toStringList().size(), 9);

        model.removeTableRow(0, 1);
        model.removeTableColumn(0, 1);
        table = model.data(model.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table[QStringLiteral("columns")].toInt(), 2);
        QCOMPARE(table[QStringLiteral("values")].toStringList().size(), 4);
    }

    void reordersWholeTableColumns()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("| A | B | C |\n"
                                  "| --- | --- | --- |\n"
                                  "| a | b | c |\n"
                                  "| d | e | f |"),
                   true);

        QVERIFY(model.moveTableColumn(0, 0, 2));
        auto table = model.data(model.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table[QStringLiteral("values")].toStringList(),
                 QStringList({ QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("A"), QStringLiteral("b"),
                               QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("e"), QStringLiteral("f"),
                               QStringLiteral("d") }));
        QCOMPARE(model.contents(),
                 QStringLiteral("| B | C | A |\n"
                                "| --- | --- | --- |\n"
                                "| b | c | a |\n"
                                "| e | f | d |"));

        QVERIFY(model.moveTableColumn(0, 2, 0));
        table = model.data(model.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table[QStringLiteral("values")].toStringList(),
                 QStringList({ QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("a"),
                               QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d"), QStringLiteral("e"),
                               QStringLiteral("f") }));
        QVERIFY(!model.moveTableColumn(0, 1, 1));
        QVERIFY(!model.moveTableColumn(0, -1, 0));
    }

    void tableLineBreaksUseGithubCompatibleHtml()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |"), true);
        model.setTableCell(0, 2, QStringLiteral("first\nsecond"));
        QVERIFY(model.contents().contains(QStringLiteral("first<br>second")));

        NoteBlockModel restored;
        restored.load(model.contents(), true);
        const auto table = restored.data(restored.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table[QStringLiteral("values")].toStringList().value(2), QStringLiteral("first\nsecond"));
    }

    void serializesListContinuationsUsingCommonMarkIndentation()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] first<br><br>"), true);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList().value(0),
                 QStringLiteral("first"));
        model.setListItem(0, 0, QStringLiteral("first\nsecond\n\n"));
        QCOMPARE(model.contents(), QStringLiteral("- [ ] first\n      second"));

        model.load(QStringLiteral("- first\n- second\n1. numbered"), true);
        model.setListItem(0, 0, QStringLiteral("first line\nsecond line"));
        model.setListItem(1, 0, QStringLiteral("numbered line\ncontinuation"));
        QCOMPARE(model.contents(),
                 QStringLiteral("- first line\n  second line\n- second\n\n"
                                "1. numbered line\n   continuation"));
    }

    void parsesIndentedListContinuationsAsOneItem()
    {
        NoteBlockModel bullets;
        bullets.load(QStringLiteral("- bullet\n  continuation\n- tail"), true);
        QCOMPARE(bullets.data(bullets.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("bullet\ncontinuation"), QStringLiteral("tail") }));
        QCOMPARE(bullets.contents(), QStringLiteral("- bullet\n  continuation\n- tail"));

        NoteBlockModel tasks;
        tasks.load(QStringLiteral("- [ ] task\n      continuation\n- [x] tail"), true);
        QCOMPARE(tasks.data(tasks.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("task\ncontinuation"), QStringLiteral("tail") }));
        QCOMPARE(tasks.contents(), QStringLiteral("- [ ] task\n      continuation\n- [x] tail"));

        NoteBlockModel numbered;
        numbered.load(QStringLiteral("1. numbered\n   continuation\n2. tail"), true);
        QCOMPARE(numbered.data(numbered.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("numbered\ncontinuation"), QStringLiteral("tail") }));
        QCOMPARE(numbered.contents(), QStringLiteral("1. numbered\n   continuation\n2. tail"));
    }

    void keepsCanonicalWriterWrapInsideLongListItem()
    {
        const QString  item = QStringLiteral("a fairly long checklist item with enough ordinary words to make "
                                              "QTextDocument wrap its canonical Markdown output across more than "
                                              "one physical source line without an explicit user line break");
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] ") + item, true);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::CheckList));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList().size(), 1);
        QVERIFY(model.contents().startsWith(QStringLiteral("- [ ] a fairly long checklist item")));
        QVERIFY(model.contents().contains(QStringLiteral("\n      wrap its canonical Markdown")));
    }

    void supportsNumberedAndIndentedLists()
    {
        NoteBlockModel numbered;
        numbered.load(QStringLiteral("1. one\n2. two"), true);
        QCOMPARE(numbered.data(numbered.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::NumberedList));

        NoteBlockModel tasks;
        tasks.load(QStringLiteral("- [ ] one\n- [ ] two"), true);
        tasks.indentListItems(0, 0, 1, 1);
        QCOMPARE(tasks.data(tasks.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1 }));
        QVERIFY(tasks.contents().contains(QStringLiteral("  - [ ] two")));
        tasks.indentListItems(0, 0, 1, -1);
        QCOMPARE(tasks.data(tasks.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 0 }));
    }

    void nestedTaskListSurvivesMarkdownRoundTrip()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] first\n- [ ] second\n- [ ] third"), true);
        model.indentListItems(0, 1, 1, 1);

        const QString  markdown = model.contents();
        NoteBlockModel restored;
        restored.load(markdown, true);

        QCOMPARE(restored.contents(), markdown);
        QCOMPARE(restored.rowCount(), 1);
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "first", "second", "third" }));
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 0 }));
    }

    void outdentedListItemAdoptsParentListType()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("1. first\n2. child\n3. third"), true);
        model.indentListItems(0, 1, 1, 1);
        QVERIFY(model.convertListLevel(0, 1, NoteBlockModel::BulletList));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::NumberedList), int(NoteBlockModel::BulletList),
                                int(NoteBlockModel::NumberedList) }));

        model.indentListItems(0, 1, 1, -1);

        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 0, 0 }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::NumberedList), int(NoteBlockModel::NumberedList),
                                int(NoteBlockModel::NumberedList) }));
        QCOMPARE(model.contents(), QStringLiteral("1. first\n2. child\n3. third"));
    }

    void reindentedListItemRestoresNestedListType()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] parent\n- [ ] first\n- [ ] second\n- [ ] tail"), true);
        model.indentListItems(0, 1, 2, 1);
        QVERIFY(model.convertListLevel(0, 1, NoteBlockModel::BulletList));

        model.indentListItems(0, 1, 1, -1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList().value(1).toInt(),
                 int(NoteBlockModel::CheckList));
        model.indentListItems(0, 1, 1, 1);

        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 1, 0 }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::CheckList), int(NoteBlockModel::BulletList),
                                int(NoteBlockModel::BulletList), int(NoteBlockModel::CheckList) }));
    }

    void taskListSurroundingNestedNumberedItemsStaysOneBlock()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] first\n- [ ] child\n- [ ] third"), true);
        model.indentListItems(0, 1, 1, 1);
        QVERIFY(model.convertListLevel(0, 1, NoteBlockModel::NumberedList));
        model.insertListItem(0, 2, QStringLiteral("new child"));

        const QString markdown = model.contents();
        QCOMPARE(markdown, QStringLiteral("- [ ] first\n    1. child\n    2. new child\n- [ ] third"));

        NoteBlockModel restored;
        restored.load(markdown, true);
        QCOMPARE(restored.rowCount(), 1);
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "first", "child", "new child", "third" }));
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 1, 0 }));
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::CheckList), int(NoteBlockModel::NumberedList),
                                int(NoteBlockModel::NumberedList), int(NoteBlockModel::CheckList) }));
        QCOMPARE(restored.contents(), markdown);
    }

    void preservesThreeLevelMixedListIndentation()
    {
        const QString  markdown = QStringLiteral("- [ ] 111\n"
                                                  "    1. ds\n"
                                                  "        - aaa bbb\n"
                                                  "    2. dsfgdg\n"
                                                  "- [ ] 32");
        NoteBlockModel model;
        model.load(markdown, true);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "111", "ds", "aaa bbb", "dsfgdg", "32" }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 2, 1, 0 }));
        QCOMPARE(model.contents(), markdown);
    }

    void parsesSerializesAndSplitsHeadingBlocks()
    {
        NoteBlockModel parsed;
        parsed.load(QStringLiteral("# First\n\ntext\n\n### Third"), true);
        QCOMPARE(parsed.rowCount(), 3);
        QCOMPARE(parsed.data(parsed.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Heading));
        QCOMPARE(parsed.data(parsed.index(0), NoteBlockModel::HeadingLevelRole).toInt(), 1);
        QCOMPARE(parsed.data(parsed.index(2), NoteBlockModel::HeadingLevelRole).toInt(), 3);
        QCOMPARE(parsed.contents(), QStringLiteral("# First\n\ntext\n\n### Third"));

        NoteBlockModel converted;
        converted.load(QStringLiteral("before\n\ntarget\n\nafter"), true);
        QCOMPARE(converted.convertTextBlockToHeading(1, 0, 2), 1);
        QCOMPARE(converted.rowCount(), 3);
        QCOMPARE(converted.data(converted.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Heading));
        QCOMPARE(converted.data(converted.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("target"));
        QCOMPARE(converted.contents(), QStringLiteral("before\n\n## target\n\nafter"));
        QCOMPARE(converted.convertTextBlockToHeading(1, 0, 0), 1);
        QCOMPARE(converted.data(converted.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
    }

    void insertsMinimalStructuredBlocks()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("text"), true);
        model.insertTable(1);
        model.insertList(2, NoteBlockModel::CheckList);
        QCOMPARE(model.rowCount(), 3);
        const auto table = model.data(model.index(1), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(table[QStringLiteral("columns")].toInt(), 2);
        QCOMPARE(table[QStringLiteral("values")].toStringList().size(), 4);
        QCOMPARE(model.data(model.index(2), NoteBlockModel::ItemsRole).toStringList(), QStringList { QString() });
    }

    void collapsesUneditedInsertedParagraphOnMarkdownRoundTrip()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |\n\n- after"), true);

        model.insertTextBlock(1);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QString());
        QVERIFY(model.isExplicitEmptyTextBlock(1));
        QVERIFY(!model.contents().contains(QStringLiteral("qtnote:empty-paragraph")));

        NoteBlockModel restored;
        restored.load(model.contents(), true);
        QCOMPARE(restored.rowCount(), 2);
        QCOMPARE(restored.data(restored.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Table));
        QCOMPARE(restored.data(restored.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BulletList));

        NoteBlockModel legacy;
        legacy.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |\n\n"
                                   "<!-- qtnote:empty-paragraph -->\n\n- after"),
                    true);
        QCOMPARE(legacy.rowCount(), 2);
        QVERIFY(!legacy.contents().contains(QStringLiteral("qtnote:empty-paragraph")));

        model.setBlockText(1, QStringLiteral("between"));
        QVERIFY(!model.isExplicitEmptyTextBlock(1));
        QCOMPARE(model.contents(), QStringLiteral("| A | B |\n| --- | --- |\n| one | two |\n\nbetween\n\n- after"));

        model.setBlockText(1, QString());
        QVERIFY(model.isExplicitEmptyTextBlock(1));
        QVERIFY(!model.contents().contains(QStringLiteral("qtnote:empty-paragraph")));

        NoteBlockModel ordinary;
        ordinary.load(QStringLiteral("title\n\n- item"), true);
        ordinary.setBlockText(0, QString());
        QVERIFY(!ordinary.isExplicitEmptyTextBlock(0));
    }

    void insertingAndConvertingListsPreservesIndentation()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] parent\n  - [ ] first\n  - [ ] second"), true);
        model.insertListItem(0, 2, QStringLiteral("new"));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 1, 1 }));

        QVERIFY(model.convertListLevel(0, 1, NoteBlockModel::NumberedList));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "parent", "first", "new", "second" }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 1, 1 }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::CheckList), int(NoteBlockModel::NumberedList),
                                int(NoteBlockModel::NumberedList), int(NoteBlockModel::NumberedList) }));
        QVERIFY(model.contents().contains(QStringLiteral("1. first")));
    }

    void removesStructuredRangesAtomically()
    {
        NoteBlockModel list;
        list.load(QStringLiteral("- one\n- two\n- three"), true);
        list.removeListItems(0, 0, 1);
        QCOMPARE(list.data(list.index(0), NoteBlockModel::ItemsRole).toStringList(), QStringList { "three" });

        NoteBlockModel table;
        table.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |\n| three | four |"), true);
        table.removeTableRows(0, 0, 1);
        const auto cells = table.data(table.index(0), NoteBlockModel::CellsRole).toMap();
        QCOMPARE(cells[QStringLiteral("values")].toStringList(), QStringList({ "three", "four" }));
    }

    void movesListSubtreesAcrossBlocks()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- source\n"
                                  "    - child\n\n"
                                  "between\n\n"
                                  "1. target\n"
                                  "2. tail"),
                   true);

        QVERIFY(model.moveListSubtree(0, 0, 2, 1, 1));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("between"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "target", "source", "child", "tail" }));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 2, 0 }));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::NumberedList), int(NoteBlockModel::BulletList),
                                int(NoteBlockModel::BulletList), int(NoteBlockModel::NumberedList) }));
        QCOMPARE(model.contents(),
                 QStringLiteral("between\n\n"
                                "1. target\n"
                                "    - source\n"
                                "        - child\n"
                                "2. tail"));
    }

    void movesAndReindentsListSubtreesAtomically()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- one\n- parent\n    - child\n- tail"), true);

        QVERIFY(model.moveListSubtree(0, 3, 0, 1, 1));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "one", "tail", "parent", "child" }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 0, 1 }));

        QVERIFY(model.moveListSubtree(0, 1, 0, 3, 0));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "one", "parent", "child", "tail" }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 0, 1, 0 }));
    }

    void detachesListSubtreesIntoStandaloneBlocks()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("before\n\n- parent\n    - child\n        1. grandchild\n- tail\n\nafter"), true);

        const int detachedRow = model.moveListRangeToBlock(1, 1, 2, 3);
        QCOMPARE(detachedRow, 3);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "parent", "tail" }));
        QCOMPARE(model.data(model.index(3), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "child", "grandchild" }));
        QCOMPARE(model.data(model.index(3), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1 }));
        QCOMPARE(model.data(model.index(3), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::NumberedList) }));
        QCOMPARE(model.contents(), QStringLiteral("before\n\n- parent\n- tail\n\nafter\n\n- child\n    1. grandchild"));

        QVERIFY(model.moveListRange(3, 0, 1, 1, 1, 1));
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "parent", "child", "grandchild", "tail" }));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 2, 0 }));
    }

    void reattachesStandaloneListsAcrossInterveningBlocks()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- child\n\nbetween\n\n- parent\n- tail"), true);

        QVERIFY(model.moveListRange(0, 0, 0, 2, 1, 1));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("between"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "parent", "child", "tail" }));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 0 }));
        QCOMPARE(model.contents(), QStringLiteral("between\n\n- parent\n    - child\n- tail"));
    }

    void movesWholeListsThroughStandaloneBoundaries()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("before\n\n- parent\n    1. child\n- tail\n\nafter"), true);

        const int movedRow = model.moveListRangeToBlock(1, 0, 2, 0);
        QCOMPARE(movedRow, 0);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ "parent", "child", "tail" }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 0 }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::NumberedList),
                                int(NoteBlockModel::BulletList) }));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("before\n\nafter"));
        QCOMPARE(model.contents(), QStringLiteral("- parent\n    1. child\n- tail\n\nbefore\n\nafter"));

        QCOMPARE(model.moveListRangeToBlock(0, 0, 2, model.rowCount()), 2);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("before"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("after"));
        QCOMPARE(model.contents(), QStringLiteral("before\n\nafter\n\n- parent\n    1. child\n- tail"));
        QCOMPARE(model.moveListRangeToBlock(2, 0, 2, 2), -1);
    }

    void movesWholeBlocks()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("first\n\n- item\n\n![image](media://image)"), true);
        QVERIFY(model.moveBlock(2, 0));
        QCOMPARE(model.contents(), QStringLiteral("![image](media://image)\n\nfirst\n\n- item"));
        QVERIFY(model.moveBlock(0, 2));
        QCOMPARE(model.contents(), QStringLiteral("first\n\n- item\n\n![image](media://image)"));
    }

    void movingMultilineTextToTitleSplitsAndRecombinesBody()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Old title\n\n- item\n\nNew title\n\nrest"), true);
        QCOMPARE(model.rowCount(), 3);

        QCOMPARE(model.moveBlockResolved(2, 0), 0);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("New title"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("rest\n\nOld title"));
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::BulletList));
        QCOMPARE(model.contents(), QStringLiteral("New title\n\nrest\n\nOld title\n\n- item"));
    }

    void movingStructuredBlockAwayRecombinesTextNeighbors()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Title\n\nA\n\n![image](media://image)\n\nB\n\n- item"), true);
        QCOMPARE(model.rowCount(), 5);

        QCOMPARE(model.moveBlockResolved(2, 4), 3);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("A\n\nB"));
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::BulletList));
        QCOMPARE(model.blockTypeAt(3), int(NoteBlockModel::Image));
        QCOMPARE(model.contents(), QStringLiteral("Title\n\nA\n\nB\n\n- item\n\n![image](media://image)"));
    }

    void movingListBlockNextToListMergesAndAdoptsResidentType()
    {
        NoteBlockModel afterTarget;
        afterTarget.load(QStringLiteral("- bullet\n\nseparator\n\n1. numbered"), true);
        QCOMPARE(afterTarget.moveBlockResolved(2, 1), 0);
        QCOMPARE(afterTarget.rowCount(), 2);
        QCOMPARE(afterTarget.data(afterTarget.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("bullet"), QStringLiteral("numbered") }));
        QCOMPARE(afterTarget.data(afterTarget.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::BulletList) }));

        NoteBlockModel beforeTarget;
        beforeTarget.load(QStringLiteral("1. numbered\n\nseparator\n\n- bullet"), true);
        QCOMPARE(beforeTarget.moveBlockResolved(0, 1), 1);
        QCOMPARE(beforeTarget.rowCount(), 2);
        QCOMPARE(beforeTarget.data(beforeTarget.index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("numbered"), QStringLiteral("bullet") }));
        QCOMPARE(beforeTarget.data(beforeTarget.index(1), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::BulletList) }));
    }

    void movingBlockAwayMergesAdjacentListsAtSourceGap()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Title\n\n- bullet\n\n![image](media://image)\n\n1. numbered\n\nend"), true);
        QCOMPARE(model.rowCount(), 5);

        QCOMPARE(model.moveBlockResolved(2, 4), 3);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("bullet"), QStringLiteral("numbered") }));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::BulletList) }));
        QCOMPARE(model.blockTypeAt(2), int(NoteBlockModel::Text));
        QCOMPARE(model.blockTypeAt(3), int(NoteBlockModel::Image));
    }

    void movingListRangeIntoListAdoptsExistingTypeAtSameLevel()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- bullet\n\nseparator\n\n1. numbered\n    - nested"), true);
        QCOMPARE(model.moveListRangeResolved(2, 0, 1, 0, 1, 0), 0);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList({ QStringLiteral("bullet"), QStringLiteral("numbered"), QStringLiteral("nested") }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 0, 1 }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::BulletList),
                                int(NoteBlockModel::BulletList) }));
    }

    void indentingListSubtreePreservesItemTypes()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- first\n- second\n    1. child"), true);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::BulletList),
                                int(NoteBlockModel::NumberedList) }));

        model.indentListItems(0, 1, 2, 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::IndentsRole).toList(), QVariantList({ 0, 1, 2 }));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemTypesRole).toList(),
                 QVariantList({ int(NoteBlockModel::BulletList), int(NoteBlockModel::BulletList),
                                int(NoteBlockModel::NumberedList) }));
    }

    void movesStyledImageBlocksWithoutChangingPresentation()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("first\n\n<p align=\"right\"><img src=\"media://image\" "
                                  "alt=\"diagram\" width=\"320\" /></p>\n\nlast"),
                   true);

        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Image));
        QVERIFY(model.moveBlock(1, 2));
        QCOMPARE(model.data(model.index(2), NoteBlockModel::ImageWidthRole).toInt(), 320);
        QCOMPARE(model.data(model.index(2), NoteBlockModel::ImageAlignmentRole).toString(), QStringLiteral("right"));
        QCOMPARE(model.contents(),
                 QStringLiteral("first\n\nlast\n\n<p align=\"right\"><img src=\"media://image\" "
                                "alt=\"diagram\" width=\"320\" /></p>"));
    }

    void findsTextAcrossStructuredBlocks()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("# Heading target\n\n- first\n- target list\n\n"
                                  "| A | B |\n| --- | --- |\n| target cell | last target |"),
                   true);

        auto match = model.findText(QStringLiteral("target"));
        QCOMPARE(match.value(QStringLiteral("blockIndex")).toInt(), 0);
        QCOMPARE(match.value(QStringLiteral("field")).toString(), QStringLiteral("heading"));
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 8);

        match = model.findText(QStringLiteral("target"), match);
        QCOMPARE(match.value(QStringLiteral("blockIndex")).toInt(), 1);
        QCOMPARE(match.value(QStringLiteral("listItemIndex")).toInt(), 1);
        QCOMPARE(match.value(QStringLiteral("field")).toString(), QStringLiteral("listItem"));

        match = model.findText(QStringLiteral("target"), match);
        QCOMPARE(match.value(QStringLiteral("blockIndex")).toInt(), 2);
        QCOMPARE(match.value(QStringLiteral("tableCellIndex")).toInt(), 2);
        QCOMPARE(match.value(QStringLiteral("field")).toString(), QStringLiteral("tableCell"));

        const auto previous = model.findText(QStringLiteral("target"), match, true);
        QCOMPARE(previous.value(QStringLiteral("blockIndex")).toInt(), 1);
        QCOMPARE(previous.value(QStringLiteral("listItemIndex")).toInt(), 1);
    }

    void findTextWrapsAndSupportsCaseSensitivity()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("Target one\n\nsecond target"), true);

        auto match = model.findText(QStringLiteral("target"), {}, false, true);
        QCOMPARE(match.value(QStringLiteral("blockIndex")).toInt(), 1);
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 7);

        match = model.findText(QStringLiteral("target"), match, false, true);
        QCOMPARE(match.value(QStringLiteral("blockIndex")).toInt(), 1);
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 7);
        QVERIFY(match.value(QStringLiteral("wrapped")).toBool());

        const auto insensitive = model.findText(QStringLiteral("target"));
        QCOMPARE(insensitive.value(QStringLiteral("start")).toInt(), 0);
    }

    void findTextWrapsWithinOneField()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("target middle target"), false);

        auto match = model.findText(QStringLiteral("target"));
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 0);
        QVERIFY(!match.value(QStringLiteral("wrapped")).toBool());

        match = model.findText(QStringLiteral("target"), match);
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 14);
        QVERIFY(!match.value(QStringLiteral("wrapped")).toBool());

        match = model.findText(QStringLiteral("target"), match);
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 0);
        QVERIFY(match.value(QStringLiteral("wrapped")).toBool());

        match = model.findText(QStringLiteral("target"), match, true);
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 14);
        QVERIFY(match.value(QStringLiteral("wrapped")).toBool());
    }

    void preservesFencedCodeBlocksLiterally()
    {
        const QString markdown = QStringLiteral("before\n\n"
                                                "````cpp\n"
                                                "- not a list\n"
                                                "**not bold**\n"
                                                "| not | a table |\n"
                                                "``` nested fence\n"
                                                "line with <br> and  two spaces  \n"
                                                "\n"
                                                "````\n\n"
                                                "after");

        NoteBlockModel model;
        model.load(markdown, true);

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::CodeBlock));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::LanguageRole).toString(), QStringLiteral("cpp"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("- not a list\n"
                                "**not bold**\n"
                                "| not | a table |\n"
                                "``` nested fence\n"
                                "line with <br> and  two spaces  \n"));
        QCOMPARE(model.contents(), markdown);

        // Editing code never coalesces Markdown link syntax or otherwise
        // canonicalizes the literal source.
        const QString literal = QStringLiteral("[a](one)[b](one)\n\n- still code");
        model.setBlockText(1, literal);
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), literal);
    }

    void preservesCodeBlockTrailingNewlines()
    {
        const QString  markdown = QStringLiteral("```python\nprint('x')\n\n```");
        NoteBlockModel model;
        model.load(markdown, true);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("print('x')\n"));
        QCOMPARE(model.contents(), markdown);

        NoteBlockModel reparsed;
        reparsed.load(model.contents(), true);
        QCOMPARE(reparsed.data(reparsed.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("print('x')\n"));
    }

    void transfersCodeBlockLanguageAndLiteralText()
    {
        NoteBlockModel source;
        source.load(QStringLiteral("```json\n{\n  \"value\": \"**literal**\"\n}\n```"), true);

        const NoteFragment fragment = source.extractBlockFragment(0, 0);
        QCOMPARE(fragment.blocks.size(), 1);
        QCOMPARE(fragment.blocks.constFirst().type, NoteFragmentBlockType::CodeBlock);
        QCOMPARE(fragment.blocks.constFirst().language, QStringLiteral("json"));
        QCOMPARE(fragment.blocks.constFirst().markdown, QStringLiteral("{\n  \"value\": \"**literal**\"\n}"));

        NoteBlockModel destination;
        destination.load(QStringLiteral("before"), true);
        QString error;
        QVERIFY2(destination.insertBlockFragment(1, fragment, &error), qPrintable(error));
        QCOMPARE(destination.contents(), QStringLiteral("before\n\n```json\n{\n  \"value\": \"**literal**\"\n}\n```"));
    }

    void previewUrlDoesNotChangeMarkdown()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("![cat](media://one)"), true);
        model.setPreviewUrls({ { QStringLiteral("media://one"), QStringLiteral("image://qtnote-media/blob") } });
        QCOMPARE(model.data(model.index(0), NoteBlockModel::PreviewUrlRole).toString(),
                 QStringLiteral("image://qtnote-media/blob"));
        QCOMPARE(model.contents(), QStringLiteral("![cat](media://one)"));
    }
};

QTEST_MAIN(NoteBlockModelTest)
#include "noteblockmodel_test.moc"
