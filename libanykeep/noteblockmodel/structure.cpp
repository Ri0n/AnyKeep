#include "../noteblockmodel.h"
#include "../notetagline.h"
#include "private.h"

namespace AnyKeep {
using namespace NoteBlockModelPrivate;

void NoteBlockModel::coalesceTextNear(QList<Block> *blocks, int row, bool markdown, int *trackedRow)
{
    if (!blocks || blocks->size() < 2)
        return;

    const int  firstMergeableRow = blocks->constFirst().type == Text ? 1 : 0;
    int        scan              = qMax(firstMergeableRow, row - 1);
    const auto mergeAt           = [blocks, markdown, trackedRow](int leftRow) {
        Block       &left  = (*blocks)[leftRow];
        const Block &right = blocks->at(leftRow + 1);
        if (!left.text.isEmpty() && !right.text.isEmpty())
            left.text += markdown ? QStringLiteral("\n\n") : QStringLiteral("\n");
        left.text += right.text;
        left.text          = coalesceAdjacentMarkdownLinks(left.text);
        left.explicitEmpty = left.text.isEmpty() && left.explicitEmpty && right.explicitEmpty;
        blocks->removeAt(leftRow + 1);
        if (!trackedRow)
            return;
        if (*trackedRow == leftRow + 1)
            *trackedRow = leftRow;
        else if (*trackedRow > leftRow + 1)
            --*trackedRow;
    };

    while (scan + 1 < blocks->size()) {
        const bool inNeighborhood = scan <= row + 1;
        if (!inNeighborhood)
            break;
        const Block &left  = blocks->at(scan);
        const Block &right = blocks->at(scan + 1);
        if (left.type == Text && right.type == Text && !left.explicitEmpty && !right.explicitEmpty) {
            mergeAt(scan);
            row  = qMax(firstMergeableRow, scan);
            scan = qMax(firstMergeableRow, scan - 1);
            continue;
        }
        ++scan;
    }
}

void NoteBlockModel::setBlockText(int row, const QString &text)
{
    if (row < 0 || row >= blocks_.size())
        return;

    const QString value = blocks_.at(row).type == CodeBlock ? text : coalesceAdjacentMarkdownLinks(text);
    if (row == 0 && blocks_.constFirst().type == Text) {
        QString normalized = value;
        normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        normalized.replace(QChar::LineSeparator, QLatin1Char('\n'));
        normalized.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        const int separator = normalized.indexOf(QLatin1Char('\n'));
        if (separator >= 0) {
            const QString title = normalized.left(separator);
            QString       body  = normalized.mid(separator + 1);
            if (markdown_) {
                while (body.startsWith(QLatin1Char('\n')))
                    body.remove(0, 1);
            }

            const bool hasBodyText = blocks_.size() > 1 && blocks_.at(1).type == Text;
            if (blocks_.constFirst().text == title && hasBodyText && blocks_.at(1).text == body)
                return;

            // A multiline assignment to the title represents the combined
            // title/body projection used by NoteEditor::text(). Keep the
            // structural title isolated and replace (rather than append to)
            // the first body text block. This also makes history restore
            // atomic and prevents "Body\nBody" duplication.
            beginResetModel();
            blocks_[0].text          = title;
            blocks_[0].explicitEmpty = false;
            if (hasBodyText) {
                blocks_[1].text          = body;
                blocks_[1].explicitEmpty = false;
            } else {
                Block following;
                following.type          = Text;
                following.text          = body;
                following.explicitEmpty = false;
                blocks_.insert(1, following);
            }
            normalizeTagLinePositions(&blocks_, markdown_);
            endResetModel();
            emit contentsChanged();
            return;
        }
    }

    setData(index(row), value, TextRole);
}

bool NoteBlockModel::mergeTextBlockWithNext(int row)
{
    if (row < 0 || row + 1 >= blocks_.size() || blocks_[row].type != Text || blocks_[row + 1].type != Text)
        return false;

    if (row == 0) {
        beginResetModel();
        blocks_[0].text          = coalesceAdjacentMarkdownLinks(blocks_[0].text + blocks_[1].text);
        blocks_[0].explicitEmpty = blocks_[0].text.isEmpty() && (blocks_[0].explicitEmpty || blocks_[1].explicitEmpty);
        blocks_.removeAt(1);
        normalizeTitleBlock(&blocks_, markdown_);
        normalizeTagLinePositions(&blocks_, markdown_);
        endResetModel();
        emit contentsChanged();
        return true;
    }

    beginRemoveRows({}, row + 1, row + 1);
    blocks_[row].text = coalesceAdjacentMarkdownLinks(blocks_[row].text + blocks_[row + 1].text);
    blocks_[row].explicitEmpty
        = blocks_[row].text.isEmpty() && (blocks_[row].explicitEmpty || blocks_[row + 1].explicitEmpty);
    blocks_.removeAt(row + 1);
    endRemoveRows();
    notifyNormalizedTagLines();
    changed(row, { TextRole });
    return true;
}

bool NoteBlockModel::splitTitleBlock(const QString &before, const QString &after)
{
    if (blocks_.isEmpty() || blocks_.constFirst().type != Text)
        return false;

    blocks_[0].text          = coalesceAdjacentMarkdownLinks(before);
    blocks_[0].explicitEmpty = false;

    Block following;
    following.type          = Text;
    following.text          = coalesceAdjacentMarkdownLinks(after);
    following.explicitEmpty = false;

    beginInsertRows({}, 1, 1);
    blocks_.insert(1, following);
    endInsertRows();
    notifyNormalizedTagLines();
    changed(0, { TextRole });
    return true;
}

void NoteBlockModel::insertTextBlock(int row)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.explicitEmpty = true;
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::appendTextBlock()
{
    const int row = blocks_.size();
    beginInsertRows({}, row, row);
    blocks_.append(Block {});
    endInsertRows();
    notifyNormalizedTagLines();
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
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::insertBlockQuote(int row)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type = BlockQuote;
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::insertCodeBlock(int row, const QString &language)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type     = CodeBlock;
    block.language = language.trimmed().toLower();
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::setCodeLanguage(int row, const QString &language)
{
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != CodeBlock)
        return;
    setData(index(row), language, LanguageRole);
}

QVariantMap NoteBlockModel::promoteTagLineFromText(int row, const QString &plainText, const QString &markdownText,
                                                   int cursorPosition, bool force)
{
    QVariantMap result;
    result.insert(QStringLiteral("handled"), false);
    const bool titleBlockCandidate   = row == 0 && row < blocks_.size() && blocks_.at(row).type == Text;
    const bool separateBodyCandidate = row == 1 && row < blocks_.size() && blocks_.at(row).type == Text
        && blocks_.constFirst().type == Text && !blocks_.constFirst().text.contains(QLatin1Char('\n'));
    if (!markdown_ || (!titleBlockCandidate && !separateBodyCandidate))
        return result;

    const QString plainProbePrefix    = separateBodyCandidate ? QStringLiteral("_\n") : QString();
    const QString markdownProbePrefix = separateBodyCandidate ? QStringLiteral("_\n\n") : QString();
    const auto    plainMatch          = NoteTagLine::findEditorDocumentTagLine(plainProbePrefix + plainText);
    if (!plainMatch || (!force && !plainMatch->trailingWhitespace))
        return result;
    const auto markdownMatch = NoteTagLine::firstMarkdownDocumentBodyLine(markdownProbePrefix + markdownText);
    if (!markdownMatch)
        return result;

    const int plainLineStart    = plainMatch->lineStart - plainProbePrefix.size();
    const int plainLineEnd      = plainMatch->lineEnd - plainProbePrefix.size();
    const int markdownLineStart = markdownMatch->lineStart - markdownProbePrefix.size();
    const int markdownLineEnd   = markdownMatch->lineEnd - markdownProbePrefix.size();
    if (plainLineStart < 0 || markdownLineStart < 0)
        return result;

    QList<Block>  replacement;
    const QString prefix = markdownText.left(markdownLineStart);
    const QString suffix = markdownText.mid(markdownLineEnd);
    if (!prefix.trimmed().isEmpty()) {
        const auto parsed = parseMarkdownCore(prefix);
        for (const Block &block : parsed)
            replacement.append(block);
    }

    Block tagLine;
    tagLine.type        = TagLine;
    tagLine.tags        = plainMatch->tags;
    const int tagOffset = replacement.size();
    replacement.append(tagLine);

    if (!suffix.trimmed().isEmpty()) {
        const auto parsed = parseMarkdownCore(suffix);
        for (const Block &block : parsed)
            replacement.append(block);
    }

    beginResetModel();
    blocks_.removeAt(row);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(row + index, replacement.at(index));
    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();

    const int tagRow = row + tagOffset;
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("tagRow"), tagRow);
    result.insert(QStringLiteral("focusTagLine"),
                  cursorPosition >= plainLineStart && cursorPosition <= plainLineEnd + 1);
    if (cursorPosition > plainLineEnd) {
        int contentStart = plainLineEnd;
        while (contentStart < plainText.size() && plainText.at(contentStart).isSpace())
            ++contentStart;
        result.insert(QStringLiteral("focusRow"), qMin(tagRow + 1, blocks_.size() - 1));
        result.insert(QStringLiteral("cursorPosition"), qMax(0, cursorPosition - contentStart));
    } else {
        result.insert(QStringLiteral("focusRow"), qMax(0, tagRow - 1));
        result.insert(QStringLiteral("cursorPosition"), qMax(0, qMin(cursorPosition, plainLineStart - 1)));
    }
    return result;
}

