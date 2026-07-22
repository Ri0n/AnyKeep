#include "noteblockmodel.h"
#include <climits>

#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>

#include <algorithm>
#include <limits>

namespace QtNote {
namespace {
    const QString TableLineBreakMarker = QStringLiteral("QTNOTE_TABLE_LINE_BREAK_7F3A");

    QString decodeTableCellLineBreaks(QString text)
    {
        static const QRegularExpression lineBreak(QStringLiteral("<br\\s*/?>"),
                                                  QRegularExpression::CaseInsensitiveOption);
        text.replace(lineBreak, QStringLiteral("\n"));
        text.replace(TableLineBreakMarker, QStringLiteral("\n"));
        return text;
    }

    bool isListType(NoteBlockModel::BlockType type)
    {
        return type == NoteBlockModel::BulletList || type == NoteBlockModel::CheckList
            || type == NoteBlockModel::NumberedList;
    }

    QStringList tableCells(QString line)
    {
        line = line.trimmed();
        if (line.startsWith('|'))
            line.remove(0, 1);
        if (line.endsWith('|'))
            line.chop(1);
        auto cells = line.split('|');
        for (auto &cell : cells) {
            cell = cell.trimmed();
            cell = decodeTableCellLineBreaks(std::move(cell));
        }
        return cells;
    }

    bool isTableSeparator(const QString &line)
    {
        static const QRegularExpression separator(
            QStringLiteral(R"(^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$)"));
        return separator.match(line).hasMatch();
    }

    QString decodeLineBreaks(QString text)
    {
        text.replace(TableLineBreakMarker, QStringLiteral("\n"));
        return text;
    }

    QString encodeLineBreaks(QString text)
    {
        text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        return text;
    }

    QString decodeListItem(QString text)
    {
        text = decodeLineBreaks(std::move(text));
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        return text;
    }

    QString encodeListItem(QString text)
    {
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        return encodeLineBreaks(std::move(text));
    }

    QString coalesceAdjacentMarkdownLinks(QString text)
    {
        static const QRegularExpression adjacentLinks(QStringLiteral(R"(\[([^\]]*)\]\(([^)\s]+)\)\[([^\]]*)\]\(\2\))"));
        QString                         previous;
        do {
            previous = text;
            text.replace(adjacentLinks, QStringLiteral("[\\1\\3](\\2)"));
        } while (text != previous);
        return text;
    }

    NoteFragmentListKind fragmentListKind(NoteBlockModel::BlockType type)
    {
        switch (type) {
        case NoteBlockModel::CheckList:
            return NoteFragmentListKind::Check;
        case NoteBlockModel::NumberedList:
            return NoteFragmentListKind::Numbered;
        default:
            return NoteFragmentListKind::Bullet;
        }
    }

    NoteBlockModel::BlockType modelListKind(NoteFragmentListKind kind)
    {
        switch (kind) {
        case NoteFragmentListKind::Check:
            return NoteBlockModel::CheckList;
        case NoteFragmentListKind::Numbered:
            return NoteBlockModel::NumberedList;
        default:
            return NoteBlockModel::BulletList;
        }
    }
}

NoteBlockModel::NoteBlockModel(QObject *parent) : QAbstractListModel(parent) { }

int NoteBlockModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : blocks_.size(); }

QVariant NoteBlockModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= blocks_.size())
        return {};
    const auto &block = blocks_.at(index.row());
    switch (role) {
    case TypeRole:
        return block.type;
    case TextRole:
        return block.text;
    case ItemsRole:
        return block.items;
    case CheckedRole:
        return block.checked;
    case CellsRole: {
        QVariantMap table;
        table.insert(QStringLiteral("values"), block.cells);
        table.insert(QStringLiteral("columns"), block.columns);
        return table;
    }
    case UrlRole:
        return block.url;
    case AltRole:
        return block.alt;
    case PreviewUrlRole:
        return previewUrls_.value(block.url, block.url);
    case IndentsRole:
        return block.indents;
    case ItemTypesRole:
        return block.itemTypes;
    case HeadingLevelRole:
        return block.headingLevel;
    default:
        return {};
    }
}

bool NoteBlockModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= blocks_.size())
        return false;
    auto   &block = blocks_[index.row()];
    QString before;
    QString after;
    bool    scalar = false;
    switch (role) {
    case TextRole:
        if (block.text == value.toString())
            return false;
        before     = block.text;
        block.text = value.toString();
        after      = block.text;
        scalar     = true;
        break;
    case ItemsRole:
        if (block.items == value.toStringList())
            return false;
        block.items = value.toStringList();
        break;
    case CheckedRole:
        if (block.checked == value.toList())
            return false;
        block.checked = value.toList();
        break;
    case UrlRole:
        if (block.url == value.toString())
            return false;
        before    = block.url;
        block.url = value.toString();
        after     = block.url;
        scalar    = true;
        break;
    case AltRole:
        if (block.alt == value.toString())
            return false;
        before    = block.alt;
        block.alt = value.toString();
        after     = block.alt;
        scalar    = true;
        break;
    default:
        return false;
    }
    if (scalar)
        emit scalarEdited(index.row(), role, -1, before, after);
    changed(index.row(), { role });
    return true;
}

Qt::ItemFlags NoteBlockModel::flags(const QModelIndex &index) const
{
    return QAbstractListModel::flags(index) | (index.isValid() ? Qt::ItemIsEditable : Qt::NoItemFlags);
}

QHash<int, QByteArray> NoteBlockModel::roleNames() const
{
    return { { TypeRole, "blockType" },
             { TextRole, "blockText" },
             { ItemsRole, "items" },
             { CheckedRole, "checkedItems" },
             { CellsRole, "table" },
             { UrlRole, "url" },
             { AltRole, "alt" },
             { PreviewUrlRole, "previewUrl" },
             { IndentsRole, "itemIndents" },
             { ItemTypesRole, "itemTypes" },
             { HeadingLevelRole, "headingLevel" } };
}

QString NoteBlockModel::contents() const
{
    if (markdown_) {
        auto result = writeMarkdown(blocks_);
        while (result.endsWith(QLatin1Char('\n')) || result.endsWith(QLatin1Char('\r')))
            result.chop(1);
        return result;
    }
    return blocks_.isEmpty() ? QString() : blocks_.constFirst().text;
}

void NoteBlockModel::setContents(const QString &contents) { load(contents, markdown_); }

