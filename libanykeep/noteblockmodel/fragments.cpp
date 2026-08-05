#include "../noteblockmodel.h"
#include "../notetagline.h"
#include "private.h"

#include <QSet>

#include <algorithm>

namespace AnyKeep {
using namespace NoteBlockModelPrivate;
namespace {
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
        case BlockQuote:
            destination.type     = NoteFragmentBlockType::BlockQuote;
            destination.markdown = source.text;
            break;
        case CodeBlock:
            destination.type     = NoteFragmentBlockType::CodeBlock;
            destination.markdown = source.text;
            destination.language = source.language;
            break;
        case TagLine:
            destination.type = NoteFragmentBlockType::TagLine;
            destination.tags = source.tags;
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
            destination.image.width     = source.imageWidth;
            destination.image.alignment = source.imageAlignment;
            break;
        case Audio:
            destination.type             = NoteFragmentBlockType::Audio;
            destination.audio.sourceUri  = source.url;
            destination.audio.title      = source.alt;
            destination.audio.durationMs = source.audioDurationMs;
            destination.audio.transcript = source.audioTranscript;
            break;
        case Attachment:
            destination.type                 = NoteFragmentBlockType::Attachment;
            destination.attachment.sourceUri = source.url;
            destination.attachment.fileName  = source.alt;
            destination.attachment.mediaType = source.attachmentMediaType;
            destination.attachment.size      = source.attachmentSize;
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

