#include "noteblockmodel.h"
#include "notetagline.h"
#include <climits>

#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>
#include <QVector>

#include <algorithm>
#include <limits>
#include <optional>

namespace AnyKeep {
namespace {
    const QString    TableLineBreakMarker       = QStringLiteral("ANYKEEP_TABLE_LINE_BREAK_7F3A");
    const QString    LegacyEmptyParagraphMarker = QStringLiteral("<!-- anykeep:empty-paragraph -->");
    constexpr int    MaxSerializedImageWidth    = 16384;
    constexpr qint64 MaxAudioDurationMs         = 7LL * 24 * 60 * 60 * 1000;

    struct HtmlImageBlock {
        QString source;
        QString alt;
        QString alignment { QStringLiteral("center") };
        int     width { 0 };

        explicit operator bool() const { return !source.isEmpty(); }
    };

    struct HtmlAudioBlock {
        QString source;
        QString title;
        qint64  durationMs { 0 };

        explicit operator bool() const { return !source.isEmpty(); }
    };

    struct HtmlAttachmentBlock {
        QString source;
        QString fileName;
        QString mediaType;
        qint64  size { 0 };

        explicit operator bool() const { return !source.isEmpty() && !fileName.isEmpty(); }
    };

    QString normalizedImageAlignment(QString alignment)
    {
        alignment = alignment.trimmed().toLower();
        return alignment == QLatin1String("left") || alignment == QLatin1String("right")
                || alignment == QLatin1String("center")
            ? alignment
            : QStringLiteral("center");
    }

    QString decodeHtmlAttribute(QString value)
    {
        // QString::toHtmlEscaped(), used by serializeHtmlImage(), emits this
        // exact set. Decode ampersand last so an escaped entity literal such
        // as &amp;quot; remains the text "&quot;" rather than becoming a quote.
        value.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        value.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
        value.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
        value.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        return value;
    }

