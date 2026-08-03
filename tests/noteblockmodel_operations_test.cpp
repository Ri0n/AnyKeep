#include <QtTest>

#include "noteblockmodel.h"
#include "notedata.h"
#include "notetagline.h"

#include "noteblockmodel_test.h"

using namespace AnyKeep;

void NoteBlockModelTest::extractsAndInsertsWholeBlockFragments()
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

void NoteBlockModelTest::extractsCrossBlockSelectionStructurally()
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

void NoteBlockModelTest::includesUnrangedBlocksCrossedBySelection()
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

void NoteBlockModelTest::removesCrossBlockSelectionIncludingListAndTable()
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

void NoteBlockModelTest::removesTrailingImageCrossedFromDocumentBoundary()
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

void NoteBlockModelTest::rejectsNonBlockFragmentInsertion()
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

void NoteBlockModelTest::replacesTextRangeWithStructuredFragmentAtomically()
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
    QCOMPARE(model.replaceTextBlockRangeWithFragment(0, QStringLiteral("before "), QStringLiteral(" after"), fragment,
                                                     &error),
             1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.contents(), QStringLiteral("before\n\n- first\n- second\n\nafter"));
}

void NoteBlockModelTest::replacesTableRectangleAndExpandsTable()
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
    table.type                = NoteFragmentBlockType::Table;
    table.table.rows          = 2;
    table.table.columns       = 2;
    table.table.headerRows    = 1;
    table.table.markdownCells = { QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"), QStringLiteral("W") };
    fragment.blocks.append(table);

    QString error;
    QVERIFY2(model.replaceTableCellsWithFragment(1, 3, fragment, &error), qPrintable(error));
    const auto cells = model.data(model.index(1), NoteBlockModel::CellsRole).toMap();
    QCOMPARE(cells.value(QStringLiteral("columns")).toInt(), 3);
    QCOMPARE(cells.value(QStringLiteral("values")).toStringList(),
             QStringList({ "A", "B", "", "1", "X", "Y", "", "Z", "W" }));
}

void NoteBlockModelTest::replacesFlatListItemWithMixedListFragment()
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
    QCOMPARE(model.replaceListItemRangeWithFragment(0, 0, QStringLiteral("before "), QStringLiteral(" after"), fragment,
                                                    &error),
             1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(model.contents(), QStringLiteral("- before\n- [x] task\n    1. nested\n- after\n- tail"));
}

void NoteBlockModelTest::rejectsListPasteIntoItemWithDescendants()
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

void NoteBlockModelTest::mergesAdjacentMarkdownParagraphs()
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

void NoteBlockModelTest::keepsStructuralBoundariesBetweenTextSections()
{
    NoteBlockModel model;
    model.load(QStringLiteral("before one\n\nbefore two\n\n- item\n\nafter one\n\nafter two"), true);

    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(), QStringLiteral("before one"));
    QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QStringLiteral("before two"));
    QCOMPARE(model.data(model.index(2), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BulletList));
    QCOMPARE(model.data(model.index(3), NoteBlockModel::TextRole).toString(), QStringLiteral("after one\n\nafter two"));
}

void NoteBlockModelTest::backspaceAtListItemStartUnlistsOrOutdents()
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

void NoteBlockModelTest::textAfterBlankLineDoesNotJoinChecklistItem()
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

void NoteBlockModelTest::editsAreSerialized()
{
    NoteBlockModel model;
    model.load(QStringLiteral("- [ ] first"), true);
    model.setChecked(0, 0, true);
    model.setListItem(0, 0, QStringLiteral("changed"));
    QCOMPARE(model.contents(), QStringLiteral("- [x] changed"));
}

void NoteBlockModelTest::nestedTaskChecksPropagateBetweenParentsAndDescendants()
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

void NoteBlockModelTest::mergesLastListItemWithFollowingTextOrListItem()
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

void NoteBlockModelTest::equalScalarWritesAreNoOps()
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
    QVERIFY(!model.setData(image, model.data(image, NoteBlockModel::ImageWidthRole), NoteBlockModel::ImageWidthRole));
    QVERIFY(!model.setData(image, model.data(image, NoteBlockModel::ImageAlignmentRole),
                           NoteBlockModel::ImageAlignmentRole));
    QCOMPARE(changed.size(), 0);
}

void NoteBlockModelTest::reportsScalarEditsWithoutExposingInternalState()
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

void NoteBlockModelTest::coalescesAdjacentLinksCreatedAcrossFormatRuns()
{
    NoteBlockModel model;
    model.load(QStringLiteral("text"), true);
    model.setBlockText(0, QStringLiteral("[Надежду ](url)[*Л*](url)[ебедев](url)[*у*](url)"));
    QCOMPARE(model.contents(), QStringLiteral("[Надежду *Л*ебедев*у*](url)"));
}

void NoteBlockModelTest::keepsLongInlineLinksOnTheirSourceLine()
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
