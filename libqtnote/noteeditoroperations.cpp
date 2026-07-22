#include "noteeditor.h"

#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QMimeData>
#include <QPalette>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextLayout>
#include <QVector>
#include <algorithm>

#include "localmediastore.h"
#include "noteblockmodel.h"
#include "notefragmentmediatransfer.h"
#include "notetransfercontroller.h"

namespace QtNote {
namespace {
    int documentEnd(const QTextDocument *document) { return document ? qMax(0, document->characterCount() - 1) : 0; }

    QString unwrapMarkdownWriterLines(const QString &markdown);
    QString markdownWithSerializedInlineFormats(QTextDocument *document);

    QString markdownRange(QTextDocument *document, int start, int end)
    {
        if (!document)
            return {};
        const int limit = documentEnd(document);
        start           = qBound(0, start, limit);
        end             = qBound(start, end, limit);
        if (start == end)
            return {};
        QTextCursor cursor(document);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextDocument selection;
        QTextCursor   destination(&selection);
        destination.insertFragment(QTextDocumentFragment(cursor));
        QString markdown = unwrapMarkdownWriterLines(markdownWithSerializedInlineFormats(&selection));
        while (markdown.endsWith(QLatin1Char('\n')) || markdown.endsWith(QLatin1Char('\r')))
            markdown.chop(1);
        return markdown;
    }

    QString unwrapMarkdownWriterLines(const QString &markdown)
    {
        const QStringList lines = markdown.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        QString           result;
        bool              previousWasContent = false;
        for (const QString &line : lines) {
            if (line.isEmpty()) {
                if (!result.endsWith(QStringLiteral("\n\n"))) {
                    if (result.endsWith(QLatin1Char('\n')))
                        result += QLatin1Char('\n');
                    else
                        result += QStringLiteral("\n\n");
                }
                previousWasContent = false;
                continue;
            }
            if (previousWasContent) {
                // Two trailing spaces are an explicit Markdown hard break.
                // Every other newline inside one QTextDocument block is a
                // soft wrap introduced by Qt's Markdown writer.
                result += result.endsWith(QStringLiteral("  ")) ? QLatin1Char('\n') : QLatin1Char(' ');
                result += line.trimmed();
            } else {
                result += line;
            }
            previousWasContent = true;
        }
        return result;
    }

