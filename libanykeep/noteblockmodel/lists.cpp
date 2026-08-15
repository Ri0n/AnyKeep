#include "../noteblockmodel.h"
#include "private.h"

#include <optional>

namespace AnyKeep {
using namespace NoteBlockModelPrivate;

void NoteBlockModel::normalizeListStorage(Block *block)
{
    if (!block || !isListType(block->type))
        return;
    while (block->indents.size() < block->items.size())
        block->indents.append(0);
    while (block->itemTypes.size() < block->items.size())
        block->itemTypes.append(block->type);
    while (block->checked.size() < block->items.size())
        block->checked.append(false);
}

void NoteBlockModel::recomputeTaskParentChecks(Block *block)
{
    if (!block || !isListType(block->type))
        return;
    normalizeListStorage(block);
    for (int parent = block->items.size() - 1; parent >= 0; --parent) {
        if (block->itemTypes.at(parent).toInt() != CheckList)
            continue;
        const int parentIndent      = block->indents.at(parent).toInt();
        bool      hasTaskDescendant = false;
        bool      allChecked        = true;
        for (int child = parent + 1; child < block->items.size() && block->indents.at(child).toInt() > parentIndent;
             ++child) {
            if (block->itemTypes.at(child).toInt() != CheckList)
                continue;
            hasTaskDescendant = true;
            allChecked        = allChecked && block->checked.at(child).toBool();
        }
        if (hasTaskDescendant)
            block->checked[parent] = allChecked;
    }
}

void NoteBlockModel::recomputeTaskParentChecks(QList<Block> *blocks)
{
    if (!blocks)
        return;
    for (Block &block : *blocks)
        recomputeTaskParentChecks(&block);
}

void NoteBlockModel::normalizeMovedListTypes(Block *block, int firstItem, int itemCount)
{
    if (!block || !isListType(block->type) || itemCount <= 0)
        return;
    normalizeListStorage(block);
    firstItem          = qBound(0, firstItem, block->items.size());
    const int  endItem = qBound(firstItem, firstItem + itemCount, block->items.size());
    const auto itemType
        = [block](int item) { return static_cast<BlockType>(block->itemTypes.value(item, int(block->type)).toInt()); };

    for (int item = firstItem; item < endItem; ++item) {
        const int                level = block->indents.at(item).toInt();
        std::optional<BlockType> existingType;
        for (int sibling = firstItem - 1; sibling >= 0; --sibling) {
            const int siblingLevel = block->indents.at(sibling).toInt();
            if (siblingLevel < level)
                break;
            if (siblingLevel == level) {
                existingType = itemType(sibling);
                break;
            }
        }
        for (int sibling = endItem; !existingType && sibling < block->items.size(); ++sibling) {
            const int siblingLevel = block->indents.at(sibling).toInt();
            if (siblingLevel < level)
                break;
            if (siblingLevel == level)
                existingType = itemType(sibling);
        }
        if (existingType && isListType(*existingType))
            block->itemTypes[item] = int(*existingType);
    }
}

void NoteBlockModel::mergeListPair(QList<Block> *blocks, int leftRow, bool residentIsLeft, int *trackedRow)
{
    if (!blocks || leftRow < 0 || leftRow + 1 >= blocks->size() || !isListType(blocks->at(leftRow).type)
        || !isListType(blocks->at(leftRow + 1).type)) {
        return;
    }

    Block left  = blocks->at(leftRow);
    Block right = blocks->at(leftRow + 1);
    normalizeListStorage(&left);
    normalizeListStorage(&right);

    Block merged = residentIsLeft ? left : right;
    if (residentIsLeft) {
        const int firstMoved = merged.items.size();
        merged.items.append(right.items);
        merged.indents.append(right.indents);
        merged.itemTypes.append(right.itemTypes);
        merged.checked.append(right.checked);
        normalizeMovedListTypes(&merged, firstMoved, right.items.size());
    } else {
        const int movedCount = left.items.size();
        merged.items         = left.items + merged.items;
        merged.indents       = left.indents + merged.indents;
        merged.itemTypes     = left.itemTypes + merged.itemTypes;
        merged.checked       = left.checked + merged.checked;
        normalizeMovedListTypes(&merged, 0, movedCount);
    }

    (*blocks)[leftRow] = std::move(merged);
    blocks->removeAt(leftRow + 1);
    if (!trackedRow)
        return;
    if (*trackedRow == leftRow || *trackedRow == leftRow + 1)
        *trackedRow = leftRow;
    else if (*trackedRow > leftRow + 1)
        --*trackedRow;
}

void NoteBlockModel::coalesceListAtBoundary(QList<Block> *blocks, int boundary, int *trackedRow)
{
    if (!blocks || blocks->size() < 2)
        return;
    boundary = qBound(0, boundary, blocks->size());
    if (boundary <= 0 || boundary >= blocks->size() || !isListType(blocks->at(boundary - 1).type)
        || !isListType(blocks->at(boundary).type)) {
        return;
    }

    int row = boundary - 1;
    mergeListPair(blocks, row, true, trackedRow);
    while (row + 1 < blocks->size() && isListType(blocks->at(row).type) && isListType(blocks->at(row + 1).type)) {
        mergeListPair(blocks, row, true, trackedRow);
    }
}

void NoteBlockModel::coalesceMovedList(QList<Block> *blocks, int *movedRow)
{
    if (!blocks || !movedRow || *movedRow < 0 || *movedRow >= blocks->size()
        || !isListType(blocks->at(*movedRow).type)) {
        return;
    }

    if (*movedRow > 0 && isListType(blocks->at(*movedRow - 1).type)) {
        mergeListPair(blocks, *movedRow - 1, true, movedRow);
    } else if (*movedRow + 1 < blocks->size() && isListType(blocks->at(*movedRow + 1).type)) {
        mergeListPair(blocks, *movedRow, false, movedRow);
    }

    while (*movedRow > 0 && isListType(blocks->at(*movedRow - 1).type))
        mergeListPair(blocks, *movedRow - 1, false, movedRow);
    while (*movedRow + 1 < blocks->size() && isListType(blocks->at(*movedRow + 1).type))
        mergeListPair(blocks, *movedRow, true, movedRow);
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
    recomputeTaskParentChecks(&block);
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
}

void NoteBlockModel::mergeListItemWithNext(int row, int item)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_[row].type))
        return;
    auto &block = blocks_[row];
    if (item < 0 || item + 1 >= block.items.size())
        return;
    const bool currentWasEmpty = block.items.at(item).isEmpty();
    if (currentWasEmpty) {
        while (block.checked.size() < block.items.size())
            block.checked.append(false);
        block.checked[item] = block.checked.at(item + 1);
    }
    block.items[item] += block.items[item + 1];
    block.items.removeAt(item + 1);
    if (item + 1 < block.indents.size())
        block.indents.removeAt(item + 1);
    if (item + 1 < block.itemTypes.size())
        block.itemTypes.removeAt(item + 1);
    if (item + 1 < block.checked.size())
        block.checked.removeAt(item + 1);
    recomputeTaskParentChecks(&block);
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
}

