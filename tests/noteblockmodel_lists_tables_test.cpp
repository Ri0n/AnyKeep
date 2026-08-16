#include <QtTest>

#include "noteblockmodel.h"
#include "notedata.h"
#include "notetagline.h"

#include "noteblockmodel_test.h"

using namespace AnyKeep;

void NoteBlockModelTest::editsTableStructure()
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

void NoteBlockModelTest::reordersWholeTableColumns()
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

void NoteBlockModelTest::tableLineBreaksUseGithubCompatibleHtml()
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

void NoteBlockModelTest::serializesListContinuationsUsingCommonMarkIndentation()
{
    NoteBlockModel model;
    model.load(QStringLiteral("- [ ] first<br><br>"), true);
    QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList().value(0), QStringLiteral("first"));
    model.setListItem(0, 0, QStringLiteral("first\nsecond\n\n"));
    QCOMPARE(model.contents(), QStringLiteral("- [ ] first\n      second"));

    model.load(QStringLiteral("- first\n- second\n1. numbered"), true);
    model.setListItem(0, 0, QStringLiteral("first line\nsecond line"));
    model.setListItem(1, 0, QStringLiteral("numbered line\ncontinuation"));
    QCOMPARE(model.contents(),
             QStringLiteral("- first line\n  second line\n- second\n\n"
                            "1. numbered line\n   continuation"));
}

void NoteBlockModelTest::parsesIndentedListContinuationsAsOneItem()
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

void NoteBlockModelTest::keepsCanonicalWriterWrapInsideLongListItem()
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

void NoteBlockModelTest::supportsNumberedAndIndentedLists()
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

void NoteBlockModelTest::nestedTaskListSurvivesMarkdownRoundTrip()
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

void NoteBlockModelTest::outdentedListItemAdoptsParentListType()
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

void NoteBlockModelTest::reindentedListItemRestoresNestedListType()
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

void NoteBlockModelTest::taskListSurroundingNestedNumberedItemsStaysOneBlock()
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

void NoteBlockModelTest::preservesThreeLevelMixedListIndentation()
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
