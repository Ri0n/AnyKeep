#ifndef NOTESMANAGERWINDOW_H
#define NOTESMANAGERWINDOW_H

#include "anykeep_export.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QUuid>

class QPointF;
class QRect;
class QQmlApplicationEngine;
class QQuickWindow;

namespace AnyKeep {

class DesktopEditorPlatformBackend;
class DesktopNoteActions;
class SpeechRecognitionController;
class SpeechRecognitionProviderInterface;
class NotesWorkspaceController;
class Note;
class UpdateController;

class ANYKEEP_EXPORT NotesManagerWindow final : public QObject {
    Q_OBJECT

public:
    explicit NotesManagerWindow(QObject *parent = nullptr);
    NotesManagerWindow(UpdateController *updates, QObject *parent);
    ~NotesManagerWindow() override;

    bool                          isReady() const;
    DesktopEditorPlatformBackend *platformBackend() const { return platformBackend_; }
    bool                          isVisible() const;
    bool                          hasOpenNote() const;
    QString                       currentStorageId() const;
    QString                       currentNoteId() const;
    QUuid                         currentDraftId() const;
    QRect                         windowGeometry() const;
    void                          setWindowGeometry(const QRect &geometry);
    bool                          openNote(const QString &storageId, const QString &noteId);
    bool                          openNote(const Note &note, const QUuid &draftId = {});
    bool                          checkpoint();
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
    void revealAfterInitialFrame();
    void restoreWindowState();
    void saveWindowState() const;

    QQmlApplicationEngine        *engine_ { nullptr };
    NotesWorkspaceController     *workspace_ { nullptr };
    DesktopEditorPlatformBackend *platformBackend_ { nullptr };
    DesktopNoteActions           *desktopActions_ { nullptr };
    SpeechRecognitionController  *speechController_ { nullptr };
    UpdateController             *updates_ { nullptr };
    QPointer<QQuickWindow>        window_;
    bool                          imageDragAccepted_ { false };
    bool                          initialRevealPending_ { false };
    bool                          initialRevealDone_ { false };
    QElapsedTimer                 initialFrameTimer_;
};

} // namespace AnyKeep

#endif // NOTESMANAGERWINDOW_H
