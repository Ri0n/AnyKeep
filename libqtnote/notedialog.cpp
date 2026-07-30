#include "notedialog.h"

#include "deintegrationinterface.h"
#include "desktopeditorplatformbackend.h"
#include "desktopnoteactions.h"
#include "draftmanager.h"
#include "editorcursorcontroller.h"
#include "foldercatalogmanager.h"
#include "folderoperationscontroller.h"
#include "localmediaimageprovider.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notestorage.h"
#include "pluginmanager.h"
#include "qtnote.h"
#include "speechrecognitioncontroller.h"
#include "stickynotesmanager.h"
#include "storageiconimageprovider.h"
#include "themediconimageprovider.h"
#include "utils.h"

#include <QCloseEvent>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlContext>
#include <QQuickItem>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>
#include <QTimer>

namespace QtNote {

QHash<QPair<QString, QString>, NoteDialog *> NoteDialog::dialogs_;
QSet<NoteDialog *>                           NoteDialog::allDialogs_;

NoteDialog::NoteDialog(const Note &note, Main *main, const QUuid &draftId) :
    QQuickView(), main_(main), editor_(new NoteEditor(note, draftId, this)),
    platformBackend_(new DesktopEditorPlatformBackend(editor_, this)), desktopActions_(new DesktopNoteActions(this)),
    speechController_(new SpeechRecognitionController(this))
{
    Q_ASSERT(main_);
    allDialogs_.insert(this);
    windowGeometryKey_ = geometryKey();
    alwaysOnTopKey_    = windowGeometryKey_ + QStringLiteral(".always-on-top");

    Qt::WindowFlags flags
        = Qt::Window | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint;
    if (QSettings().value(alwaysOnTopKey_, false).toBool())
        flags |= Qt::WindowStaysOnTopHint;
    setFlags(flags);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setMinimumSize(QSize(320, 240));
    resize(560, 520);
    setColor(QGuiApplication::palette().color(QPalette::Base));

    installLocalMediaImageProvider(engine());
    installStorageIconImageProvider(engine());
    installThemedIconImageProvider(engine());
    installEditorCursorController(rootContext());
    rootContext()->setContextProperty(QStringLiteral("noteEditor"), editor_);
    rootContext()->setContextProperty(QStringLiteral("desktopEditorPlatform"), platformBackend_);
    rootContext()->setContextProperty(QStringLiteral("desktopNoteActions"), desktopActions_);
    rootContext()->setContextProperty(QStringLiteral("desktopSpeech"), speechController_);
    rootContext()->setContextProperty(QStringLiteral("standaloneHost"), this);

    desktopActions_->setEditor(editor_);
    speechController_->setEditor(editor_);
    speechController_->setProvider(main_->pluginManager()->speechRecognitionProvider());
    main_->pluginManager()->attachEditorPlatformBackend(platformBackend_);
    platformBackend_->setDragSource(this);

    connect(main_, &Main::settingsUpdated, this, [this] {
        platformBackend_->reloadVisualSettings();
        speechController_->setProvider(main_->pluginManager()->speechRecognitionProvider());
    });
    connect(editor_, &NoteEditor::textChanged, this, &NoteDialog::updateWindowTitle);
    connect(platformBackend_, &EditorPlatformBackend::operationFailed, this, &NoteDialog::operationFailed);
    connect(desktopActions_, &DesktopNoteActions::operationFailed, this, &NoteDialog::operationFailed);
    connect(speechController_, &SpeechRecognitionController::operationFailed, this, &NoteDialog::operationFailed);
    connect(this, SIGNAL(operationFailed(QString)), main_, SLOT(notifyError(QString)));

    setSource(QUrl(QStringLiteral("qrc:/qml/StandaloneNoteWindow.qml")));
    if (status() == QQuickView::Error)
        qWarning() << "Failed to create standalone note QML window" << errors();
    if (rootObject())
        editor_->registerEditorView(rootObject());

    if (!note.id().isEmpty()) {
        Q_ASSERT(!findDialog(note.storageId(), note.id()));
        dialogs_.insert({ note.storageId(), note.id() }, this);
    }

    const auto storage = note.storage();
    if (storage)
        setIcon(storage->noteIcon());
    updateWindowTitle();

    const auto  restore = main_->restoreWindowGeometry(this, windowGeometryKey_);
    const QRect stored  = QSettings().value(windowGeometryKey_).toRect();
    if (restore == WindowGeometryRestoreResult::Pending) {
        if (stored.isValid())
            resize(stored.size());
    } else if (restore == WindowGeometryRestoreResult::Unsupported) {
        if (stored.isValid() && screen() && screen()->geometry().intersects(stored)) {
            setGeometry(stored);
        } else if (screen()) {
            const QRect available = screen()->availableGeometry();
            const int   x         = QRandomGenerator::global()->bounded(available.left() + available.width() / 4,
                                                                        available.left() + available.width() / 2);
            const int   y         = QRandomGenerator::global()->bounded(available.top() + available.height() / 4,
                                                                        available.top() + available.height() / 2);
            setPosition(x, y);
        }
    }
}

NoteDialog::~NoteDialog()
{
    removeFromRegistry();
    platformBackend_->setDragSource(nullptr);
    setSource(QUrl());
}

NoteDialog *NoteDialog::findDialog(const QString &storageId, const QString &noteId)
{
    return dialogs_.value({ storageId, noteId });
}

QList<NoteDialog *> NoteDialog::openDialogs() { return allDialogs_.values(); }

void NoteDialog::setText(const QString &text)
{
    editor_->setText(text);
    if (rootObject())
        QMetaObject::invokeMethod(rootObject(), "focusInitialEditor", Qt::QueuedConnection);
}

void NoteDialog::registerWindowGeometry() { main_->restoreWindowGeometry(this, windowGeometryKey_); }

bool NoteDialog::alwaysOnTop() const { return flags().testFlag(Qt::WindowStaysOnTopHint); }

void NoteDialog::reportError(const QString &message)
{
    if (!message.isEmpty())
        emit operationFailed(message);
}

bool NoteDialog::pinAvailable() const
{
    return main_->stickyNotesManager()->isAvailable() && !editor_->noteId().isEmpty();
}

void NoteDialog::requestClose() { requestDeferredClose(); }

void NoteDialog::requestDeferredClose()
{
    if (closing_ || closeQueued_)
        return;

    closeQueued_ = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            closeQueued_ = false;
            close();
        },
        Qt::QueuedConnection);
}