bool NoteBlockModel::mergeListItemWithFollowingBlock(int row, int item)
{
    if (row < 0 || row + 1 >= blocks_.size() || !isListType(blocks_[row].type)
        || item != blocks_[row].items.size() - 1) {
        return false;
    }

    auto       &current = blocks_[row];
    const Block next    = blocks_.at(row + 1);
    if (next.type == Text) {
        current.items[item] += next.text;
        beginRemoveRows({}, row + 1, row + 1);
        blocks_.removeAt(row + 1);
        endRemoveRows();
        notifyNormalizedTagLines();
        changed(row, { ItemsRole });
        return true;
    }
    if (!isListType(next.type) || next.items.isEmpty())
        return false;

    current.items[item] += next.items.constFirst();
    const int currentIndent  = current.indents.value(item).toInt();
    const int nextBaseIndent = next.indents.value(0).toInt();
    for (int nextItem = 1; nextItem < next.items.size(); ++nextItem) {
        current.items.append(next.items.at(nextItem));
        current.indents.append(qMax(0, currentIndent + next.indents.value(nextItem).toInt() - nextBaseIndent));
        current.itemTypes.append(next.itemTypes.value(nextItem, next.type));
        current.checked.append(next.checked.value(nextItem, false));
    }
    beginRemoveRows({}, row + 1, row + 1);
    blocks_.removeAt(row + 1);
    endRemoveRows();
    notifyNormalizedTagLines();
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
    return true;
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
    recomputeTaskParentChecks(&block);
    changed(row, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
}

bool NoteBlockModel::moveListRange(int sourceRow, int sourceFirstItem, int sourceLastItem, int targetRow,
                                   int targetItem, int targetIndent)
{
    return moveListRangeResolved(sourceRow, sourceFirstItem, sourceLastItem, targetRow, targetItem, targetIndent) >= 0;
}

int NoteBlockModel::moveListRangeResolved(int sourceRow, int sourceFirstItem, int sourceLastItem, int targetRow,
                                          int targetItem, int targetIndent)
{
    if (sourceRow < 0 || sourceRow >= blocks_.size() || targetRow < 0 || targetRow >= blocks_.size()
        || !isListType(blocks_[sourceRow].type) || !isListType(blocks_[targetRow].type)) {
        return -1;
    }

    normalizeListStorage(&blocks_[sourceRow]);
    if (targetRow != sourceRow)
        normalizeListStorage(&blocks_[targetRow]);

    Block &source = blocks_[sourceRow];
    if (sourceFirstItem < 0 || sourceLastItem < sourceFirstItem || sourceLastItem >= source.items.size())
        return -1;

    const int sourceIndent = source.indents.at(sourceFirstItem).toInt();
    const int movedCount   = sourceLastItem - sourceFirstItem + 1;

    const int remainingTargetItems
        = targetRow == sourceRow ? source.items.size() - movedCount : blocks_[targetRow].items.size();
    if (targetItem < 0 || targetItem > remainingTargetItems)
        return -1;

    const QStringList  movedItems     = source.items.mid(sourceFirstItem, movedCount);
    const QVariantList movedIndents   = source.indents.mid(sourceFirstItem, movedCount);
    const QVariantList movedItemTypes = source.itemTypes.mid(sourceFirstItem, movedCount);
    const QVariantList movedChecked   = source.checked.mid(sourceFirstItem, movedCount);

    const bool removeSourceBlock = source.items.size() == movedCount && sourceRow != targetRow;
    if (removeSourceBlock)
        beginResetModel();
    for (int index = 0; index < movedCount; ++index) {
        source.items.removeAt(sourceFirstItem);
        source.indents.removeAt(sourceFirstItem);
        source.itemTypes.removeAt(sourceFirstItem);
        source.checked.removeAt(sourceFirstItem);
    }

    int adjustedTargetRow = targetRow;
    if (removeSourceBlock) {
        blocks_.removeAt(sourceRow);
        if (sourceRow < targetRow)
            --adjustedTargetRow;
    }

    Block &target = blocks_[adjustedTargetRow];
    normalizeListStorage(&target);
    const int maximumIndent = targetItem == 0 ? 0 : target.indents.value(targetItem - 1).toInt() + 1;
    targetIndent            = qBound(0, targetIndent, maximumIndent);
    const int indentDelta   = targetIndent - sourceIndent;
    for (int index = 0; index < movedCount; ++index) {
        target.items.insert(targetItem + index, movedItems.at(index));
        target.indents.insert(targetItem + index, qMax(0, movedIndents.at(index).toInt() + indentDelta));
        target.itemTypes.insert(targetItem + index, movedItemTypes.at(index));
        target.checked.insert(targetItem + index, movedChecked.at(index));
    }
    normalizeMovedListTypes(&target, targetItem, movedCount);
    if (!removeSourceBlock)
        recomputeTaskParentChecks(&source);
    recomputeTaskParentChecks(&target);

    if (removeSourceBlock) {
        int sourceGap         = sourceRow;
        int resolvedTargetRow = adjustedTargetRow;
        if (normalizeTitleBlock(&blocks_, markdown_)) {
            if (sourceGap >= 1)
                ++sourceGap;
            if (resolvedTargetRow >= 1)
                ++resolvedTargetRow;
        }
        coalesceTextNear(&blocks_, sourceGap, markdown_, &resolvedTargetRow);
        coalesceListAtBoundary(&blocks_, sourceGap, &resolvedTargetRow);
        recomputeTaskParentChecks(&blocks_);
        normalizeTagLinePositions(&blocks_, markdown_, true);
        endResetModel();
        emit contentsChanged();
        return resolvedTargetRow;
    } else if (sourceRow == targetRow) {
        changed(sourceRow, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
        return sourceRow;
    } else {
        changed(sourceRow, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
        changed(targetRow, { ItemsRole, IndentsRole, ItemTypesRole, CheckedRole });
        return targetRow;
    }
}

int NoteBlockModel::moveListRangeToBlock(int sourceRow, int sourceFirstItem, int sourceLastItem, int targetRow)
{
    if (sourceRow < 0 || sourceRow >= blocks_.size() || !isListType(blocks_[sourceRow].type) || targetRow < 0
        || targetRow > blocks_.size()) {
        return -1;
    }

    Block source = blocks_.at(sourceRow);
    normalizeListStorage(&source);

    if (sourceFirstItem < 0 || sourceLastItem < sourceFirstItem || sourceLastItem >= source.items.size())
        return -1;

    const int  movedCount       = sourceLastItem - sourceFirstItem + 1;
    const bool removesWholeList = movedCount == source.items.size();
    if (removesWholeList && (targetRow == sourceRow || targetRow == sourceRow + 1))
        return -1;

    Block detached;
    detached.type = static_cast<BlockType>(source.itemTypes.value(sourceFirstItem, source.type).toInt());
    if (!isListType(detached.type))
        detached.type = source.type;
    detached.items         = source.items.mid(sourceFirstItem, movedCount);
    detached.itemTypes     = source.itemTypes.mid(sourceFirstItem, movedCount);
    detached.checked       = source.checked.mid(sourceFirstItem, movedCount);
    const int sourceIndent = source.indents.at(sourceFirstItem).toInt();
    for (const QVariant &indent : source.indents.mid(sourceFirstItem, movedCount))
        detached.indents.append(qMax(0, indent.toInt() - sourceIndent));

    beginResetModel();
    int sourceGap = -1;
    if (removesWholeList) {
        blocks_.removeAt(sourceRow);
        sourceGap = sourceRow;
        if (targetRow > sourceRow)
            --targetRow;
    } else {
        for (int index = sourceLastItem; index >= sourceFirstItem; --index) {
            source.items.removeAt(index);
            source.indents.removeAt(index);
            source.itemTypes.removeAt(index);
            source.checked.removeAt(index);
        }
        blocks_[sourceRow] = source;
    }

    targetRow = qBound(0, targetRow, blocks_.size());
    blocks_.insert(targetRow, detached);
    if (sourceGap >= 0 && targetRow <= sourceGap)
        ++sourceGap;
    int resolvedRow = targetRow;
    if (normalizeTitleBlock(&blocks_, markdown_)) {
        if (sourceGap >= 1)
            ++sourceGap;
        if (resolvedRow >= 1)
            ++resolvedRow;
    }
    normalizeTagLinePositions(&blocks_, markdown_, true);
    if (sourceGap >= 0) {
        coalesceTextNear(&blocks_, sourceGap, markdown_, &resolvedRow);
        coalesceListAtBoundary(&blocks_, sourceGap, &resolvedRow);
    }
    coalesceMovedList(&blocks_, &resolvedRow);
    recomputeTaskParentChecks(&blocks_);
    normalizeTagLinePositions(&blocks_, markdown_, true);
    endResetModel();
    emit contentsChanged();
    return resolvedRow;
}

bool NoteBlockModel::moveListSubtree(int sourceRow, int sourceItem, int targetRow, int targetItem, int targetIndent)
{
    if (sourceRow < 0 || sourceRow >= blocks_.size() || !isListType(blocks_[sourceRow].type))
        return false;

    const auto normalizeList = [](Block &block) {
        while (block.indents.size() < block.items.size())
            block.indents.append(0);
    };
    normalizeList(blocks_[sourceRow]);

    const Block &source = blocks_[sourceRow];
    if (sourceItem < 0 || sourceItem >= source.items.size())
        return false;

    const int sourceIndent = source.indents.at(sourceItem).toInt();
    int       sourceEnd    = sourceItem + 1;
    while (sourceEnd < source.items.size() && source.indents.at(sourceEnd).toInt() > sourceIndent)
        ++sourceEnd;
    return moveListRange(sourceRow, sourceItem, sourceEnd - 1, targetRow, targetItem, targetIndent);
}

void NoteBlockModel::convertListToText(int row)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_[row].type))
        return;
    blocks_[row] = Block {};
    changed(row, { TypeRole, TextRole, ItemsRole, CheckedRole });
}

