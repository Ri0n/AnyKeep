#include "notesmanagerwindow.h"

#include "desktopeditorplatformbackend.h"
#include "desktopnoteactions.h"
#include "editorcursorcontroller.h"
#include "localmediaimageprovider.h"
#include "notemanager.h"
#include "notestorage.h"
#include "notesworkspacecontroller.h"
#include "settingswindow.h"
#include "speechrecognitioncontroller.h"
#include "storageiconimageprovider.h"
#include "themediconimageprovider.h"
#include "updatecontroller.h"

#include <QDebug>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtGlobal>

namespace AnyKeep {

NotesManagerWindow::NotesManagerWindow(QObject *parent) : NotesManagerWindow(nullptr, parent) { }

NotesManagerWindow::NotesManagerWindow(UpdateController *updates, QObject *parent) : QObject(parent), updates_(updates)
{
    workspace_        = new NotesWorkspaceController(this);
    platformBackend_  = new DesktopEditorPlatformBackend(workspace_->editor(), this);
    desktopActions_   = new DesktopNoteActions(this);
    speechController_ = new SpeechRecognitionController(this);
    desktopActions_->setEditor(workspace_->editor());
    speechController_->setEditor(workspace_->editor());
    // Register this before QML bindings observe currentEditorChanged. New
    // delegates must attach their text documents to the backend for the same
    // editor rather than being cleared by a late backend switch.
    connect(workspace_, &NotesWorkspaceController::currentEditorChanged, this, [this] {
        platformBackend_->setEditor(workspace_->editor());
        desktopActions_->setEditor(workspace_->editor());
        speechController_->setEditor(workspace_->editor());
    });
    connect(platformBackend_, &EditorPlatformBackend::operationFailed, this, &NotesManagerWindow::operationFailed);
    connect(desktopActions_, &DesktopNoteActions::operationFailed, this, &NotesManagerWindow::operationFailed);
    connect(speechController_, &SpeechRecognitionController::operationFailed, this,
            &NotesManagerWindow::operationFailed);
    connect(workspace_, &NotesWorkspaceController::openStandaloneRequested, this,
            &NotesManagerWindow::openNoteRequested);
    connect(workspace_, &NotesWorkspaceController::storageSettingsRequested, this, [this](const QString &storageId) {
        const auto storage = NoteManager::instance()->storage(storageId);
        if (!storage)
            return;
        auto *controller = storage->createSettingsController(nullptr);
        if (!controller)
            return;
        QUrl component = storage->settingsComponent();
        if (component.isEmpty())
            component = QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml"));
        auto *settings
            = new SettingsWindow(controller, component, storage->name() + QStringLiteral(": ") + tr("Settings"), this);
        settings->show();
    });

    engine_ = new QQmlApplicationEngine(this);
    installLocalMediaImageProvider(engine_);
    installStorageIconImageProvider(engine_);
    installThemedIconImageProvider(engine_);
    installEditorCursorController(engine_->rootContext());
    engine_->rootContext()->setContextProperty(QStringLiteral("notesWorkspace"), workspace_);
    engine_->rootContext()->setContextProperty(QStringLiteral("desktopEditorPlatform"), platformBackend_);
    engine_->rootContext()->setContextProperty(QStringLiteral("desktopNoteActions"), desktopActions_);
    engine_->rootContext()->setContextProperty(QStringLiteral("desktopSpeech"), speechController_);
    engine_->rootContext()->setContextProperty(QStringLiteral("anykeepUpdates"), updates_);
    engine_->load(QUrl(QStringLiteral("qrc:/qml/NotesManagerWindow.qml")));

    if (!engine_->rootObjects().isEmpty())
        window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().constFirst());
    if (!window_)
        qWarning() << "Failed to create the QML note manager window";
    else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        // ApplicationWindow padding was introduced in Qt 6.9. Keep the
        // desktop manager flush with the top edge on newer Qt versions
        // without making the shared QML fail to load on Qt 6.4.
        window_->setProperty("topPadding", 0.0);
#endif
        window_->installEventFilter(this);
        restoreWindowState();
    }

    platformBackend_->setDragSource(window_.data());
}

NotesManagerWindow::~NotesManagerWindow()
{
    saveWindowState();
    requestWorkspaceClose();

    // workspace_, platformBackend_, and engine_ are QObject children of this
    // object. QObject child deletion order must not be allowed to destroy the
    // workspace while live QML bindings still reference its context property.
    // Tear down the engine (and therefore the root window/bindings) explicitly
    // before QObject starts deleting the remaining children.
    platformBackend_->setDragSource(nullptr);
    delete engine_;
    engine_ = nullptr;
    window_.clear();
}

void NotesManagerWindow::setSpeechRecognitionProvider(SpeechRecognitionProviderInterface *provider)
{
    speechController_->setProvider(provider);
}

bool NotesManagerWindow::isReady() const { return !window_.isNull(); }
bool NotesManagerWindow::isVisible() const { return window_ && window_->isVisible(); }

void NotesManagerWindow::show()
{
    if (!window_)
        return;

#ifdef Q_OS_WIN
    if (!window_->isVisible() && !initialRevealDone_ && !initialRevealPending_) {
        // Match standalone notes and reveal this native window only after its
        // first scene-graph frame has actually reached the swap chain.
        initialRevealPending_ = true;
        initialFrameTimer_.start();
        window_->setOpacity(0.0);
        connect(window_.data(), &QQuickWindow::frameSwapped, this, &NotesManagerWindow::revealAfterInitialFrame,
                Qt::SingleShotConnection);
        window_->show();
        window_->raise();
        window_->requestActivate();
        QTimer::singleShot(1000, this, &NotesManagerWindow::revealAfterInitialFrame);
        return;
    }
#endif
    window_->show();
    window_->raise();
    window_->requestActivate();
}

void NotesManagerWindow::revealAfterInitialFrame()
{
    if (!initialRevealPending_)
        return;

    initialRevealPending_ = false;
    initialRevealDone_    = true;
    qDebug() << "Note manager first frame ready in" << initialFrameTimer_.elapsed() << "ms";
    if (window_ && window_->isVisible())
        window_->setOpacity(1.0);
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

} // namespace AnyKeep