QVariantMap NoteBlockModel::convertTagLineToText(int row, const QString &text, int cursorPosition)
{
    QVariantMap result;
    result.insert(QStringLiteral("handled"), false);
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != TagLine)
        return result;

    const QString value = text.isNull() ? NoteTagLine::serialize(blocks_.at(row).tags) : text;
    beginResetModel();
    Block &block = blocks_[row];
    block.type   = Text;
    block.text   = value;
    block.tags.clear();
    block.explicitEmpty = value.isEmpty();
    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("focusRow"), row);
    result.insert(QStringLiteral("cursorPosition"),
                  cursorPosition < 0 ? value.size() : qBound(0, cursorPosition, value.size()));
    return result;
}

QVariantMap NoteBlockModel::setTagLineTag(int row, int tagIndex, const QString &value, int cursorPosition)
{
    QVariantMap result;
    result.insert(QStringLiteral("handled"), false);
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != TagLine || tagIndex < 0
        || tagIndex >= blocks_.at(row).tags.size()) {
        return result;
    }

    const QString tokenText = value.trimmed();
    if (tokenText.isEmpty() || tokenText == QLatin1String("#"))
        return removeTagLineTag(row, tagIndex);

    const QStringList parsed = NoteTagLine::parseLine(tokenText);
    if (parsed.isEmpty()) {
        QStringList tokens;
        const auto &tags       = blocks_.at(row).tags;
        int         tokenStart = 0;
        for (int index = 0; index < tags.size(); ++index) {
            const QString token = index == tagIndex ? tokenText : QLatin1Char('#') + tags.at(index);
            if (index == tagIndex)
                tokenStart = tokens.join(QLatin1Char(' ')).size() + (tokens.isEmpty() ? 0 : 1);
            tokens.append(token);
        }
        const int restoredCursor = cursorPosition < 0 ? -1 : tokenStart + qBound(0, cursorPosition, tokenText.size());
        return convertTagLineToText(row, tokens.join(QLatin1Char(' ')), restoredCursor);
    }

    const QStringList original = blocks_.at(row).tags;
    QStringList       updated  = original;
    updated.removeAt(tagIndex);
    int insertion = tagIndex;
    for (const QString &name : parsed) {
        const int duplicate = updated.indexOf(name);
        if (duplicate >= 0) {
            insertion = duplicate + 1;
            continue;
        }
        updated.insert(qMin(insertion, updated.size()), name);
        ++insertion;
    }
    if (updated != original) {
        blocks_[row].tags = updated;
        changed(row, { TagsRole, TextRole });
    }
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("tagLine"), true);
    result.insert(QStringLiteral("focusTag"), qBound(0, insertion - 1, updated.size() - 1));
    return result;
}

