#include "mobileeditorplatformbackend.h"

#include "androidplatformservices.h"
#include "noteeditor.h"

#include <utility>

namespace QtNote {

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
    emit operationFailed(tr("Could not open the Android image picker."));
    return false;
}

} // namespace QtNote
