#include <QtTest>

#include "noteblockmodel.h"
#include "notedata.h"
#include "notetagline.h"

#include "noteblockmodel_test.h"

using namespace AnyKeep;

void NoteBlockModelTest::titleIsAlwaysASeparateTextBlock()
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
    QCOMPARE(markdown.data(markdown.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("Body paragraph"));
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

void NoteBlockModelTest::structuredPasteIntoTitleKeepsFollowingBlocks()
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

void NoteBlockModelTest::movingWholeListBlockRestoresTagLineAtBodyBoundary()
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

void NoteBlockModelTest::inlineCodeListItemKeepsLeadingDashes()
{
    const QString  source = QStringLiteral("Title\n\n- `--pytest-opts=\"`--maxfail=0`\"` # comment");
    NoteBlockModel model;
    model.load(source, true);
    QCOMPARE(model.contents(), source);
}

void NoteBlockModelTest::parsesPromotesAndSerializesTagLine()
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

void NoteBlockModelTest::tagLineUsesFormatSpecificBodyBoundary()
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

void NoteBlockModelTest::escapedOrMixedTagLinesRemainOrdinaryText()
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

void NoteBlockModelTest::invalidTagEditRestoresOrdinaryTextAndCursor()
{
    NoteBlockModel model;
    model.load(QStringLiteral("Title\n\n*tb *work\n\nhello"), true);

    const QVariantMap result = model.setTagLineTag(1, 0, QStringLiteral("*tb?"), 3);
    QVERIFY(result.value(QStringLiteral("handled")).toBool());
    QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
    QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("*tb? *work"));
    QCOMPARE(result.value(QStringLiteral("cursorPosition")).toInt(), 3);
}

void NoteBlockModelTest::tagLineRoundTripsThroughBlockFragment()
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

void NoteBlockModelTest::structuredPastePromotesOnlyTheFirstBodyTagLine()
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

void NoteBlockModelTest::tagLineBecomesOrdinaryTextWhenContentMovesBeforeIt()
{
    NoteBlockModel model;
    model.load(QStringLiteral("Title\n\n*tb\n\nbody"), true);

    model.insertImage(1, QStringLiteral("media://image"), QStringLiteral("image"));
    QCOMPARE(model.data(model.index(2), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
    QCOMPARE(model.data(model.index(2), NoteBlockModel::TextRole).toString(), QStringLiteral("*tb"));
    QVERIFY(NoteData::tagsFromText(model.contents().section(QStringLiteral("\n\n"), 1)).isEmpty());
}

void NoteBlockModelTest::parsesAndWritesGithubBlocks()
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

void NoteBlockModelTest::serializesAndParsesImagePresentation()
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
    QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ImageAlignmentRole).toString(), QStringLiteral("right"));
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
    QCOMPARE(restored.data(restored.index(0), NoteBlockModel::ImageAlignmentRole).toString(), QStringLiteral("center"));

    const QString spacedHtml
        = QStringLiteral("<p align=\"left\"><img src=\"media://spaced\" alt=\"A  &quot;B&quot;\" /></p>");
    NoteBlockModel spaced;
    spaced.load(spacedHtml, true);
    QCOMPARE(spaced.data(spaced.index(0), NoteBlockModel::AltRole).toString(), QStringLiteral("A  \"B\""));
    QCOMPARE(spaced.contents(), spacedHtml);
}

void NoteBlockModelTest::serializesParsesAndTransfersAudioBlocks()
{
    const QString uri = QStringLiteral("anykeep-media:/11111111-1111-1111-1111-111111111111/recording.m4a");
    const QString html
        = QStringLiteral("<audio controls src=\"%1\" title=\"Meeting &amp; notes\" "
                         "data-anykeep-duration-ms=\"91234\"></audio>\n"
                         "<div data-anykeep-audio-transcript=\"1\">First line&lt;br&gt;<br />Second &amp; final</div>")
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
    QVERIFY(inserted.contents().contains(QStringLiteral("data-anykeep-duration-ms=\"2500\"")));
}

void NoteBlockModelTest::serializesParsesAndTransfersAttachments()
{
    const QString uri  = QStringLiteral("anykeep-media:/22222222-2222-2222-2222-222222222222/spec.pdf");
    const QString html = QStringLiteral("<a href=\"%1\" data-anykeep-attachment=\"1\" "
                                        "data-anykeep-media-type=\"application/pdf\" data-anykeep-size=\"123456\">"
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

void NoteBlockModelTest::parsesSerializesAndTransfersBlockQuotes()
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

void NoteBlockModelTest::convertsBetweenBlockQuotesAndHeadings()
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

void NoteBlockModelTest::splitsStructuredBlockIntoFollowingParagraph()
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

void NoteBlockModelTest::leadingTablePipeDoesNotCreatePhantomColumn()
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

void NoteBlockModelTest::preservesGithubUnderlineMarkup()
{
    NoteBlockModel model;
    model.load(QStringLiteral("before <ins>one</ins> after\n\n"
                              "| A | B |\n| --- | --- |\n| <u>two</u> | plain |"),
               true);

    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(),
             QStringLiteral("before <ins>one</ins> after"));
    const auto table = model.data(model.index(1), NoteBlockModel::CellsRole).toMap();
    QCOMPARE(table.value(QStringLiteral("values")).toStringList(), QStringList({ "A", "B", "<u>two</u>", "plain" }));
    QCOMPARE(model.contents(),
             QStringLiteral("before <ins>one</ins> after\n\n"
                            "| A | B |\n| --- | --- |\n| <u>two</u> | plain |"));
}