QVariantMap NoteBlockModel::appendTagLineTag(int row, const QString &value, int cursorPosition)
{
    QVariantMap result;
    result.insert(QStringLiteral("handled"), false);
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != TagLine)
        return result;

    const QString tokenText = value.trimmed();
    if (tokenText.isEmpty() || tokenText == QLatin1String("#")) {
        result.insert(QStringLiteral("handled"), true);
        result.insert(QStringLiteral("tagLine"), true);
        result.insert(QStringLiteral("focusDraft"), true);
        return result;
    }

    const QStringList parsed = NoteTagLine::parseLine(tokenText);
    if (parsed.isEmpty()) {
        const QString prefix     = NoteTagLine::serialize(blocks_.at(row).tags);
        const QString valueText  = prefix.isEmpty() ? tokenText : prefix + QLatin1Char(' ') + tokenText;
        const int     tokenStart = prefix.isEmpty() ? 0 : prefix.size() + 1;
        const int restoredCursor = cursorPosition < 0 ? -1 : tokenStart + qBound(0, cursorPosition, tokenText.size());
        return convertTagLineToText(row, valueText, restoredCursor);
    }

    auto &tags        = blocks_[row].tags;
    bool  changedTags = false;
    for (const QString &name : parsed) {
        if (!tags.contains(name)) {
            tags.append(name);
            changedTags = true;
        }
    }
    if (changedTags)
        changed(row, { TagsRole, TextRole });
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("tagLine"), true);
    result.insert(QStringLiteral("focusDraft"), true);
    return result;
}

