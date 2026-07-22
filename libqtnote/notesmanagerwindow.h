#ifndef NOTESMANAGERWINDOW_H
#define NOTESMANAGERWINDOW_H

#include "qtnote_export.h"

#include <QObject>
#include <QPointer>

class QPointF;
class QQmlApplicationEngine;
class QQuickWindow;

namespace QtNote {

class DesktopEditorPlatformBackend;
class NotesWorkspaceController;

class QTNOTE_EXPORT NotesManagerWindow final : public QObject {
    Q_OBJECT

public:
    explicit NotesManagerWindow(QObject *parent = nullptr);
    ~NotesManagerWindow() override;

    bool                          isReady() const;
    DesktopEditorPlatformBackend *platformBackend() const { return platformBackend_; }
    bool                          isVisible() const;
    void                          show();
    bool                          close();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
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
    QPointer<QQuickWindow>        window_;
    bool                          imageDragAccepted_ { false };
};

} // namespace QtNote

#endif // NOTESMANAGERWINDOW_H