int NoteBlockModel::unlistListItem(int row, int item)
{
    if (row < 0 || row >= blocks_.size() || !isListType(blocks_[row].type))
        return -1;

    Block source = blocks_.at(row);
    if (item < 0 || item >= source.items.size())
        return -1;
    while (source.indents.size() < source.items.size())
        source.indents.append(0);
    while (source.itemTypes.size() < source.items.size())
        source.itemTypes.append(source.type);
    while (source.checked.size() < source.items.size())
        source.checked.append(false);

    const int sourceIndent = source.indents.at(item).toInt();
    if (sourceIndent > 0) {
        int subtreeEnd = item + 1;
        while (subtreeEnd < source.items.size() && source.indents.at(subtreeEnd).toInt() > sourceIndent)
            ++subtreeEnd;
        indentListItems(row, item, subtreeEnd - 1, -1);
        return row;
    }

    const auto listSlice = [](const Block &block, int first, int count) {
        Block result;
        result.type          = block.type;
        result.items         = block.items.mid(first, count);
        result.indents       = block.indents.mid(first, count);
        result.itemTypes     = block.itemTypes.mid(first, count);
        result.checked       = block.checked.mid(first, count);
        const auto firstType = static_cast<BlockType>(result.itemTypes.value(0, int(block.type)).toInt());
        if (isListType(firstType))
            result.type = firstType;
        return result;
    };

    int subtreeEnd = item + 1;
    while (subtreeEnd < source.items.size() && source.indents.at(subtreeEnd).toInt() > sourceIndent)
        ++subtreeEnd;

    QList<Block> replacement;
    if (item > 0)
        replacement.append(listSlice(source, 0, item));

    Block paragraph;
    paragraph.type = Text;
    paragraph.text = source.items.at(item);
    replacement.append(paragraph);
    const int paragraphRow = row + replacement.size() - 1;

    if (item + 1 < source.items.size()) {
        Block     after           = listSlice(source, item + 1, source.items.size() - item - 1);
        const int descendantCount = subtreeEnd - item - 1;
        for (int index = 0; index < descendantCount; ++index)
            after.indents[index] = qMax(0, after.indents.at(index).toInt() - 1);
        replacement.append(after);
    }

    beginResetModel();
    blocks_.removeAt(row);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(row + index, replacement.at(index));
    int resolvedParagraphRow = paragraphRow;
    coalesceTextNear(&blocks_, paragraphRow, markdown_, &resolvedParagraphRow);
    recomputeTaskParentChecks(&blocks_);
    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();
    return resolvedParagraphRow;
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

    QVector<int> oldIndents;
    oldIndents.reserve(lastItem - firstItem + 1);
    for (int item = firstItem; item <= lastItem; ++item) {
        const int oldIndent = block.indents[item].toInt();
        oldIndents.append(oldIndent);
        int       indent    = qMax(0, oldIndent + delta);
        const int maximum   = item == 0 ? 0 : block.indents[item - 1].toInt() + 1;
        indent              = qMin(indent, maximum);
        block.indents[item] = indent;
    }

    // Resolve list types from leaves to roots after all indentation levels have
    // reached their final values. Otherwise a root can adopt the type of a
    // descendant that has not moved to its new level yet.
    for (int item = lastItem; item >= firstItem; --item) {
        const int oldIndent = oldIndents.at(item - firstItem);
        const int indent    = block.indents.at(item).toInt();
        if (indent < oldIndent) {
            for (int ancestor = item - 1; ancestor >= 0; --ancestor) {
                if (block.indents.at(ancestor).toInt() == indent) {
                    block.itemTypes[item] = block.itemTypes.at(ancestor);
                    break;
                }
            }
        } else if (indent > oldIndent) {
            bool foundType = false;
            for (int sibling = item - 1; sibling >= 0 && block.indents.at(sibling).toInt() >= indent; --sibling) {
                if (block.indents.at(sibling).toInt() == indent) {
                    block.itemTypes[item] = block.itemTypes.at(sibling);
                    foundType             = true;
                    break;
                }
            }
            for (int sibling = item + 1;
                 !foundType && sibling < block.items.size() && block.indents.at(sibling).toInt() >= indent; ++sibling) {
                if (block.indents.at(sibling).toInt() == indent) {
                    block.itemTypes[item] = block.itemTypes.at(sibling);
                    foundType             = true;
                }
            }
        }
    }
    recomputeTaskParentChecks(&block);
    changed(row, { IndentsRole, ItemTypesRole, CheckedRole });
}

