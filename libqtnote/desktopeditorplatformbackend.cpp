#include "desktopeditorplatformbackend.h"

#include "localmediastore.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "notefragmentmediatransfer.h"
#include "notehighlighter.h"
#include "notetransfercontroller.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPixmap>
#include <QQuickTextDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QUrl>
#include <QWidget>

#include <utility>

namespace QtNote {
namespace {
    int documentEnd(const QTextDocument *document) { return document ? qMax(0, document->characterCount() - 1) : 0; }

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

    bool isUsableImageFragment(const NoteFragment &fragment)
    {
        if (fragment.blocks.isEmpty())
            return false;
        for (const auto &block : fragment.blocks) {
            if (block.type != NoteFragmentBlockType::Image || block.image.sourceUri.isEmpty())
                return false;
            const QUrl source(block.image.sourceUri);
            if (source.scheme().compare(QStringLiteral("qtnote-media"), Qt::CaseInsensitive) != 0)
                continue;
            bool hasMedia = false;
            for (const auto &media : fragment.media) {
                if (media.sourceUri == block.image.sourceUri && media.reference.isValid()) {
                    hasMedia = true;
                    break;
                }
            }
            if (!hasMedia)
                return false;
        }
        return true;
    }

    QStringList localImageFiles(const QMimeData *mimeData)
    {
        QStringList result;
        if (!mimeData || !mimeData->hasUrls())
            return result;
        QMimeDatabase database;
        for (const auto &url : mimeData->urls()) {
            const QString fileName = url.toLocalFile();
            if (fileName.isEmpty() || !QFileInfo(fileName).isFile())
                continue;
            if (database.mimeTypeForFile(fileName, QMimeDatabase::MatchContent)
                    .name()
                    .startsWith(QLatin1String("image/")))
                result.append(fileName);
        }
        return result;
    }
}

DesktopEditorPlatformBackend::DesktopEditorPlatformBackend(QObject *parent) : QObject(parent) { }
DesktopEditorPlatformBackend::DesktopEditorPlatformBackend(NoteEditor *editor, QObject *parent) : QObject(parent)
{
    setEditor(editor);
}
DesktopEditorPlatformBackend::~DesktopEditorPlatformBackend() = default;

NoteEditor *DesktopEditorPlatformBackend::editor() const { return editor_.data(); }

void DesktopEditorPlatformBackend::setDialogParent(QWidget *parent) { dialogParent_ = parent; }

void DesktopEditorPlatformBackend::setEditor(NoteEditor *editor)
{
    if (editor_ == editor)
        return;
    if (editor_)
        disconnect(editor_, nullptr, this, nullptr);
    editor_ = editor;
    clearRegisteredDocuments();
    if (editor_) {
        connect(editor_, &NoteEditor::formatChanged, this, &DesktopEditorPlatformBackend::canInsertImagesChanged);
        connect(editor_, &QObject::destroyed, this, [this] {
            editor_.clear();
            clearRegisteredDocuments();
            emit canInsertImagesChanged();
        });
    }
    emit canInsertImagesChanged();
}

bool DesktopEditorPlatformBackend::canInsertImages() const { return editor_ && editor_->canInsertImages(); }

void DesktopEditorPlatformBackend::registerTextDocument(QQuickTextDocument *document, bool titleDocument)
{
    if (!document || !document->textDocument())
        return;
    document->textDocument()->setUndoRedoEnabled(false);
    auto *highlighter = new NoteHighlighter(document->textDocument());
    for (const auto &item : std::as_const(extensions_)) {
        const auto type = NoteHighlighter::ExtType(item.type);
        if (type == NoteHighlighter::Other || (type == NoteHighlighter::Title && !titleDocument))
            continue;
        highlighter->addExtension(item.extension, type);
        if (type == NoteHighlighter::SpellCheck && !spellCheckEnabled_)
            highlighter->disableExtension(type);
    }
    highlighters_.append({ highlighter, titleDocument });
    highlighter->rehighlight();
}

QVariantList DesktopEditorPlatformBackend::spellCheckRanges(QQuickTextDocument *document)
{
    QVariantList result;
    if (!spellCheckEnabled_ || !document || !document->textDocument())
        return result;
    auto      *textDocument      = document->textDocument();
    const auto visibleTextIsHref = [textDocument](int position) {
        const int limit = documentEnd(textDocument);
        if (position < 0 || position >= limit)
            return false;
        const QTextCharFormat format = formatAt(textDocument, position);
        const QString         href   = format.anchorHref().trimmed();
        if (!format.isAnchor() || href.isEmpty())
            return false;
        int start = position;
        while (start > 0) {
            const auto previous = formatAt(textDocument, start - 1);
            if (!previous.isAnchor() || previous.anchorHref() != href)
                break;
            --start;
        }
        int end = position + 1;
        while (end < limit) {
            const auto next = formatAt(textDocument, end);
            if (!next.isAnchor() || next.anchorHref() != href)
                break;
            ++end;
        }
        QTextCursor cursor(textDocument);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QString visible = cursor.selectedText().trimmed();
        visible.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        return visible == href || (href.startsWith(QStringLiteral("mailto:")) && visible == href.mid(7))
            || (visible.startsWith(QStringLiteral("www.")) && href == QStringLiteral("https://") + visible);
    };

    for (auto block = textDocument->begin(); block.isValid(); block = block.next()) {
        if (!block.layout())
            continue;
        for (const auto &range : block.layout()->formats()) {
            if (!range.format.property(SpellCheckFormatProperty).toBool())
                continue;
            const int start = block.position() + range.start;
            if (visibleTextIsHref(start))
                continue;
            result.append(
                QVariantMap { { QStringLiteral("start"), start }, { QStringLiteral("length"), range.length } });
        }
    }
    return result;
}

QStringList DesktopEditorPlatformBackend::spellingSuggestions(const QString &word) const
{
    if (!spellCheckEnabled_)
        return {};
    for (const auto &item : extensions_) {
        if (item.type != int(NoteHighlighter::SpellCheck))
            continue;
        if (auto spell = std::dynamic_pointer_cast<SpellCheckExtension>(item.extension))
            return spell->suggestions(word);
    }
    return {};
}

void DesktopEditorPlatformBackend::addToSpellingDictionary(const QString &word)
{
    for (const auto &item : extensions_) {
        if (item.type != int(NoteHighlighter::SpellCheck))
            continue;
        if (auto spell = std::dynamic_pointer_cast<SpellCheckExtension>(item.extension)) {
            spell->addToDictionary(word);
            rehighlight();
            return;
        }
    }
}

void DesktopEditorPlatformBackend::saveImageAs(const QString &url)
{
    if (!editor_)
        return;
    const auto            media     = editor_->media();
    const MediaReference *reference = nullptr;
    for (const auto &candidate : media) {
        if (candidate.uri() == url) {
            reference = &candidate;
            break;
        }
    }
    if (!reference) {
        QMessageBox::warning(dialogParent_, tr("Save Image As"), tr("The image data is not available locally."));
        return;
    }
    const auto loaded = LocalMediaStore::instance()->data(reference->blobId);
    if (!loaded) {
        QMessageBox::warning(dialogParent_, tr("Save Image As"), tr("Could not read the image: %1").arg(loaded.error));
        return;
    }
    const QString name        = reference->portableName.isEmpty() ? reference->originalName : reference->portableName;
    const QString initialPath = QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath(name);
    const QString fileName
        = QFileDialog::getSaveFileName(dialogParent_, tr("Save Image As"), initialPath,
                                       tr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg);;All files (*)"));
    if (fileName.isEmpty())
        return;
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(loaded.value) != loaded.value.size() || !file.commit())
        QMessageBox::warning(dialogParent_, tr("Save Image As"),
                             tr("Could not save the image: %1").arg(file.errorString()));
}

