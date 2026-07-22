#include "stickynotewindow.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include "stickynotesintegrationinterface.h"

namespace QtNote {

namespace {
    constexpr int MinimumWidth  = 220;
    constexpr int MinimumHeight = 140;
    constexpr int GripSize      = 18;
    constexpr int CornerRadius  = 7;

    Qt::WindowFlags stickyWindowFlags()
    {
        const auto type = QGuiApplication::platformName() == QLatin1String("xcb") ? Qt::Window : Qt::Tool;
        return type | Qt::FramelessWindowHint;
    }
}

StickyNoteWindow::StickyNoteWindow(const QUuid &stickyId, StickyNotesServiceInterface *service, QWidget *parent) :
    QWidget(parent, stickyWindowFlags()), stickyId_(stickyId), service_(service)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(MinimumWidth, MinimumHeight);
    setMouseTracking(true);
    setWindowTitle(tr("QtNote Sticky Note"));
    geometryCommitTimer_.setSingleShot(true);
    geometryCommitTimer_.setInterval(250);
    connect(&geometryCommitTimer_, &QTimer::timeout, this, [this] { emit geometryCommitted(geometry()); });
    setStyleSheet(
        QStringLiteral("QLabel { color: #3d361f; background: transparent; }"
                       "QToolButton { color: #3d361f; background: transparent; border: none; border-radius: 11px; "
                       "font-size: 18px; font-weight: bold; padding: 0px; }"
                       "QToolButton:hover { background: rgba(80,60,10,38); }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 24);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    title_         = new QLabel(this);
    auto titleFont = title_->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.1);
    title_->setFont(titleFont);
    title_->setTextInteractionFlags(Qt::NoTextInteraction);
    title_->setAttribute(Qt::WA_TransparentForMouseEvents);
    header->addWidget(title_, 1);
    auto *closeButton = new QToolButton(this);
    closeButton->setText(QStringLiteral("✕"));
    closeButton->setFixedSize(24, 24);
    closeButton->setAutoRaise(true);
    closeButton->setToolTip(tr("Unpin"));
    header->addWidget(closeButton);
    layout->addLayout(header);

    body_ = new QLabel(this);
    body_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    body_->setWordWrap(true);
    body_->setTextInteractionFlags(Qt::NoTextInteraction);
    body_->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(body_, 1);

    connect(closeButton, &QToolButton::clicked, this, [this] {
        if (service_)
            service_->unpin(stickyId_);
    });
    if (service_ && service_->notifier())
        connect(service_->notifier(), SIGNAL(notesChanged()), this, SLOT(refresh()));
    refresh();
}

void StickyNoteWindow::refresh()
{
    const auto document = QJsonDocument::fromJson(service_ ? service_->noteJson(stickyId_).toUtf8() : QByteArray {});
    const auto object   = document.object();
    title_->setText(object.value(QStringLiteral("title")).toString(tr("[No Title]")));
    body_->setText(object.value(QStringLiteral("body")).toString());
}

bool StickyNoteWindow::inResizeGrip(const QPoint &position) const
{
    return position.x() >= width() - GripSize && position.y() >= height() - GripSize;
}

void StickyNoteWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    pressGlobalPosition_ = event->globalPosition().toPoint();
    pressGeometry_       = geometry();
    resizing_            = inResizeGrip(event->pos());
    dragging_            = !resizing_;
    if (windowHandle()) {
        const bool started = resizing_ ? windowHandle()->startSystemResize(Qt::RightEdge | Qt::BottomEdge)
                                       : windowHandle()->startSystemMove();
        if (started) {
            resizing_ = false;
            dragging_ = false;
        }
    }
    event->accept();
}

void StickyNoteWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging_ && !resizing_) {
        setCursor(inResizeGrip(event->pos()) ? Qt::SizeFDiagCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint delta = event->globalPosition().toPoint() - pressGlobalPosition_;
    if (resizing_)
        resize(qMax(MinimumWidth, pressGeometry_.width() + delta.x()),
               qMax(MinimumHeight, pressGeometry_.height() + delta.y()));
    else
        move(pressGeometry_.topLeft() + delta);
    event->accept();
}

void StickyNoteWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && (dragging_ || resizing_)) {
        dragging_ = false;
        resizing_ = false;
        emit geometryCommitted(geometry());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void StickyNoteWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && service_) {
        service_->open(stickyId_);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void StickyNoteWindow::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    if (isVisible())
        geometryCommitTimer_.start();
}

void StickyNoteWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (isVisible())
        geometryCommitTimer_.start();
}

void StickyNoteWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF backgroundRect = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);
    painter.setPen(QPen(QColor(90, 75, 20, 140), 1.5));
    painter.setBrush(QColor(QStringLiteral("#fff2a8")));
    painter.drawRoundedRect(backgroundRect, CornerRadius, CornerRadius);

    const QPointF gripCenter(width() - 4.0, height() - 4.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(61, 54, 31, 140), 1.5));
    for (const int radius : { 4, 8, 12 }) {
        const QRectF arcRect(gripCenter.x() - radius, gripCenter.y() - radius, radius * 2.0, radius * 2.0);
        painter.drawArc(arcRect, 90 * 16, 90 * 16);
    }
}

} // namespace QtNote