QVariantMap NoteBlockModel::findText(const QString &text, const QVariantMap &after, bool backwards,
                                     bool caseSensitive) const
{
    if (text.isEmpty() || blocks_.isEmpty())
        return {};

    struct SearchField {
        int     blockIndex     = -1;
        int     listItemIndex  = -1;
        int     tableCellIndex = -1;
        QString editorField;
        QString text;
    };
    struct Match {
        SearchField field;
        int         fieldOrder = -1;
        int         start      = -1;
    };

    QList<SearchField> fields;
    for (int blockIndex = 0; blockIndex < blocks_.size(); ++blockIndex) {
        const auto &block = blocks_.at(blockIndex);
        switch (block.type) {
        case Text:
            fields.append({ blockIndex, -1, -1, QStringLiteral("text"), block.text });
            break;
        case Heading:
            fields.append({ blockIndex, -1, -1, QStringLiteral("heading"), block.text });
            break;
        case BulletList:
        case CheckList:
        case NumberedList:
            for (int item = 0; item < block.items.size(); ++item)
                fields.append({ blockIndex, item, -1, QStringLiteral("listItem"), block.items.at(item) });
            break;
        case Table:
            for (int cell = 0; cell < block.cells.size(); ++cell)
                fields.append({ blockIndex, -1, cell, QStringLiteral("tableCell"), block.cells.at(cell) });
            break;
        case Image:
            // Image alt text is not edited by a text delegate yet, so selecting
            // a search match there would have no visible target.
            break;
        }
    }

    const auto   sensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QList<Match> matches;
    for (int fieldOrder = 0; fieldOrder < fields.size(); ++fieldOrder) {
        const auto &field = fields.at(fieldOrder);
        int         from  = 0;
        while (from <= field.text.size()) {
            const int position = field.text.indexOf(text, from, sensitivity);
            if (position < 0)
                break;
            matches.append({ field, fieldOrder, position });
            from = position + qMax(1, text.size());
        }
    }
    if (matches.isEmpty())
        return {};

    auto sameAddress = [](const SearchField &field, const QVariantMap &address) {
        return field.blockIndex == address.value(QStringLiteral("blockIndex"), -1).toInt()
            && field.listItemIndex == address.value(QStringLiteral("listItemIndex"), -1).toInt()
            && field.tableCellIndex == address.value(QStringLiteral("tableCellIndex"), -1).toInt()
            && field.editorField == address.value(QStringLiteral("field")).toString();
    };

    int  selected = backwards ? matches.size() - 1 : 0;
    bool wrapped  = false;
    if (!after.isEmpty()) {
        int afterFieldOrder = -1;
        for (int i = 0; i < fields.size(); ++i) {
            if (sameAddress(fields.at(i), after)) {
                afterFieldOrder = i;
                break;
            }
        }

        if (afterFieldOrder >= 0) {
            const int afterStart = after.value(QStringLiteral("start"), backwards ? INT_MAX : -1).toInt();
            selected             = -1;
            if (backwards) {
                for (int i = matches.size() - 1; i >= 0; --i) {
                    const auto &match = matches.at(i);
                    if (match.fieldOrder < afterFieldOrder
                        || (match.fieldOrder == afterFieldOrder && match.start < afterStart)) {
                        selected = i;
                        break;
                    }
                }
                if (selected < 0) {
                    selected = matches.size() - 1;
                    wrapped  = true;
                }
            } else {
                for (int i = 0; i < matches.size(); ++i) {
                    const auto &match = matches.at(i);
                    if (match.fieldOrder > afterFieldOrder
                        || (match.fieldOrder == afterFieldOrder && match.start > afterStart)) {
                        selected = i;
                        break;
                    }
                }
                if (selected < 0) {
                    selected = 0;
                    wrapped  = true;
                }
            }
        }
    }

    const auto &match = matches.at(selected);
    QVariantMap result;
    result.insert(QStringLiteral("blockIndex"), match.field.blockIndex);
    result.insert(QStringLiteral("listItemIndex"), match.field.listItemIndex);
    result.insert(QStringLiteral("tableCellIndex"), match.field.tableCellIndex);
    result.insert(QStringLiteral("field"), match.field.editorField);
    result.insert(QStringLiteral("start"), match.start);
    result.insert(QStringLiteral("length"), text.size());
    result.insert(QStringLiteral("wrapped"), wrapped);
    return result;
}

QList<NoteBlockModel::Block> NoteBlockModel::cloneBlocks(const QList<Block> &blocks)
{
    // A QList copy would share its backing storage with the live model. That
    // makes every subsequent character edit detach the entire block list while
    // a history command exists. Keep the list container independent; strings
    // and variants remain implicitly shared until an individual field changes.
    QList<Block> result;
    result.reserve(blocks.size());
    for (const auto &block : blocks)
        result.append(block);
    return result;
}

NoteBlockModel::State NoteBlockModel::state() const { return { cloneBlocks(blocks_), markdown_ }; }

bool NoteBlockModel::restoreState(const State &state)
{
    if (this->state() == state)
        return false;
    const bool formatChanged = markdown_ != state.markdown;
    beginResetModel();
    blocks_   = cloneBlocks(state.blocks);
    markdown_ = state.markdown;
    endResetModel();
    if (formatChanged)
        emit markdownChanged();
    emit contentsChanged();
    return true;
}

void NoteBlockModel::load(const QString &contents, bool markdown)
{
    beginResetModel();
    markdown_ = markdown;
    blocks_   = markdown ? parseMarkdown(contents) : QList<Block> { Block { Text, contents } };
    endResetModel();
    emit markdownChanged();
    emit contentsChanged();
}

void NoteBlockModel::setBlockText(int row, const QString &text)
{
    setData(index(row), coalesceAdjacentMarkdownLinks(text), TextRole);
}

bool NoteBlockModel::mergeTextBlockWithNext(int row)
{
    if (row < 0 || row + 1 >= blocks_.size() || blocks_[row].type != Text || blocks_[row + 1].type != Text)
        return false;

    beginRemoveRows({}, row + 1, row + 1);
    blocks_[row].text = coalesceAdjacentMarkdownLinks(blocks_[row].text + blocks_[row + 1].text);
    blocks_.removeAt(row + 1);
    endRemoveRows();
    changed(row, { TextRole });
    return true;
}

void NoteBlockModel::setListItem(int row, int item, const QString &text)
{
    if (row < 0 || row >= blocks_.size() || item < 0 || item >= blocks_[row].items.size())
        return;
    const QString value = coalesceAdjacentMarkdownLinks(text);
    if (blocks_[row].items[item] == value)
        return;
    const QString before     = blocks_[row].items[item];
    blocks_[row].items[item] = value;
    emit scalarEdited(row, ItemsRole, item, before, value);
    changed(row, { ItemsRole });
}

void NoteBlockModel::insertListItem(int row, int item, const QString &text)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_[row].type))
        return;
    auto &block        = blocks_[row];
    item               = qBound(0, item, block.items.size());
    const int indent   = item > 0 ? block.indents.value(item - 1).toInt() : 0;
    const int itemType = item > 0 ? block.itemTypes.value(item - 1, block.type).toInt() : block.type;
    block.items.insert(item, decodeListItem(text));
    block.indents.insert(item, indent);
    block.itemTypes.insert(item, itemType);
    block.checked.insert(item, false);
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
}

void NoteBlockModel::mergeListItemWithNext(int row, int item)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_[row].type))
        return;
    auto &block = blocks_[row];
    if (item < 0 || item + 1 >= block.items.size())
        return;
    block.items[item] += block.items[item + 1];
    block.items.removeAt(item + 1);
    if (item + 1 < block.indents.size())
        block.indents.removeAt(item + 1);
    if (item + 1 < block.itemTypes.size())
        block.itemTypes.removeAt(item + 1);
    if (item + 1 < block.checked.size())
        block.checked.removeAt(item + 1);
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
}

