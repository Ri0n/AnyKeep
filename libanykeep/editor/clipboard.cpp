#include "noteeditor.h"

#include "private.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QSet>

#include <utility>

#include "localmediastore.h"
#include "noteblockmodel.h"
#include "notetransfercontroller.h"

namespace AnyKeep {

using EditorOperationsPrivate::decodeSelectionRanges;

NoteFragment NoteEditor::documentFragment() const
{
    return withMedia(model_->extractBlockFragment(0, model_->rowCount() - 1));
}

NoteFragment NoteEditor::withMedia(NoteFragment fragment) const
{
    QSet<QString> includedUris;
    for (const NoteFragmentBlock &block : std::as_const(fragment.blocks)) {
        const QString sourceUri = block.type == NoteFragmentBlockType::Image ? block.image.sourceUri
            : block.type == NoteFragmentBlockType::Audio                     ? block.audio.sourceUri
            : block.type == NoteFragmentBlockType::Attachment                ? block.attachment.sourceUri
                                                                             : QString();
        if (sourceUri.isEmpty() || includedUris.contains(sourceUri))
            continue;
        for (const MediaReference &reference : media()) {
            if (reference.isValid() && reference.uri() == sourceUri) {
                NoteFragmentMedia media;
                media.sourceUri = sourceUri;
                media.reference = reference;
                fragment.media.append(media);
                includedUris.insert(sourceUri);
                break;
            }
        }
    }

    // A single copied image is also useful outside AnyKeep.  Keep its PNG data
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

namespace {

    bool setFragmentClipboard(const NoteFragment &fragment, QClipboard::Mode mode, const QString &fallback,
                              bool markdownAsPlainText = false)
    {
        auto *clipboard = QGuiApplication::clipboard();
        if (!clipboard || (mode == QClipboard::Selection && !clipboard->supportsSelection()))
            return false;
        NoteTransferController controller;
        auto                   exported = controller.createMimeData(fragment);
        if (exported) {
            if (markdownAsPlainText) {
                QString       error;
                const QString markdown = NoteTransferController::markdownForFragment(fragment, &error);
                if (!error.isEmpty())
                    return false;
                // The Windows clipboard can synthesize CF_UNICODETEXT from HTML and prefer it over an explicitly
                // replaced text/plain payload. Plain-text Markdown copies must not advertise HTML there.
                exported.mimeData->removeFormat(QStringLiteral("text/html"));
                exported.mimeData->setText(markdown);
            }
            clipboard->setMimeData(exported.mimeData.release(), mode);
        } else {
            clipboard->setText(fallback, mode);
        }
        return true;
    }

    NoteFragment markdownFragment(const QString &markdown)
    {
        NoteBlockModel model;
        model.load(markdown, true);
        NoteFragment fragment = model.extractBlockFragment(0, model.rowCount() - 1);
        fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;
        return fragment;
    }

    NoteFragment plainTextFragment(const QString &text)
    {
        NoteFragment fragment;
        fragment.sourceFormat = NoteFragmentSourceFormat::PlainText;
        NoteFragmentBlock block;
        block.type     = NoteFragmentBlockType::Text;
        block.markdown = text;
        fragment.blocks.append(block);
        return fragment;
    }

} // namespace

void NoteEditor::copyToClipboard(const QString &text)
{
    setFragmentClipboard(plainTextFragment(text), QClipboard::Clipboard, text);
}

void NoteEditor::copyMarkdownToClipboard(const QString &markdown)
{
    // The visible selection is Markdown, not one literal text block.  Keeping
    // it as a Text block would make our private MIME format treat list/table
    // syntax as ordinary characters on the next AnyKeep paste.  Parse it back
    // through the block model so the private and public representations carry
    // the same structure.
    const NoteFragment fragment = markdownFragment(markdown);

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

void NoteEditor::copyMarkdownAsPlainTextToClipboard(const QString &markdown)
{
    const NoteFragment fragment = markdownFragment(markdown);
    setFragmentClipboard(fragment, QClipboard::Clipboard, markdown, true);
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

bool NoteEditor::copyBlockToClipboard(int row)
{
    if (row < 0 || row >= model_->rowCount())
        return false;
    const NoteFragment fragment = withMedia(model_->extractBlockFragment(row, row));
    if (fragment.blocks.isEmpty())
        return false;
    QString       error;
    const QString fallback = NoteTransferController::plainTextForFragment(fragment, &error);
    return error.isEmpty() && setFragmentClipboard(fragment, QClipboard::Clipboard, fallback);
}

bool NoteEditor::copySelectionAsMarkdownToClipboard(const QVariantList &encodedRanges)
{
    NoteFragment fragment = withMedia(model_->extractSelectionFragment(decodeSelectionRanges(encodedRanges)));
    if (fragment.blocks.isEmpty())
        return false;
    QString       error;
    const QString fallback = NoteTransferController::markdownForFragment(fragment, &error);
    return error.isEmpty() && setFragmentClipboard(fragment, QClipboard::Clipboard, fallback, true);
}

bool NoteEditor::copyTextToPrimarySelection(const QString &text)
{
    return setFragmentClipboard(plainTextFragment(text), QClipboard::Selection, text);
}

bool NoteEditor::copyMarkdownToPrimarySelection(const QString &markdown)
{
    return setFragmentClipboard(markdownFragment(markdown), QClipboard::Selection, markdown);
}

bool NoteEditor::copySelectionToPrimarySelection(const QVariantList &encodedRanges)
{
    NoteFragment fragment = withMedia(model_->extractSelectionFragment(decodeSelectionRanges(encodedRanges)));
    if (fragment.blocks.isEmpty())
        return false;
    QString       error;
    const QString fallback = NoteTransferController::plainTextForFragment(fragment, &error);
    return error.isEmpty() && setFragmentClipboard(fragment, QClipboard::Selection, fallback);
}

} // namespace AnyKeep