    QHash<QString, QString> htmlAttributes(const QString &source)
    {
        static const QRegularExpression attribute(QStringLiteral(R"(([A-Za-z_:][-A-Za-z0-9_:.]*)\s*=\s*(["'])(.*?)\2)"),
                                                  QRegularExpression::DotMatchesEverythingOption);
        QHash<QString, QString>         result;
        auto                            iterator = attribute.globalMatch(source);
        while (iterator.hasNext()) {
            const auto match = iterator.next();
            result.insert(match.captured(1).toLower(), decodeHtmlAttribute(match.captured(3)));
        }
        return result;
    }

    HtmlImageBlock parseHtmlImageBlock(const QString &line)
    {
        static const QRegularExpression outer(
            QStringLiteral(R"(^\s*(?:<p\b([^>]*)>\s*)?<img\b([^>]*)/?>\s*(?:</p>)?\s*$)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = outer.match(line);
        if (!match.hasMatch())
            return {};

        const auto     imageAttributes = htmlAttributes(match.captured(2));
        HtmlImageBlock result;
        result.source = imageAttributes.value(QStringLiteral("src")).trimmed();
        result.alt    = imageAttributes.value(QStringLiteral("alt"));
        if (result.source.isEmpty())
            return {};

        const auto paragraphAttributes = htmlAttributes(match.captured(1));
        result.alignment               = normalizedImageAlignment(paragraphAttributes.value(QStringLiteral("align")));
        QString widthText              = imageAttributes.value(QStringLiteral("width")).trimmed();
        if (widthText.endsWith(QLatin1String("px"), Qt::CaseInsensitive))
            widthText.chop(2);
        bool      widthOk = false;
        const int width   = widthText.toInt(&widthOk);
        if (widthOk)
            result.width = qBound(0, width, MaxSerializedImageWidth);
        return result;
    }

    QString serializeHtmlImage(const QString &source, const QString &alt, int width, const QString &alignment)
    {
        QString image = QStringLiteral("<img src=\"%1\" alt=\"%2\"").arg(source.toHtmlEscaped(), alt.toHtmlEscaped());
        if (width > 0)
            image += QStringLiteral(" width=\"%1\"").arg(qBound(1, width, MaxSerializedImageWidth));
        image += QStringLiteral(" />");
        return QStringLiteral("<p align=\"%1\">%2</p>").arg(normalizedImageAlignment(alignment), image);
    }

    HtmlAudioBlock parseHtmlAudioBlock(const QString &line)
    {
        static const QRegularExpression outer(QStringLiteral(R"(^\s*<audio\b([^>]*?)(?:>\s*</audio>|/>)\s*$)"),
                                              QRegularExpression::CaseInsensitiveOption);
        const auto                      match = outer.match(line);
        if (!match.hasMatch())
            return {};
        const auto     attributes = htmlAttributes(match.captured(1));
        HtmlAudioBlock result;
        result.source           = attributes.value(QStringLiteral("src")).trimmed();
        result.title            = attributes.value(QStringLiteral("title"));
        bool         durationOk = false;
        const qint64 duration   = attributes.value(QStringLiteral("data-anykeep-duration-ms")).toLongLong(&durationOk);
        if (durationOk)
            result.durationMs = qBound<qint64>(0, duration, MaxAudioDurationMs);
        return result;
    }

    QString parseHtmlAudioTranscript(const QString &line)
    {
        static const QRegularExpression outer(QStringLiteral(R"(^\s*<div\b([^>]*)>(.*)</div>\s*$)"),
                                              QRegularExpression::CaseInsensitiveOption
                                                  | QRegularExpression::DotMatchesEverythingOption);
        const auto                      match = outer.match(line);
        if (!match.hasMatch())
            return {};
        const auto attributes = htmlAttributes(match.captured(1));
        if (!attributes.contains(QStringLiteral("data-anykeep-audio-transcript")))
            return {};
        QString                         text = match.captured(2);
        static const QRegularExpression breaks(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
        text.replace(breaks, QStringLiteral("\n"));
        text.replace(TableLineBreakMarker, QStringLiteral("\n"));
        return decodeHtmlAttribute(text);
    }

    QString serializeHtmlAudio(const QString &source, const QString &title, qint64 durationMs,
                               const QString &transcript)
    {
        QString result
            = QStringLiteral("<audio controls src=\"%1\" title=\"%2\" data-anykeep-duration-ms=\"%3\"></audio>")
                  .arg(source.toHtmlEscaped(), title.toHtmlEscaped(),
                       QString::number(qBound<qint64>(0, durationMs, MaxAudioDurationMs)));
        if (!transcript.isEmpty()) {
            QString escaped = transcript.toHtmlEscaped();
            escaped.replace(QLatin1Char('\n'), QStringLiteral("<br />"));
            result += QStringLiteral("\n<div data-anykeep-audio-transcript=\"1\">%1</div>").arg(escaped);
        }
        return result;
    }

    HtmlAttachmentBlock parseHtmlAttachmentBlock(const QString &line)
    {
        static const QRegularExpression outer(QStringLiteral(R"(^\s*<a\b([^>]*)>(.*?)</a>\s*$)"),
                                              QRegularExpression::CaseInsensitiveOption
                                                  | QRegularExpression::DotMatchesEverythingOption);
        const auto                      match = outer.match(line);
        if (!match.hasMatch())
            return {};
        const auto attributes = htmlAttributes(match.captured(1));
        if (!attributes.contains(QStringLiteral("data-anykeep-attachment")))
            return {};
        HtmlAttachmentBlock result;
        result.source       = attributes.value(QStringLiteral("href")).trimmed();
        result.fileName     = decodeHtmlAttribute(match.captured(2)).trimmed();
        result.mediaType    = attributes.value(QStringLiteral("data-anykeep-media-type")).trimmed();
        bool         sizeOk = false;
        const qint64 size   = attributes.value(QStringLiteral("data-anykeep-size")).toLongLong(&sizeOk);
        if (sizeOk)
            result.size = qMax<qint64>(0, size);
        return result;
    }

    QString serializeHtmlAttachment(const QString &source, const QString &fileName, const QString &mediaType,
                                    qint64 size)
    {
        return QStringLiteral("<a href=\"%1\" data-anykeep-attachment=\"1\" data-anykeep-media-type=\"%2\" "
                              "data-anykeep-size=\"%3\">%4</a>")
            .arg(source.toHtmlEscaped(), mediaType.toHtmlEscaped(), QString::number(qMax<qint64>(0, size)),
                 fileName.toHtmlEscaped());
    }

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

    QString decodeListItem(QString text)
    {
        text = decodeLineBreaks(std::move(text));
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        return text;
    }

    int leadingSpaceCount(const QString &line)
    {
        int count = 0;
        while (count < line.size() && line.at(count) == QLatin1Char(' '))
            ++count;
        return count;
    }

    QString serializeListItem(QString text, int indentColumns, const QString &marker)
    {
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        const QStringList lines  = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        QString           result = QString(indentColumns, QLatin1Char(' ')) + marker + lines.value(0);
        const QString     continuationIndent(indentColumns + marker.size(), QLatin1Char(' '));
        for (int line = 1; line < lines.size(); ++line) {
            result += QLatin1Char('\n');
            if (!lines.at(line).isEmpty())
                result += continuationIndent + lines.at(line);
        }
        return result;
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
    case ImageWidthRole:
        return block.imageWidth;
    case ImageAlignmentRole:
        return block.imageAlignment;
    case TagsRole:
        return block.tags;
    case AudioDurationRole:
        return block.audioDurationMs;
    case AudioTranscriptRole:
        return block.audioTranscript;
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
    case ImageWidthRole: {
        if (block.type != Image)
            return false;
        const int width = qBound(0, value.toInt(), MaxSerializedImageWidth);
        if (block.imageWidth == width)
            return false;
        before           = QString::number(block.imageWidth);
        block.imageWidth = width;
        after            = QString::number(block.imageWidth);
        scalar           = true;
        break;
    }
    case ImageAlignmentRole: {
        if (block.type != Image)
            return false;
        const QString alignment = normalizedImageAlignment(value.toString());
        if (block.imageAlignment == alignment)
            return false;
        before               = block.imageAlignment;
        block.imageAlignment = alignment;
        after                = block.imageAlignment;
        scalar               = true;
        break;
    }
    case AudioTranscriptRole:
        if (block.type != Audio || block.audioTranscript == value.toString())
            return false;
        before                = block.audioTranscript;
        block.audioTranscript = value.toString();
        after                 = block.audioTranscript;
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
             { ImageWidthRole, "imageWidth" },
             { ImageAlignmentRole, "imageAlignment" },
             { TagsRole, "tags" },
             { AudioDurationRole, "audioDuration" },
             { AudioTranscriptRole, "audioTranscript" },
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
        case BlockQuote:
            fields.append({ blockIndex, -1, -1, QStringLiteral("blockquote"), block.text });
            break;
        case CodeBlock:
            fields.append({ blockIndex, -1, -1, QStringLiteral("code"), block.text });
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
        case Audio:
        case Attachment:
            // Structural media labels are not QTextDocument editors, so a
            // text-search result cannot expose a visible selection there yet.
            break;
        case TagLine:
            // Tag chips are edited by a dedicated structural delegate. Search
            // result selection currently addresses QTextDocument editors only.
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
    normalizeTagLinePositions(&blocks_, markdown_);
    endResetModel();
    emit contentsChanged();
    return paragraphRow;
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
    changed(row, { IndentsRole, ItemTypesRole });
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

bool NoteBlockModel::moveTableColumn(int row, int from, int to)
{
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != Table)
        return false;
    auto &block = blocks_[row];
    if (block.columns <= 1 || from < 0 || from >= block.columns)
        return false;
    to = qBound(0, to, block.columns - 1);
    if (from == to)
        return false;

    const int rows = block.cells.size() / block.columns;
    for (int tableRow = 0; tableRow < rows; ++tableRow) {
        const int     source = tableRow * block.columns + from;
        const QString cell   = block.cells.takeAt(source);
        block.cells.insert(tableRow * block.columns + to, cell);
    }
    changed(row, { CellsRole });
    return true;
}

void NoteBlockModel::setImageUrl(int row, const QString &url) { setData(index(row), url, UrlRole); }
void NoteBlockModel::setImageAlt(int row, const QString &alt) { setData(index(row), alt, AltRole); }
void NoteBlockModel::setImageWidth(int row, int width) { setData(index(row), width, ImageWidthRole); }
void NoteBlockModel::setImageAlignment(int row, const QString &alignment)
{
    setData(index(row), alignment, ImageAlignmentRole);
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
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::appendAudio(const QString &url, const QString &title, qint64 durationMs)
{
    insertAudio(blocks_.size(), url, title, durationMs);
}

void NoteBlockModel::insertAudio(int row, const QString &url, const QString &title, qint64 durationMs)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type            = Audio;
    block.url             = url;
    block.alt             = title;
    block.audioDurationMs = qBound<qint64>(0, durationMs, MaxAudioDurationMs);
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

bool NoteBlockModel::setAudioTranscript(int row, const QString &transcript)
{
    return setData(index(row), transcript, AudioTranscriptRole);
}

bool NoteBlockModel::setAudioTitle(int row, const QString &title)
{
    if (blockTypeAt(row) != Audio)
        return false;
    return setData(index(row), title.trimmed(), AltRole);
}

void NoteBlockModel::appendAttachment(const QString &url, const QString &fileName, const QString &mediaType,
                                      qint64 size)
{
    insertAttachment(blocks_.size(), url, fileName, mediaType, size);
}

void NoteBlockModel::insertAttachment(int row, const QString &url, const QString &fileName, const QString &mediaType,
                                      qint64 size)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type                = Attachment;
    block.url                 = url;
    block.alt                 = fileName;
    block.attachmentMediaType = mediaType;
    block.attachmentSize      = qMax<qint64>(0, size);
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

int NoteBlockModel::blockTypeAt(int row) const
{
    return row >= 0 && row < blocks_.size() ? int(blocks_.at(row).type) : -1;
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
    notifyNormalizedTagLines();
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
    if (tokenText.isEmpty() || tokenText == QLatin1String("*"))
        return removeTagLineTag(row, tagIndex);

    const QStringList parsed = NoteTagLine::parseLine(tokenText);
    if (parsed.isEmpty()) {
        QStringList tokens;
        const auto &tags       = blocks_.at(row).tags;
        int         tokenStart = 0;
        for (int index = 0; index < tags.size(); ++index) {
            const QString token = index == tagIndex ? tokenText : QLatin1Char('*') + tags.at(index);
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
    if (tokenText.isEmpty() || tokenText == QLatin1String("*")) {
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

QList<NoteBlockModel::Block> NoteBlockModel::parseMarkdown(const QString &source)
{
    QString normalized = source;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const auto tagLine = NoteTagLine::findMarkdownDocumentTagLine(normalized);
    if (!tagLine)
        return parseMarkdownCore(normalized);

    QList<Block> result;
    const auto   appendPart = [&result](const QString &part) {
        if (part.trimmed().isEmpty())
            return;
        const auto parsed = parseMarkdownCore(part);
        for (const Block &block : parsed)
            result.append(block);
    };
    appendPart(normalized.left(tagLine->lineStart));
    Block tags;
    tags.type = TagLine;
    tags.tags = tagLine->tags;
    result.append(tags);
    appendPart(normalized.mid(tagLine->lineEnd));
    if (result.isEmpty())
        result.append(Block {});
    return result;
}

QList<NoteBlockModel::Block> NoteBlockModel::parseMarkdownCore(const QString &source)
{
    QString normalized = source;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    struct Fence {
        QChar   marker;
        int     length { 0 };
        QString language;
    };
    const auto openingFence = [](const QString &line, Fence *fence) {
        int offset = 0;
        while (offset < line.size() && offset < 4 && line.at(offset) == QLatin1Char(' '))
            ++offset;
        if (offset > 3 || offset >= line.size())
            return false;
        const QChar marker = line.at(offset);
        if (marker != QLatin1Char('`') && marker != QLatin1Char('~'))
            return false;
        int end = offset;
        while (end < line.size() && line.at(end) == marker)
            ++end;
        if (end - offset < 3)
            return false;
        QString info = line.mid(end).trimmed();
        if (marker == QLatin1Char('`') && info.contains(QLatin1Char('`')))
            return false;
        if (fence) {
            fence->marker   = marker;
            fence->length   = end - offset;
            fence->language = info.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0).trimmed().toLower();
        }
        return true;
    };
    const auto closingFence = [](const QString &line, const Fence &fence) {
        int offset = 0;
        while (offset < line.size() && offset < 4 && line.at(offset) == QLatin1Char(' '))
            ++offset;
        if (offset > 3)
            return false;
        int end = offset;
        while (end < line.size() && line.at(end) == fence.marker)
            ++end;
        return end - offset >= fence.length && line.mid(end).trimmed().isEmpty();
    };

    QList<Block> result;
    const auto   appendMarkdown = [&result](const QStringList &sourceLines, int first, int last) {
        if (last <= first)
            return;
        const QString part = sourceLines.mid(first, last - first).join(QLatin1Char('\n'));
        if (part.trimmed().isEmpty())
            return;
        const auto parsed = parseMarkdownWithoutCode(part);
        for (const auto &block : parsed)
            result.append(block);
    };

    int segmentStart = 0;
    for (int line = 0; line < lines.size();) {
        Fence fence;
        if (!openingFence(lines.at(line), &fence)) {
            ++line;
            continue;
        }

        int close = line + 1;
        while (close < lines.size() && !closingFence(lines.at(close), fence))
            ++close;
        appendMarkdown(lines, segmentStart, line);

        Block block;
        block.type     = CodeBlock;
        block.language = fence.language;
        block.text     = lines.mid(line + 1, close - line - 1).join(QLatin1Char('\n'));
        result.append(block);

        if (close >= lines.size()) {
            segmentStart = lines.size();
            break;
        }
        line         = close + 1;
        segmentStart = line;
    }
    appendMarkdown(lines, segmentStart, lines.size());
    if (result.isEmpty())
        result.append(Block {});
    return result;
}

QList<NoteBlockModel::Block> NoteBlockModel::parseMarkdownWithoutCode(const QString &source)
{
    // QTextDocument is the Markdown reader. Parsing its canonical Markdown keeps
    // Qt's CommonMark interpretation as the single source of truth.
    QTextDocument document;
    QStringList   protectedLines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (auto line = protectedLines.begin(); line != protectedLines.end();) {
        if (line->trimmed() == LegacyEmptyParagraphMarker)
            line = protectedLines.erase(line);
        else
            ++line;
    }
    QString                         protectedSource = protectedLines.join(QLatin1Char('\n'));
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
    const QStringList sourceLines         = protectedSource.split('\n');
    const QStringList canonicalLines      = document.toMarkdown(QTextDocument::MarkdownDialectGitHub).split('\n');
    const auto        hasListContinuation = [](const QStringList &candidate) {
        int contentColumn = -1;
        for (const QString &line : candidate) {
            const auto taskItem     = task.match(line);
            const auto bulletItem   = bullet.match(line);
            const auto numberedItem = numbered.match(line);
            if (taskItem.hasMatch()) {
                contentColumn = taskItem.capturedStart(3);
            } else if (bulletItem.hasMatch()) {
                contentColumn = bulletItem.capturedStart(2);
            } else if (numberedItem.hasMatch()) {
                contentColumn = numberedItem.capturedStart(2);
            } else if (!line.trimmed().isEmpty()) {
                if (contentColumn >= 0 && leadingSpaceCount(line) >= contentColumn)
                    return true;
                contentColumn = -1;
            }
        }
        return false;
    };
    const auto hasTable = [](const QStringList &candidate) {
        for (int index = 0; index + 1 < candidate.size(); ++index) {
            if (candidate.at(index).contains('|') && isTableSeparator(candidate.at(index + 1)))
                return true;
        }
        return false;
    };
    const auto tableColumnCounts = [](const QStringList &candidate) {
        QList<int> result;
        for (int index = 0; index + 1 < candidate.size(); ++index) {
            if (candidate.at(index).contains('|') && isTableSeparator(candidate.at(index + 1)))
                result.append(tableCells(candidate.at(index)).size());
        }
        return result;
    };
    static const QRegularExpression inlineLink(
        QStringLiteral(R"((?<!!)\[(?:\\.|[^\]\\\n])*\]\((?:\\.|[^)\\\n])*\)|<https?://[^>\n]+>)"));
    static const QRegularExpression inlineUnderline(QStringLiteral(R"(<(?:ins|u)(?:\s[^>]*)?>[\s\S]*?</(?:ins|u)\s*>)"),
                                                    QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression inlineCode(QStringLiteral(R"((?<!`)`+[^\n]*?`+)"));
    static const QRegularExpression quote(QStringLiteral(R"(^\s*>\s?(.*)$)"));
    const bool                      preserveInlineSourceLines = inlineLink.match(protectedSource).hasMatch()
        || inlineUnderline.match(protectedSource).hasMatch() || inlineCode.match(protectedSource).hasMatch();
    const bool hasHtmlMedia = std::any_of(sourceLines.cbegin(), sourceLines.cend(), [](const QString &line) {
        return bool(parseHtmlImageBlock(line)) || bool(parseHtmlAudioBlock(line))
            || bool(parseHtmlAttachmentBlock(line));
    });
    // QTextDocument remains the Markdown reader for inline semantics, but its
    // writer wraps long paragraphs (very often around a link). Such a soft
    // wrap becomes a real newline in plain-text mode and can split a list.
    // Preserve source line boundaries for linked content and correctly
    // indented list continuations. Do the same when Qt flattens a GFM table
    // following a task list or reinterprets an optional outer pipe as a
    // phantom empty column.
    const bool tableShapeChanged   = tableColumnCounts(sourceLines) != tableColumnCounts(canonicalLines);
    const bool preserveSourceLines = preserveInlineSourceLines || hasHtmlMedia || hasListContinuation(sourceLines)
        || std::any_of(sourceLines.cbegin(), sourceLines.cend(),
                       [](const QString &line) { return quote.match(line).hasMatch(); })
        || (hasTable(sourceLines) && (!hasTable(canonicalLines) || tableShapeChanged));
    const QStringList              &lines = preserveSourceLines ? sourceLines : canonicalLines;
    QList<Block>                    result;
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
        const auto quoteMatch = quote.match(lines[i]);
        if (quoteMatch.hasMatch()) {
            Block       block;
            QStringList quoteLines;
            block.type = BlockQuote;
            while (i < lines.size()) {
                const auto line = quote.match(lines[i]);
                if (!line.hasMatch())
                    break;
                quoteLines.append(line.captured(1));
                ++i;
            }
            block.text = quoteLines.join(QLatin1Char('\n'));
            result.append(block);
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
                const auto taskItem     = task.match(lines[i]);
                const auto bulletItem   = bullet.match(lines[i]);
                const auto numberedItem = numbered.match(lines[i]);
                if (!taskItem.hasMatch() && !bulletItem.hasMatch() && !numberedItem.hasMatch())
                    break;
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
                const int contentColumn      = taskItem.hasMatch() ? taskItem.capturedStart(3)
                         : bulletItem.hasMatch()                   ? bulletItem.capturedStart(2)
                                                                   : numberedItem.capturedStart(2);
                const int continuationColumn = preserveSourceLines ? contentColumn : candidateIndent.size() + 2;
                ++i;
                while (i < lines.size()) {
                    if (task.match(lines[i]).hasMatch() || bullet.match(lines[i]).hasMatch()
                        || numbered.match(lines[i]).hasMatch())
                        break;
                    if (lines[i].trimmed().isEmpty()) {
                        int next = i + 1;
                        while (next < lines.size() && lines[next].trimmed().isEmpty())
                            ++next;
                        if (next >= lines.size() || task.match(lines[next]).hasMatch()
                            || bullet.match(lines[next]).hasMatch() || numbered.match(lines[next]).hasMatch()
                            || leadingSpaceCount(lines[next]) < continuationColumn)
                            break;
                        if (!preserveSourceLines) {
                            i = next;
                            continue;
                        }
                        while (i < next) {
                            block.items.last() += QLatin1Char('\n');
                            ++i;
                        }
                        continue;
                    }
                    if (leadingSpaceCount(lines[i]) < continuationColumn)
                        break;
                    block.items.last() += QLatin1Char('\n') + lines[i].mid(continuationColumn);
                    ++i;
                }
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
        const HtmlAudioBlock htmlAudio = parseHtmlAudioBlock(lines[i]);
        if (htmlAudio) {
            Block block;
            block.type            = Audio;
            block.url             = htmlAudio.source;
            block.alt             = htmlAudio.title;
            block.audioDurationMs = htmlAudio.durationMs;
            ++i;
            if (i < lines.size()) {
                const QString transcript = parseHtmlAudioTranscript(lines.at(i));
                if (!transcript.isNull()) {
                    block.audioTranscript = transcript;
                    ++i;
                }
            }
            result.append(block);
            continue;
        }
        const HtmlAttachmentBlock htmlAttachment = parseHtmlAttachmentBlock(lines[i]);
        if (htmlAttachment) {
            Block block;
            block.type                = Attachment;
            block.url                 = htmlAttachment.source;
            block.alt                 = htmlAttachment.fileName;
            block.attachmentMediaType = htmlAttachment.mediaType;
            block.attachmentSize      = htmlAttachment.size;
            result.append(block);
            ++i;
            continue;
        }
        const HtmlImageBlock htmlImage = parseHtmlImageBlock(lines[i]);
        if (htmlImage) {
            Block block;
            block.type           = Image;
            block.url            = htmlImage.source;
            block.alt            = htmlImage.alt;
            block.imageWidth     = htmlImage.width;
            block.imageAlignment = htmlImage.alignment;
            result.append(block);
            ++i;
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
               && !quote.match(lines[i]).hasMatch() && !parseHtmlAudioBlock(lines[i])
               && !parseHtmlAttachmentBlock(lines[i]) && !parseHtmlImageBlock(lines[i])
               && !(i + 1 < lines.size() && lines[i].contains('|') && isTableSeparator(lines[i + 1])))
            paragraph.append(lines[i++]);
        const QString text = paragraph.join('\n');
        if (!result.isEmpty() && result.constLast().type == Text && !result.constLast().explicitEmpty) {
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
            // Deliberately empty editor rows are session-only insertion
            // points. Markdown collapses them naturally on save/reload; do
            // not leak a private marker into plain-text mode.
            value = block.text;
            break;
        case TagLine:
            value = NoteTagLine::serialize(block.tags);
            break;
        case Heading:
            value = QString(qBound(1, block.headingLevel, 6), QLatin1Char('#')) + QLatin1Char(' ') + block.text;
            break;
        case BlockQuote: {
            QStringList quotedLines;
            for (const QString &line : block.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts))
                quotedLines.append(line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line);
            value = quotedLines.join(QLatin1Char('\n'));
            break;
        }
        case BulletList:
        case CheckList:
        case NumberedList:
            for (int i = 0; i < block.items.size(); ++i) {
                const auto type    = BlockType(block.itemTypes.value(i, block.type).toInt());
                const int  columns = qMax(0, block.indents.value(i).toInt()) * 4;
                QString    marker;
                if (type == CheckList) {
                    marker = QStringLiteral("- [%1] ").arg(block.checked.value(i).toBool() ? "x" : " ");
                } else if (type == NumberedList) {
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
                    marker = QStringLiteral("%1. ").arg(number);
                } else {
                    marker = QStringLiteral("- ");
                }
                value += serializeListItem(block.items[i], columns, marker) + QLatin1Char('\n');
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
        case CodeBlock: {
            int longestRun = 0;
            int currentRun = 0;
            for (const QChar ch : block.text) {
                if (ch == QLatin1Char('`'))
                    longestRun = qMax(longestRun, ++currentRun);
                else
                    currentRun = 0;
            }
            const QString fence(qMax(3, longestRun + 1), QLatin1Char('`'));
            QString       language = block.language.trimmed().toLower();
            language.remove(QRegularExpression(QStringLiteral("[^a-z0-9_+.#-]")));
            // The final newline before the closing fence is structural. Always
            // write it separately so an actual trailing newline in block.text
            // is represented by an additional empty source line and survives
            // parse -> serialize -> parse unchanged.
            value = fence + language + QLatin1Char('\n') + block.text;
            if (!block.text.isEmpty())
                value += QLatin1Char('\n');
            value += fence;
            break;
        }
        case Image:
            if (block.imageWidth > 0 || normalizedImageAlignment(block.imageAlignment) != QLatin1String("center")) {
                value = serializeHtmlImage(block.url, block.alt, block.imageWidth, block.imageAlignment);
            } else {
                value = QStringLiteral("![%1](%2)").arg(block.alt, block.url);
            }
            break;
        case Audio:
            value = serializeHtmlAudio(block.url, block.alt, block.audioDurationMs, block.audioTranscript);
            break;
        case Attachment:
            value = serializeHtmlAttachment(block.url, block.alt, block.attachmentMediaType, block.attachmentSize);
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
} // namespace AnyKeep
