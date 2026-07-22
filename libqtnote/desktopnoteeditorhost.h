#ifndef DESKTOPNOTEEDITORHOST_H
#define DESKTOPNOTEEDITORHOST_H

#include "qtnote_export.h"

#include <QPointer>
#include <QWidget>

class QMimeData;
class QPointF;
class QQuickWidget;
class QShowEvent;

namespace QtNote {

class DesktopEditorPlatformBackend;
class NoteBlockModel;
class NoteEditor;

class QTNOTE_EXPORT DesktopNoteEditorHost final : public QWidget {
    Q_OBJECT

public:
    explicit DesktopNoteEditorHost(NoteEditor *editor, QWidget *parent = nullptr);
    ~DesktopNoteEditorHost() override;

    NoteEditor                   *editor() const;
    NoteBlockModel               *model() const;
    DesktopEditorPlatformBackend *platformBackend() const { return platformBackend_; }
    QQuickWidget                 *quickWidget() const { return quick_; }

    void flushPendingEditorChanges();
    void insertText(const QString &text);
    void focusEditor();
    void insertTable();
    void insertList(int type);

signals:
    void focusReceived();
    void focusLost();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void updateFocusWindow();
    int  insertionRowAt(const QPointF &position) const;
    bool canAcceptImageDrop(const QMimeData *mimeData) const;
    bool handleImageDrop(const QMimeData *mimeData, int row);

    QPointer<NoteEditor>          editor_;
    DesktopEditorPlatformBackend *platformBackend_ { nullptr };
    QQuickWidget                 *quick_ { nullptr };
    QPointer<QWidget>             focusWindow_;
    bool                          imageDragAccepted_ { false };
};

} // namespace QtNote

#endif // DESKTOPNOTEEDITORHOST_H
