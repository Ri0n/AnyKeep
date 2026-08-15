#include "desktopnoteeditorhost.h"

#include "desktopeditorplatformbackend.h"
#include "editorcursorcontroller.h"
#include "localmediaimageprovider.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "textdroputils.h"
#include "themediconimageprovider.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QPalette>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace AnyKeep {
namespace {
    bool invokeQmlBoolean(QObject *object, const char *method)
    {
        if (!object)
            return false;
        QVariant result;
        return QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, result)) && result.toBool();
    }

    QVariantMap captureQmlEditorState(QObject *object)
    {
        if (!object)
            return {};
        QVariant result;
        if (!QMetaObject::invokeMethod(object, "captureEditorState", Q_RETURN_ARG(QVariant, result)))
            return {};
        return result.toMap();
    }

    bool invokeQmlTextDrop(QObject *object, const QString &text, const QPointF &position,
                           const TextDropUtils::CodeDetection &code)
    {
        if (!object || text.isEmpty())
            return false;
        QVariant inserted;
        return QMetaObject::invokeMethod(object, "insertDroppedTextAtPoint", Q_RETURN_ARG(QVariant, inserted),
                                         Q_ARG(QVariant, text), Q_ARG(QVariant, position.x()),
                                         Q_ARG(QVariant, position.y()), Q_ARG(QVariant, code.language),
                                         Q_ARG(QVariant, code.isCode))
            && inserted.toBool();
    }

}

DesktopNoteEditorHost::DesktopNoteEditorHost(NoteEditor *editor, QWidget *parent) : QWidget(parent), editor_(editor)
{
    Q_ASSERT(editor_);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    platformBackend_ = new DesktopEditorPlatformBackend(editor_, this);
    if (qGuiApp)
        qGuiApp->installEventFilter(this);
    platformBackend_->setDialogParent(this);

    quick_ = new QQuickWidget(this);
    setFocusPolicy(Qt::StrongFocus);
    quick_->setFocusPolicy(Qt::StrongFocus);
    quick_->setAcceptDrops(true);
    setFocusProxy(quick_);
    quick_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    updateClearColor();
    installLocalMediaImageProvider(quick_->engine());
    installThemedIconImageProvider(quick_->engine());
    installEditorCursorController(quick_->rootContext());
    quick_->rootContext()->setContextProperty(QStringLiteral("noteBlockModel"), editor_->model());
    quick_->rootContext()->setContextProperty(QStringLiteral("noteEditor"), editor_);
    quick_->rootContext()->setContextProperty(QStringLiteral("desktopEditorPlatform"), platformBackend_);
    quick_->setSource(QUrl(QStringLiteral("qrc:/qml/DesktopNoteEditor.qml")));
    quick_->installEventFilter(this);
    layout->addWidget(quick_);

    platformBackend_->setDragSource(quick_);
    editor_->registerEditorView(quick_->rootObject());
    updateFocusWindow();
}

DesktopNoteEditorHost::~DesktopNoteEditorHost()
{
    if (quick_)
        quick_->setSource(QUrl());
}

bool DesktopNoteEditorHost::event(QEvent *event)
{
    const bool paletteChanged
        = event && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange);
    const bool handled = QWidget::event(event);
    if (paletteChanged)
        updateClearColor();
    return handled;
}

void DesktopNoteEditorHost::updateClearColor()
{
    if (!quick_)
        return;
    const QPalette applicationPalette = qGuiApp ? QGuiApplication::palette() : palette();
    quick_->setClearColor(applicationPalette.color(QPalette::Base));
}

NoteEditor     *DesktopNoteEditorHost::editor() const { return editor_.data(); }
NoteBlockModel *DesktopNoteEditorHost::model() const { return editor_ ? editor_->model() : nullptr; }

void DesktopNoteEditorHost::flushPendingEditorChanges()
{
    if (quick_ && quick_->rootObject())
        QMetaObject::invokeMethod(quick_->rootObject(), "flushPendingEditorChanges");
}

void DesktopNoteEditorHost::insertText(const QString &text)
{
    QVariant inserted;
    if (quick_ && quick_->rootObject()
        && QMetaObject::invokeMethod(quick_->rootObject(), "insertTextAtCursor", Q_RETURN_ARG(QVariant, inserted),
                                     Q_ARG(QVariant, text))
        && inserted.toBool()) {
        return;
    }
    if (model())
        model()->appendText(text);
}

void DesktopNoteEditorHost::focusEditor()
{
    quick_->setFocus(Qt::OtherFocusReason);
    if (quick_->rootObject())
        QMetaObject::invokeMethod(quick_->rootObject(), "focusInitialEditor");
}

void DesktopNoteEditorHost::insertTable()
{
    if (quick_->rootObject())
        QMetaObject::invokeMethod(quick_->rootObject(), "insertTableBlock");
}

void DesktopNoteEditorHost::insertList(int type)
{
    if (quick_->rootObject())
        QMetaObject::invokeMethod(quick_->rootObject(), "insertListBlock", Q_ARG(QVariant, type));
}

void DesktopNoteEditorHost::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateFocusWindow();
}

void DesktopNoteEditorHost::updateFocusWindow()
{
    QWidget *current = window();
    if (focusWindow_ == current)
        return;
    if (focusWindow_)
        focusWindow_->removeEventFilter(this);
    focusWindow_ = current;
    if (focusWindow_ && focusWindow_ != this)
        focusWindow_->installEventFilter(this);
}

