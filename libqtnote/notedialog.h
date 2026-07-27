#ifndef NOTEDIALOG_H
#define NOTEDIALOG_H

#include "note.h"

#include <QHash>
#include <QPointer>
#include <QQuickView>
#include <QSet>
#include <QUuid>

namespace QtNote {

class DesktopEditorPlatformBackend;
class DesktopNoteActions;
class Main;
class NoteEditor;
class SpeechRecognitionController;

// Thin desktop window host. Editing, autosave, find, formatting, deletion UI,
// and toolbar composition live in the shared NoteEditorPane QML component.
class NoteDialog final : public QQuickView {
    Q_OBJECT
    Q_PROPERTY(bool alwaysOnTop READ alwaysOnTop NOTIFY alwaysOnTopChanged)
    Q_PROPERTY(bool pinAvailable READ pinAvailable CONSTANT)
    Q_PROPERTY(bool askBeforeDelete READ askBeforeDelete CONSTANT)

public:
    explicit NoteDialog(const Note &note, Main *main, const QUuid &draftId = {});
    ~NoteDialog() override;

    static NoteDialog         *findDialog(const QString &storageId, const QString &noteId);
    static QList<NoteDialog *> openDialogs();

    NoteEditor *editor() const { return editor_; }
    void        setText(const QString &text);
    void        registerWindowGeometry();

    bool alwaysOnTop() const;
    bool pinAvailable() const;
    bool askBeforeDelete() const;

    Q_INVOKABLE void requestClose();
    Q_INVOKABLE bool deleteNote();
    Q_INVOKABLE bool pinNote();
    Q_INVOKABLE void setAlwaysOnTop(bool enabled);

public slots:
    void trashRequested();

signals:
    void alwaysOnTopChanged();
    void operationFailed(const QString &message);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool event(QEvent *event) override;

private:
    QString geometryKey() const;
    void    updateWindowTitle();
    void    saveGeometryState(bool remove = false);
    void    removeFromRegistry();
    void    flushEditorChanges();
    void    requestDeferredClose();
    int     insertionRowAt(const QPointF &position) const;

    Main                         *main_ { nullptr };
    NoteEditor                   *editor_ { nullptr };
    DesktopEditorPlatformBackend *platformBackend_ { nullptr };
    DesktopNoteActions           *desktopActions_ { nullptr };
    SpeechRecognitionController  *speechController_ { nullptr };
    QString                       windowGeometryKey_;
    QString                       alwaysOnTopKey_;
    bool                          trashRequested_ { false };
    bool                          pinning_ { false };
    bool                          closing_ { false };
    bool                          closeQueued_ { false };
    bool                          imageDragAccepted_ { false };

    static QHash<QPair<QString, QString>, NoteDialog *> dialogs_;
    static QSet<NoteDialog *>                           allDialogs_;
};

} // namespace QtNote

#endif // NOTEDIALOG_H
