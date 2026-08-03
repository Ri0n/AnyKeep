#include <QtTest>

#include "noteblockmodel.h"
#include "notedata.h"
#include "notetagline.h"

#include "noteblockmodel_test.h"

using namespace AnyKeep;

void NoteBlockModelTest::findsTextAcrossStructuredBlocks()
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

void NoteBlockModelTest::findTextWrapsAndSupportsCaseSensitivity()
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

void NoteBlockModelTest::findTextWrapsWithinOneField()
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

void NoteBlockModelTest::preservesFencedCodeBlocksLiterally()
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

void NoteBlockModelTest::preservesCodeBlockTrailingNewlines()
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

void NoteBlockModelTest::transfersCodeBlockLanguageAndLiteralText()
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

void NoteBlockModelTest::previewUrlDoesNotChangeMarkdown()
{
    NoteBlockModel model;
    model.load(QStringLiteral("![cat](media://one)"), true);
    model.setPreviewUrls({ { QStringLiteral("media://one"), QStringLiteral("image://anykeep-media/blob") } });
    QCOMPARE(model.data(model.index(0), NoteBlockModel::PreviewUrlRole).toString(),
             QStringLiteral("image://anykeep-media/blob"));
    QCOMPARE(model.contents(), QStringLiteral("![cat](media://one)"));
}
