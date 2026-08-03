#ifndef STICKYNOTEWINDOW_H
#define STICKYNOTEWINDOW_H

#include <QPoint>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include "anykeep_export.h"

class QLabel;
class QToolButton;

namespace AnyKeep {

class StickyNotesServiceInterface;

class ANYKEEP_EXPORT StickyNoteWindow : public QWidget {
    Q_OBJECT
public:
    StickyNoteWindow(const QUuid &stickyId, StickyNotesServiceInterface *service, QWidget *parent = nullptr);

    QUuid stickyId() const { return stickyId_; }
public slots:
    void refresh();

signals:
    void geometryCommitted(const QRect &geometry);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    bool inResizeGrip(const QPoint &position) const;

    QUuid                        stickyId_;
    StickyNotesServiceInterface *service_ = nullptr;
    QLabel                      *title_   = nullptr;
    QLabel                      *body_    = nullptr;
    QPoint                       pressGlobalPosition_;
    QRect                        pressGeometry_;
    QTimer                       geometryCommitTimer_;
    bool                         dragging_ = false;
    bool                         resizing_ = false;
};

} // namespace AnyKeep

#endif // STICKYNOTEWINDOW_H
