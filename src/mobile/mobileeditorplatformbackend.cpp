#include "mobileeditorplatformbackend.h"

#include "androidplatformservices.h"
#include "localmediastore.h"
#include "noteeditor.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace AnyKeep {

MobileEditorPlatformBackend::MobileEditorPlatformBackend(AndroidPlatformServices *services, QObject *parent) :
    EditorPlatformBackend(parent), services_(services)
{
    Q_ASSERT(services_);
    connect(services_, &AndroidPlatformServices::imageSelected, this,
            [this](const QByteArray &data, const QString &name, const QString &mediaType) {
                const int                  row             = std::exchange(pendingRow_, -1);
                const QPointer<NoteEditor> requestedEditor = std::exchange(pendingEditor_, QPointer<NoteEditor> {});
                if (!requestedEditor || requestedEditor != editor())
                    return;
                if (!insertImageData(data, name, mediaType, row))
                    emit operationFailed(tr("Could not insert the selected image."));
            });
    connect(services_, &AndroidPlatformServices::fileSelected, this,
            [this](const QByteArray &data, const QString &name, const QString &mediaType) {
                const int                  row = std::exchange(pendingAttachmentRow_, -1);
                const QPointer<NoteEditor> requestedEditor
                    = std::exchange(pendingAttachmentEditor_, QPointer<NoteEditor> {});
                if (!requestedEditor || requestedEditor != editor())
                    return;
                if (!insertAttachmentData(data, name, mediaType, row))
                    emit operationFailed(tr("Could not attach the selected file."));
            });
}

bool MobileEditorPlatformBackend::insertImage(int row)
{
    if (!canInsertImages() || !services_)
        return false;
    pendingEditor_ = editor();
    pendingRow_    = row;
    if (services_->requestImage())
        return true;
    pendingEditor_.clear();
    pendingRow_ = -1;
    emit operationFailed(tr("Could not open the system image picker."));
    return false;
}

bool MobileEditorPlatformBackend::insertPhoto(int row)
{
    if (!canInsertImages() || !services_)
        return false;
    pendingEditor_ = editor();
    pendingRow_    = row;
    if (services_->requestPhoto())
        return true;
    pendingEditor_.clear();
    pendingRow_ = -1;
    emit operationFailed(tr("Could not open the system camera."));
    return false;
}

bool MobileEditorPlatformBackend::insertAttachment(int row)
{
    if (!canInsertAttachments() || !services_)
        return false;
    pendingAttachmentEditor_ = editor();
    pendingAttachmentRow_    = row;
    if (services_->requestFile())
        return true;
    pendingAttachmentEditor_.clear();
    pendingAttachmentRow_ = -1;
    emit operationFailed(tr("Could not open the system file picker."));
    return false;
}

void MobileEditorPlatformBackend::openAttachment(const QString &url)
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
    const auto loaded = LocalMediaStore::instance()->data(reference->blobId);
    if (!loaded) {
        emit operationFailed(tr("Could not read the attached file: %1").arg(loaded.error));
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
    QSaveFile     file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(loaded.value) != loaded.value.size() || !file.commit()) {
        emit operationFailed(tr("Could not prepare the attached file: %1").arg(file.errorString()));
        return;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        emit operationFailed(tr("No application could open the attached file."));
}

void MobileEditorPlatformBackend::saveAttachmentAs(const QString &url)
{
    if (!editor() || !services_)
        return;
    const auto media     = editor()->media();
    const auto reference = std::find_if(media.cbegin(), media.cend(), [&url](const MediaReference &item) {
        return item.isValid() && item.uri() == url;
    });
    if (reference == media.cend()) {
        emit operationFailed(tr("The attached file is not available locally."));
        return;
    }
    const auto loaded = LocalMediaStore::instance()->data(reference->blobId);
    if (!loaded) {
        emit operationFailed(tr("Could not read the attached file: %1").arg(loaded.error));
        return;
    }
    const QString name = reference->originalName.isEmpty()
        ? (reference->portableName.isEmpty() ? QStringLiteral("attachment") : reference->portableName)
        : reference->originalName;
    const QString mime
        = reference->mediaType.isEmpty() ? QStringLiteral("application/octet-stream") : reference->mediaType;
    if (!services_->exportData(name, mime, loaded.value))
        emit operationFailed(tr("Could not open the system file export picker."));
}

} // namespace AnyKeep
