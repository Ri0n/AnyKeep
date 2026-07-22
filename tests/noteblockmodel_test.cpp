#include <QtTest>

#include "noteblockmodel.h"

using namespace QtNote;

class NoteBlockModelTest : public QObject {
    Q_OBJECT

private slots:
    void plainTextIsOneBlock()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("title\nbody"), false);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.contents(), QStringLiteral("title\nbody"));
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
        model.load(QStringLiteral("first paragraph\n\nsecond paragraph"), true);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::Text));
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("first paragraph\n\nsecond paragraph"));
        QCOMPARE(model.contents(), QStringLiteral("first paragraph\n\nsecond paragraph"));
    }

    void keepsStructuralBoundariesBetweenTextSections()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("before one\n\nbefore two\n\n- item\n\nafter one\n\nafter two"), true);

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("before one\n\nbefore two"));
        QCOMPARE(model.data(model.index(1), NoteBlockModel::TypeRole).toInt(), int(NoteBlockModel::BulletList));
        QCOMPARE(model.data(model.index(2), NoteBlockModel::TextRole).toString(),
                 QStringLiteral("after one\n\nafter two"));
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

        QCOMPARE(edits.size(), 5);
        QCOMPARE(edits.at(0).at(1).toInt(), int(NoteBlockModel::TextRole));
        QCOMPARE(edits.at(1).at(1).toInt(), int(NoteBlockModel::ItemsRole));
        QCOMPARE(edits.at(1).at(2).toInt(), 0);
        QCOMPARE(edits.at(2).at(1).toInt(), int(NoteBlockModel::CellsRole));
        QCOMPARE(edits.at(2).at(2).toInt(), 2);
        QCOMPARE(edits.at(3).at(1).toInt(), int(NoteBlockModel::UrlRole));
        QCOMPARE(edits.at(4).at(1).toInt(), int(NoteBlockModel::AltRole));
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

    void stripsTrailingListBreaksButKeepsInternalBreaks()
    {
        NoteBlockModel model;
        model.load(QStringLiteral("- [ ] first<br><br>"), true);
        QCOMPARE(model.data(model.index(0), NoteBlockModel::ItemsRole).toStringList().value(0),
                 QStringLiteral("first"));
        model.setListItem(0, 0, QStringLiteral("first\nsecond\n\n"));
        QCOMPARE(model.contents(), QStringLiteral("- [ ] first<br>second"));
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
        QCOMPARE(converted.convertTextBlockToHeading(0, 10, 2), 1);
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
        QCOMPARE(match.value(QStringLiteral("start")).toInt(), 7);

        match = model.findText(QStringLiteral("target"), match, false, true);
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
