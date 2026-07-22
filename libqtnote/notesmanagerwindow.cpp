#include "notesmanagerwindow.h"

#include "desktopeditorplatformbackend.h"
#include "localmediaimageprovider.h"
#include "notesworkspacecontroller.h"
#include "storageiconimageprovider.h"
#include "themediconimageprovider.h"

#include <QDebug>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSettings>
#include <QUrl>
#include <QVariant>

namespace QtNote {

NotesManagerWindow::NotesManagerWindow(QObject *parent) : QObject(parent)
{
    workspace_       = new NotesWorkspaceController(this);
    platformBackend_ = new DesktopEditorPlatformBackend(workspace_->editor(), this);
    // Register this before QML bindings observe currentEditorChanged. New
    // delegates must attach their text documents to the backend for the same
    // editor rather than being cleared by a late backend switch.
    connect(workspace_, &NotesWorkspaceController::currentEditorChanged, this,
            [this] { platformBackend_->setEditor(workspace_->editor()); });
    connect(workspace_, &NotesWorkspaceController::openStandaloneRequested, this,
            &NotesManagerWindow::openNoteRequested);

    engine_ = new QQmlApplicationEngine(this);
    installLocalMediaImageProvider(engine_);
    installStorageIconImageProvider(engine_);
    installThemedIconImageProvider(engine_);
    engine_->rootContext()->setContextProperty(QStringLiteral("notesWorkspace"), workspace_);
    engine_->rootContext()->setContextProperty(QStringLiteral("desktopEditorPlatform"), platformBackend_);
    engine_->load(QUrl(QStringLiteral("qrc:/qml/NotesManagerWindow.qml")));

    if (!engine_->rootObjects().isEmpty())
        window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().constFirst());
    if (!window_)
        qWarning() << "Failed to create the QML note manager window";
    else {
        window_->installEventFilter(this);
        restoreWindowState();
    }

    platformBackend_->setDragSource(window_.data());
}

NotesManagerWindow::~NotesManagerWindow()
{
    saveWindowState();
    requestWorkspaceClose();
}

bool NotesManagerWindow::isReady() const { return !window_.isNull(); }
bool NotesManagerWindow::isVisible() const { return window_ && window_->isVisible(); }

void NotesManagerWindow::show()
{
    if (!window_)
        return;
    window_->show();
    window_->raise();
    window_->requestActivate();
}

bool NotesManagerWindow::close()
{
    if (!requestWorkspaceClose())
        return false;
    if (window_)
        window_->close();
    return true;
}

bool NotesManagerWindow::requestWorkspaceClose()
{
    if (!workspace_ || !workspace_->currentEditor())
        return true;
    if (window_) {
        QVariant result;
        if (QMetaObject::invokeMethod(window_.data(), "closeWorkspace", Q_RETURN_ARG(QVariant, result)))
            return result.toBool();
    }
    return workspace_->closeCurrentNote();
}

void NotesManagerWindow::flushEditorChanges()
{
    if (window_)
        QMetaObject::invokeMethod(window_.data(), "flushEditorChanges");
}

int NotesManagerWindow::insertionRowAt(const QPointF &position) const
{
    if (!window_)
        return -1;
    QVariant result;
    if (!QMetaObject::invokeMethod(window_.data(), "insertionRowAtPoint", Q_RETURN_ARG(QVariant, result),
                                   Q_ARG(QVariant, position.x()), Q_ARG(QVariant, position.y()))) {
        return -1;
    }
    return result.toInt();
}

void NotesManagerWindow::restoreWindowState()
{
    if (!window_)
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("note-manager-window"));

    const int width  = settings.value(QStringLiteral("width"), window_->width()).toInt();
    const int height = settings.value(QStringLiteral("height"), window_->height()).toInt();
    window_->resize(qMax(width, window_->minimumWidth()), qMax(height, window_->minimumHeight()));

    if (settings.contains(QStringLiteral("x")) && settings.contains(QStringLiteral("y"))) {
        window_->setPosition(settings.value(QStringLiteral("x")).toInt(), settings.value(QStringLiteral("y")).toInt());
    }

    if (settings.contains(QStringLiteral("navigationWidth"))) {
        window_->setProperty("navigationWidth", settings.value(QStringLiteral("navigationWidth")).toReal());
    }
}

void NotesManagerWindow::saveWindowState() const
{
    if (!window_)
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("note-manager-window"));
    settings.setValue(QStringLiteral("width"), window_->width());
    settings.setValue(QStringLiteral("height"), window_->height());
    settings.setValue(QStringLiteral("x"), window_->x());
    settings.setValue(QStringLiteral("y"), window_->y());
    settings.setValue(QStringLiteral("navigationWidth"), window_->property("navigationWidth"));
}

bool NotesManagerWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != window_.data())
        return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::DragEnter) {
        auto *dragEvent    = static_cast<QDragEnterEvent *>(event);
        imageDragAccepted_ = platformBackend_->canAcceptImageMimeData(dragEvent->mimeData());
        if (imageDragAccepted_) {
            dragEvent->setDropAction(Qt::CopyAction);
            dragEvent->accept();
            return true;
        }
    } else if (event->type() == QEvent::DragMove && imageDragAccepted_) {
        auto *dragEvent = static_cast<QDragMoveEvent *>(event);
        if (insertionRowAt(dragEvent->position()) >= 0) {
            dragEvent->setDropAction(Qt::CopyAction);
            dragEvent->accept();
        } else {
            dragEvent->ignore();
        }
        return true;
    } else if (event->type() == QEvent::Hide) {
        saveWindowState();
    } else if (event->type() == QEvent::DragLeave) {
        imageDragAccepted_ = false;
    } else if (event->type() == QEvent::Drop && imageDragAccepted_) {
        auto *dropEvent    = static_cast<QDropEvent *>(event);
        imageDragAccepted_ = false;
        flushEditorChanges();
        const int row = insertionRowAt(dropEvent->position());
        if (row >= 0 && platformBackend_->insertImageMimeData(dropEvent->mimeData(), row)) {
            dropEvent->setDropAction(Qt::CopyAction);
            dropEvent->accept();
        } else {
            dropEvent->ignore();
        }
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace QtNote