void NoteBlockModel::removeListItem(int row, int item)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_[row].type))
        return;
    if (blocks_[row].items.size() <= 1 || item < 0 || item >= blocks_[row].items.size())
        return;
    removeListItems(row, item, item);
}

void NoteBlockModel::removeListItems(int row, int firstItem, int lastItem)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].items.isEmpty())
        return;
    auto &block = blocks_[row];
    firstItem   = qBound(0, firstItem, block.items.size() - 1);
    lastItem    = qBound(firstItem, lastItem, block.items.size() - 1);
    if (firstItem == 0 && lastItem == block.items.size() - 1) {
        block = Block {};
        changed(row, { TypeRole, TextRole, ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
        return;
    }
    for (int item = lastItem; item >= firstItem; --item) {
        block.items.removeAt(item);
        if (item < block.indents.size())
            block.indents.removeAt(item);
        if (item < block.itemTypes.size())
            block.itemTypes.removeAt(item);
        if (item < block.checked.size())
            block.checked.removeAt(item);
    }
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
}

void NoteBlockModel::convertListToText(int row)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_[row].type))
        return;
    blocks_[row] = Block {};
    changed(row, { TypeRole, TextRole, ItemsRole, CheckedRole });
}

void NoteBlockModel::indentListItems(int row, int firstItem, int lastItem, int delta)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].items.isEmpty())
        return;
    auto &block = blocks_[row];
    firstItem   = qBound(0, firstItem, block.items.size() - 1);
    lastItem    = qBound(firstItem, lastItem, block.items.size() - 1);
    while (block.indents.size() < block.items.size())
        block.indents.append(0);
    while (block.itemTypes.size() < block.items.size())
        block.itemTypes.append(block.type);
    for (int item = firstItem; item <= lastItem; ++item) {
        const int oldIndent = block.indents[item].toInt();
        int       indent    = qMax(0, oldIndent + delta);
        const int maximum   = item == 0 ? 0 : block.indents[item - 1].toInt() + 1;
        indent              = qMin(indent, maximum);
        block.indents[item] = indent;

        if (indent < oldIndent) {
            for (int ancestor = item - 1; ancestor >= 0; --ancestor) {
                if (block.indents[ancestor].toInt() == indent) {
                    block.itemTypes[item] = block.itemTypes[ancestor];
                    break;
                }
            }
        } else if (indent > oldIndent) {
            bool foundType = false;
            for (int sibling = item - 1; sibling >= 0 && block.indents[sibling].toInt() >= indent; --sibling) {
                if (block.indents[sibling].toInt() == indent) {
                    block.itemTypes[item] = block.itemTypes[sibling];
                    foundType             = true;
                    break;
                }
            }
            for (int sibling = item + 1;
                 !foundType && sibling < block.items.size() && block.indents[sibling].toInt() >= indent; ++sibling) {
                if (block.indents[sibling].toInt() == indent) {
                    block.itemTypes[item] = block.itemTypes[sibling];
                    foundType             = true;
                }
            }
        }
    }
    changed(row, { IndentsRole, ItemTypesRole });
}

void NoteBlockModel::setChecked(int row, int item, bool checked)
{
    if (row < 0 || row >= blocks_.size() || item < 0 || item >= blocks_[row].checked.size())
        return;
    if (blocks_[row].checked[item].toBool() == checked)
        return;
    blocks_[row].checked[item] = checked;
    changed(row, { CheckedRole });
}

void NoteBlockModel::setTableCell(int row, int cell, const QString &text)
{
    if (row < 0 || row >= blocks_.size() || cell < 0 || cell >= blocks_[row].cells.size())
        return;
    const QString value = coalesceAdjacentMarkdownLinks(decodeTableCellLineBreaks(text));
    if (blocks_[row].cells[cell] == value)
        return;
    const QString before     = blocks_[row].cells[cell];
    blocks_[row].cells[cell] = value;
    emit scalarEdited(row, CellsRole, cell, before, value);
    changed(row, { CellsRole });
}

void NoteBlockModel::insertTableRow(int row, int tableRow)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto     &block = blocks_[row];
    const int rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    tableRow        = qBound(0, tableRow, rows);
    for (int column = 0; column < block.columns; ++column)
        block.cells.insert(tableRow * block.columns, QString());
    changed(row, { CellsRole });
}

void NoteBlockModel::removeTableRow(int row, int tableRow)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    const auto &block = blocks_[row];
    const int   rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    if (rows <= 1 || tableRow < 0 || tableRow >= rows)
        return;
    removeTableRows(row, tableRow, tableRow);
}

void NoteBlockModel::removeTableRows(int row, int firstRow, int lastRow)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto     &block = blocks_[row];
    const int rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    if (rows <= 1)
        return;
    firstRow              = qBound(0, firstRow, rows - 1);
    lastRow               = qBound(firstRow, lastRow, rows - 1);
    const int removeCount = qMin(lastRow - firstRow + 1, rows - 1);
    for (int cell = 0; cell < removeCount * block.columns; ++cell)
        block.cells.removeAt(firstRow * block.columns);
    changed(row, { CellsRole });
}

void NoteBlockModel::insertTableColumn(int row, int column)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto     &block = blocks_[row];
    const int rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    column          = qBound(0, column, block.columns);
    for (int tableRow = rows - 1; tableRow >= 0; --tableRow)
        block.cells.insert(tableRow * block.columns + column, QString());
    ++block.columns;
    changed(row, { CellsRole });
}

void NoteBlockModel::removeTableColumn(int row, int column)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto &block = blocks_[row];
    if (block.columns <= 1 || column < 0 || column >= block.columns)
        return;
    const int rows = block.cells.size() / block.columns;
    for (int tableRow = rows - 1; tableRow >= 0; --tableRow)
        block.cells.removeAt(tableRow * block.columns + column);
    --block.columns;
    changed(row, { CellsRole });
}

void NoteBlockModel::setImageUrl(int row, const QString &url) { setData(index(row), url, UrlRole); }
void NoteBlockModel::setImageAlt(int row, const QString &alt) { setData(index(row), alt, AltRole); }

void NoteBlockModel::appendTextBlock()
{
    const int row = blocks_.size();
    beginInsertRows({}, row, row);
    blocks_.append(Block {});
    endInsertRows();
    emit contentsChanged();
}

void NoteBlockModel::appendText(const QString &text)
{
    if (!blocks_.isEmpty() && blocks_.last().type == Text) {
        auto &value = blocks_.last().text;
        if (!value.isEmpty() && !value.back().isSpace())
            value += QLatin1Char(' ');
        value += text;
        changed(blocks_.size() - 1, { TextRole });
        return;
    }
    const int row = blocks_.size();
    beginInsertRows({}, row, row);
    Block block;
    block.text = text;
    blocks_.append(block);
    endInsertRows();
    emit contentsChanged();
}

void NoteBlockModel::appendImage(const QString &url, const QString &alt) { insertImage(blocks_.size(), url, alt); }

void NoteBlockModel::insertImage(int row, const QString &url, const QString &alt)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type = Image;
    block.url  = url;
    block.alt  = alt;
    blocks_.insert(row, block);
    endInsertRows();
    emit contentsChanged();
}