    int previousRow = -1;
    for (int first = 0; first < ranges.size();) {
        const int row  = ranges.at(first).blockIndex;
        int       last = first + 1;
        while (last < ranges.size() && ranges.at(last).blockIndex == row)
            ++last;
        if (row < 0 || row >= blocks_.size()) {
            first = last;
            continue;
        }

        // Text editors are the visible selection endpoints, but structural
        // media blocks and tag lines have no TextArea of their own.
        // Every block strictly between two ranged rows is nevertheless fully
        // crossed by the document selection and must be present on the
        // clipboard, just as it is removed by removeSelectionRanges().
        if (previousRow >= 0 && row > previousRow + 1) {
            const NoteFragment crossed = extractBlockFragment(previousRow + 1, row - 1);
            fragment.blocks.append(crossed.blocks);
        }
        previousRow = row;

        const Block       &source = blocks_.at(row);
        const NoteFragment exact  = extractBlockFragment(row, row);
        if (exact.blocks.isEmpty()) {
            first = last;
            continue;
        }
        NoteFragmentBlock block = exact.blocks.constFirst();

        if (source.type == Text || source.type == Heading || source.type == BlockQuote || source.type == CodeBlock) {
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
        } else if (source.type == Image || source.type == Audio || source.type == Attachment
                   || source.type == TagLine) {
            const bool wholeBlock = std::all_of(ranges.cbegin() + first, ranges.cbegin() + last,
                                                [](const auto &range) { return range.wholeEditor; });
            if (wholeBlock) {
                fragment.blocks.append(block);
            } else if (source.type == Image || source.type == Audio || source.type == Attachment) {
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
    const auto boundaryRemainder = [&sliceList](const Block &source, const QList<NoteBlockSelectionRange> &group,
                                                bool prefix, Block *result) {
        if (group.isEmpty())
            return false;
        *result = source;
        if (source.type == Text || source.type == Heading || source.type == BlockQuote || source.type == CodeBlock) {
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
        // A structural block fully crossed by the selection is removed. A
        // selection beginning or ending inside editable image metadata keeps it.
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
    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();
    return qMin(firstRow, blocks_.size() - 1);
}

int NoteBlockModel::replaceTextSelectionWithCodeBlock(const QList<NoteBlockSelectionRange> &ranges,
                                                      const QString &plainText, const QString &language)
{
    if (!markdown_ || ranges.isEmpty())
        return -1;
    const int firstRow = ranges.constFirst().blockIndex;
    const int lastRow  = ranges.constLast().blockIndex;
    if (firstRow <= 0 || lastRow < firstRow || lastRow >= blocks_.size())
        return -1;

    QSet<int> selectedRows;
    for (const NoteBlockSelectionRange &range : ranges) {
        if (range.blockIndex < firstRow || range.blockIndex > lastRow || range.listItemIndex >= 0
            || range.tableCellIndex >= 0 || selectedRows.contains(range.blockIndex)) {
            return -1;
        }
        selectedRows.insert(range.blockIndex);
    }
    for (int row = firstRow; row <= lastRow; ++row) {
        if (!selectedRows.contains(row) || blocks_.at(row).type != Text)
            return -1;
    }

    QString codeText = plainText;
    codeText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    codeText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    codeText.replace(QChar::LineSeparator, QLatin1Char('\n'));
    codeText.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    if (!codeText.contains(QLatin1Char('\n')))
        return -1;

    QList<Block> replacement;
    if (!ranges.constFirst().before.isEmpty()) {
        Block prefix = blocks_.at(firstRow);
        prefix.text  = ranges.constFirst().before;
        replacement.append(prefix);
    }
    const int codeRow = firstRow + replacement.size();
    Block     code;
    code.type     = CodeBlock;
    code.text     = codeText;
    code.language = language.trimmed().toLower();
    replacement.append(code);
    if (!ranges.constLast().after.isEmpty()) {
        Block suffix = blocks_.at(lastRow);
        suffix.text  = ranges.constLast().after;
        replacement.append(suffix);
    }

    beginResetModel();
    for (int row = lastRow; row >= firstRow; --row)
        blocks_.removeAt(row);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(firstRow + index, replacement.at(index));
    normalizeTitleBlock(&blocks_, markdown_);
    normalizeTagLinePositions(&blocks_, markdown_, true);
    endResetModel();
    emit contentsChanged();
    return codeRow;
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
        case NoteFragmentBlockType::BlockQuote:
            destination.type = BlockQuote;
            destination.text = source.markdown;
            break;
        case NoteFragmentBlockType::CodeBlock:
            destination.type     = CodeBlock;
            destination.text     = source.markdown;
            destination.language = source.language.trimmed().toLower();
            break;
        case NoteFragmentBlockType::TagLine:
            if (source.tags.isEmpty() || std::any_of(source.tags.cbegin(), source.tags.cend(), [](const QString &tag) {
                    return !NoteTagLine::isValidTagName(tag);
                })) {
                if (error)
                    *error = QStringLiteral("tag line fragment has invalid tags");
                return false;
            }
            destination.type = TagLine;
            for (const QString &tag : source.tags)
                if (!destination.tags.contains(tag))
                    destination.tags.append(tag);
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
            destination.type           = Image;
            destination.url            = source.image.sourceUri;
            destination.alt            = source.image.alt;
            destination.imageWidth     = qBound(0, source.image.width, MaxSerializedImageWidth);
            destination.imageAlignment = normalizedImageAlignment(source.image.alignment);
            break;
        case NoteFragmentBlockType::Audio:
            if (source.audio.sourceUri.isEmpty() || source.audio.durationMs < 0
                || source.audio.durationMs > MaxAudioDurationMs) {
                if (error)
                    *error = QStringLiteral("audio fragment is invalid");
                return false;
            }
            destination.type            = Audio;
            destination.url             = source.audio.sourceUri;
            destination.alt             = source.audio.title;
            destination.audioDurationMs = source.audio.durationMs;
            destination.audioTranscript = source.audio.transcript;
            break;
        case NoteFragmentBlockType::Attachment:
            if (source.attachment.sourceUri.isEmpty() || source.attachment.fileName.isEmpty()
                || source.attachment.size < 0) {
                if (error)
                    *error = QStringLiteral("attachment fragment is invalid");
                return false;
            }
            destination.type                = Attachment;
            destination.url                 = source.attachment.sourceUri;
            destination.alt                 = source.attachment.fileName;
            destination.attachmentMediaType = source.attachment.mediaType;
            destination.attachmentSize      = source.attachment.size;
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
    notifyNormalizedTagLines(true);
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
    if (row == 0 && replacement.constFirst().type != Text) {
        if (error)
            *error = QStringLiteral("structured fragment cannot replace the title");
        return -1;
    }

    beginResetModel();
    blocks_.removeAt(row);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(row + index, replacement.at(index));
    normalizeTitleBlock(&blocks_, markdown_);
    normalizeTagLinePositions(&blocks_, markdown_, true);
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
} // namespace AnyKeep
