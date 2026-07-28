#include "notetransfercontroller.h"

#include "noteblockmodel.h"

#include <QBuffer>
#include <QMimeData>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextTable>
#include <QUrl>

#include <algorithm>

namespace QtNote {
namespace {

    QString normalizePlainText(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        text.replace(QChar::LineSeparator, QLatin1Char('\n'));
        text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        return text;
    }

    NoteTransferController::ImportResult failed(const QString &error)
    {
        NoteTransferController::ImportResult result;
        result.error = error;
        return result;
    }

    QString markdownFromFragment(const NoteFragment &fragment, QString *error)
    {
        NoteBlockModel model;
        // NoteBlockModel defaults to plain text.  A transfer fragment is rendered
        // as GFM regardless of the source mode so that all public clipboard
        // formats preserve the structural representation.
        model.load(QString(), true);
        model.removeBlock(0);
        QString modelError;
        if (!model.insertBlockFragment(0, fragment, &modelError)) {
            if (error)
                *error = modelError;
            return {};
        }
        return model.contents();
    }

    NoteTransferController::ImportResult fragmentFromMarkdown(const QString &markdown, const QString &sourceMimeType)
    {
        NoteBlockModel model;
        model.load(markdown, true);

        NoteTransferController::ImportResult result;
        result.fragment              = model.extractBlockFragment(0, model.rowCount() - 1);
        result.fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
        result.sourceMimeType        = sourceMimeType;
        return result;
    }

    struct UnderlineRange {
        int start = 0;
        int end   = 0;
    };