int NoteBlockModel::blockTypeAt(int row) const
{
    return row >= 0 && row < blocks_.size() ? int(blocks_.at(row).type) : -1;
}

void NoteBlockModel::insertTable(int row)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type    = Table;
    block.columns = 2;
    block.cells   = { QString(), QString(), QString(), QString() };
    blocks_.insert(row, block);
    endInsertRows();
    emit contentsChanged();
}

void NoteBlockModel::insertList(int row, BlockType type)
{
    if (!isListType(type))
        return;
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type      = type;
    block.items     = { QString() };
    block.indents   = { 0 };
    block.itemTypes = { type };
    block.checked   = { false };
    blocks_.insert(row, block);
    endInsertRows();
    emit contentsChanged();
}

bool NoteBlockModel::convertListLevel(int row, int item, BlockType type)
{
    if (row < 0 || row >= blocks_.size() || item < 0 || !isListType(type))
        return false;
    auto &block = blocks_[row];
    if (!isListType(block.type) || item >= block.items.size())
        return false;
    while (block.itemTypes.size() < block.items.size())
        block.itemTypes.append(block.type);
    while (block.checked.size() < block.items.size())
        block.checked.append(false);
    const int level = block.indents.value(item).toInt();
    int       begin = item;
    while (begin > 0 && block.indents.value(begin - 1).toInt() >= level)
        --begin;
    int end = item + 1;
    while (end < block.items.size() && block.indents.value(end).toInt() >= level)
        ++end;
    for (int i = begin; i < end; ++i)
        if (block.indents.value(i).toInt() == level)
            block.itemTypes[i] = type;
    changed(row, { ItemTypesRole, CheckedRole });
    return true;
}

int NoteBlockModel::convertTextBlockToHeading(int row, int position, int level)
{
    if (row < 0 || row >= blocks_.size() || level < 0 || level > 6)
        return -1;
    if (blocks_[row].type == Heading) {
        blocks_[row].type         = level == 0 ? Text : Heading;
        blocks_[row].headingLevel = level;
        changed(row, { TypeRole, HeadingLevelRole });
        return row;
    }
    if (blocks_[row].type != Text || level == 0)
        return -1;

    const QString text        = blocks_[row].text;
    position                  = qBound(0, position, text.size());
    const int separatorBefore = text.lastIndexOf(QStringLiteral("\n\n"), qMax(0, position - 1));
    const int paragraphStart  = separatorBefore < 0 ? 0 : separatorBefore + 2;
    const int separatorAfter  = text.indexOf(QStringLiteral("\n\n"), position);
    const int paragraphEnd    = separatorAfter < 0 ? text.size() : separatorAfter;

    QList<Block> replacement;
    if (paragraphStart > 0) {
        Block before;
        before.text = text.left(paragraphStart - 2);
        replacement.append(before);
    }
    Block heading;
    heading.type            = Heading;
    heading.text            = text.mid(paragraphStart, paragraphEnd - paragraphStart);
    heading.headingLevel    = level;
    const int headingOffset = replacement.size();
    replacement.append(heading);
    if (paragraphEnd < text.size()) {
        Block after;
        after.text = text.mid(paragraphEnd + 2);
        replacement.append(after);
    }

    beginResetModel();
    blocks_.removeAt(row);
    for (int i = 0; i < replacement.size(); ++i)
        blocks_.insert(row + i, replacement[i]);
    endResetModel();
    emit contentsChanged();
    return row + headingOffset;
}

void NoteBlockModel::removeBlock(int row)
{
    if (row < 0 || row >= blocks_.size())
        return;
    beginRemoveRows({}, row, row);
    blocks_.removeAt(row);
    endRemoveRows();
    emit contentsChanged();
}

void NoteBlockModel::setPreviewUrls(const QHash<QString, QString> &urls)
{
    previewUrls_ = urls;
    if (!blocks_.isEmpty())
        emit dataChanged(index(0), index(blocks_.size() - 1), { PreviewUrlRole });
}

NoteFragment NoteBlockModel::extractBlockFragment(int firstRow, int lastRow) const
{
    NoteFragment fragment;
    fragment.sourceFormat = markdown_ ? NoteFragmentSourceFormat::Markdown : NoteFragmentSourceFormat::PlainText;
    if (blocks_.isEmpty() || lastRow < 0 || firstRow >= blocks_.size())
        return fragment;

    firstRow = qBound(0, firstRow, blocks_.size() - 1);
    lastRow  = qBound(firstRow, lastRow, blocks_.size() - 1);
    for (int row = firstRow; row <= lastRow; ++row) {
        const Block      &source = blocks_.at(row);
        NoteFragmentBlock destination;
        switch (source.type) {
        case Text:
            destination.type     = NoteFragmentBlockType::Text;
            destination.markdown = source.text;
            break;
        case Heading:
            destination.type         = NoteFragmentBlockType::Heading;
            destination.markdown     = source.text;
            destination.headingLevel = source.headingLevel;
            break;
        case BulletList:
        case CheckList:
        case NumberedList:
            destination.type = NoteFragmentBlockType::List;
            for (int item = 0; item < source.items.size(); ++item) {
                NoteFragmentListItem value;
                value.markdown = source.items.at(item);
                value.indent   = qMax(0, source.indents.value(item).toInt());
                value.kind     = fragmentListKind(BlockType(source.itemTypes.value(item, source.type).toInt()));
                value.checked  = source.checked.value(item).toBool();
                destination.listItems.append(value);
            }
            break;
        case Table:
            destination.type                = NoteFragmentBlockType::Table;
            destination.table.columns       = source.columns;
            destination.table.rows          = source.columns > 0 ? source.cells.size() / source.columns : 0;
            destination.table.headerRows    = destination.table.rows > 0 ? 1 : 0;
            destination.table.markdownCells = source.cells;
            break;
        case Image:
            destination.type            = NoteFragmentBlockType::Image;
            destination.image.sourceUri = source.url;
            destination.image.alt       = source.alt;
            break;
        }
        fragment.blocks.append(destination);
    }
    return fragment;
}

