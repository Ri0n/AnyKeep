#include "../noteblockmodel.h"
#include "../notetagline.h"
#include "private.h"

#include <qqml.h>

namespace AnyKeep {
using namespace NoteBlockModelPrivate;

namespace {
// Register the enum as a real QML type. Context properties are unsuitable for
// enum constants used by separately compiled QML components: those components
// need a lexical type import, not a value injected into one root context.
[[maybe_unused]] const int noteBlockTypeQmlId
    = qmlRegisterUncreatableMetaObject(NoteBlockModel::staticMetaObject, "AnyKeep.Editor", 1, 0,
                                       "NoteBlockType", "NoteBlockType exposes NoteBlockModel enums only");
} // namespace

NoteBlockModel::NoteBlockModel(QObject *parent) : QAbstractListModel(parent) {}

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
        return block.type == TagLine ? NoteTagLine::serialize(block.tags) : block.text;
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
    case LanguageRole:
        return block.language;
    case MediaDisplayWidthRole:
        return block.mediaDisplayWidth;
    case MediaAlignmentRole:
        return block.mediaAlignment;
    case TagsRole:
        return block.tags;
    case MediaTranscriptRole:
        return block.mediaTranscript;
    case MediaTypeRole:
        return block.mediaType;
    case MediaDurationRole:
        return block.mediaDurationMs;
    case MediaWidthRole:
        return block.mediaWidth;
    case MediaHeightRole:
        return block.mediaHeight;
    case AttachmentMediaTypeRole:
        return block.attachmentMediaType;
    case AttachmentSizeRole:
        return block.attachmentSize;
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
        if (block.type == Text && blocks_.size() == 1)
            block.explicitEmpty = false;
        after  = block.text;
        scalar = true;
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
    case LanguageRole:
        if (block.language == value.toString().trimmed().toLower())
            return false;
        before         = block.language;
        block.language = value.toString().trimmed().toLower();
        after          = block.language;
        scalar         = true;
        break;
    case MediaDisplayWidthRole: {
        if (block.type != Media)
            return false;
        const int width = qBound(0, value.toInt(), MaxSerializedImageWidth);
        if (block.mediaDisplayWidth == width)
            return false;
        before           = QString::number(block.mediaDisplayWidth);
        block.mediaDisplayWidth = width;
        after            = QString::number(block.mediaDisplayWidth);
        scalar           = true;
        break;
    }
    case MediaAlignmentRole: {
        if (block.type != Image)
            return false;
        const QString alignment = normalizedImageAlignment(value.toString());
        if (block.mediaAlignment == alignment)
            return false;
        before               = block.mediaAlignment;
        block.mediaAlignment = alignment;
        after                = block.mediaAlignment;
        scalar               = true;
        break;
    }
    case MediaTypeRole:
        if (block.type != Media || block.mediaType == value.toString())
            return false;
        block.mediaType = value.toString().trimmed().toLower();
        break;
    case MediaDurationRole:
        if (block.type != Media || block.mediaDurationMs == qMax<qint64>(0, value.toLongLong()))
            return false;
        block.mediaDurationMs = qMax<qint64>(0, value.toLongLong());
        break;
    case MediaWidthRole:
        if (block.type != Media || block.mediaWidth == qMax(0, value.toInt()))
            return false;
        block.mediaWidth = qMax(0, value.toInt());
        break;
    case MediaHeightRole:
        if (block.type != Media || block.mediaHeight == qMax(0, value.toInt()))
            return false;
        block.mediaHeight = qMax(0, value.toInt());
        break;
    case MediaTranscriptRole:
        if (block.type != Media || block.mediaTranscript == value.toString())
            return false;
        before                = block.mediaTranscript;
        block.mediaTranscript = value.toString();
        after                 = block.mediaTranscript;
        scalar                = true;
        break;
    default:
        return false;
    }
    if (scalar)
        emit scalarEdited(index.row(), role, -1, before, after);
    if (role == TextRole) {
        bool mayPrecedeTagLine = index.row() == 0;
        for (int row = 1; !mayPrecedeTagLine && row < blocks_.size(); ++row) {
            const Block &candidate = blocks_.at(row);
            if (candidate.type == TagLine) {
                mayPrecedeTagLine = index.row() <= row;
                break;
            }
            if (candidate.type != Text || !candidate.text.trimmed().isEmpty())
                break;
        }
        if (mayPrecedeTagLine)
            notifyNormalizedTagLines();
    }
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
             { HeadingLevelRole, "headingLevel" },
             { LanguageRole, "codeLanguage" },
             { MediaDisplayWidthRole, "mediaDisplayWidth" },
             { MediaAlignmentRole, "mediaAlignment" },
             { TagsRole, "tags" },
                          { MediaTranscriptRole, "mediaTranscript" },
        { MediaTypeRole, "mediaType" },
        { MediaDurationRole, "mediaDuration" },
        { MediaWidthRole, "mediaWidth" },
        { MediaHeightRole, "mediaHeight" },
             { AttachmentMediaTypeRole, "attachmentMediaType" },
             { AttachmentSizeRole, "attachmentSize" } };
}