    QVector<UnderlineRange> underlineRanges(const QTextDocument *document)
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
                // HTML links are commonly underlined by their presentation
                // style. That is not an explicit <u>/<ins> annotation and
                // must not leak into the imported Markdown.
                if ((format.isAnchor() && !format.anchorHref().isEmpty()) || !format.fontUnderline())
                    continue;
                if (!ranges.isEmpty() && ranges.last().end == fragment.position())
                    ranges.last().end += fragment.length();
                else
                    ranges.append({ fragment.position(), fragment.position() + fragment.length() });
            }
        }
        return ranges;
    }

    QString markdownWithGithubUnderlines(const QTextDocument *source)
    {
        const QVector<UnderlineRange> ranges = underlineRanges(source);
        if (ranges.isEmpty())
            return source ? source->toMarkdown(QTextDocument::MarkdownDialectGitHub) : QString();

        std::unique_ptr<QTextDocument> document(source->clone());
        QTextCharFormat                clearUnderline;
        clearUnderline.setFontUnderline(false);
        for (const UnderlineRange &range : ranges) {
            QTextCursor cursor(document.get());
            cursor.setPosition(range.start);
            cursor.setPosition(range.end, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(clearUnderline);
        }

        QVector<QPair<QString, QString>> markers;
        markers.reserve(ranges.size());
        for (int index = 0; index < ranges.size(); ++index) {
            markers.append({ QStringLiteral("QTNOTEINSOPEN7F3A%1END7F3A").arg(index),
                             QStringLiteral("QTNOTEINSCLOSE7F3A%1END7F3A").arg(index) });
        }
        for (int index = ranges.size() - 1; index >= 0; --index) {
            QTextCursor close(document.get());
            close.setPosition(ranges.at(index).end);
            close.insertText(markers.at(index).second, QTextCharFormat());
            QTextCursor open(document.get());
            open.setPosition(ranges.at(index).start);
            open.insertText(markers.at(index).first, QTextCharFormat());
        }

        QString markdown = document->toMarkdown(QTextDocument::MarkdownDialectGitHub);
        for (int index = 0; index < markers.size(); ++index) {
            markdown.replace(markers.at(index).first, QStringLiteral("<ins>"));
            markdown.replace(markers.at(index).second, QStringLiteral("</ins>"));
        }
        return markdown;
    }

    QString protectGithubUnderlineTags(QString markdown)
    {
        static const QRegularExpression underline(QStringLiteral(R"(^(<(ins|u)(?:\s[^>]*)?>([\s\S]*?)</\2\s*>))"),
                                                  QRegularExpression::CaseInsensitiveOption);
        const QString                   openMarker  = QStringLiteral("QTNOTEINSOPEN7F3A");
        const QString                   closeMarker = QStringLiteral("QTNOTEINSCLOSE7F3A");
        QString                         result;
        QString                         codeDelimiter;
        for (int index = 0; index < markdown.size();) {
            const QChar character = markdown.at(index);
            if (character == QLatin1Char('\\') && index + 1 < markdown.size()) {
                result += markdown.mid(index, 2);
                index += 2;
                continue;
            }
            if (character == QLatin1Char('`')) {
                int end = index;
                while (end < markdown.size() && markdown.at(end) == QLatin1Char('`'))
                    ++end;
                const QString delimiter = markdown.mid(index, end - index);
                if (codeDelimiter.isEmpty() || codeDelimiter == delimiter)
                    codeDelimiter = codeDelimiter.isEmpty() ? delimiter : QString();
                result += delimiter;
                index = end;
                continue;
            }
            if (codeDelimiter.isEmpty() && character == QLatin1Char('<')) {
                const QRegularExpressionMatch match
                    = underline.match(markdown.mid(index), 0, QRegularExpression::NormalMatch,
                                      QRegularExpression::AnchorAtOffsetMatchOption);
                if (match.hasMatch()) {
                    result += openMarker + match.captured(3) + closeMarker;
                    index += match.capturedLength();
                    continue;
                }
            }
            result += character;
            ++index;
        }
        return result;
    }

    void applyGithubUnderlineMarkers(QTextDocument *document)
    {
        if (!document)
            return;
        static const QString openMarker  = QStringLiteral("QTNOTEINSOPEN7F3A");
        static const QString closeMarker = QStringLiteral("QTNOTEINSCLOSE7F3A");
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
            QTextCharFormat format;
            format.setFontUnderline(true);
            content.mergeCharFormat(format);
            search.setPosition(qMax(start, end));
        }
    }

    void setMarkdownPreservingGithubUnderlines(QTextDocument *document, const QString &markdown)
    {
        if (!document)
            return;
        document->setMarkdown(protectGithubUnderlineTags(markdown), QTextDocument::MarkdownDialectGitHub);
        applyGithubUnderlineMarkers(document);
    }

    QString markdownFromDocumentRange(QTextDocument *document, int start, int end)
    {
        if (!document || start >= end)
            return {};
        const int   limit = qMax(0, document->characterCount() - 1);
        QTextCursor cursor(document);
        cursor.setPosition(qBound(0, start, limit));
        cursor.setPosition(qBound(0, end, limit), QTextCursor::KeepAnchor);
        QTextDocument selected;
        QTextCursor   destination(&selected);
        destination.insertFragment(QTextDocumentFragment(cursor));
        QString markdown = markdownWithGithubUnderlines(&selected);
        while (markdown.endsWith(QLatin1Char('\n')) || markdown.endsWith(QLatin1Char('\r')))
            markdown.chop(1);
        return markdown;
    }

    void appendMarkdownBlocks(NoteFragment *destination, const QString &markdown)
    {
        if (!destination || markdown.trimmed().isEmpty())
            return;
        const auto parsed = fragmentFromMarkdown(markdown, QStringLiteral("text/html"));
        destination->blocks.append(parsed.fragment.blocks);
    }

    NoteFragmentBlock tableBlockFromDocument(QTextTable *table)
    {
        NoteFragmentBlock block;
        block.type             = NoteFragmentBlockType::Table;
        block.table.rows       = table ? table->rows() : 0;
        block.table.columns    = table ? table->columns() : 0;
        block.table.headerRows = table ? qBound(0, table->format().headerRowCount(), table->rows()) : 0;
        if (!table)
            return block;

        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                const QTextTableCell cell = table->cellAt(row, column);
                // QTextTable::cellAt returns the spanning cell for every
                // covered coordinate. Keep the top-left value once and leave
                // the remaining rectangle empty in our rectangular model.
                if (!cell.isValid() || cell.row() != row || cell.column() != column) {
                    block.table.markdownCells.append(QString());
                    continue;
                }
                block.table.markdownCells.append(markdownFromDocumentRange(
                    table->document(), cell.firstCursorPosition().position(), cell.lastCursorPosition().position()));
            }
        }
        return block;
    }

    NoteTransferController::ImportResult fragmentFromHtml(const QString &html)
    {
        QTextDocument document;
        document.setHtml(html);

        NoteTransferController::ImportResult result;
        result.sourceMimeType        = QStringLiteral("text/html");
        result.fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;

        QTextFrame *root      = document.rootFrame();
        int         textStart = -1;
        int         textEnd   = -1;
        const auto  flushText = [&] {
            if (textStart >= 0)
                appendMarkdownBlocks(&result.fragment, markdownFromDocumentRange(&document, textStart, textEnd));
            textStart = -1;
            textEnd   = -1;
        };

        for (auto iterator = root->begin(); !iterator.atEnd(); ++iterator) {
            if (const QTextBlock block = iterator.currentBlock(); block.isValid()) {
                if (block.text().trimmed().isEmpty())
                    continue;
                if (textStart < 0)
                    textStart = block.position();
                textEnd = block.position() + block.length();
                continue;
            }

            QTextFrame *frame = iterator.currentFrame();
            flushText();
            if (auto *table = dynamic_cast<QTextTable *>(frame)) {
                NoteFragmentBlock block = tableBlockFromDocument(table);
                if (block.table.rows > 0 && block.table.columns > 0)
                    result.fragment.blocks.append(block);
            } else if (frame) {
                appendMarkdownBlocks(
                    &result.fragment,
                    markdownFromDocumentRange(&document, frame->firstPosition(), frame->lastPosition()));
            }
        }
        flushText();

        if (result.fragment.blocks.isEmpty())
            return fragmentFromMarkdown(markdownWithGithubUnderlines(&document), QStringLiteral("text/html"));
        return result;
    }

    QString textForMime(const QMimeData *mime, const QString &baseType)
    {
        if (!mime)
            return {};
        if (mime->hasFormat(baseType))
            return QString::fromUtf8(mime->data(baseType));
        for (const QString &format : mime->formats()) {
            if (format.startsWith(baseType + QLatin1Char(';'), Qt::CaseInsensitive))
                return QString::fromUtf8(mime->data(format));
        }
        return {};
    }

    NoteFragment tableFragmentFromTsv(const QString &tsv)
    {
        const QStringList  lines   = tsv.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        int                columns = 0;
        QList<QStringList> rows;
        for (QString line : lines) {
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            const QStringList cells = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
            columns                 = qMax(columns, cells.size());
            rows.append(cells);
        }

        NoteFragment      fragment;
        NoteFragmentBlock table;
        table.type             = NoteFragmentBlockType::Table;
        table.table.rows       = rows.size();
        table.table.columns    = columns;
        table.table.headerRows = rows.isEmpty() ? 0 : 1;
        for (const QStringList &sourceRow : rows) {
            for (int column = 0; column < columns; ++column)
                table.table.markdownCells.append(sourceRow.value(column));
        }
        if (table.table.rows > 0 && table.table.columns > 0)
            fragment.blocks.append(table);
        return fragment;
    }

} // namespace