NoteFragment NoteBlockModel::extractSelectionFragment(const QList<NoteBlockSelectionRange> &ranges) const
{
    NoteFragment fragment;
    fragment.sourceFormat = markdown_ ? NoteFragmentSourceFormat::Markdown : NoteFragmentSourceFormat::PlainText;
    if (ranges.isEmpty())
        return fragment;

    for (int first = 0; first < ranges.size();) {
        const int row  = ranges.at(first).blockIndex;
        int       last = first + 1;
        while (last < ranges.size() && ranges.at(last).blockIndex == row)
            ++last;
        if (row < 0 || row >= blocks_.size()) {
            first = last;
            continue;
        }

        const Block       &source = blocks_.at(row);
        const NoteFragment exact  = extractBlockFragment(row, row);
        if (exact.blocks.isEmpty()) {
            first = last;
            continue;
        }
        NoteFragmentBlock block = exact.blocks.constFirst();

        if (source.type == Text || source.type == Heading) {
            block.markdown = ranges.at(first).markdown;
            if (source.type == Heading && !ranges.at(first).wholeEditor) {
                block.type         = NoteFragmentBlockType::Text;
                block.headingLevel = 0;
            }
            fragment.blocks.append(block);
        } else if (isListType(source.type)) {
            block.listItems.clear();
            int baseIndent = std::numeric_limits<int>::max();
            for (int index = first; index < last; ++index) {
                const auto &range = ranges.at(index);
                if (range.listItemIndex < 0 || range.listItemIndex >= source.items.size())
                    continue;
                NoteFragmentListItem item = exact.blocks.constFirst().listItems.at(range.listItemIndex);
                item.markdown             = range.wholeEditor ? source.items.at(range.listItemIndex) : range.markdown;
                baseIndent                = qMin(baseIndent, item.indent);
                block.listItems.append(item);
            }
            if (!block.listItems.isEmpty()) {
                for (auto &item : block.listItems)
                    item.indent -= baseIndent;
                fragment.blocks.append(block);
            }
        } else if (source.type == Table && source.columns > 0) {
            QHash<int, NoteBlockSelectionRange> selected;
            int                                 minRow    = std::numeric_limits<int>::max();
            int                                 maxRow    = -1;
            int                                 minColumn = std::numeric_limits<int>::max();
            int                                 maxColumn = -1;
            for (int index = first; index < last; ++index) {
                const auto &range = ranges.at(index);
                if (range.tableCellIndex < 0 || range.tableCellIndex >= source.cells.size())
                    continue;
                selected.insert(range.tableCellIndex, range);
                minRow    = qMin(minRow, range.tableCellIndex / source.columns);
                maxRow    = qMax(maxRow, range.tableCellIndex / source.columns);
                minColumn = qMin(minColumn, range.tableCellIndex % source.columns);
                maxColumn = qMax(maxColumn, range.tableCellIndex % source.columns);
            }
            if (!selected.isEmpty()) {
                block.table.rows       = maxRow - minRow + 1;
                block.table.columns    = maxColumn - minColumn + 1;
                block.table.headerRows = minRow == 0 ? 1 : 0;
                block.table.markdownCells.clear();
                for (int tableRow = minRow; tableRow <= maxRow; ++tableRow) {
                    for (int column = minColumn; column <= maxColumn; ++column) {
                        const int  cell  = tableRow * source.columns + column;
                        const auto range = selected.value(cell);
                        block.table.markdownCells.append(range.wholeEditor ? source.cells.value(cell) : range.markdown);
                    }
                }
                fragment.blocks.append(block);
            }
        } else if (source.type == Image) {
            const bool wholeImage = std::all_of(ranges.cbegin() + first, ranges.cbegin() + last,
                                                [](const auto &range) { return range.wholeEditor; });
            if (wholeImage) {
                fragment.blocks.append(block);
            } else {
                NoteFragmentBlock text;
                text.type = NoteFragmentBlockType::Text;
                QStringList parts;
                for (int index = first; index < last; ++index)
                    if (!ranges.at(index).markdown.isEmpty())
                        parts.append(ranges.at(index).markdown);
                text.markdown = parts.join(QLatin1Char('\n'));
                if (!text.markdown.isEmpty())
                    fragment.blocks.append(text);
            }
        }
        first = last;
    }
    return fragment;
}

int NoteBlockModel::removeSelectionRanges(const QList<NoteBlockSelectionRange> &ranges)
{
    if (ranges.isEmpty() || blocks_.isEmpty())
        return -1;
    const int firstRow = ranges.constFirst().blockIndex;
    const int lastRow  = ranges.constLast().blockIndex;
    if (firstRow < 0 || lastRow <= firstRow || lastRow >= blocks_.size())
        return -1;

    const auto groupForRow = [&ranges](int row) {
        QList<NoteBlockSelectionRange> group;
        for (const auto &range : ranges)
            if (range.blockIndex == row)
                group.append(range);
        return group;
    };
    const auto sliceList = [](const Block &source, int first, int last) {
        Block result = source;
        result.items.clear();
        result.indents.clear();
        result.itemTypes.clear();
        result.checked.clear();
        for (int item = qMax(0, first); item <= last && item < source.items.size(); ++item) {
            result.items.append(source.items.at(item));
            result.indents.append(source.indents.value(item));
            result.itemTypes.append(source.itemTypes.value(item));
            result.checked.append(source.checked.value(item));
        }
        return result;
    };
    const auto boundaryRemainder
        = [&sliceList](const Block &source, const QList<NoteBlockSelectionRange> &group, bool prefix, Block *result) {
              if (group.isEmpty())
                  return false;
              *result = source;
              if (source.type == Text || source.type == Heading) {
                  result->text = prefix ? group.constFirst().before : group.constLast().after;
                  return !result->text.isEmpty();
              }
              if (isListType(source.type)) {
                  const int boundary = prefix ? group.constFirst().listItemIndex : group.constLast().listItemIndex;
                  if (boundary < 0 || boundary >= source.items.size())
                      return false;
                  *result                    = prefix ? sliceList(source, 0, boundary - 1)
                                                      : sliceList(source, boundary + 1, source.items.size() - 1);
                  const QString boundaryText = prefix ? group.constFirst().before : group.constLast().after;
                  if (!boundaryText.isEmpty()) {
                      const int insertAt = prefix ? result->items.size() : 0;
                      result->items.insert(insertAt, boundaryText);
                      result->indents.insert(insertAt, source.indents.value(boundary));
                      result->itemTypes.insert(insertAt, source.itemTypes.value(boundary));
                      result->checked.insert(insertAt, source.checked.value(boundary));
                  }
                  return !result->items.isEmpty();
              }
              if (source.type == Table) {
                  QSet<int> wholeCells;
                  for (const auto &range : group)
                      if (range.tableCellIndex >= 0 && range.wholeEditor)
                          wholeCells.insert(range.tableCellIndex);
                  if (wholeCells.size() == source.cells.size())
                      return false;
                  for (int index = 0; index < group.size(); ++index) {
                      const auto &range = group.at(index);
                      if (range.tableCellIndex < 0 || range.tableCellIndex >= result->cells.size())
                          continue;
                      result->cells[range.tableCellIndex] = prefix && index == 0 ? range.before
                          : !prefix && index + 1 == group.size()                 ? range.after
                                                                                 : QString();
                  }
                  return true;
              }
              // An image fully crossed by the selection is removed. A selection
              // beginning or ending inside its editable metadata keeps the image.
              return !std::all_of(group.cbegin(), group.cend(), [](const auto &range) { return range.wholeEditor; });
          };

    Block      firstRemainder;
    Block      lastRemainder;
    const bool hasFirst = boundaryRemainder(blocks_.at(firstRow), groupForRow(firstRow), true, &firstRemainder);
    const bool hasLast  = boundaryRemainder(blocks_.at(lastRow), groupForRow(lastRow), false, &lastRemainder);

    QList<Block> replacement;
    if (hasFirst && hasLast && firstRemainder.type == Text && lastRemainder.type == Text) {
        firstRemainder.text += lastRemainder.text;
        replacement.append(firstRemainder);
    } else {
        if (hasFirst)
            replacement.append(firstRemainder);
        if (hasLast)
            replacement.append(lastRemainder);
    }
    const bool removesEntireDocument = firstRow == 0 && lastRow == blocks_.size() - 1;
    if (replacement.isEmpty() && removesEntireDocument)
        replacement.append(Block {});

    beginResetModel();
    for (int row = lastRow; row >= firstRow; --row)
        blocks_.removeAt(row);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(firstRow + index, replacement.at(index));
    endResetModel();
    emit contentsChanged();
    return qMin(firstRow, blocks_.size() - 1);
}

