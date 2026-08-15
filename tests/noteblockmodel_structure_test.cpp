#include <QtTest>

#include "noteblockmodel.h"
#include "notedata.h"
#include "notetagline.h"

#include "noteblockmodel_test.h"

using namespace AnyKeep;

void NoteBlockModelTest::parsesSerializesAndSplitsHeadingBlocks()
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

void NoteBlockModelTest::insertsMinimalStructuredBlocks()
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

void NoteBlockModelTest::collapsesUneditedInsertedParagraphOnMarkdownRoundTrip()
{
    NoteBlockModel model;
    model.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |\n\n- after"), true);

    model.insertTextBlock(1);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
    QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(), QString());
    QVERIFY(model.isExplicitEmptyTextBlock(1));
    QVERIFY(!model.contents().contains(QStringLiteral("anykeep:empty-paragraph")));

    NoteBlockModel restored;
    restored.load(model.contents(), true);
    QCOMPARE(restored.rowCount(), 2);
    QCOMPARE(restored.data(restored.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Table));
    QCOMPARE(restored.data(restored.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BulletList));

    NoteBlockModel legacy;
    legacy.load(QStringLiteral("| A | B |\n| --- | --- |\n| one | two |\n\n"
                               "<!-- anykeep:empty-paragraph -->\n\n- after"),
                true);
    QCOMPARE(legacy.rowCount(), 2);
    QVERIFY(!legacy.contents().contains(QStringLiteral("anykeep:empty-paragraph")));

    model.setBlockText(1, QStringLiteral("between"));
    QVERIFY(!model.isExplicitEmptyTextBlock(1));
    QCOMPARE(model.contents(), QStringLiteral("| A | B |\n| --- | --- |\n| one | two |\n\nbetween\n\n- after"));

    model.setBlockText(1, QString());
    QVERIFY(model.isExplicitEmptyTextBlock(1));
    QVERIFY(!model.contents().contains(QStringLiteral("anykeep:empty-paragraph")));

    NoteBlockModel ordinary;
    ordinary.load(QStringLiteral("title\n\n- item"), true);
    ordinary.setBlockText(0, QString());
    QVERIFY(!ordinary.isExplicitEmptyTextBlock(0));
}

void NoteBlockModelTest::insertingAndConvertingListsPreservesIndentation()
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

void NoteBlockModelTest::coalescesBlocksAfterListConversions()
{
    NoteBlockModel unlisted;
    unlisted.load(QStringLiteral("title\n\n- one\n- two\n- three"), true);
    QCOMPARE(unlisted.unlistListItem(1, 0), 1);
    QCOMPARE(unlisted.unlistListItem(2, 0), 1);
    QCOMPARE(unlisted.unlistListItem(2, 0), 1);
    QCOMPARE(unlisted.rowCount(), 2);
    QCOMPARE(unlisted.blockTypeAt(1), int(NoteBlockModel::Text));
    QCOMPARE(unlisted.data(unlisted.index(1), NoteBlockModel::TextRole).toString(),
             QStringLiteral("one\n\ntwo\n\nthree"));

    NoteBlockModel adjacentLists;
    adjacentLists.load(QStringLiteral("title\n\nfirst"), true);
    adjacentLists.insertList(2, NoteBlockModel::BulletList);
    adjacentLists.setListItem(2, 0, QStringLiteral("second"));
    const QVariantMap converted = adjacentLists.convertTextRangeToList(1, 0, 5, NoteBlockModel::BulletList);
    QVERIFY(converted.value(QStringLiteral("handled")).toBool());
    QCOMPARE(adjacentLists.rowCount(), 2);
    QCOMPARE(adjacentLists.blockTypeAt(1), int(NoteBlockModel::BulletList));
    QCOMPARE(adjacentLists.data(adjacentLists.index(1), NoteBlockModel::ItemsRole).toStringList(),
             QStringList({ "first", "second" }));
}

