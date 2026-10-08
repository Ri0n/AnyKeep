#include "desktopeditorplatformbackend.h"

#include "localmediastore.h"
#include "mediarangeservice.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "notetransfercontroller.h"
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
#include <QRunnable>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUrl>
#include <QWidget>
#include <QWindow>

#include <algorithm>

namespace AnyKeep {
namespace {

    bool isUsableImageFragment(const NoteFragment &fragment)
    {
        if (fragment.blocks.size() != 1 || fragment.blocks.constFirst().type != NoteFragmentBlockType::Media
            || !fragment.blocks.constFirst().media.mediaType.startsWith(QLatin1String("image/"))) {
            return false;
        }
        const QString sourceUri = fragment.blocks.constFirst().media.sourceUri;
        for (const auto &media : fragment.media) {
            if (media.sourceUri == sourceUri && media.reference.isValid())
                return true;
        }
        return QUrl(sourceUri).scheme().compare(QStringLiteral("anykeep-media"), Qt::CaseInsensitive) != 0;
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

DesktopEditorPlatformBackend::DesktopEditorPlatformBackend(QObject *parent) : EditorPlatformBackend(parent) {}

DesktopEditorPlatformBackend::DesktopEditorPlatformBackend(NoteEditor *editor, QObject *parent) :
    EditorPlatformBackend(editor, parent)
{
}

DesktopEditorPlatformBackend::~DesktopEditorPlatformBackend() = default;

void DesktopEditorPlatformBackend::setDialogParent(QWidget *parent) { dialogParent_ = parent; }

bool DesktopEditorPlatformBackend::chooseFileImportMode(const QString &fileName, MediaFileImportMode *mode)
{
    if (!mode)
        return false;

    QMessageBox dialog(QMessageBox::Question, tr("Store file"),
                       tr("How should AnyKeep use %1?").arg(QFileInfo(fileName).fileName()), QMessageBox::NoButton,
                       dialogParent_);
    dialog.setInformativeText(tr("Copying into encrypted storage makes the note independent of the original file. "
                                 "Keeping the file in place avoids another full copy, but the note will depend on that "
                                 "file remaining available and unchanged."));
    auto *copyButton = dialog.addButton(tr("Copy into encrypted storage"), QMessageBox::AcceptRole);
    auto *keepButton = dialog.addButton(tr("Keep file in place"), QMessageBox::ActionRole);
    dialog.addButton(QMessageBox::Cancel);
    dialog.exec();

    if (dialog.clickedButton() == copyButton) {
        *mode = MediaFileImportMode::CopyIntoStore;
        return true;
    }
    if (dialog.clickedButton() == keepButton) {
        *mode = MediaFileImportMode::KeepInPlace;
        return true;
    }
    return false;
}

bool DesktopEditorPlatformBackend::referenceFileAsync(const QString &fileName, int row, bool attachment,
                                                      MediaFileImportMode mode)
{
    auto *target = editor();
    if (!target || (attachment ? !target->canInsertAttachments() : !target->canInsertMedia()))
        return false;

    auto   *store = LocalMediaStore::instance();
    QString initializeError;
    if (!store->initialize(&initializeError)) {
        emit operationFailed(initializeError);
        return false;
    }

    const QPointer<DesktopEditorPlatformBackend> backend(this);
    const QPointer<NoteEditor>                   targetEditor(target);
    QThreadPool::globalInstance()->start(
        QRunnable::create([backend, targetEditor, store, fileName, row, attachment, mode] {
            const auto referenced = mode == MediaFileImportMode::KeepInPlace ? store->referenceFile(fileName)
                                                                             : store->importFile(fileName);
            if (!backend)
                return;
            QMetaObject::invokeMethod(
                backend,
                [backend, targetEditor, referenced, row, attachment]() {
                    if (!backend || !targetEditor)
                        return;
                    if (!referenced) {
                        emit backend->operationFailed(referenced.error);
                        return;
                    }

                    if (attachment) {
                        if (targetEditor->insertAttachment(referenced.value, row))
                            emit backend->mediaInserted({ referenced.value });
                        return;
                    }

                    if (!targetEditor->canInsertMedia())
                        return;
                    targetEditor->beginHistoryTransaction(QStringLiteral("insert-media"));
                    const int insertionRow = row < 0 ? targetEditor->model()->rowCount()
                                                     : qBound(0, row, targetEditor->model()->rowCount());
                    targetEditor->insertMedia(referenced.value, 0, 0, 0, insertionRow);
                    targetEditor->endHistoryTransaction();
                },
                Qt::QueuedConnection);
        }));
    return true;
}

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
    const auto loaded = LocalMediaStore::instance()->data(*reference);
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
        || editor()->model()->blockTypeAt(row) != int(NoteBlockModel::Media)) {
        return false;
    }
    NoteFragment fragment = editor()->model()->extractBlockFragment(row, row);
    for (const auto &reference : editor()->media()) {
        if (reference.isValid() && reference.uri() == fragment.blocks.constFirst().media.sourceUri) {
            fragment.media.append({ fragment.blocks.constFirst().media.sourceUri, reference, {} });
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
        const auto loaded = LocalMediaStore::instance()->data(*reference);
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

bool DesktopEditorPlatformBackend::insertMedia(int row)
{
    if (!canInsertMedia())
        return false;
    const QString fileName
        = QFileDialog::getOpenFileName(dialogParent_, tr("Insert media"), QString(),
                                       tr("Media files (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg *.mp3 *.wav *.ogg "
                                          "*.flac *.m4a *.aac *.mp4 *.m4v *.webm *.mov *.mkv *.avi);;All files (*)"));
    if (fileName.isEmpty())
        return false;
    MediaFileImportMode mode;
    if (!chooseFileImportMode(fileName, &mode))
        return false;
    return referenceFileAsync(fileName, row, false, mode);
}

bool DesktopEditorPlatformBackend::insertAttachment(int row)
{
    if (!canInsertAttachments() || !editor())
        return false;
    const QString fileName = QFileDialog::getOpenFileName(dialogParent_, tr("Attach file"));
    if (fileName.isEmpty())
        return false;
    MediaFileImportMode mode;
    if (!chooseFileImportMode(fileName, &mode))
        return false;
    return referenceFileAsync(fileName, row, true, mode);
}

void DesktopEditorPlatformBackend::openAttachment(const QString &url)
{
    if (!editor())
        return;
    const auto media     = editor()->media();
    const auto reference = std::find_if(media.cbegin(), media.cend(), [&url](const MediaReference &item) {
        return item.isValid() && item.uri() == url;
    });
    if (reference == media.cend()) {
        emit operationFailed(tr("The attached file is not available locally."));
        return;
    }
    if (!attachmentOpenDirectory_)
        attachmentOpenDirectory_
            = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/anykeep-attachment-open-XXXXXX"));
    if (!attachmentOpenDirectory_->isValid()) {
        emit operationFailed(tr("Could not create a temporary directory for the attached file."));
        return;
    }
    QString name
        = QFileInfo(reference->originalName.isEmpty() ? reference->portableName : reference->originalName).fileName();
    if (name.isEmpty())
        name = QStringLiteral("attachment");
    const QString directory
        = QDir(attachmentOpenDirectory_->path()).filePath(reference->id.toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(directory)) {
        emit operationFailed(tr("Could not prepare the attached file for opening."));
        return;
    }
    const QString path = QDir(directory).filePath(name);
    if (!LocalMediaStore::instance()->contains(*reference)) {
        exportRemoteAttachment(*reference, path, true);
        return;
    }
    const auto loaded = LocalMediaStore::instance()->data(*reference);
    if (!loaded) {
        emit operationFailed(loaded.error);
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(loaded.value) != loaded.value.size() || !file.commit()) {
        emit operationFailed(tr("Could not prepare the attached file: %1").arg(file.errorString()));
        return;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        emit operationFailed(tr("No application could open the attached file."));
}

void DesktopEditorPlatformBackend::saveAttachmentAs(const QString &url)
{
    if (!editor())
        return;
    const auto media     = editor()->media();
    const auto reference = std::find_if(media.cbegin(), media.cend(), [&url](const MediaReference &item) {
        return item.isValid() && item.uri() == url;
    });
    if (reference == media.cend()) {
        emit operationFailed(tr("The attached file is not available locally."));
        return;
    }
    QString name
        = QFileInfo(reference->originalName.isEmpty() ? reference->portableName : reference->originalName).fileName();
    if (name.isEmpty())
        name = QStringLiteral("attachment");
    const QString initialPath
        = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(name);
    const QString fileName = QFileDialog::getSaveFileName(dialogParent_, tr("Save Attached File As"), initialPath);
    if (fileName.isEmpty())
        return;
    if (!LocalMediaStore::instance()->contains(*reference)) {
        exportRemoteAttachment(*reference, fileName, false);
        return;
    }
    const auto loaded = LocalMediaStore::instance()->data(*reference);
    if (!loaded) {
        emit operationFailed(loaded.error);
        return;
    }
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(loaded.value) != loaded.value.size() || !file.commit())
        emit operationFailed(tr("Could not save the attached file: %1").arg(file.errorString()));
}

void DesktopEditorPlatformBackend::exportRemoteAttachment(const MediaReference &reference, const QString &path,
                                                          bool openAfter)
{
    const auto url = MediaRangeService::urlFor(reference);
    if (url.isEmpty()) {
        emit operationFailed(tr("No device can supply the attached file."));
        return;
    }
    auto file = std::make_shared<QSaveFile>(path);
    file->setDirectWriteFallback(false);
    if (!file->open(QIODevice::WriteOnly)) {
        MediaRangeService::releaseUrl(url);
        emit operationFailed(file->errorString());
        return;
    }
    file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    auto *manager = new QNetworkAccessManager(this);
    auto *reply   = manager->get(QNetworkRequest(url));
    reply->setReadBufferSize(128 * 1024);
    auto       hash  = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto       count = std::make_shared<qint64>(0);
    const auto drain = [reply, file, hash, count, reference] {
        while (reply->bytesAvailable() > 0) {
            const auto bytes = reply->read(64 * 1024);
            if (bytes.isEmpty())
                break;
            *count += bytes.size();
            if (*count > reference.size || file->write(bytes) != bytes.size()) {
                reply->abort();
                return;
            }
            hash->addData(bytes);
        }
    };
    connect(reply, &QNetworkReply::readyRead, reply, drain);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, manager, file, hash, count, reference, path, openAfter, url, drain] {
                drain();
                MediaRangeService::releaseUrl(url);
                const bool verified = reply->error() == QNetworkReply::NoError && *count == reference.size
                    && hash->result() == reference.checksum;
                if (!verified || !file->commit()) {
                    file->cancelWriting();
                    emit operationFailed(tr("Could not verify or save the attached file."));
                } else if (openAfter && !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
                    emit operationFailed(tr("No application could open the attached file."));
                }
                manager->deleteLater();
            });
    connect(manager, &QObject::destroyed, [url] { MediaRangeService::releaseUrl(url); });
}

QString DesktopEditorPlatformBackend::materializeDragImage(const MediaReference &reference, const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    if (!dragExportDirectory_)
        dragExportDirectory_
            = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/anykeep-image-drag-XXXXXX"));
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

} // namespace AnyKeep