bool NoteBlockModel::blocksFromFragment(const NoteFragment &fragment, QList<Block> *blocks, QString *error)
{
    if (fragment.kind != NoteFragmentKind::BlockSequence) {
        if (error)
            *error = QStringLiteral("fragment is not a block sequence");
        return false;
    }

    blocks->clear();
    blocks->reserve(fragment.blocks.size());
    for (const NoteFragmentBlock &source : fragment.blocks) {
        Block destination;
        switch (source.type) {
        case NoteFragmentBlockType::Text:
            destination.type = Text;
            destination.text = source.markdown;
            break;
        case NoteFragmentBlockType::Heading:
            if (source.headingLevel < 1 || source.headingLevel > 6) {
                if (error)
                    *error = QStringLiteral("heading has invalid level");
                return false;
            }
            destination.type         = Heading;
            destination.text         = source.markdown;
            destination.headingLevel = source.headingLevel;
            break;
        case NoteFragmentBlockType::List:
            if (source.listItems.isEmpty()) {
                if (error)
                    *error = QStringLiteral("list fragment has no items");
                return false;
            }
            destination.type = modelListKind(source.listItems.constFirst().kind);
            for (const NoteFragmentListItem &item : source.listItems) {
                if (item.indent < 0 || item.indent > 128) {
                    if (error)
                        *error = QStringLiteral("list item has invalid indentation");
                    return false;
                }
                destination.items.append(item.markdown);
                destination.indents.append(item.indent);
                destination.itemTypes.append(modelListKind(item.kind));
                destination.checked.append(item.checked);
            }
            break;
        case NoteFragmentBlockType::Table:
            if (source.table.rows < 1 || source.table.columns < 1
                || qint64(source.table.rows) * source.table.columns != source.table.markdownCells.size()) {
                if (error)
                    *error = QStringLiteral("table fragment has invalid geometry");
                return false;
            }
            destination.type    = Table;
            destination.columns = source.table.columns;
            destination.cells   = source.table.markdownCells;
            break;
        case NoteFragmentBlockType::Image:
            if (source.image.sourceUri.isEmpty()) {
                if (error)
                    *error = QStringLiteral("image fragment has no source URI");
                return false;
            }
            destination.type = Image;
            destination.url  = source.image.sourceUri;
            destination.alt  = source.image.alt;
            break;
        }
        blocks->append(destination);
    }
    return true;
}

bool NoteBlockModel::insertBlockFragment(int row, const NoteFragment &fragment, QString *error)
{
    QList<Block> replacement;
    if (!blocksFromFragment(fragment, &replacement, error))
        return false;

    if (replacement.isEmpty())
        return true;
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row + replacement.size() - 1);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(row + index, replacement.at(index));
    endInsertRows();
    emit contentsChanged();
    return true;
}

int NoteBlockModel::replaceTextBlockRangeWithFragment(int row, const QString &before, const QString &after,
                                                      const NoteFragment &fragment, QString *error)
{
    if (row < 0 || row >= blocks_.size() || (blocks_.at(row).type != Text && blocks_.at(row).type != Heading)) {
        if (error)
            *error = QStringLiteral("target is not a text block");
        return -1;
    }

    QList<Block> replacement;
    if (!blocksFromFragment(fragment, &replacement, error))
        return -1;

    const Block original  = blocks_.at(row);
    const auto  textBlock = [&original](const QString &text) {
        Block block;
        block.type         = original.type;
        block.text         = text.trimmed();
        block.headingLevel = original.headingLevel;
        return block;
    };
    int insertedRow = row;
    if (!before.isEmpty()) {
        replacement.prepend(textBlock(before));
        ++insertedRow;
    }
    if (!after.isEmpty())
        replacement.append(textBlock(after));
    if (replacement.isEmpty())
        replacement.append(textBlock(QString()));

    beginResetModel();
    blocks_.removeAt(row);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(row + index, replacement.at(index));
    endResetModel();
    emit contentsChanged();
    return insertedRow;
}

bool NoteBlockModel::replaceTableCellsWithFragment(int row, int firstCell, const NoteFragment &fragment, QString *error)
{
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != Table) {
        if (error)
            *error = QStringLiteral("target is not a table");
        return false;
    }
    if (fragment.kind != NoteFragmentKind::BlockSequence || fragment.blocks.size() != 1
        || fragment.blocks.constFirst().type != NoteFragmentBlockType::Table) {
        if (error)
            *error = QStringLiteral("fragment is not a single table");
        return false;
    }
    const NoteFragmentTable &source = fragment.blocks.constFirst().table;
    if (source.rows < 1 || source.columns < 1 || qint64(source.rows) * source.columns != source.markdownCells.size()) {
        if (error)
            *error = QStringLiteral("table fragment has invalid geometry");
        return false;
    }

    Block    &destination = blocks_[row];
    const int oldColumns  = destination.columns;
    const int oldRows     = oldColumns > 0 ? destination.cells.size() / oldColumns : 0;
    if (oldColumns < 1 || oldRows < 1 || firstCell < 0 || firstCell >= oldRows * oldColumns) {
        if (error)
            *error = QStringLiteral("target table cell is invalid");
        return false;
    }
    const int targetRow    = firstCell / oldColumns;
    const int targetColumn = firstCell % oldColumns;
    const int newColumns   = qMax(oldColumns, targetColumn + source.columns);
    const int newRows      = qMax(oldRows, targetRow + source.rows);
    if (qint64(newRows) * newColumns > 100000) {
        if (error)
            *error = QStringLiteral("expanded table is too large");
        return false;
    }

    QStringList cells(newRows * newColumns);
    for (int sourceRow = 0; sourceRow < oldRows; ++sourceRow)
        for (int column = 0; column < oldColumns; ++column)
            cells[sourceRow * newColumns + column] = destination.cells.at(sourceRow * oldColumns + column);
    for (int sourceRow = 0; sourceRow < source.rows; ++sourceRow)
        for (int column = 0; column < source.columns; ++column)
            cells[(targetRow + sourceRow) * newColumns + targetColumn + column]
                = source.markdownCells.at(sourceRow * source.columns + column);

    destination.columns = newColumns;
    destination.cells   = cells;
    changed(row, { CellsRole });
    return true;
}