void NoteBlockModelTest::recalculatesTaskParentsAfterStructuralListChanges()
{
    NoteBlockModel inserted;
    inserted.load(QStringLiteral("- [x] parent\n  - [x] child"), true);
    inserted.insertListItem(0, 2, QStringLiteral("new child"));
    QCOMPARE(inserted.data(inserted.index(0), NoteBlockModel::CheckedRole).toList(),
             QVariantList({ false, true, false }));

    NoteBlockModel indented;
    indented.load(QStringLiteral("- [x] parent\n  - [x] child\n- [ ] sibling"), true);
    indented.indentListItems(0, 2, 2, 1);
    QCOMPARE(indented.data(indented.index(0), NoteBlockModel::CheckedRole).toList(),
             QVariantList({ false, true, false }));

    NoteBlockModel moved;
    moved.load(QStringLiteral("- [x] parent\n  - [x] child\n- [ ] sibling"), true);
    QVERIFY(moved.moveListRange(0, 2, 2, 0, 2, 1));
    QCOMPARE(moved.data(moved.index(0), NoteBlockModel::CheckedRole).toList(), QVariantList({ false, true, false }));
}

void NoteBlockModelTest::removesStructuredRangesAtomically()
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

void NoteBlockModelTest::movesListSubtreesAcrossBlocks()
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

void NoteBlockModelTest::movesAndReindentsListSubtreesAtomically()
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

void NoteBlockModelTest::detachesListSubtreesIntoStandaloneBlocks()
{
    NoteBlockModel model;
    model.load(QStringLiteral("before\n\n- parent\n    - child\n        1. grandchild\n- tail\n\nafter"), true);

    const int detachedRow = model.moveListRangeToBlock(1, 1, 2, 3);
    QCOMPARE(detachedRow, 3);
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.data(model.index(1), NoteBlockModel::ItemsRole).toStringList(), QStringList({ "parent", "tail" }));
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

void NoteBlockModelTest::reattachesStandaloneListsAcrossInterveningBlocks()
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

void NoteBlockModelTest::movesWholeListsThroughStandaloneBoundaries()
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

void NoteBlockModelTest::movesWholeBlocks()
{
    NoteBlockModel model;
    model.load(QStringLiteral("first\n\n- item\n\n![image](media://image)"), true);
    QVERIFY(model.moveBlock(2, 0));
    QCOMPARE(model.contents(), QStringLiteral("![image](media://image)\n\nfirst\n\n- item"));
    QVERIFY(model.moveBlock(0, 2));
    QCOMPARE(model.contents(), QStringLiteral("first\n\n- item\n\n![image](media://image)"));
}

void NoteBlockModelTest::movingMultilineTextToTitleSplitsAndRecombinesBody()
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

void NoteBlockModelTest::movingBlockBeforeFormerTitleRecombinesText()
{
    NoteBlockModel model;
    model.load(QStringLiteral("Old title\n\n- item\n\nNew title\n\nrest"), true);

    // Moving multiline text to the top splits its first paragraph into the
    // dedicated title block. Moving a list before it removes that special
    // position, so the former title must rejoin its body text.
    QCOMPARE(model.moveBlockResolved(2, 0), 0);
    QCOMPARE(model.moveBlockResolved(2, 0), 0);

    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.blockTypeAt(0), int(NoteBlockModel::BulletList));
    QCOMPARE(model.data(model.index(1), NoteBlockModel::TextRole).toString(),
             QStringLiteral("New title\n\nrest\n\nOld title"));
    QCOMPARE(model.contents(), QStringLiteral("- item\n\nNew title\n\nrest\n\nOld title"));
}

void NoteBlockModelTest::movingStructuredBlockAwayRecombinesTextNeighbors()
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

void NoteBlockModelTest::movingListBlockNextToListMergesAndAdoptsResidentType()
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

void NoteBlockModelTest::movingBlockAwayMergesAdjacentListsAtSourceGap()
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

void NoteBlockModelTest::movingListRangeIntoListAdoptsExistingTypeAtSameLevel()
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

void NoteBlockModelTest::indentingListSubtreePreservesItemTypes()
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

void NoteBlockModelTest::movesStyledImageBlocksWithoutChangingPresentation()
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
