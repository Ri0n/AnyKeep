#include "../noteblockmodel.h"
#include "../notetagline.h"
#include "private.h"

#include <QRegularExpression>
#include <QTextDocument>
#include <QVector>

#include <algorithm>
#include <limits>

namespace AnyKeep {
using namespace NoteBlockModelPrivate;
namespace {
    const QString LegacyEmptyParagraphMarker = QStringLiteral("<!-- anykeep:empty-paragraph -->");

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
} // namespace AnyKeep