int NoteBlockModel::replaceListItemRangeWithFragment(int row, int item, const QString &before, const QString &after,
                                                     const NoteFragment &fragment, QString *error)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_.at(row).type) || item < 0
        || item >= blocks_.at(row).items.size()) {
        if (error)
            *error = QStringLiteral("target is not a list item");
        return -1;
    }
    if (fragment.kind != NoteFragmentKind::BlockSequence || fragment.blocks.size() != 1
        || fragment.blocks.constFirst().type != NoteFragmentBlockType::List
        || fragment.blocks.constFirst().listItems.isEmpty()) {
        if (error)
            *error = QStringLiteral("fragment is not a single non-empty list");
        return -1;
    }

    Block    &destination  = blocks_[row];
    const int targetIndent = destination.indents.value(item).toInt();
    if (item + 1 < destination.items.size() && destination.indents.value(item + 1).toInt() > targetIndent) {
        if (error)
            *error = QStringLiteral("target list item has nested descendants");
        return -1;
    }

    struct ReplacementItem {
        QString   text;
        int       indent;
        BlockType type;
        bool      checked;
    };
    QList<ReplacementItem> replacement;
    const auto             appendOriginal = [&](const QString &text) {
        if (!text.trimmed().isEmpty()) {
            replacement.append({ text.trimmed(), targetIndent,
                                 BlockType(destination.itemTypes.value(item, destination.type).toInt()),
                                 destination.checked.value(item).toBool() });
        }
    };
    appendOriginal(before);

    const auto &sourceItems      = fragment.blocks.constFirst().listItems;
    const int   sourceBaseIndent = sourceItems.constFirst().indent;
    for (const NoteFragmentListItem &source : sourceItems) {
        if (source.indent < sourceBaseIndent || source.indent - sourceBaseIndent > 128) {
            if (error)
                *error = QStringLiteral("fragment has invalid list indentation");
            return -1;
        }
        replacement.append({ source.markdown, targetIndent + source.indent - sourceBaseIndent,
                             modelListKind(source.kind), source.checked });
    }
    appendOriginal(after);

    const int focusItem = item + (!before.trimmed().isEmpty() ? 1 : 0);
    destination.items.removeAt(item);
    destination.indents.removeAt(item);
    destination.itemTypes.removeAt(item);
    destination.checked.removeAt(item);
    for (int index = 0; index < replacement.size(); ++index) {
        const ReplacementItem &value = replacement.at(index);
        destination.items.insert(item + index, value.text);
        destination.indents.insert(item + index, value.indent);
        destination.itemTypes.insert(item + index, value.type);
        destination.checked.insert(item + index, value.checked);
    }
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
    return focusItem;
}