QVariantMap NoteBlockModel::removeTagLineTag(int row, int tagIndex)
{
    QVariantMap result;
    result.insert(QStringLiteral("handled"), false);
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != TagLine || tagIndex < 0
        || tagIndex >= blocks_.at(row).tags.size()) {
        return result;
    }
    if (blocks_.at(row).tags.size() == 1)
        return convertTagLineToText(row, QStringLiteral(""), 0);

    blocks_[row].tags.removeAt(tagIndex);
    changed(row, { TagsRole, TextRole });
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("tagLine"), true);
    result.insert(QStringLiteral("focusTag"), qMin(tagIndex, blocks_.at(row).tags.size() - 1));
    return result;
}

bool NoteBlockModel::moveTagLineTag(int row, int from, int to)
{
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != TagLine || from < 0
        || from >= blocks_.at(row).tags.size()) {
        return false;
    }
    to = qBound(0, to, blocks_.at(row).tags.size() - 1);
    if (from == to)
        return false;
    blocks_[row].tags.move(from, to);
    changed(row, { TagsRole, TextRole });
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
    if (blocks_[row].type == BlockQuote && level == 0) {
        blocks_[row].type         = Text;
        blocks_[row].headingLevel = 0;
        changed(row, { TypeRole, HeadingLevelRole });
        return row;
    }
    if ((blocks_[row].type != Text && blocks_[row].type != BlockQuote) || level == 0)
        return -1;

    const BlockType sourceType = blocks_[row].type;
    const QString   text       = blocks_[row].text;
    position                   = qBound(0, position, text.size());
    const int separatorBefore  = text.lastIndexOf(QStringLiteral("\n\n"), qMax(0, position - 1));
    const int paragraphStart   = separatorBefore < 0 ? 0 : separatorBefore + 2;
    const int separatorAfter   = text.indexOf(QStringLiteral("\n\n"), position);
    const int paragraphEnd     = separatorAfter < 0 ? text.size() : separatorAfter;

    QList<Block> replacement;
    if (paragraphStart > 0) {
        Block before;
        before.type = sourceType;
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
        after.type = sourceType;
        after.text = text.mid(paragraphEnd + 2);
        replacement.append(after);
    }

    beginResetModel();
    blocks_.removeAt(row);
    for (int i = 0; i < replacement.size(); ++i)
        blocks_.insert(row + i, replacement[i]);
    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();
    return row + headingOffset;
}

