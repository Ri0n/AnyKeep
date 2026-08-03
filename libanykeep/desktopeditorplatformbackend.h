#ifndef DESKTOPEDITORPLATFORMBACKEND_H
#define DESKTOPEDITORPLATFORMBACKEND_H

#include "editorplatformbackend.h"

#include <QPointer>
#include <memory>

class QMimeData;
class QTemporaryDir;
class QWidget;

namespace AnyKeep {

class ANYKEEP_EXPORT DesktopEditorPlatformBackend final : public EditorPlatformBackend {
    Q_OBJECT

public:
    explicit DesktopEditorPlatformBackend(QObject *parent = nullptr);
    DesktopEditorPlatformBackend(NoteEditor *editor, QObject *parent = nullptr);
    ~DesktopEditorPlatformBackend() override;

    void setDialogParent(QWidget *parent);
    void setDragSource(QObject *source) { dragSource_ = source; }

    Q_INVOKABLE void saveImageAs(const QString &url) override;
    Q_INVOKABLE bool startImageDrag(int row) override;
    Q_INVOKABLE bool insertImage(int row = -1) override;
    Q_INVOKABLE bool insertAttachment(int row = -1) override;
    Q_INVOKABLE void openAttachment(const QString &url);
    Q_INVOKABLE void saveAttachmentAs(const QString &url);

private:
    QString materializeDragImage(const MediaReference &reference, const QByteArray &data);

    QPointer<QWidget>              dialogParent_;
    QPointer<QObject>              dragSource_;
    std::unique_ptr<QTemporaryDir> dragExportDirectory_;
    std::unique_ptr<QTemporaryDir> attachmentOpenDirectory_;
};

} // namespace AnyKeep

#endif // DESKTOPEDITORPLATFORMBACKEND_H
