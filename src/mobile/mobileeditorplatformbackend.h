#ifndef ANYKEEP_MOBILEEDITORPLATFORMBACKEND_H
#define ANYKEEP_MOBILEEDITORPLATFORMBACKEND_H

#include "editorplatformbackend.h"

#include <QPointer>
#include <QTemporaryDir>

#include <memory>

namespace AnyKeep {

class AndroidPlatformServices;

class MobileEditorPlatformBackend final : public EditorPlatformBackend {
    Q_OBJECT

public:
    explicit MobileEditorPlatformBackend(AndroidPlatformServices *services, QObject *parent = nullptr);

    Q_INVOKABLE bool insertMedia(int row = -1) override;
    Q_INVOKABLE bool insertPhoto(int row = -1);
    Q_INVOKABLE bool insertAttachment(int row = -1) override;
    Q_INVOKABLE void openAttachment(const QString &url);
    Q_INVOKABLE void saveAttachmentAs(const QString &url);

private:
    AndroidPlatformServices       *services_ { nullptr };
    QPointer<NoteEditor>           pendingPhotoEditor_;
    int                            pendingPhotoRow_ { -1 };
    QPointer<NoteEditor>           pendingMediaEditor_;
    int                            pendingMediaRow_ { -1 };
    QPointer<NoteEditor>           pendingAttachmentEditor_;
    int                            pendingAttachmentRow_ { -1 };
    std::unique_ptr<QTemporaryDir> attachmentOpenDirectory_;
};

} // namespace AnyKeep

#endif // ANYKEEP_MOBILEEDITORPLATFORMBACKEND_H