NoteTransferController::ExportResult NoteTransferController::createMimeData(const NoteFragment &fragment) const
{
    ExportResult                   result;
    const QByteArray               encoded    = encodeNoteFragment(fragment);
    const NoteFragmentDecodeResult validation = decodeNoteFragment(encoded);
    if (!validation) {
        result.error = validation.error;
        return result;
    }

    QString       error;
    const QString plainText = plainTextForFragment(fragment, &error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }

    auto mime = std::make_unique<QMimeData>();
    mime->setData(QString::fromLatin1(FragmentMimeType), encoded);
    mime->setText(plainText);

    if (fragment.sourceFormat == NoteFragmentSourceFormat::Markdown) {
        const QString markdown = markdownForFragment(fragment, &error);
        if (!error.isEmpty()) {
            result.error = error;
            return result;
        }
        const QString html = htmlForFragment(fragment, &error);
        if (!error.isEmpty()) {
            result.error = error;
            return result;
        }
        mime->setData(QString::fromLatin1(MarkdownMimeType), markdown.toUtf8());
        mime->setHtml(html);
    }

    const QString tsv = tsvForFragment(fragment);
    if (!tsv.isNull())
        mime->setData(QString::fromLatin1(TsvMimeType), tsv.toUtf8());

    if (fragment.blocks.size() == 1 && fragment.blocks.constFirst().type == NoteFragmentBlockType::Image) {
        const auto media = std::find_if(fragment.media.cbegin(), fragment.media.cend(), [&fragment](const auto &item) {
            return item.sourceUri == fragment.blocks.constFirst().image.sourceUri;
        });
        if (media != fragment.media.cend() && !media->data.isEmpty()) {
            QImage image;
            image.loadFromData(media->data);
            if (!image.isNull()) {
                mime->setImageData(image);
                QBuffer buffer;
                buffer.open(QIODevice::WriteOnly);
                if (image.save(&buffer, "PNG"))
                    mime->setData(QStringLiteral("image/png"), buffer.data());
            }
        }
    }

    QList<QUrl> urls;
    for (const NoteFragmentBlock &block : fragment.blocks) {
        if (block.type != NoteFragmentBlockType::Image)
            continue;
        const QUrl url(block.image.sourceUri);
        if (url.isValid() && url.scheme().compare(QStringLiteral("qtnote-media"), Qt::CaseInsensitive) != 0)
            urls.append(url);
    }
    if (!urls.isEmpty())
        mime->setUrls(urls);

    result.mimeData = std::move(mime);
    return result;
}

