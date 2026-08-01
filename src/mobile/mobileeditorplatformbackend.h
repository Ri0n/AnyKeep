#ifndef QTNOTE_MOBILEEDITORPLATFORMBACKEND_H
#define QTNOTE_MOBILEEDITORPLATFORMBACKEND_H

#include "editorplatformbackend.h"

#include <QPointer>
#include <QTemporaryDir>

#include <memory>

namespace QtNote {

class AndroidPlatformServices;

class MobileEditorPlatformBackend final : public EditorPlatformBackend {
    Q_OBJECT

public:
    explicit MobileEditorPlatformBackend(AndroidPlatformServices *services, QObject *parent = nullptr);

    Q_INVOKABLE bool insertImage(int row = -1) override;
    Q_INVOKABLE bool insertPhoto(int row = -1);
    Q_INVOKABLE bool insertAttachment(int row = -1) override;
    Q_INVOKABLE void openAttachment(const QString &url);
    Q_INVOKABLE void saveAttachmentAs(const QString &url);

private:
    AndroidPlatformServices       *services_ { nullptr };
    QPointer<NoteEditor>           pendingEditor_;
    int                            pendingRow_ { -1 };
    QPointer<NoteEditor>           pendingAttachmentEditor_;
    int                            pendingAttachmentRow_ { -1 };
    std::unique_ptr<QTemporaryDir> attachmentOpenDirectory_;
};

} // namespace QtNote

#endif // QTNOTE_MOBILEEDITORPLATFORMBACKEND_H