bool DesktopEditorPlatformBackend::startImageDrag(int row)
{
    if (!editor_ || !dragSource_ || !editor_->isMarkdown()
        || editor_->model()->blockTypeAt(row) != int(NoteBlockModel::Image)) {
        return false;
    }
    NoteFragment fragment = editor_->model()->extractBlockFragment(row, row);
    for (const auto &reference : editor_->media()) {
        if (reference.isValid() && reference.uri() == fragment.blocks.constFirst().image.sourceUri) {
            fragment.media.append({ fragment.blocks.constFirst().image.sourceUri, reference, {} });
            break;
        }
    }
    if (!isUsableImageFragment(fragment))
        return false;

    NoteTransferController controller;
    auto                   exported = controller.createMimeData(fragment);
    if (!exported)
        return false;

    QByteArray            imageData;
    const MediaReference *reference = fragment.media.isEmpty() ? nullptr : &fragment.media.constFirst().reference;
    if (reference) {
        const auto loaded = LocalMediaStore::instance()->data(reference->blobId);
        if (loaded)
            imageData = loaded.value;
    }
    if (reference && !imageData.isEmpty()) {
        const QString exportedFile = materializeDragImage(*reference, imageData);
        if (!exportedFile.isEmpty()) {
            auto urls = exported.mimeData->urls();
            urls.prepend(QUrl::fromLocalFile(exportedFile));
            exported.mimeData->setUrls(urls);
        }
    }

    QDrag drag(dragSource_);
    drag.setMimeData(exported.mimeData.release());
    QImage preview;
    preview.loadFromData(imageData);
    if (!preview.isNull()) {
        const auto thumbnail = preview.scaled(QSize(256, 192), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        drag.setPixmap(QPixmap::fromImage(thumbnail));
        drag.setHotSpot(QPoint(thumbnail.width() / 2, thumbnail.height() / 2));
    }
    drag.exec(Qt::CopyAction, Qt::CopyAction);
    return true;
}

bool DesktopEditorPlatformBackend::insertImage(int row)
{
    if (!canInsertImages())
        return false;
    const QString fileName
        = QFileDialog::getOpenFileName(dialogParent_, tr("Insert image"), QString(),
                                       tr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg);;All files (*)"));
    if (fileName.isEmpty())
        return false;
    return insertImageFiles({ fileName }, row);
}

bool DesktopEditorPlatformBackend::insertClipboardImage(int row)
{
    if (!canInsertImages())
        return false;
    const auto *mimeData = QGuiApplication::clipboard()->mimeData();
    if (!mimeData)
        return false;

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(mimeData);
    const bool structured = imported && !imported.hasImage() && imported.sourceMimeType != QStringLiteral("text/plain")
        && !imported.fragment.blocks.isEmpty();
    if (structured)
        return false;

    if (mimeData->hasImage()) {
        const auto image = qvariant_cast<QImage>(mimeData->imageData());
        if (!image.isNull()) {
            const QString name
                = QStringLiteral("Clipboard_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
            return insertRasterImage(image, name, row);
        }
    }
    return insertImageMimeData(mimeData, row);
}

bool DesktopEditorPlatformBackend::insertRasterImage(const QImage &image, const QString &name, int row)
{
    if (!canInsertImages() || image.isNull())
        return false;
    QByteArray encoded;
    QBuffer    buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        QMessageBox::warning(dialogParent_, tr("Insert image"), tr("Could not encode the image."));
        return false;
    }
    const auto imported = LocalMediaStore::instance()->importData(encoded, name, QStringLiteral("image/png"));
    if (!imported) {
        QMessageBox::warning(dialogParent_, tr("Insert image"), imported.error);
        return false;
    }
    return insertImportedImages({ imported.value }, row, QStringLiteral("insert-image"));
}

bool DesktopEditorPlatformBackend::insertImageFiles(const QStringList &fileNames, int row)
{
    if (!canInsertImages() || fileNames.isEmpty())
        return false;
    QList<MediaReference> references;
    for (const auto &fileName : fileNames) {
        const auto imported = LocalMediaStore::instance()->importFile(fileName);
        if (!imported) {
            QMessageBox::warning(dialogParent_, tr("Insert image"), imported.error);
            return false;
        }
        references.append(imported.value);
    }
    return insertImportedImages(references, row, QStringLiteral("insert-images"));
}

bool DesktopEditorPlatformBackend::canAcceptImageMimeData(const QMimeData *mimeData) const
{
    if (!canInsertImages() || !mimeData)
        return false;
    NoteTransferController controller;
    const auto             imported = controller.importMimeData(mimeData);
    return (imported && (imported.hasImage() || canInsertImageFragment(imported.fragment)))
        || !localImageFiles(mimeData).isEmpty();
}

bool DesktopEditorPlatformBackend::insertImageMimeData(const QMimeData *mimeData, int row)
{
    if (!canAcceptImageMimeData(mimeData))
        return false;
    NoteTransferController controller;
    const auto             imported = controller.importMimeData(mimeData);
    if (imported && canInsertImageFragment(imported.fragment))
        return insertImageFragment(imported.fragment, row);
    if (imported && imported.hasImage()) {
        const QString name
            = QStringLiteral("Dropped_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        return insertRasterImage(imported.image, name, row);
    }
    return insertImageFiles(localImageFiles(mimeData), row);
}

bool DesktopEditorPlatformBackend::canInsertImageFragment(const NoteFragment &fragment) const
{
    return canInsertImages() && isUsableImageFragment(fragment);
}

bool DesktopEditorPlatformBackend::insertImageFragment(const NoteFragment &sourceFragment, int row)
{
    if (!editor_ || !canInsertImageFragment(sourceFragment))
        return false;

    NoteFragment          fragment = sourceFragment;
    QList<MediaReference> insertedMedia;
    if (!fragment.media.isEmpty()) {
        const auto cloned = NoteFragmentMediaTransfer::cloneForDestination(fragment, *LocalMediaStore::instance(),
                                                                           LocalMediaStore::instance());
        if (!cloned) {
            qWarning() << "Desktop image drop media import failed:" << cloned.error;
            return false;
        }
        fragment      = cloned.fragment;
        insertedMedia = cloned.importedMedia;
    }

    const int insertionRow = row < 0 ? editor_->model()->rowCount() : qBound(0, row, editor_->model()->rowCount());
    editor_->beginHistoryTransaction(QStringLiteral("drop-image"));
    QString    error;
    const bool inserted = editor_->model()->insertBlockFragment(insertionRow, fragment, &error);
    if (inserted && !insertedMedia.isEmpty()) {
        auto manifest = editor_->media();
        manifest.append(insertedMedia);
        editor_->setMedia(manifest);
        emit mediaInserted(insertedMedia);
    }
    editor_->endHistoryTransaction();

    if (!inserted)
        qWarning() << "Desktop image drop insertion failed:" << error;
    return inserted;
}

void DesktopEditorPlatformBackend::setSpellCheckEnabled(bool enabled)
{
    if (spellCheckEnabled_ == enabled)
        return;
    spellCheckEnabled_ = enabled;
    for (const auto &registered : std::as_const(highlighters_)) {
        if (!registered.highlighter)
            continue;
        if (enabled)
            registered.highlighter->enableExtension(NoteHighlighter::SpellCheck);
        else
            registered.highlighter->disableExtension(NoteHighlighter::SpellCheck);
    }
    rehighlight();
    emit spellCheckEnabledChanged();
}

void DesktopEditorPlatformBackend::addHighlightExtension(const std::shared_ptr<HighlighterExtension> &extension,
                                                         int                                          type)
{
    extensions_.append({ extension, type });
    for (const auto &registered : std::as_const(highlighters_)) {
        if (registered.highlighter && type != int(NoteHighlighter::Other)
            && (type != int(NoteHighlighter::Title) || registered.titleDocument)) {
            registered.highlighter->addExtension(extension, NoteHighlighter::ExtType(type));
        }
    }
    rehighlight();
}

void DesktopEditorPlatformBackend::rehighlight()
{
    highlighters_.removeIf([](const auto &registered) { return registered.highlighter.isNull(); });
    for (const auto &registered : std::as_const(highlighters_))
        registered.highlighter->rehighlight();
}

bool DesktopEditorPlatformBackend::insertImportedImages(const QList<MediaReference> &references, int row,
                                                        const QString &historyKind)
{
    if (!editor_ || references.isEmpty() || !canInsertImages())
        return false;
    editor_->beginHistoryTransaction(historyKind);
    auto media = editor_->media();
    media.append(references);
    editor_->setMedia(media);
    int insertionRow = row < 0 ? editor_->model()->rowCount() : qBound(0, row, editor_->model()->rowCount());
    for (const auto &reference : references)
        editor_->model()->insertImage(insertionRow++, reference.uri(), reference.originalName);
    editor_->endHistoryTransaction();
    emit mediaInserted(references);
    return true;
}

QString DesktopEditorPlatformBackend::materializeDragImage(const MediaReference &reference, const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    if (!dragExportDirectory_)
        dragExportDirectory_
            = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/qtnote-image-drag-XXXXXX"));
    if (!dragExportDirectory_->isValid())
        return {};
    QString name
        = QFileInfo(reference.portableName.isEmpty() ? reference.originalName : reference.portableName).fileName();
    if (name.isEmpty())
        name = QStringLiteral("image");
    const QString directory = QDir(dragExportDirectory_->path()).filePath(reference.id.toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(directory))
        return {};
    const QString path = QDir(directory).filePath(name);
    QSaveFile     file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return {};
    return path;
}

void DesktopEditorPlatformBackend::clearRegisteredDocuments() { highlighters_.clear(); }

} // namespace QtNote