bool NoteDialog::trashNote()
{
    flushEditorChanges();
    if (!editor_->noteId().isEmpty()) {
        auto *folderCatalog = FolderCatalogManager::instance();
        if (!folderCatalog->isAvailable()) {
            emit operationFailed(tr("The encrypted folder catalog is unavailable"));
            return false;
        }
        if (!DraftManager::instance()->isLastEditingSession(editor_->draftId())) {
            emit operationFailed(tr("The note is open in another editor and cannot be moved to the recycle bin yet"));
            return false;
        }
        const QUuid previousFolderId = folderCatalog->catalog().folderForNote(editor_->storageId(), editor_->noteId());
        if (!editor_->discardAndClose()) {
            emit operationFailed(editor_->errorString());
            return false;
        }
        const auto error = folderCatalog->recycleNote(editor_->storageId(), editor_->noteId(), previousFolderId);
        if (error) {
            emit operationFailed(error.message);
            return false;
        }
        auto *folderOperations = FolderOperationsController::instance();
        if (!folderOperations->assignNoteFolder(editor_->storageId(), editor_->noteId(), FolderCatalog::recycleBinId(),
                                                true)) {
            emit operationFailed(folderOperations->errorString());
            return false;
        }
    }
    trashRequested_ = true;
    requestDeferredClose();
    return true;
}

bool NoteDialog::pinNote()
{
    flushEditorChanges();
    if (!editor_->save()) {
        emit operationFailed(editor_->errorString());
        return false;
    }
    pinning_ = true;
    requestDeferredClose();
    return true;
}

void NoteDialog::setAlwaysOnTop(bool enabled)
{
    if (alwaysOnTop() == enabled)
        return;

    const bool wasVisible = isVisible();
    auto       newFlags   = flags();
    newFlags.setFlag(Qt::WindowStaysOnTopHint, enabled);

    // Some window-system backends only recreate the native window after a
    // hide/setFlags/show sequence. Calling setFlag() on the visible QQuickView
    // was therefore a no-op on affected desktop configurations.
    if (wasVisible)
        hide();
    setFlags(newFlags);
    QSettings().setValue(alwaysOnTopKey_, enabled);

    // Wayland does not let a client reliably impose stacking by changing a
    // Qt window flag. The KDE integration queues this recreated surface for
    // the companion KWin script, which applies Window.keepAbove in KWin.
    if (QGuiApplication::platformName().contains(QLatin1String("wayland"), Qt::CaseInsensitive))
        main_->restoreWindowGeometry(this, windowGeometryKey_);

    if (wasVisible) {
        show();
        main_->activateWindow(this);
    }
    emit alwaysOnTopChanged();
}

