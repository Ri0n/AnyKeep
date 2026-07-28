#include "desktopeditorplatformbackend.h"

#include "localmediastore.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "notetransfercontroller.h"

#include <QCursor>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QWidget>
#include <QWindow>

namespace QtNote {
namespace {

    bool isUsableImageFragment(const NoteFragment &fragment)
    {
        if (fragment.blocks.size() != 1 || fragment.blocks.constFirst().type != NoteFragmentBlockType::Image)
            return false;
        const QString sourceUri = fragment.blocks.constFirst().image.sourceUri;
        for (const auto &media : fragment.media) {
            if (media.sourceUri == sourceUri && media.reference.isValid())
                return true;
        }
        return QUrl(sourceUri).scheme().compare(QStringLiteral("qtnote-media"), Qt::CaseInsensitive) != 0;
    }

    bool cursorIsOutsideWindow(QObject *source)
    {
        const QPoint cursor = QCursor::pos();
        if (auto *widget = qobject_cast<QWidget *>(source)) {
            const QWidget *window = widget->window();
            return window && !window->frameGeometry().contains(cursor);
        }
        if (auto *window = qobject_cast<QWindow *>(source))
            return !window->frameGeometry().contains(cursor);
        return false;
    }

} // namespace

DesktopEditorPlatformBackend::DesktopEditorPlatformBackend(QObject *parent) : EditorPlatformBackend(parent) { }

DesktopEditorPlatformBackend::DesktopEditorPlatformBackend(NoteEditor *editor, QObject *parent) :
    EditorPlatformBackend(editor, parent)
{
}

DesktopEditorPlatformBackend::~DesktopEditorPlatformBackend() = default;

void DesktopEditorPlatformBackend::setDialogParent(QWidget *parent) { dialogParent_ = parent; }

void DesktopEditorPlatformBackend::saveImageAs(const QString &url)
{
    if (!editor())
        return;
    const auto            media     = editor()->media();
    const MediaReference *reference = nullptr;
    for (const auto &candidate : media) {
        if (candidate.uri() == url) {
            reference = &candidate;
            break;
        }
    }
    if (!reference) {
        emit operationFailed(tr("The image data is not available locally."));
        return;
    }
    const auto loaded = LocalMediaStore::instance()->data(reference->blobId);
    if (!loaded) {
        emit operationFailed(tr("Could not read the image: %1").arg(loaded.error));
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
        emit operationFailed(tr("Could not save the image: %1").arg(file.errorString()));
}

bool DesktopEditorPlatformBackend::startImageDrag(int row)
{
    if (!editor() || !dragSource_ || !editor()->isMarkdown()
        || editor()->model()->blockTypeAt(row) != int(NoteBlockModel::Image)) {
        return false;
    }
    NoteFragment fragment = editor()->model()->extractBlockFragment(row, row);
    for (const auto &reference : editor()->media()) {
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
    // The return value means "remove the source block", not merely "the drag
    // started". QML can therefore keep the mutation in its normal undoable
    // structural transaction.
    return cursorIsOutsideWindow(dragSource_);
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
    QString error;
    return insertImageFiles({ fileName }, row, &error);
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

} // namespace QtNote
