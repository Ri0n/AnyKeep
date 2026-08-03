#ifndef NOTESMANAGERWINDOW_H
#define NOTESMANAGERWINDOW_H

#include "anykeep_export.h"

#include <QObject>
#include <QPointer>

class QPointF;
class QQmlApplicationEngine;
class QQuickWindow;

namespace AnyKeep {

class DesktopEditorPlatformBackend;
class DesktopNoteActions;
class SpeechRecognitionController;
class SpeechRecognitionProviderInterface;
class NotesWorkspaceController;

class ANYKEEP_EXPORT NotesManagerWindow final : public QObject {
    Q_OBJECT

public:
    explicit NotesManagerWindow(QObject *parent = nullptr);
    ~NotesManagerWindow() override;

    bool                          isReady() const;
    DesktopEditorPlatformBackend *platformBackend() const { return platformBackend_; }
    bool                          isVisible() const;
    void                          show();
    void                          setSpeechRecognitionProvider(SpeechRecognitionProviderInterface *provider);
    bool                          close();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void operationFailed(const QString &message);
    void openNoteRequested(const QString &storageId, const QString &noteId);

private:
    int  insertionRowAt(const QPointF &position) const;
    bool requestWorkspaceClose();
    void flushEditorChanges();
    void restoreWindowState();
    void saveWindowState() const;

    QQmlApplicationEngine        *engine_ { nullptr };
    NotesWorkspaceController     *workspace_ { nullptr };
    DesktopEditorPlatformBackend *platformBackend_ { nullptr };
    DesktopNoteActions           *desktopActions_ { nullptr };
    SpeechRecognitionController  *speechController_ { nullptr };
    QPointer<QQuickWindow>        window_;
    bool                          imageDragAccepted_ { false };
};

} // namespace AnyKeep

#endif // NOTESMANAGERWINDOW_H