QString NoteBlockModel::contents() const
{
    if (markdown_) {
        auto result = writeMarkdown(blocks_);
        while (result.endsWith(QLatin1Char('\n')) || result.endsWith(QLatin1Char('\r')))
            result.chop(1);
        return result;
    }
    if (blocks_.isEmpty())
        return {};

    QString result = blocks_.constFirst().text;
    for (int row = 1; row < blocks_.size(); ++row) {
        if (blocks_.at(row).type != Text)
            continue;
        result += QLatin1Char('\n') + blocks_.at(row).text;
    }
    return result;
}

void NoteBlockModel::setContents(const QString &contents) { load(contents, markdown_); }

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

bool NoteBlockModel::normalizeTagLinePositions(QList<Block> *blocks, bool markdown, bool promoteTextCandidate)
{
    if (!blocks || blocks->isEmpty())
        return false;

    const Block &title = blocks->constFirst();
    const bool   titleOnly
        = markdown && title.type == Text && !title.text.trimmed().isEmpty() && !title.text.contains(QLatin1Char('\n'));
    bool bodyContentSeen = false;
    bool keptTagLine     = false;
    bool changed         = false;

    for (int row = 0; row < blocks->size(); ++row) {
        Block &block = (*blocks)[row];
        if (block.type == TagLine) {
            const bool validPosition
                = row > 0 && titleOnly && !bodyContentSeen && !keptTagLine && !block.tags.isEmpty();
            if (validPosition) {
                keptTagLine     = true;
                bodyContentSeen = true;
                continue;
            }

            Block ordinaryText;
            ordinaryText.type          = Text;
            ordinaryText.text          = NoteTagLine::serialize(block.tags);
            ordinaryText.explicitEmpty = ordinaryText.text.isEmpty();
            block                      = ordinaryText;
            changed                    = true;
        }

        if (row == 0)
            continue;
        if (block.type == Text && block.text.trimmed().isEmpty())
            continue;
        if (promoteTextCandidate && titleOnly && !bodyContentSeen && !keptTagLine && block.type == Text
            && !block.text.contains(QLatin1Char('\n'))) {
            const QStringList tags = NoteTagLine::parseLine(block.text);
            if (!tags.isEmpty()) {
                Block tagLine;
                tagLine.type = TagLine;
                tagLine.tags = tags;
                block        = tagLine;
                keptTagLine  = true;
                changed      = true;
            }
        }
        bodyContentSeen = true;
    }
    return changed;
}

bool NoteBlockModel::normalizeTitleBlock(QList<Block> *blocks, bool markdown)
{
    if (!blocks)
        return false;
    if (blocks->isEmpty()) {
        blocks->append(Block {});
        return true;
    }

    if (blocks->constFirst().type != Text)
        return false;

    Block    &title     = blocks->first();
    const int separator = title.text.indexOf(QLatin1Char('\n'));
    if (separator < 0)
        return false;

    QString body = title.text.mid(separator + 1);
    if (markdown) {
        while (body.startsWith(QLatin1Char('\n')))
            body.remove(0, 1);
    }
    title.text          = title.text.left(separator);
    title.explicitEmpty = false;

    Block following;
    following.type = Text;
    following.text = body;
    // The body row is the stable editable area below the title. It is not a
    // transient inter-block insertion point and must survive focus changes.
    following.explicitEmpty = false;
    blocks->insert(1, following);
    return true;
}

void NoteBlockModel::notifyNormalizedTagLines(bool promoteTextCandidate)
{
    if (!normalizeTagLinePositions(&blocks_, markdown_, promoteTextCandidate) || blocks_.isEmpty())
        return;
    emit dataChanged(index(0), index(blocks_.size() - 1), { TypeRole, TextRole, TagsRole });
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
    normalizeTitleBlock(&blocks_, markdown_);
    normalizeTagLinePositions(&blocks_, markdown_);
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
    normalizeTitleBlock(&blocks_, markdown_);
    normalizeTagLinePositions(&blocks_, markdown_, true);
    endResetModel();
    emit markdownChanged();
    emit contentsChanged();
}

int NoteBlockModel::blockTypeAt(int row) const
{
    return row >= 0 && row < blocks_.size() ? int(blocks_.at(row).type) : -1;
}

QString NoteBlockModel::blockTextAt(int row) const
{
    return row >= 0 && row < blocks_.size() ? blocks_.at(row).text : QString();
}

int NoteBlockModel::listItemCountAt(int row) const
{
    return row >= 0 && row < blocks_.size() && isListType(blocks_.at(row).type) ? blocks_.at(row).items.size() : 0;
}

bool NoteBlockModel::isExplicitEmptyTextBlock(int row) const
{
    return row >= 0 && row < blocks_.size() && blocks_.at(row).type == Text && blocks_.at(row).text.isEmpty()
        && blocks_.at(row).explicitEmpty;
}

void NoteBlockModel::changed(int row, const QList<int> &roles)
{
    emit dataChanged(index(row), index(row), roles);
    emit contentsChanged();
}
} // namespace AnyKeep