void NoteBlockModel::setChecked(int row, int item, bool checked)
{
    if (row < 0 || row >= blocks_.size() || item < 0 || item >= blocks_[row].items.size())
        return;
    auto &block = blocks_[row];
    while (block.indents.size() < block.items.size())
        block.indents.append(0);
    while (block.itemTypes.size() < block.items.size())
        block.itemTypes.append(block.type);
    while (block.checked.size() < block.items.size())
        block.checked.append(false);

    bool       changedValue = false;
    const auto setValue     = [&block, &changedValue](int index, bool value) {
        if (block.checked[index].toBool() == value)
            return;
        block.checked[index] = value;
        changedValue         = true;
    };

    setValue(item, checked);
    const int itemIndent = block.indents.at(item).toInt();
    int       subtreeEnd = item + 1;
    while (subtreeEnd < block.items.size() && block.indents.at(subtreeEnd).toInt() > itemIndent)
        ++subtreeEnd;
    for (int descendant = item + 1; descendant < subtreeEnd; ++descendant) {
        if (block.itemTypes.at(descendant).toInt() == CheckList)
            setValue(descendant, checked);
    }

    int childIndent = itemIndent;
    for (int candidate = item - 1; candidate >= 0;) {
        while (candidate >= 0 && block.indents.at(candidate).toInt() >= childIndent)
            --candidate;
        if (candidate < 0)
            break;

        const int parent       = candidate;
        const int parentIndent = block.indents.at(parent).toInt();
        if (block.itemTypes.at(parent).toInt() == CheckList) {
            bool hasTaskDescendant = false;
            bool allChecked        = true;
            for (int descendant = parent + 1;
                 descendant < block.items.size() && block.indents.at(descendant).toInt() > parentIndent; ++descendant) {
                if (block.itemTypes.at(descendant).toInt() != CheckList)
                    continue;
                hasTaskDescendant = true;
                allChecked        = allChecked && block.checked.at(descendant).toBool();
            }
            if (hasTaskDescendant)
                setValue(parent, allChecked);
        }
        childIndent = parentIndent;
        candidate   = parent - 1;
    }

    if (!changedValue)
        return;
    changed(row, { CheckedRole });
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
    notifyNormalizedTagLines();
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
    recomputeTaskParentChecks(&block);
    changed(row, { ItemTypesRole, CheckedRole });
    return true;
}
} // namespace AnyKeep