void NoteDialog::trashRequested()
{
    trashRequested_ = true;
    editor_->discardAndClose();
    requestDeferredClose();
}

void NoteDialog::closeEvent(QCloseEvent *event)
{
    if (closing_) {
        event->accept();
        return;
    }
    closing_     = true;
    closeQueued_ = false;
    flushEditorChanges();
    if (!trashRequested_ && !editor_->close()) {
        closing_ = false;
        emit operationFailed(editor_->errorString());
        event->ignore();
        return;
    }

    const bool  awaitingPublication = editor_->hasPersistedDraft();
    const QRect preferredGeometry   = frameGeometry();
    saveGeometryState(trashRequested_);
    removeFromRegistry();
    if (pinning_)
        main_->pinNote(editor_->note(), editor_->draftId(), awaitingPublication, preferredGeometry);

    // Keep the QML object tree alive until deleteLater() runs. Destroying it
    // from closeEvent() is unsafe when close was requested by a QML signal
    // handler (for example, the toolbar delete action).
    event->accept();
    QQuickView::closeEvent(event);
    deleteLater();
}

bool NoteDialog::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate) {
        flushEditorChanges();
        editor_->save();
    } else if (event->type() == QEvent::WindowActivate) {
        editor_->reloadNewerDraft();
    } else if (event->type() == QEvent::DragEnter) {
        auto *drag         = static_cast<QDragEnterEvent *>(event);
        imageDragAccepted_ = platformBackend_->canAcceptImageMimeData(drag->mimeData());
        if (imageDragAccepted_) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
    } else if (event->type() == QEvent::DragMove && imageDragAccepted_) {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        drag->setDropAction(Qt::CopyAction);
        drag->accept();
        return true;
    } else if (event->type() == QEvent::DragLeave) {
        imageDragAccepted_ = false;
    } else if (event->type() == QEvent::Drop && imageDragAccepted_) {
        auto *drop         = static_cast<QDropEvent *>(event);
        imageDragAccepted_ = false;
        flushEditorChanges();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF dropPosition = drop->position();
#else
        const QPointF dropPosition = drop->posF();
#endif
        if (platformBackend_->insertImageMimeData(drop->mimeData(), insertionRowAt(dropPosition))) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
        } else {
            drop->ignore();
        }
        return true;
    }
    return QQuickView::event(event);
}

QString NoteDialog::geometryKey() const
{
    if (!editor_->noteId().isEmpty())
        return QStringLiteral("geometry.%1.%2").arg(editor_->storageId(), editor_->noteId());
    return QStringLiteral("geometry.draft.%1").arg(editor_->draftIdString());
}

void NoteDialog::updateWindowTitle()
{
    const QString firstLine = editor_->text().section(QLatin1Char('\n'), 0, 0).trimmed();
    setTitle(Utils::cuttedDots(firstLine.isEmpty() ? tr("[No Title]") : firstLine, 256));
}

void NoteDialog::saveGeometryState(bool remove)
{
    QSettings settings;
    if (remove) {
        main_->removeWindowGeometry(windowGeometryKey_);
        settings.remove(windowGeometryKey_);
        settings.remove(alwaysOnTopKey_);
        return;
    }
    if (!main_->saveWindowGeometry(this, windowGeometryKey_))
        settings.setValue(windowGeometryKey_, QRect(position(), size()));
}

void NoteDialog::removeFromRegistry()
{
    allDialogs_.remove(this);
    if (editor_ && !editor_->noteId().isEmpty())
        dialogs_.remove({ editor_->storageId(), editor_->noteId() });
}

void NoteDialog::flushEditorChanges()
{
    if (rootObject())
        QMetaObject::invokeMethod(rootObject(), "flushPendingEditorChanges");
}

int NoteDialog::insertionRowAt(const QPointF &position) const
{
    if (!rootObject())
        return editor_->model()->rowCount();
    QVariant result;
    if (!QMetaObject::invokeMethod(rootObject(), "insertionRowAtPoint", Q_RETURN_ARG(QVariant, result),
                                   Q_ARG(QVariant, position.x()), Q_ARG(QVariant, position.y()))) {
        return editor_->model()->rowCount();
    }
    return qBound(0, result.toInt(), editor_->model()->rowCount());
}

} // namespace QtNote
