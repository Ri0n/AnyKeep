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
    Q_INVOKABLE bool insertMedia(int row = -1) override;
    Q_INVOKABLE bool insertAttachment(int row = -1) override;
    Q_INVOKABLE void openAttachment(const QString &url);
    Q_INVOKABLE void saveAttachmentAs(const QString &url);
    // Whether the attachment is a file kept in place on this device, rather
    // than a copy in encrypted storage or a file only another device has.
    Q_INVOKABLE bool isAttachmentKeptInPlace(const QString &url) const;
    // Shows the file kept in place in the system file manager.
    Q_INVOKABLE void showAttachmentInFolder(const QString &url);

    // File-manager drops use the same local media store as picker imports.
    bool canAcceptFileMimeData(const QMimeData *mimeData) const;
    bool insertFileMimeData(const QMimeData *mimeData, int row = -1);

signals:
    // The attachment's content moved between encrypted storage and a file.
    void attachmentStorageChanged(const QString &url);

private:
    bool    findAttachment(const QString &url, MediaReference *reference) const;
    bool    chooseFileImportMode(const QString &fileName, MediaFileImportMode *mode);
    bool    chooseSaveMode(const QString &fileName, bool *keepOnlySavedFile);
    void    moveAttachmentToFileAsync(const MediaReference &reference, const QString &fileName, const QString &url);
    bool    referenceFileAsync(const QString &fileName, int row, bool attachment, MediaFileImportMode mode);
    void    exportRemoteAttachment(const MediaReference &reference, const QString &path, bool openAfter);
    QString materializeDragImage(const MediaReference &reference, const QByteArray &data);

    QPointer<QWidget>              dialogParent_;
    QPointer<QObject>              dragSource_;
    std::unique_ptr<QTemporaryDir> dragExportDirectory_;
    std::unique_ptr<QTemporaryDir> attachmentOpenDirectory_;
};

} // namespace AnyKeep

#endif // DESKTOPEDITORPLATFORMBACKEND_H
