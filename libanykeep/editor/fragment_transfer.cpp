#include "noteeditor.h"

#include "private.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QPalette>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>

#include <algorithm>

#include "localmediastore.h"
#include "noteblockmodel.h"
#include "notefragmentmediatransfer.h"
#include "notetransfercontroller.h"
#include "textdroputils.h"

namespace AnyKeep {

using EditorOperationsPrivate::decodeSelectionRanges;
using EditorOperationsPrivate::documentEnd;
using EditorOperationsPrivate::markdownRange;

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

QVariantMap NoteEditor::replaceListSelectionWithText(const QVariantList &encodedRanges, int focusBlock, int focusItem,
                                                     const QString &text)
{
    QList<NoteBlockSelectionRange> ranges = decodeSelectionRanges(encodedRanges);
    if (ranges.isEmpty() || focusBlock < 0 || focusItem < 0)
        return {};

    std::sort(ranges.begin(), ranges.end(),
              [](const auto &left, const auto &right) { return left.listItemIndex < right.listItemIndex; });

    const QStringList items = model_->data(model_->index(focusBlock), NoteBlockModel::ItemsRole).toStringList();
    if (items.isEmpty())
        return {};
    for (qsizetype index = 0; index < ranges.size(); ++index) {
        const auto &range = ranges.at(index);
        if (range.blockIndex != focusBlock || range.listItemIndex < 0 || range.listItemIndex >= items.size()
            || range.tableCellIndex >= 0
            || (index > 0 && range.listItemIndex != ranges.at(index - 1).listItemIndex + 1)) {
            return {};
        }
    }

    QString replacement = text;
    replacement.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    replacement.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const auto &first     = ranges.constFirst();
    const auto &last      = ranges.constLast();
    const int   firstItem = first.listItemIndex;
    const int   lastItem  = last.listItemIndex;
    int         resultItem;
    int         resultPosition;

    if (!first.before.isEmpty()) {
        // The first item survives. All following selected items, including a
        // partially selected last one, collapse into it.
        model_->setListItem(focusBlock, firstItem, first.before + replacement + last.after);
        if (lastItem > firstItem)
            model_->removeListItems(focusBlock, firstItem + 1, lastItem);
        resultItem     = firstItem;
        resultPosition = first.before.size() + replacement.size();
    } else if (!last.after.isEmpty()) {
        // The first item was selected completely, so preserve the last item's
        // list metadata and move it into the vacated position.
        if (lastItem > firstItem)
            model_->removeListItems(focusBlock, firstItem, lastItem - 1);
        model_->setListItem(focusBlock, firstItem, replacement + last.after);
        resultItem     = firstItem;
        resultPosition = replacement.size();
    } else {
        // Every affected item was selected completely. Delete the items first,
        // then insert at the same structural deletion point.
        const bool removesWholeList = firstItem == 0 && lastItem == items.size() - 1;
        model_->removeListItems(focusBlock, firstItem, lastItem);
        if (removesWholeList) {
            if (!replacement.isEmpty())
                model_->setBlockText(focusBlock, replacement);
            return {
                { QStringLiteral("handled"), true },
                { QStringLiteral("focusBlock"), focusBlock },
                { QStringLiteral("focusItem"), -1 },
                { QStringLiteral("focusMarkdownPosition"), replacement.size() },
            };
        }

        if (firstItem > 0) {
            resultItem           = firstItem - 1;
            const QString prefix = items.at(resultItem);
            if (!replacement.isEmpty())
                model_->setListItem(focusBlock, resultItem, prefix + replacement);
            resultPosition = prefix.size() + replacement.size();
        } else {
            resultItem           = 0;
            const QString suffix = items.at(lastItem + 1);
            if (!replacement.isEmpty())
                model_->setListItem(focusBlock, resultItem, replacement + suffix);
            resultPosition = replacement.size();
        }
    }

    return {
        { QStringLiteral("handled"), true },
        { QStringLiteral("focusBlock"), focusBlock },
        { QStringLiteral("focusItem"), resultItem },
        { QStringLiteral("focusMarkdownPosition"), resultPosition },
    };
}

QVariantMap NoteEditor::replaceListSelectionFromClipboard(const QVariantList &encodedRanges, int focusBlock,
                                                          int focusItem)
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData  *mimeData  = clipboard ? clipboard->mimeData() : nullptr;
    if (!mimeData || !mimeData->hasText())
        return {};
    return replaceListSelectionWithText(encodedRanges, focusBlock, focusItem, mimeData->text());
}

QVariantMap NoteEditor::convertSelectionToCodeBlock(const QVariantList &encodedRanges, const QString &plainText,
                                                    const QString &language)
{
    QVariantMap result;
    const int   row
        = model_->replaceTextSelectionWithCodeBlock(decodeSelectionRanges(encodedRanges), plainText, language);
    if (row < 0)
        return result;
    result.insert(QStringLiteral("handled"), true);
    result.insert(QStringLiteral("focusRow"), row);
    return result;
}

int NoteEditor::insertDroppedCodeBlock(int row, const QString &before, const QString &after, const QString &value,
                                       const QString &language)
{
    if (!model_->markdown())
        return -1;
    NoteFragment fragment;
    fragment.kind         = NoteFragmentKind::BlockSequence;
    fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
    NoteFragmentBlock code;
    code.type     = NoteFragmentBlockType::CodeBlock;
    code.markdown = value;
    code.markdown.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    code.markdown.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    code.language = language.trimmed().toLower();
    fragment.blocks.append(code);
    QString error;
    return model_->replaceTextBlockRangeWithFragment(row, before, after, fragment, &error);
}