int NoteBlockModel::convertTextBlockToQuote(int row, int position, bool quote)
{
    if (row < 0 || row >= blocks_.size())
        return -1;
    if (blocks_[row].type == BlockQuote) {
        if (quote)
            return row;
        blocks_[row].type         = Text;
        blocks_[row].headingLevel = 0;
        changed(row, { TypeRole, HeadingLevelRole });
        return row;
    }
    if (blocks_[row].type == Heading && quote) {
        blocks_[row].type         = BlockQuote;
        blocks_[row].headingLevel = 0;
        changed(row, { TypeRole, HeadingLevelRole });
        return row;
    }
    if (blocks_[row].type != Text || !quote)
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
    Block blockQuote;
    blockQuote.type       = BlockQuote;
    blockQuote.text       = text.mid(paragraphStart, paragraphEnd - paragraphStart);
    const int quoteOffset = replacement.size();
    replacement.append(blockQuote);
    if (paragraphEnd < text.size()) {
        Block after;
        after.text = text.mid(paragraphEnd + 2);
        replacement.append(after);
    }

    beginResetModel();
    blocks_.removeAt(row);
    for (int index = 0; index < replacement.size(); ++index)
        blocks_.insert(row + index, replacement.at(index));
    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();
    return row + quoteOffset;
}

bool NoteBlockModel::splitStructuredBlockToText(int row, const QString &before, const QString &after)
{
    if (row < 0 || row >= blocks_.size() || (blocks_.at(row).type != Heading && blocks_.at(row).type != BlockQuote)) {
        return false;
    }

    blocks_[row].text = coalesceAdjacentMarkdownLinks(before);

    Block following;
    following.type          = Text;
    following.text          = coalesceAdjacentMarkdownLinks(after);
    following.explicitEmpty = following.text.isEmpty();

    beginInsertRows({}, row + 1, row + 1);
    blocks_.insert(row + 1, following);
    endInsertRows();
    notifyNormalizedTagLines();
    changed(row, { TextRole });
    return true;
}

bool NoteBlockModel::moveBlock(int row, int targetRow) { return moveBlockResolved(row, targetRow) >= 0; }

int NoteBlockModel::moveBlockResolved(int row, int targetRow)
{
    if (row < 0 || row >= blocks_.size() || targetRow < 0 || targetRow >= blocks_.size() || row == targetRow)
        return -1;

    int sourceGap = targetRow < row ? row + 1 : row;
    int movedRow  = targetRow;
    beginResetModel();
    blocks_.move(row, targetRow);
    if (normalizeTitleBlock(&blocks_, markdown_)) {
        if (sourceGap >= 1)
            ++sourceGap;
        if (movedRow >= 1)
            ++movedRow;
    }
    normalizeTagLinePositions(&blocks_, markdown_);

    coalesceTextNear(&blocks_, sourceGap, markdown_, &movedRow);
    coalesceListAtBoundary(&blocks_, sourceGap, &movedRow);
    if (movedRow >= 0 && movedRow < blocks_.size() && blocks_.at(movedRow).type == Text)
        coalesceTextNear(&blocks_, movedRow == 0 ? 1 : movedRow, markdown_, &movedRow);
    else
        coalesceMovedList(&blocks_, &movedRow);

    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();
    return movedRow;
}

void NoteBlockModel::removeBlock(int row)
{
    if (row < 0 || row >= blocks_.size())
        return;
    beginRemoveRows({}, row, row);
    blocks_.removeAt(row);
    endRemoveRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::setPreviewUrls(const QHash<QString, QString> &urls)
{
    previewUrls_ = urls;
    if (!blocks_.isEmpty())
        emit dataChanged(index(0), index(blocks_.size() - 1), { PreviewUrlRole });
}
} // namespace AnyKeep
