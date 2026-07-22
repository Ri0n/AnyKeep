#include "desktopnoteeditorhost.h"

#include "desktopeditorplatformbackend.h"
#include "localmediaimageprovider.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "themediconimageprovider.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace QtNote {
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

}

DesktopNoteEditorHost::DesktopNoteEditorHost(NoteEditor *editor, QWidget *parent) : QWidget(parent), editor_(editor)
{
    Q_ASSERT(editor_);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    platformBackend_ = new DesktopEditorPlatformBackend(editor_, this);
    platformBackend_->setDialogParent(this);

    quick_ = new QQuickWidget(this);
    setFocusPolicy(Qt::StrongFocus);
    quick_->setFocusPolicy(Qt::StrongFocus);
    quick_->setAcceptDrops(true);
    setFocusProxy(quick_);
    quick_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick_->setClearColor(palette().color(QPalette::Base));
    installLocalMediaImageProvider(quick_->engine());
    installThemedIconImageProvider(quick_->engine());
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
    if (watched == focusWindow_) {
        if (event->type() == QEvent::WindowDeactivate) {
            flushPendingEditorChanges();
            emit focusLost();
        } else if (event->type() == QEvent::WindowActivate) {
            // A sibling manager/standalone window may have checkpointed a newer
            // revision while this shell was inactive. The NoteWidget connection
            // is queued and NoteEditor::reloadNewerDraft() still refuses to
            // overwrite local dirty state.
            emit focusReceived();
        }
    }

    if (watched == quick_) {
        if (event->type() == QEvent::DragEnter) {
            auto *dragEvent    = static_cast<QDragEnterEvent *>(event);
            imageDragAccepted_ = canAcceptImageDrop(dragEvent->mimeData());
            if (imageDragAccepted_) {
                dragEvent->setDropAction(Qt::CopyAction);
                dragEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::DragMove && imageDragAccepted_) {
            auto *dragEvent = static_cast<QDragMoveEvent *>(event);
            dragEvent->setDropAction(Qt::CopyAction);
            dragEvent->accept();
            return true;
        } else if (event->type() == QEvent::DragLeave) {
            imageDragAccepted_ = false;
        } else if (event->type() == QEvent::Drop && imageDragAccepted_) {
            auto         *dropEvent = static_cast<QDropEvent *>(event);
            const QPointF position  = dropEvent->position();
            imageDragAccepted_      = false;
            if (handleImageDrop(dropEvent->mimeData(), insertionRowAt(position))) {
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
            emit focusReceived();
            QTimer::singleShot(0, this, &DesktopNoteEditorHost::focusEditor);
        } else if (event->type() == QEvent::FocusOut) {
            flushPendingEditorChanges();
            emit focusLost();
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace QtNote