int NoteEditor::insertPlainText(QQuickTextDocument *quickDocument, int start, int end, const QString &value)
{
    if (!quickDocument || !quickDocument->textDocument())
        return -1;

    QString text = value;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QTextDocument *document = quickDocument->textDocument();
    const int      limit    = documentEnd(document);
    start                   = qBound(0, start, limit);
    end                     = qBound(start, end, limit);

    QTextCursor cursor(document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    // Passing an empty format explicitly prevents HTML/RTF clipboard styles
    // and the surrounding QTextDocument character format from leaking into
    // the title line.
    cursor.insertText(text, QTextCharFormat());
    return cursor.position();
}

int NoteEditor::pastePlainText(QQuickTextDocument *quickDocument, int start, int end)
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData  *mimeData  = clipboard ? clipboard->mimeData() : nullptr;
    if (!mimeData || !mimeData->hasText())
        return -1;
    return insertPlainText(quickDocument, start, end, mimeData->text());
}

int NoteEditor::pastePrimarySelection(QQuickTextDocument *quickDocument, int start, int end)
{
    if (!quickDocument || !quickDocument->textDocument())
        return -1;
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData  *mimeData
        = clipboard && clipboard->supportsSelection() ? clipboard->mimeData(QClipboard::Selection) : nullptr;
    if (!mimeData || !mimeData->hasText())
        return -1;

    QString text = mimeData->text();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QTextDocument *document = quickDocument->textDocument();
    const int      limit    = documentEnd(document);
    start                   = qBound(0, start, limit);
    end                     = qBound(start, end, limit);
    QTextCursor cursor(document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.insertText(text, QTextCharFormat());
    return cursor.position();
}

void NoteEditor::normalizePastedTextFormats(QQuickTextDocument *quickDocument, int start, int end) const
{
    if (!quickDocument || !quickDocument->textDocument() || start >= end)
        return;

    QTextDocument *document = quickDocument->textDocument();
    const int      limit    = documentEnd(document);
    start                   = qBound(0, start, limit);
    end                     = qBound(start, end, limit);

    struct FormatRange {
        int             start;
        int             end;
        QTextCharFormat format;
    };
    QList<FormatRange> ranges;
    for (QTextBlock block = document->findBlock(start); block.isValid() && block.position() < end;
         block            = block.next()) {
        for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid() || fragment.charFormat().isImageFormat())
                continue;
            const int rangeStart = qMax(start, fragment.position());
            const int rangeEnd   = qMin(end, fragment.position() + fragment.length());
            if (rangeStart >= rangeEnd)
                continue;

            QTextCharFormat format = fragment.charFormat();
            format.clearProperty(QTextFormat::BackgroundBrush);
            format.clearProperty(QTextFormat::TextOutline);
            format.clearProperty(QTextFormat::TextUnderlineColor);
            format.clearProperty(QTextFormat::OldTextUnderlineColor);
            if (format.isAnchor() && !format.anchorHref().isEmpty())
                format.setForeground(QGuiApplication::palette().link());
            else
                format.clearProperty(QTextFormat::ForegroundBrush);
            ranges.append({ rangeStart, rangeEnd, format });
        }
    }

    for (const FormatRange &range : ranges) {
        QTextCursor cursor(document);
        cursor.setPosition(range.start);
        cursor.setPosition(range.end, QTextCursor::KeepAnchor);
        cursor.setCharFormat(range.format);
    }
}

QVariantMap NoteEditor::pasteStructuredFromClipboard(QQuickTextDocument *quickDocument, int row, int start, int end)
{
    QVariantMap result;
    if (!model_->markdown() || !quickDocument || !quickDocument->textDocument())
        return result;

    const QMimeData *mimeData           = QGuiApplication::clipboard()->mimeData();
    const bool       hasNativeStructure = mimeData
        && (mimeData->hasFormat(QString::fromLatin1(NoteTransferController::FragmentMimeType))
            || mimeData->hasFormat(QString::fromLatin1(NoteTransferController::MarkdownMimeType))
            || mimeData->hasFormat(QString::fromLatin1(NoteTransferController::TsvMimeType)));
    const TextDropUtils::CodeDetection code
        = hasNativeStructure ? TextDropUtils::CodeDetection {} : TextDropUtils::detectCode(mimeData);
    // Source editors often put both text/plain and presentation-oriented HTML
    // on the clipboard. The HTML can turn one selection into many paragraphs.
    // For strongly recognized source, the plain representation is canonical.
    if (row > 0 && code.isCode) {
        QTextDocument *document = quickDocument->textDocument();
        const int      limit    = documentEnd(document);
        const int      insertedRow
            = insertDroppedCodeBlock(row, markdownRange(document, 0, start), markdownRange(document, end, limit),
                                     TextDropUtils::plainText(mimeData), code.language);
        if (insertedRow >= 0) {
            result.insert(QStringLiteral("handled"), true);
            result.insert(QStringLiteral("focusRow"), insertedRow);
        }
        return result;
    }

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(mimeData);
    if (!imported || imported.sourceMimeType == QStringLiteral("text/plain") || imported.hasImage()
        || imported.fragment.blocks.isEmpty()) {
        return result;
    }

    // The title is a plain first-line field, but pasting a whole AnyKeep
    // document into an empty/new note must retain its following structural
    // blocks. A one-block inline fragment still uses the plain-text title path
    // so bold/link formatting cannot leak into the title.
    const int targetDocumentEnd = documentEnd(quickDocument->textDocument());
    if (row == 0
        && (start != 0 || end != targetDocumentEnd || imported.fragment.blocks.size() < 2
            || imported.fragment.blocks.constFirst().type != NoteFragmentBlockType::Text)) {
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

} // namespace AnyKeep