    QTextCharFormat formatAt(QTextDocument *document, int position)
    {
        const int limit = documentEnd(document);
        if (!document || position < 0 || position >= limit)
            return {};

        const QTextBlock block = document->findBlock(position);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.contains(position))
                return fragment.charFormat();
        }
        return {};
    }

    void setLinkOnRange(QTextDocument *document, int start, int end, const QString &href)
    {
        if (!document || start >= end)
            return;

        struct FormatRange {
            int             start;
            int             end;
            QTextCharFormat format;
        };
        QVector<FormatRange> ranges;

        for (QTextBlock block = document->findBlock(start); block.isValid() && block.position() < end;
             block            = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid())
                    continue;
                const int rangeStart = qMax(start, fragment.position());
                const int rangeEnd   = qMin(end, fragment.position() + fragment.length());
                if (rangeStart >= rangeEnd)
                    continue;

                QTextCharFormat format = fragment.charFormat();
                if (href.isEmpty()) {
                    format.setAnchor(false);
                    format.clearProperty(QTextFormat::AnchorHref);
                    format.clearProperty(QTextFormat::AnchorName);
                    format.clearProperty(QTextFormat::ForegroundBrush);
                } else {
                    format.setAnchor(true);
                    format.setAnchorHref(href);
                    format.setForeground(QGuiApplication::palette().link());
                }
                ranges.append({ rangeStart, rangeEnd, format });
            }
        }

        for (const auto &range : ranges) {
            QTextCursor cursor(document);
            cursor.setPosition(range.start);
            cursor.setPosition(range.end, QTextCursor::KeepAnchor);
            cursor.setCharFormat(range.format);
        }
    }

    const QString LinkLabelMarkerPrefix = QStringLiteral("QTNOTELINKLABEL7F3A");
    const QString LinkLabelMarkerSuffix = QStringLiteral("END7F3A");

    QString protectLinkLabels(const QString &markdown)
    {
        if (!markdown.contains(QLatin1Char('[')))
            return markdown;

        // Keep the complete Markdown spelling of a link label opaque while
        // NoteBlockModel canonicalizes the document through QTextDocument.
        // QTextDocument otherwise drops or rearranges nested emphasis located
        // in the middle of a word inside a link label.
        static const QRegularExpression link(
            QStringLiteral(R"((?<!!)\[((?:\\.|[^\]\\\n])*)\]\(((?:\\.|[^)\\\n])*)\))"));

        QString result;
        int     copiedUntil = 0;
        auto    links       = link.globalMatch(markdown);
        while (links.hasNext()) {
            const auto       match   = links.next();
            const QByteArray encoded = match.captured(1).toUtf8().toHex().toUpper();

            result += markdown.mid(copiedUntil, match.capturedStart() - copiedUntil);
            result += QLatin1Char('[') + LinkLabelMarkerPrefix + QString::fromLatin1(encoded) + LinkLabelMarkerSuffix
                + QStringLiteral("](") + match.captured(2) + QLatin1Char(')');
            copiedUntil = match.capturedEnd();
        }

        if (copiedUntil == 0)
            return markdown;
        result += markdown.mid(copiedUntil);
        return result;
    }

    QString restoreLinkLabels(const QString &markdown)
    {
        static const QRegularExpression marker(LinkLabelMarkerPrefix + QStringLiteral("([0-9A-Fa-f]+)")
                                               + LinkLabelMarkerSuffix);

        QString result;
        int     copiedUntil = 0;
        auto    markers     = marker.globalMatch(markdown);
        while (markers.hasNext()) {
            const auto       match   = markers.next();
            const QByteArray decoded = QByteArray::fromHex(match.captured(1).toLatin1());

            result += markdown.mid(copiedUntil, match.capturedStart() - copiedUntil);
            result += QString::fromUtf8(decoded);
            copiedUntil = match.capturedEnd();
        }

        if (copiedUntil == 0)
            return markdown;
        result += markdown.mid(copiedUntil);
        return result;
    }

    void restoreProtectedMarkdown(NoteBlockModel *model)
    {
        if (!model)
            return;

        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex modelIndex = model->index(row);
            const auto type = NoteBlockModel::BlockType(model->data(modelIndex, NoteBlockModel::TypeRole).toInt());

            switch (type) {
            case NoteBlockModel::Text:
            case NoteBlockModel::Heading: {
                const QString text     = model->data(modelIndex, NoteBlockModel::TextRole).toString();
                const QString restored = restoreLinkLabels(text);
                if (restored != text)
                    model->setBlockText(row, restored);
                break;
            }
            case NoteBlockModel::BulletList:
            case NoteBlockModel::CheckList:
            case NoteBlockModel::NumberedList: {
                const QStringList items = model->data(modelIndex, NoteBlockModel::ItemsRole).toStringList();
                for (int item = 0; item < items.size(); ++item) {
                    const QString restored = restoreLinkLabels(items.at(item));
                    if (restored != items.at(item))
                        model->setListItem(row, item, restored);
                }
                break;
            }
            case NoteBlockModel::Table: {
                const QVariantMap table = model->data(modelIndex, NoteBlockModel::CellsRole).toMap();
                const QStringList cells = table.value(QStringLiteral("values")).toStringList();
                for (int cell = 0; cell < cells.size(); ++cell) {
                    const QString restored = restoreLinkLabels(cells.at(cell));
                    if (restored != cells.at(cell))
                        model->setTableCell(row, cell, restored);
                }
                break;
            }
            case NoteBlockModel::Image:
                break;
            }
        }
    }
    struct InlineStyle {
        bool bold      = false;
        bool italic    = false;
        bool strike    = false;
        bool code      = false;
        bool underline = false;

        bool operator==(const InlineStyle &other) const
        {
            return bold == other.bold && italic == other.italic && strike == other.strike && code == other.code
                && underline == other.underline;
        }
    };

    struct LinkGroup {
        int                  start = 0;
        int                  end   = 0;
        QString              href;
        QString              text;
        QVector<InlineStyle> styles;
    };

    struct CodeGroup {
        int         start = 0;
        int         end   = 0;
        QString     text;
        InlineStyle style;
    };

    InlineStyle explicitInlineStyle(const QTextCharFormat &format)
    {
        InlineStyle style;
        style.bold      = format.hasProperty(QTextFormat::FontWeight) && format.fontWeight() >= QFont::Bold;
        style.italic    = format.hasProperty(QTextFormat::FontItalic) && format.fontItalic();
        style.strike    = format.hasProperty(QTextFormat::FontStrikeOut) && format.fontStrikeOut();
        style.code      = format.hasProperty(QTextFormat::FontFixedPitch) && format.fontFixedPitch();
        style.underline = format.hasProperty(QTextFormat::TextUnderlineStyle)
            && format.underlineStyle() != QTextCharFormat::NoUnderline;
        return style;
    }

    QVector<LinkGroup> documentLinkGroups(const QTextDocument *document)
    {
        QVector<LinkGroup> groups;
        if (!document)
            return groups;

        int     previousEnd = -1;
        QString previousHref;
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || fragment.length() <= 0)
                    continue;

                const QTextCharFormat format = fragment.charFormat();
                const QString         href   = format.isAnchor() ? format.anchorHref() : QString();
                if (href.isEmpty()) {
                    previousEnd = -1;
                    previousHref.clear();
                    continue;
                }

                const bool continuesGroup
                    = !groups.isEmpty() && previousEnd == fragment.position() && previousHref == href;
                if (!continuesGroup) {
                    LinkGroup group;
                    group.start = fragment.position();
                    group.href  = href;
                    groups.append(group);
                }

                auto             &group        = groups.last();
                const QString     fragmentText = fragment.text();
                const InlineStyle style        = explicitInlineStyle(format);
                group.text += fragmentText;
                for (qsizetype index = 0; index < fragmentText.size(); ++index)
                    group.styles.append(style);

                group.end    = fragment.position() + fragment.length();
                previousEnd  = group.end;
                previousHref = href;
            }
            previousEnd = -1;
            previousHref.clear();
        }
        return groups;
    }

    QVector<CodeGroup> documentCodeGroups(const QTextDocument *document)
    {
        QVector<CodeGroup> groups;
        if (!document)
            return groups;
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || fragment.length() <= 0)
                    continue;
                const QTextCharFormat format = fragment.charFormat();
                const InlineStyle     style  = explicitInlineStyle(format);
                if ((format.isAnchor() && !format.anchorHref().isEmpty()) || !style.code)
                    continue;
                if (!groups.isEmpty() && groups.last().end == fragment.position() && groups.last().style == style) {
                    groups.last().end = fragment.position() + fragment.length();
                    groups.last().text += fragment.text();
                } else {
                    groups.append(
                        { fragment.position(), fragment.position() + fragment.length(), fragment.text(), style });
                }
            }
        }
        return groups;
    }

    QString escapeMarkdownLabelText(const QString &text)
    {
        QString escaped;
        escaped.reserve(text.size() * 2);
        static const QString special = QStringLiteral(R"(\\`*[]_~)");
        for (const QChar character : text) {
            if (character == QChar::ParagraphSeparator || character == QLatin1Char('\n')) {
                escaped += QStringLiteral("<br>");
                continue;
            }
            if (special.contains(character))
                escaped += QLatin1Char('\\');
            escaped += character;
        }
        return escaped;
    }

    QString codeSpan(const QString &text)
    {
        int maximumRun = 0;
        int currentRun = 0;
        for (const QChar character : text) {
            if (character == QLatin1Char('`')) {
                maximumRun = qMax(maximumRun, ++currentRun);
            } else {
                currentRun = 0;
            }
        }
        const QString delimiter(maximumRun + 1, QLatin1Char('`'));
        return delimiter + text + delimiter;
    }

    QVector<QString> inlineDelimiters(const InlineStyle &style)
    {
        QVector<QString> delimiters;
        if (style.underline)
            delimiters.append(QStringLiteral("<ins>"));
        if (style.strike)
            delimiters.append(QStringLiteral("~~"));
        if (style.bold)
            delimiters.append(QStringLiteral("**"));
        if (style.italic)
            delimiters.append(QStringLiteral("*"));
        return delimiters;
    }

    QString closingInlineDelimiter(const QString &delimiter)
    {
        return delimiter == QLatin1String("<ins>") ? QStringLiteral("</ins>") : delimiter;
    }

    QString serializedMarkdownCode(CodeGroup group)
    {
        group.style.code                  = false;
        const QVector<QString> delimiters = inlineDelimiters(group.style);
        QString                result;
        for (const QString &delimiter : delimiters)
            result += delimiter;
        result += codeSpan(group.text);
        for (int index = delimiters.size() - 1; index >= 0; --index)
            result += closingInlineDelimiter(delimiters.at(index));
        return result;
    }

    QString formattedLinkRange(const LinkGroup &group, int start, int length)
    {
        const int        end = qMin(start + length, int(group.text.size()));
        QString          result;
        QVector<QString> activeDelimiters;

        for (int position = qMax(0, start); position < end;) {
            const InlineStyle style      = group.styles.value(position);
            int               segmentEnd = position + 1;
            while (segmentEnd < end && group.styles.value(segmentEnd) == style)
                ++segmentEnd;

            const QVector<QString> desiredDelimiters = inlineDelimiters(style);
            int                    common            = 0;
            while (common < activeDelimiters.size() && common < desiredDelimiters.size()
                   && activeDelimiters.at(common) == desiredDelimiters.at(common)) {
                ++common;
            }
            for (int index = activeDelimiters.size() - 1; index >= common; --index)
                result += closingInlineDelimiter(activeDelimiters.at(index));
            for (int index = common; index < desiredDelimiters.size(); ++index)
                result += desiredDelimiters.at(index);

            const QString segment = group.text.mid(position, segmentEnd - position);
            result += style.code ? codeSpan(segment) : escapeMarkdownLabelText(segment);
            activeDelimiters = desiredDelimiters;
            position         = segmentEnd;
        }

        for (int index = activeDelimiters.size() - 1; index >= 0; --index)
            result += closingInlineDelimiter(activeDelimiters.at(index));
        return result;
    }

    QString markdownLinkDestination(QString href)
    {
        href.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        href.replace(QLatin1Char('('), QStringLiteral("\\("));
        href.replace(QLatin1Char(')'), QStringLiteral("\\)"));
        return href;
    }

    QString serializedMarkdownLink(const LinkGroup &group)
    {
        return QLatin1Char('[') + formattedLinkRange(group, 0, group.text.size()) + QStringLiteral("](")
            + markdownLinkDestination(group.href) + QLatin1Char(')');
    }

    struct UnderlineRange {
        int start = 0;
        int end   = 0;
    };

    QVector<UnderlineRange> documentUnderlineRanges(const QTextDocument *document)
    {
        QVector<UnderlineRange> ranges;
        if (!document)
            return ranges;
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || fragment.length() <= 0)
                    continue;
                const QTextCharFormat format = fragment.charFormat();
                if ((format.isAnchor() && !format.anchorHref().isEmpty()) || explicitInlineStyle(format).code
                    || !format.fontUnderline())
                    continue;
                if (!ranges.isEmpty() && ranges.last().end == fragment.position())
                    ranges.last().end += fragment.length();
                else
                    ranges.append({ fragment.position(), fragment.position() + fragment.length() });
            }
        }
        return ranges;
    }

    QString markdownWithSerializedInlineFormats(QTextDocument *document)
    {
        const QVector<LinkGroup>      links        = documentLinkGroups(document);
        const QVector<CodeGroup>      codes        = documentCodeGroups(document);
        const QVector<UnderlineRange> underlines   = documentUnderlineRanges(document);
        bool                          hasUnderline = !underlines.isEmpty();
        for (const LinkGroup &group : links)
            hasUnderline = hasUnderline
                || std::any_of(group.styles.cbegin(), group.styles.cend(),
                               [](const InlineStyle &style) { return style.underline; });
        for (const CodeGroup &group : codes)
            hasUnderline = hasUnderline || group.style.underline;
        if (links.isEmpty() && codes.isEmpty() && !hasUnderline)
            return document->toMarkdown(QTextDocument::MarkdownDialectGitHub);

        std::unique_ptr<QTextDocument> copy(document->clone());
        if (hasUnderline) {
            QTextCursor all(copy.get());
            all.select(QTextCursor::Document);
            QTextCharFormat clearUnderline;
            clearUnderline.setFontUnderline(false);
            all.mergeCharFormat(clearUnderline);
        }

        QVector<QString> linkMarkers;
        linkMarkers.reserve(links.size());
        for (int index = 0; index < links.size(); ++index) {
            linkMarkers.append(QStringLiteral("QTNOTESERIALIZEDLINK7F3A%1END7F3A").arg(index));
        }
        QVector<QString> codeMarkers;
        codeMarkers.reserve(codes.size());
        for (int index = 0; index < codes.size(); ++index)
            codeMarkers.append(QStringLiteral("QTNOTESERIALIZEDCODE8C53%1END8C53").arg(index));
        QVector<QPair<QString, QString>> underlineMarkers;
        underlineMarkers.reserve(underlines.size());
        for (int index = 0; index < underlines.size(); ++index) {
            underlineMarkers.append({ QStringLiteral("QTNOTEINSOPEN8C53%1END8C53").arg(index),
                                      QStringLiteral("QTNOTEINSCLOSE8C53%1END8C53").arg(index) });
        }

        struct Replacement {
            enum Kind { Underline, Link, Code };
            int  start = 0;
            int  end   = 0;
            int  index = 0;
            Kind kind  = Underline;
        };
        QVector<Replacement> replacements;
        replacements.reserve(links.size() + codes.size() + underlines.size());
        for (int index = 0; index < links.size(); ++index)
            replacements.append({ links.at(index).start, links.at(index).end, index, Replacement::Link });
        for (int index = 0; index < codes.size(); ++index)
            replacements.append({ codes.at(index).start, codes.at(index).end, index, Replacement::Code });
        for (int index = 0; index < underlines.size(); ++index)
            replacements.append(
                { underlines.at(index).start, underlines.at(index).end, index, Replacement::Underline });
        std::sort(replacements.begin(), replacements.end(),
                  [](const Replacement &left, const Replacement &right) { return left.start > right.start; });

        for (const Replacement &replacement : replacements) {
            if (replacement.kind == Replacement::Link || replacement.kind == Replacement::Code) {
                QTextCursor cursor(copy.get());
                cursor.setPosition(replacement.start);
                cursor.setPosition(replacement.end, QTextCursor::KeepAnchor);
                const QString &marker = replacement.kind == Replacement::Link ? linkMarkers.at(replacement.index)
                                                                              : codeMarkers.at(replacement.index);
                cursor.insertText(marker, QTextCharFormat());
                continue;
            }
            QTextCursor close(copy.get());
            close.setPosition(replacement.end);
            close.insertText(underlineMarkers.at(replacement.index).second, QTextCharFormat());
            QTextCursor open(copy.get());
            open.setPosition(replacement.start);
            open.insertText(underlineMarkers.at(replacement.index).first, QTextCharFormat());
        }

        QString markdown = copy->toMarkdown(QTextDocument::MarkdownDialectGitHub);
        for (int index = 0; index < links.size(); ++index)
            markdown.replace(linkMarkers.at(index), serializedMarkdownLink(links.at(index)));
        for (int index = 0; index < codes.size(); ++index)
            markdown.replace(codeMarkers.at(index), serializedMarkdownCode(codes.at(index)));
        for (int index = 0; index < underlines.size(); ++index) {
            markdown.replace(underlineMarkers.at(index).first, QStringLiteral("<ins>"));
            markdown.replace(underlineMarkers.at(index).second, QStringLiteral("</ins>"));
        }
        return markdown;
    }

    QList<NoteBlockSelectionRange> decodeSelectionRanges(const QVariantList &encodedRanges)
    {
        QList<NoteBlockSelectionRange> ranges;
        ranges.reserve(encodedRanges.size());
        for (const QVariant &encoded : encodedRanges) {
            const QVariantMap       map = encoded.toMap();
            NoteBlockSelectionRange range;
            range.blockIndex     = map.value(QStringLiteral("blockIndex"), -1).toInt();
            range.listItemIndex  = map.value(QStringLiteral("listItemIndex"), -1).toInt();
            range.tableCellIndex = map.value(QStringLiteral("tableCellIndex"), -1).toInt();
            range.markdown       = map.value(QStringLiteral("markdown")).toString();
            range.wholeEditor    = map.value(QStringLiteral("wholeEditor")).toBool();
            range.before         = map.value(QStringLiteral("before")).toString();
            range.after          = map.value(QStringLiteral("after")).toString();
            ranges.append(range);
        }
        return ranges;
    }

}

