#include "noteeditor.h"

#include "private.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QQuickTextDocument>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

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
    const QString codeLanguage = hasNativeStructure ? QString() : TextDropUtils::codeLanguage(mimeData);
    // Source editors often put both text/plain and presentation-oriented HTML
    // on the clipboard. The HTML can turn one selection into many paragraphs.
    // For strongly recognized source, the plain representation is canonical.
    if (row > 0 && !codeLanguage.isEmpty()) {
        QTextDocument *document = quickDocument->textDocument();
        const int      limit    = documentEnd(document);
        const int      insertedRow
            = insertDroppedCodeBlock(row, markdownRange(document, 0, start), markdownRange(document, end, limit),
                                     TextDropUtils::plainText(mimeData), codeLanguage);
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