int DesktopNoteEditorHost::insertionRowAt(const QPointF &position) const
{
    if (!quick_ || !quick_->rootObject() || !model())
        return model() ? model()->rowCount() : 0;
    QVariant result;
    if (!QMetaObject::invokeMethod(quick_->rootObject(), "insertionRowAtPoint", Q_RETURN_ARG(QVariant, result),
                                   Q_ARG(QVariant, position.x()), Q_ARG(QVariant, position.y()))) {
        return model()->rowCount();
    }
    return qBound(0, result.toInt(), model()->rowCount());
}

bool DesktopNoteEditorHost::canAcceptImageDrop(const QMimeData *mimeData) const
{
    return platformBackend_ && platformBackend_->canAcceptImageMimeData(mimeData);
}

bool DesktopNoteEditorHost::handleImageDrop(const QMimeData *mimeData, int row)
{
    return platformBackend_ && platformBackend_->insertImageMimeData(mimeData, row);
}

bool DesktopNoteEditorHost::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qGuiApp && event->type() == QEvent::ApplicationPaletteChange) {
        // The platform theme updates the application palette independently of
        // this child widget's inherited palette notification. Refresh after
        // the application event has completed so both paths use the new Base.
        QTimer::singleShot(0, this, &DesktopNoteEditorHost::updateClearColor);
    }

    if (watched == focusWindow_) {
        if (event->type() == QEvent::WindowDeactivate) {
            flushPendingEditorChanges();
            if (focusReported_) {
                focusReported_ = false;
                emit focusLost();
            }
        } else if (event->type() == QEvent::WindowActivate) {
            // A sibling manager/standalone window may have checkpointed a newer
            // revision while this shell was inactive. The NoteWidget connection
            // is queued and NoteEditor::reloadNewerDraft() still refuses to
            // overwrite local dirty state.
            if (!focusReported_) {
                focusReported_ = true;
                emit focusReceived();
            }
        }
    }

    if (watched == quick_) {
        if (event->type() == QEvent::DragEnter) {
            auto *dragEvent    = static_cast<QDragEnterEvent *>(event);
            imageDragAccepted_ = canAcceptImageDrop(dragEvent->mimeData());
            textDragAccepted_  = !imageDragAccepted_ && !TextDropUtils::plainText(dragEvent->mimeData()).isEmpty();
            if (imageDragAccepted_ || textDragAccepted_) {
                dragEvent->setDropAction(Qt::CopyAction);
                dragEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::DragMove && (imageDragAccepted_ || textDragAccepted_)) {
            auto *dragEvent = static_cast<QDragMoveEvent *>(event);
            dragEvent->setDropAction(Qt::CopyAction);
            dragEvent->accept();
            return true;
        } else if (event->type() == QEvent::DragLeave) {
            imageDragAccepted_ = false;
            textDragAccepted_  = false;
        } else if (event->type() == QEvent::Drop && (imageDragAccepted_ || textDragAccepted_)) {
            auto         *dropEvent = static_cast<QDropEvent *>(event);
            const QPointF position  = dropEvent->position();
            const bool    imageDrop = imageDragAccepted_;
            imageDragAccepted_      = false;
            textDragAccepted_       = false;
            const bool handled      = imageDrop
                     ? handleImageDrop(dropEvent->mimeData(), insertionRowAt(position))
                     : invokeQmlTextDrop(quick_->rootObject(), TextDropUtils::plainText(dropEvent->mimeData()), position,
                                         TextDropUtils::detectCode(dropEvent->mimeData()));
            if (handled) {
                dropEvent->setDropAction(Qt::CopyAction);
                dropEvent->accept();
            } else {
                dropEvent->ignore();
            }
            return true;
        } else if (event->type() == QEvent::KeyPress) {
            auto    *keyEvent = static_cast<QKeyEvent *>(event);
            QObject *root     = quick_->rootObject();
            if (invokeQmlBoolean(root, "documentHistoryOwnsFocus")) {
                const auto modifiers = keyEvent->modifiers();
                const bool plainText = !keyEvent->text().isEmpty()
                    && !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
                const bool deletion = keyEvent->key() == Qt::Key_Backspace || keyEvent->key() == Qt::Key_Delete;
                editor_->updateHistoryViewState(captureQmlEditorState(root), !(plainText || deletion));
                if (keyEvent->matches(QKeySequence::Undo) && editor_->undo())
                    return true;
                if (keyEvent->matches(QKeySequence::Redo) && editor_->redo())
                    return true;
                if (keyEvent->matches(QKeySequence::Copy) && invokeQmlBoolean(root, "copyActiveSelection"))
                    return true;
                if (keyEvent->matches(QKeySequence::Cut) && invokeQmlBoolean(root, "cutActiveSelection"))
                    return true;
                if (keyEvent->matches(QKeySequence::Paste) && invokeQmlBoolean(root, "pasteClipboard"))
                    return true;
            }
        } else if (event->type() == QEvent::InputMethod) {
            QObject *root = quick_->rootObject();
            if (invokeQmlBoolean(root, "documentHistoryOwnsFocus"))
                editor_->updateHistoryViewState(captureQmlEditorState(root), false);
        } else if (event->type() == QEvent::FocusIn) {
            if (!focusReported_) {
                focusReported_ = true;
                emit focusReceived();
            }
            QTimer::singleShot(0, this, &DesktopNoteEditorHost::focusEditor);
        } else if (event->type() == QEvent::FocusOut) {
            flushPendingEditorChanges();
            if (focusReported_) {
                focusReported_ = false;
                emit focusLost();
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace AnyKeep