void NoteEditor::loadDocument(const QString &contents, Note::Format format, LoadPolicy policy)
{
    const bool        markdown                 = format != Note::PlainText;
    const bool        modeChanged              = model_->markdown() != markdown;
    const bool        formatConversion         = policy == LoadPolicy::RecordFormatConversion && modeChanged;
    const QVariantMap beforeView               = captureEditorViewState();
    const bool        nestedHistoryTransaction = historyInTransaction();

    beginHistoryTransaction(formatConversion ? QStringLiteral("format-conversion") : QStringLiteral("load"),
                            beforeView);
    model_->load(markdown ? protectLinkLabels(contents) : contents, markdown);
    if (markdown)
        restoreProtectedMarkdown(model_);

    if (formatConversion)
        endHistoryTransaction(beforeView);
    else {
        resetContent(model_->contents(), markdown ? Note::Markdown : Note::PlainText);
        resetHistory();
    }

    if (formatConversion && !nestedHistoryTransaction)
        scheduleEditorViewRestore(beforeView);
    emit documentLoaded(modeChanged, formatConversion);
}

bool NoteEditor::primaryModifierPressed() const
{
    const auto modifiers = QGuiApplication::keyboardModifiers();
    return modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier);
}

QVariantMap NoteEditor::linkInfo(QQuickTextDocument *quickDocument, int start, int end) const
{
    QVariantMap result;
    result.insert(QStringLiteral("valid"), false);
    if (!quickDocument || !quickDocument->textDocument())
        return result;

    auto     *document = quickDocument->textDocument();
    const int limit    = documentEnd(document);
    start              = qBound(0, start, limit);
    end                = qBound(start, end, limit);

    QString href;
    if (start == end && limit > 0) {
        int             probe  = qMin(start, limit - 1);
        QTextCharFormat format = formatAt(document, probe);
        if ((!format.isAnchor() || format.anchorHref().isEmpty()) && probe > 0) {
            --probe;
            format = formatAt(document, probe);
        }
        if (format.isAnchor() && !format.anchorHref().isEmpty()) {
            href  = format.anchorHref();
            start = probe;
            while (start > 0) {
                const QTextCharFormat previous = formatAt(document, start - 1);
                if (!previous.isAnchor() || previous.anchorHref() != href)
                    break;
                --start;
            }
            end = probe + 1;
            while (end < limit) {
                const QTextCharFormat next = formatAt(document, end);
                if (!next.isAnchor() || next.anchorHref() != href)
                    break;
                ++end;
            }
        }
    } else if (start < end) {
        const QTextCharFormat first = formatAt(document, start);
        if (first.isAnchor() && !first.anchorHref().isEmpty()) {
            const QString candidate = first.anchorHref();
            bool          sameLink  = true;
            for (int position = start + 1; position < end; ++position) {
                const QTextCharFormat format = formatAt(document, position);
                if (!format.isAnchor() || format.anchorHref() != candidate) {
                    sameLink = false;
                    break;
                }
            }
            if (sameLink)
                href = candidate;
        }
    }

    result.insert(QStringLiteral("valid"), true);
    result.insert(QStringLiteral("start"), start);
    result.insert(QStringLiteral("end"), end);
    result.insert(QStringLiteral("href"), href);
    return result;
}