QList<NoteBlockModel::Block> NoteBlockModel::parseMarkdown(const QString &source)
{
    // QTextDocument is the Markdown reader. Parsing its canonical Markdown keeps
    // Qt's CommonMark interpretation as the single source of truth.
    QTextDocument                   document;
    QString                         protectedSource = source;
    static const QRegularExpression lineBreak(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression trailingListBreaks(QStringLiteral("(?:<br\\s*/?>[ \\t]*)+(?=\\r?\\n|$)"),
                                                       QRegularExpression::CaseInsensitiveOption);
    protectedSource.replace(trailingListBreaks, QString());
    protectedSource.replace(lineBreak, TableLineBreakMarker);
    static const QRegularExpression task(QStringLiteral(R"(^(\s*)[-*+]\s+\[([ xX])\]\s+(.*)$)"));
    static const QRegularExpression bullet(QStringLiteral(R"(^(\s*)[-*+]\s+(.*)$)"));
    static const QRegularExpression numbered(QStringLiteral(R"(^(\s*)\d+[.)]\s+(.*)$)"));
    QList<int>                      sourceListIndents;
    for (const auto &line : protectedSource.split('\n')) {
        const auto taskItem     = task.match(line);
        const auto bulletItem   = bullet.match(line);
        const auto numberedItem = numbered.match(line);
        if (taskItem.hasMatch())
            sourceListIndents.append(taskItem.capturedLength(1));
        else if (bulletItem.hasMatch())
            sourceListIndents.append(bulletItem.capturedLength(1));
        else if (numberedItem.hasMatch())
            sourceListIndents.append(numberedItem.capturedLength(1));
    }
    document.setMarkdown(protectedSource, QTextDocument::MarkdownDialectGitHub);
    const QStringList sourceLines    = protectedSource.split('\n');
    const QStringList canonicalLines = document.toMarkdown(QTextDocument::MarkdownDialectGitHub).split('\n');
    const auto        hasTable       = [](const QStringList &candidate) {
        for (int index = 0; index + 1 < candidate.size(); ++index) {
            if (candidate.at(index).contains('|') && isTableSeparator(candidate.at(index + 1)))
                return true;
        }
        return false;
    };
    static const QRegularExpression inlineLink(
        QStringLiteral(R"((?<!!)\[(?:\\.|[^\]\\\n])*\]\((?:\\.|[^)\\\n])*\)|<https?://[^>\n]+>)"));
    static const QRegularExpression inlineUnderline(QStringLiteral(R"(<(?:ins|u)(?:\s[^>]*)?>[\s\S]*?</(?:ins|u)\s*>)"),
                                                    QRegularExpression::CaseInsensitiveOption);
    const bool                      preserveInlineSourceLines
        = inlineLink.match(protectedSource).hasMatch() || inlineUnderline.match(protectedSource).hasMatch();
    // QTextDocument remains the Markdown reader for inline semantics, but its
    // writer wraps long paragraphs (very often around a link). Such a soft
    // wrap becomes a real newline in plain-text mode and can split a list.
    // Preserve source line boundaries for linked content. Do the same when
    // Qt flattens a GFM table following a task list.
    const QStringList &lines = preserveInlineSourceLines || (hasTable(sourceLines) && !hasTable(canonicalLines))
        ? sourceLines
        : canonicalLines;
    QList<Block>       result;
    static const QRegularExpression image(QStringLiteral(R"(^\s*!\[([^\]]*)\]\((\S+?)(?:\s+"[^"]*")?\)\s*$)"));
    static const QRegularExpression heading(QStringLiteral(R"(^\s*(#{1,6})\s+(.+?)\s*#*\s*$)"));
    int                             canonicalListItems = 0;
    for (const auto &line : lines)
        if (task.match(line).hasMatch() || bullet.match(line).hasMatch() || numbered.match(line).hasMatch())
            ++canonicalListItems;
    const bool preserveSourceListIndents = canonicalListItems == sourceListIndents.size();
    int        sourceListItem            = 0;

    for (int i = 0; i < lines.size();) {
        if (lines[i].trimmed().isEmpty()) {
            ++i;
            continue;
        }
        const auto headingMatch = heading.match(lines[i]);
        if (headingMatch.hasMatch()) {
            Block block;
            block.type         = Heading;
            block.text         = headingMatch.captured(2);
            block.headingLevel = headingMatch.capturedLength(1);
            result.append(block);
            ++i;
            continue;
        }
        auto       match         = task.match(lines[i]);
        const auto bulletMatch   = bullet.match(lines[i]);
        const auto numberedMatch = numbered.match(lines[i]);
        if (match.hasMatch() || bulletMatch.hasMatch() || numberedMatch.hasMatch()) {
            Block      block;
            QList<int> indentColumns;
            while (i < lines.size()) {
                if (lines[i].trimmed().isEmpty()) {
                    int next = i + 1;
                    while (next < lines.size() && lines[next].trimmed().isEmpty())
                        ++next;
                    if (next >= lines.size())
                        break;
                    const auto    nextTask     = task.match(lines[next]);
                    const auto    nextBullet   = bullet.match(lines[next]);
                    const auto    nextNumbered = numbered.match(lines[next]);
                    const QString nextIndent   = nextTask.hasMatch() ? nextTask.captured(1)
                          : nextBullet.hasMatch()                    ? nextBullet.captured(1)
                          : nextNumbered.hasMatch()                  ? nextNumbered.captured(1)
                                                                     : QString();
                    if (nextIndent.isEmpty())
                        break;
                    i = next;
                }
                const auto      taskItem     = task.match(lines[i]);
                const auto      bulletItem   = bullet.match(lines[i]);
                const auto      numberedItem = numbered.match(lines[i]);
                BlockType       itemType;
                const BlockType candidateType     = taskItem.hasMatch() ? CheckList
                        : numberedItem.hasMatch()                       ? NumberedList
                                                                        : BulletList;
                const QString   candidateIndent   = taskItem.hasMatch() ? taskItem.captured(1)
                        : bulletItem.hasMatch()                         ? bulletItem.captured(1)
                        : numberedItem.hasMatch()                       ? numberedItem.captured(1)
                                                                        : QString();
                const int       rawIndent         = preserveSourceListIndents
                                  ? sourceListIndents.value(sourceListItem, candidateIndent.size())
                                  : candidateIndent.size();
                const bool returnsFromNestedLevel = !block.indents.isEmpty() && block.indents.constLast().toInt() > 0;
                if (!block.itemTypes.isEmpty() && rawIndent == 0 && !returnsFromNestedLevel
                    && candidateType != BlockType(block.itemTypes.constLast().toInt()))
                    break;
                ++sourceListItem;
                while (!indentColumns.isEmpty() && indentColumns.constLast() > rawIndent)
                    indentColumns.removeLast();
                if (indentColumns.isEmpty() || indentColumns.constLast() < rawIndent)
                    indentColumns.append(rawIndent);
                const int level = indentColumns.size() - 1;
                if (taskItem.hasMatch()) {
                    itemType = CheckList;
                    block.indents.append(level);
                    block.checked.append(taskItem.captured(2).compare(QStringLiteral("x"), Qt::CaseInsensitive) == 0);
                    block.items.append(decodeListItem(taskItem.captured(3)));
                } else if (bulletItem.hasMatch()) {
                    itemType = BulletList;
                    block.indents.append(level);
                    block.checked.append(false);
                    block.items.append(decodeListItem(bulletItem.captured(2)));
                } else if (numberedItem.hasMatch()) {
                    itemType = NumberedList;
                    block.indents.append(level);
                    block.checked.append(false);
                    block.items.append(decodeListItem(numberedItem.captured(2)));
                } else {
                    break;
                }
                if (block.itemTypes.isEmpty())
                    block.type = itemType;
                block.itemTypes.append(itemType);
                ++i;
            }
            result.append(block);
            continue;
        }
        if (i + 1 < lines.size() && lines[i].contains('|') && isTableSeparator(lines[i + 1])) {
            Block block;
            block.type    = Table;
            auto header   = tableCells(lines[i]);
            block.columns = header.size();
            block.cells.append(header);
            i += 2;
            while (i < lines.size() && lines[i].contains('|') && !lines[i].trimmed().isEmpty()) {
                auto row = tableCells(lines[i++]);
                while (row.size() < block.columns)
                    row.append(QString());
                block.cells.append(row.mid(0, block.columns));
            }
            result.append(block);
            continue;
        }
        match = image.match(lines[i]);
        if (match.hasMatch()) {
            Block block;
            block.type = Image;
            block.alt  = match.captured(1);
            block.url  = match.captured(2);
            result.append(block);
            ++i;
            continue;
        }
        QStringList paragraph;
        while (i < lines.size() && !lines[i].trimmed().isEmpty() && !task.match(lines[i]).hasMatch()
               && !bullet.match(lines[i]).hasMatch() && !numbered.match(lines[i]).hasMatch()
               && !image.match(lines[i]).hasMatch() && !heading.match(lines[i]).hasMatch()
               && !(i + 1 < lines.size() && lines[i].contains('|') && isTableSeparator(lines[i + 1])))
            paragraph.append(lines[i++]);
        const QString text = paragraph.join('\n');
        if (!result.isEmpty() && result.constLast().type == Text) {
            if (!result.last().text.isEmpty() && !text.isEmpty())
                result.last().text += QStringLiteral("\n\n");
            result.last().text += text;
        } else {
            Block block;
            block.type = Text;
            block.text = text;
            result.append(block);
        }
    }
    if (result.isEmpty())
        result.append(Block {});
    return result;
}

QString NoteBlockModel::writeMarkdown(const QList<Block> &blocks)
{
    QStringList output;
    for (const auto &block : blocks) {
        QString value;
        switch (block.type) {
        case Text:
            value = block.text;
            break;
        case Heading:
            value = QString(qBound(1, block.headingLevel, 6), QLatin1Char('#')) + QLatin1Char(' ') + block.text;
            break;
        case BulletList:
        case CheckList:
        case NumberedList:
            for (int i = 0; i < block.items.size(); ++i) {
                const auto type    = BlockType(block.itemTypes.value(i, block.type).toInt());
                const int  columns = qMax(0, block.indents.value(i).toInt()) * 4;
                value += QString(columns, QLatin1Char(' '));
                if (type == CheckList)
                    value += QStringLiteral("- [%1] %2\n")
                                 .arg(block.checked.value(i).toBool() ? "x" : " ", encodeListItem(block.items[i]));
                else if (type == NumberedList) {
                    const int level  = block.indents.value(i).toInt();
                    int       number = 1;
                    for (int previous = i - 1; previous >= 0; --previous) {
                        const int previousLevel = block.indents.value(previous).toInt();
                        if (previousLevel < level)
                            break;
                        const auto previousType = BlockType(block.itemTypes.value(previous, block.type).toInt());
                        if (previousLevel == level && previousType == NumberedList)
                            ++number;
                    }
                    value += QStringLiteral("%1. %2\n").arg(number).arg(encodeListItem(block.items[i]));
                } else
                    value += QStringLiteral("- %1\n").arg(encodeListItem(block.items[i]));
            }
            value.chop(value.endsWith('\n') ? 1 : 0);
            break;
        case Table:
            for (int row = 0; row * block.columns < block.cells.size(); ++row) {
                value += QLatin1String("| ");
                for (int col = 0; col < block.columns; ++col) {
                    auto cell = block.cells.value(row * block.columns + col);
                    cell.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
                    value += cell + QLatin1String(" | ");
                }
                value.chop(1);
                value += QLatin1Char('\n');
                if (row == 0) {
                    value += QLatin1String("|");
                    for (int col = 0; col < block.columns; ++col)
                        value += QLatin1String(" --- |");
                    value += QLatin1Char('\n');
                }
            }
            value = value.trimmed();
            break;
        case Image:
            value = QStringLiteral("![%1](%2)").arg(block.alt, block.url);
            break;
        }
        output.append(value);
    }
    return output.join(QStringLiteral("\n\n"));
}

void NoteBlockModel::changed(int row, const QList<int> &roles)
{
    emit dataChanged(index(row), index(row), roles);
    emit contentsChanged();
}
} // namespace QtNote