NoteTransferController::ImportResult NoteTransferController::importMimeData(const QMimeData *mimeData) const
{
    if (!mimeData)
        return failed(QStringLiteral("clipboard data is unavailable"));

    if (mimeData->hasFormat(QString::fromLatin1(FragmentMimeType))) {
        const NoteFragmentDecodeResult decoded
            = decodeNoteFragment(mimeData->data(QString::fromLatin1(FragmentMimeType)));
        if (decoded) {
            ImportResult result;
            result.fragment       = decoded.fragment;
            result.sourceMimeType = QString::fromLatin1(FragmentMimeType);
            return result;
        }
        // A malformed private MIME payload must not stop an ordinary external
        // fallback representation on the same clipboard from being pasted.
    }

    const QString markdown = textForMime(mimeData, QString::fromLatin1(MarkdownMimeType));
    if (!markdown.isNull())
        return fragmentFromMarkdown(markdown, QString::fromLatin1(MarkdownMimeType));

    const QString tsv = textForMime(mimeData, QString::fromLatin1(TsvMimeType));
    if (!tsv.isNull()) {
        ImportResult result;
        result.fragment       = tableFragmentFromTsv(tsv);
        result.sourceMimeType = QString::fromLatin1(TsvMimeType);
        if (result.fragment.blocks.isEmpty())
            result.error = QStringLiteral("TSV contains no table cells");
        return result;
    }

    // Spreadsheets commonly offer both HTML and TSV, plus a bitmap preview.
    // TSV is lossless for the cell rectangle and maps directly to our table
    // block, whereas QTextDocument may represent the HTML table as rich text.
    if (mimeData->hasHtml()) {
        return fragmentFromHtml(mimeData->html());
    }

    if (mimeData->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mimeData->imageData());
        if (!image.isNull()) {
            ImportResult result;
            result.image          = image;
            result.sourceMimeType = QStringLiteral("image/*");
            return result;
        }
    }

    if (mimeData->hasUrls()) {
        QStringList urls;
        for (const QUrl &url : mimeData->urls())
            if (url.isValid())
                urls.append(url.toDisplayString());
        if (!urls.isEmpty()) {
            ImportResult result = fragmentFromMarkdown(urls.join(QLatin1Char('\n')), QStringLiteral("text/uri-list"));
            return result;
        }
    }

    if (mimeData->hasText())
        return fragmentFromMarkdown(mimeData->text(), QStringLiteral("text/plain"));
    return failed(QStringLiteral("clipboard format is not supported"));
}

QString NoteTransferController::markdownForFragment(const NoteFragment &fragment, QString *error)
{
    if (error)
        error->clear();
    return markdownFromFragment(fragment, error);
}

QString NoteTransferController::plainTextForFragment(const NoteFragment &fragment, QString *error)
{
    if (fragment.sourceFormat == NoteFragmentSourceFormat::PlainText) {
        if (error)
            error->clear();
        QStringList blocks;
        for (const NoteFragmentBlock &block : fragment.blocks)
            blocks.append(block.markdown);
        return normalizePlainText(blocks.join(QStringLiteral("\n\n")));
    }
    const QString markdown = markdownForFragment(fragment, error);
    if (error && !error->isEmpty())
        return {};
    static const QString hardBreakMarker   = QStringLiteral("QTNOTEPLAINHARDLINEBREAK8D27");
    QString              protectedMarkdown = markdown;
    protectedMarkdown.replace(QRegularExpression(QStringLiteral(" {2,}\\r?\\n")), hardBreakMarker);
    QTextDocument document;
    setMarkdownPreservingGithubUnderlines(&document, protectedMarkdown);
    QStringList paragraphs;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
        paragraphs.append(block.text());
    QString plainText = normalizePlainText(paragraphs.join(QStringLiteral("\n\n")));
    plainText.replace(hardBreakMarker, QStringLiteral("\n"));
    return plainText;
}

QString NoteTransferController::htmlForFragment(const NoteFragment &fragment, QString *error)
{
    const QString markdown = markdownForFragment(fragment, error);
    if (error && !error->isEmpty())
        return {};
    QTextDocument document;
    setMarkdownPreservingGithubUnderlines(&document, markdown);
    return document.toHtml();
}

QString NoteTransferController::tsvForFragment(const NoteFragment &fragment)
{
    if (fragment.blocks.size() != 1 || fragment.blocks.constFirst().type != NoteFragmentBlockType::Table)
        return QString();
    const NoteFragmentTable &table = fragment.blocks.constFirst().table;
    if (table.rows < 1 || table.columns < 1 || table.rows * table.columns != table.markdownCells.size())
        return {};

    QStringList rows;
    for (int row = 0; row < table.rows; ++row) {
        QStringList columns;
        for (int column = 0; column < table.columns; ++column) {
            QString cell = table.markdownCells.at(row * table.columns + column);
            cell.replace(QLatin1Char('\t'), QLatin1Char(' '));
            cell.replace(QLatin1Char('\n'), QStringLiteral(" "));
            columns.append(cell);
        }
        rows.append(columns.join(QLatin1Char('\t')));
    }
    return rows.join(QLatin1Char('\n'));
}

} // namespace QtNote