int NoteEditor::setLink(QQuickTextDocument *quickDocument, int start, int end, const QString &href)
{
    if (!quickDocument || !quickDocument->textDocument())
        return -1;

    auto     *document = quickDocument->textDocument();
    const int limit    = documentEnd(document);
    start              = qBound(0, start, limit);
    end                = qBound(start, end, limit);

    QTextCursor cursor(document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    if (!cursor.hasSelection() && !href.isEmpty()) {
        const QString placeholder = tr("link");
        cursor.insertText(placeholder);
        end = start + placeholder.size();
    }

    if (start < end)
        setLinkOnRange(document, start, end, href);
    return end;
}

int NoteEditor::applyInlineFormat(QQuickTextDocument *quickDocument, int start, int end, const QString &style)
{
    if (!quickDocument || !quickDocument->textDocument())
        return -1;
    auto *document = quickDocument->textDocument();
    start          = qBound(0, start, document->characterCount() - 1);
    end            = qBound(start, end, document->characterCount() - 1);
    QTextCursor cursor(document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);

    QString placeholder;
    if (!cursor.hasSelection()) {
        placeholder = style == QLatin1String("code") ? tr("code")
            : style == QLatin1String("link")         ? tr("link")
                                                     : tr("text");
        cursor.insertText(placeholder);
        cursor.setPosition(start);
        cursor.setPosition(start + placeholder.size(), QTextCursor::KeepAnchor);
        end = start + placeholder.size();
    }

    const auto      current = cursor.charFormat();
    QTextCharFormat format;
    if (style == QLatin1String("bold"))
        format.setFontWeight(current.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
    else if (style == QLatin1String("italic"))
        format.setFontItalic(!current.fontItalic());
    else if (style == QLatin1String("strike"))
        format.setFontStrikeOut(!current.fontStrikeOut());
    else if (style == QLatin1String("underline"))
        format.setFontUnderline(!current.fontUnderline());
    else if (style == QLatin1String("code")) {
        format.setFontFixedPitch(!current.fontFixedPitch());
        if (!current.fontFixedPitch()) {
            format.setFontFamilies({ QStringLiteral("monospace") });
        } else {
            format.setFontFamilies(document->defaultFont().families());
        }
    } else if (style == QLatin1String("link")) {
        format.setAnchor(!current.isAnchor());
        format.setAnchorHref(current.isAnchor() ? QString() : QStringLiteral("url"));
    } else {
        return -1;
    }
    cursor.mergeCharFormat(format);
    return end;
}

void NoteEditor::applyInlineHtmlFormatting(QQuickTextDocument *quickDocument) const
{
    if (!quickDocument || !quickDocument->textDocument())
        return;

    static const QString openMarker  = QStringLiteral("QTNOTEINSOPEN7F3A");
    static const QString closeMarker = QStringLiteral("QTNOTEINSCLOSE7F3A");
    auto                *document    = quickDocument->textDocument();
    QTextCursor          search(document);
    while (true) {
        QTextCursor open = document->find(openMarker, search);
        if (open.isNull())
            break;
        QTextCursor close = document->find(closeMarker, open);
        if (close.isNull())
            break;

        const int start = open.selectionStart();
        const int end   = close.selectionStart() - openMarker.size();
        close.removeSelectedText();
        open.removeSelectedText();

        QTextCursor content(document);
        content.setPosition(start);
        content.setPosition(qMax(start, end), QTextCursor::KeepAnchor);
        QTextCharFormat underline;
        underline.setFontUnderline(true);
        content.mergeCharFormat(underline);
        search.setPosition(qMax(start, end));
    }
}

QString NoteEditor::markdownText(QQuickTextDocument *quickDocument) const
{
    if (!quickDocument || !quickDocument->textDocument())
        return {};

    auto   *document = quickDocument->textDocument();
    QString markdown = unwrapMarkdownWriterLines(markdownWithSerializedInlineFormats(document));
    while (markdown.endsWith(QLatin1Char('\n')) || markdown.endsWith(QLatin1Char('\r')))
        markdown.chop(1);
    return markdown;
}

QString NoteEditor::markdownTableCellText(QQuickTextDocument *quickDocument) const
{
    if (!quickDocument || !quickDocument->textDocument())
        return {};

    // QTextDocument serializes a visual line separator as an ordinary
    // newline, indistinguishable from its own soft wrapping.  Protect every
    // intentional separator before invoking the normal Markdown serializer,
    // then restore it using the table-safe GFM spelling.
    std::unique_ptr<QTextDocument> document(quickDocument->textDocument()->clone());
    const QString                  marker = QStringLiteral("QTNOTETABLEHARDBREAK8C53");
    for (int position = document->characterCount() - 2; position >= 0; --position) {
        const QChar character = document->characterAt(position);
        if (character != QChar::LineSeparator && character != QChar::ParagraphSeparator)
            continue;
        QTextCursor cursor(document.get());
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        cursor.insertText(marker, QTextCharFormat());
    }

    QString markdown = unwrapMarkdownWriterLines(markdownWithSerializedInlineFormats(document.get()));
    while (markdown.endsWith(QLatin1Char('\n')) || markdown.endsWith(QLatin1Char('\r')))
        markdown.chop(1);
    markdown.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    markdown.replace(marker, QStringLiteral("<br>"));
    return markdown;
}

QString NoteEditor::markdownSelection(QQuickTextDocument *quickDocument, int start, int end) const
{
    if (!quickDocument || !quickDocument->textDocument())
        return {};
    return markdownRange(quickDocument->textDocument(), start, end);
}

NoteFragment NoteEditor::documentFragment() const
{
    return withMedia(model_->extractBlockFragment(0, model_->rowCount() - 1));
}

NoteFragment NoteEditor::withMedia(NoteFragment fragment) const
{
    for (const NoteFragmentBlock &block : std::as_const(fragment.blocks)) {
        if (block.type != NoteFragmentBlockType::Image)
            continue;
        for (const MediaReference &reference : media()) {
            if (reference.isValid() && reference.uri() == block.image.sourceUri) {
                NoteFragmentMedia media;
                media.sourceUri = block.image.sourceUri;
                media.reference = reference;
                fragment.media.append(media);
                break;
            }
        }
    }

    // A single copied image is also useful outside QtNote.  Keep its PNG data
    // reasonably bounded; larger images retain their internal blob reference.
    if (fragment.blocks.size() == 1 && fragment.blocks.constFirst().type == NoteFragmentBlockType::Image
        && fragment.media.size() == 1
        && fragment.media.constFirst().reference.size <= NoteTransferController::PortableImageDataLimit) {
        const auto data = LocalMediaStore::instance()->data(fragment.media.constFirst().reference.blobId);
        if (data)
            fragment.media.first().data = data.value;
    }
    return fragment;
}

void NoteEditor::copyToClipboard(const QString &text)
{
    NoteFragment fragment;
    fragment.sourceFormat = NoteFragmentSourceFormat::PlainText;
    NoteFragmentBlock block;
    block.type     = NoteFragmentBlockType::Text;
    block.markdown = text;
    fragment.blocks.append(block);

    NoteTransferController controller;
    auto                   exported = controller.createMimeData(fragment);
    if (exported)
        QGuiApplication::clipboard()->setMimeData(exported.mimeData.release());
    else
        QGuiApplication::clipboard()->setText(text);
}

void NoteEditor::copyMarkdownToClipboard(const QString &markdown)
{
    // The visible selection is Markdown, not one literal text block.  Keeping
    // it as a Text block would make our private MIME format treat list/table
    // syntax as ordinary characters on the next QtNote paste.  Parse it back
    // through the block model so the private and public representations carry
    // the same structure.
    NoteBlockModel model;
    model.load(markdown, true);
    NoteFragment fragment = model.extractBlockFragment(0, model.rowCount() - 1);
    fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;

    NoteTransferController controller;
    auto                   exported = controller.createMimeData(fragment);
    if (exported) {
        QGuiApplication::clipboard()->setMimeData(exported.mimeData.release());
        qInfo() << "QML clipboard copy: selection blocks=" << fragment.blocks.size()
                << "formats=" << QGuiApplication::clipboard()->mimeData()->formats();
    } else {
        QGuiApplication::clipboard()->setText(markdown);
        qWarning() << "QML clipboard copy fell back to plain text:" << exported.error;
    }
}

void NoteEditor::copyDocumentToClipboard()
{
    NoteTransferController controller;
    const NoteFragment     fragment = documentFragment();
    auto                   exported = controller.createMimeData(fragment);
    if (exported) {
        QGuiApplication::clipboard()->setMimeData(exported.mimeData.release());
        qInfo() << "QML clipboard copy: whole document blocks=" << fragment.blocks.size()
                << "formats=" << QGuiApplication::clipboard()->mimeData()->formats();
    } else {
        QGuiApplication::clipboard()->setText(model_->contents());
        qWarning() << "QML clipboard copy fell back to plain text:" << exported.error;
    }
}

bool NoteEditor::copySelectionToClipboard(const QVariantList &encodedRanges)
{
    NoteFragment fragment = withMedia(model_->extractSelectionFragment(decodeSelectionRanges(encodedRanges)));
    if (fragment.blocks.isEmpty())
        return false;

    NoteTransferController controller;
    auto                   exported = controller.createMimeData(fragment);
    if (!exported) {
        qWarning() << "QML structured selection copy failed:" << exported.error;
        return false;
    }
    QGuiApplication::clipboard()->setMimeData(exported.mimeData.release());
    qInfo() << "QML clipboard copy: structured selection blocks=" << fragment.blocks.size()
            << "formats=" << QGuiApplication::clipboard()->mimeData()->formats();
    return true;
}

QVariantMap NoteEditor::deleteSelection(const QVariantList &encodedRanges)
{
    const QList<NoteBlockSelectionRange> ranges   = decodeSelectionRanges(encodedRanges);
    const int                            focusRow = model_->removeSelectionRanges(ranges);
    if (focusRow < 0)
        return {};
    QVariantMap result { { QStringLiteral("handled"), true }, { QStringLiteral("focusRow"), focusRow } };
    // If the selection started inside an editor, its prefix survived at the
    // same structural address. Preserve the exact QTextDocument position of
    // that boundary instead of merely focusing the beginning of the block.
    if (!ranges.isEmpty() && !ranges.constFirst().before.isEmpty() && !encodedRanges.isEmpty()) {
        const QVariantMap first = encodedRanges.constFirst().toMap();
        result.insert(QStringLiteral("focusPosition"), first.value(QStringLiteral("selectionStart"), 0));
    }
    return result;
}

QVariantMap NoteEditor::pasteStructuredFromClipboard(QQuickTextDocument *quickDocument, int row, int start, int end)
{
    QVariantMap result;
    if (!model_->markdown() || !quickDocument || !quickDocument->textDocument())
        return result;

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(QGuiApplication::clipboard()->mimeData());
    if (!imported || imported.sourceMimeType == QStringLiteral("text/plain") || imported.hasImage()
        || imported.fragment.blocks.isEmpty()) {
        return result;
    }

    NoteFragment          fragment = imported.fragment;
    QList<MediaReference> insertedMedia;
    if (!fragment.media.isEmpty()) {
        const auto cloned = NoteFragmentMediaTransfer::cloneForDestination(fragment, *LocalMediaStore::instance(),
                                                                           LocalMediaStore::instance());
        if (!cloned) {
            result.insert(QStringLiteral("error"), cloned.error);
            return result;
        }
        fragment      = cloned.fragment;
        insertedMedia = cloned.importedMedia;
    }

    QTextDocument *document = quickDocument->textDocument();
    const int      limit    = documentEnd(document);

    QString   error;
    const int insertedRow = model_->replaceTextBlockRangeWithFragment(
        row, markdownRange(document, 0, start), markdownRange(document, end, limit), fragment, &error);
    if (insertedRow < 0) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("focusRow"), insertedRow);
    if (!insertedMedia.isEmpty()) {
        auto manifest = media();
        manifest.append(insertedMedia);
        setMedia(manifest);
        emit mediaInserted(insertedMedia);
    }
    return result;
}

QVariantMap NoteEditor::pasteTableFromClipboard(int row, int cell)
{
    QVariantMap result;
    if (!model_->markdown())
        return result;
    NoteTransferController controller;
    const auto             imported = controller.importMimeData(QGuiApplication::clipboard()->mimeData());
    if (!imported || imported.sourceMimeType == QStringLiteral("text/plain") || imported.hasImage()
        || !imported.fragment.media.isEmpty() || imported.fragment.blocks.size() != 1
        || imported.fragment.blocks.constFirst().type != NoteFragmentBlockType::Table) {
        return result;
    }

    QString error;
    if (!model_->replaceTableCellsWithFragment(row, cell, imported.fragment, &error)) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }
    result.insert(QStringLiteral("handled"), true);
    return result;
}

QVariantMap NoteEditor::pasteListFromClipboard(QQuickTextDocument *quickDocument, int row, int item, int start, int end)
{
    QVariantMap result;
    if (!model_->markdown() || !quickDocument || !quickDocument->textDocument())
        return result;
    NoteTransferController controller;
    const auto             imported = controller.importMimeData(QGuiApplication::clipboard()->mimeData());
    if (!imported || imported.sourceMimeType == QStringLiteral("text/plain") || imported.hasImage()
        || !imported.fragment.media.isEmpty() || imported.fragment.blocks.size() != 1
        || imported.fragment.blocks.constFirst().type != NoteFragmentBlockType::List) {
        return result;
    }

    QTextDocument *document = quickDocument->textDocument();
    const int      limit    = documentEnd(document);
    QString        error;
    const int      focusItem = model_->replaceListItemRangeWithFragment(
        row, item, markdownRange(document, 0, start), markdownRange(document, end, limit), imported.fragment, &error);
    if (focusItem < 0) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("focusItem"), focusItem);
    return result;
}

} // namespace QtNote
