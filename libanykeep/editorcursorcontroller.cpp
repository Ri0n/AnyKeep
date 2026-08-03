#include "editorcursorcontroller.h"

#include <QCursor>
#include <QGuiApplication>
#include <QPixmap>
#include <QQmlContext>
#include <QString>

namespace AnyKeep {

EditorCursorController::EditorCursorController(QObject *parent) : QObject(parent) { }

EditorCursorController::~EditorCursorController() { restoreOverrideCursor(); }

void EditorCursorController::setOverrideCursor(int shape)
{
    const auto cursorShape = Qt::CursorShape(shape);
    if (overrideCursorActive_) {
        QGuiApplication::changeOverrideCursor(QCursor(cursorShape));
        return;
    }

    QGuiApplication::setOverrideCursor(QCursor(cursorShape));
    overrideCursorActive_ = true;
}

void EditorCursorController::setTrashCursor()
{
    const QPixmap trashPixmap(QStringLiteral(":/icons/trash"));
    if (trashPixmap.isNull()) {
        setOverrideCursor(Qt::ForbiddenCursor);
        return;
    }

    const QCursor cursor(trashPixmap, trashPixmap.width() / 2, trashPixmap.height() / 2);
    if (overrideCursorActive_) {
        QGuiApplication::changeOverrideCursor(cursor);
        return;
    }

    QGuiApplication::setOverrideCursor(cursor);
    overrideCursorActive_ = true;
}

void EditorCursorController::restoreOverrideCursor()
{
    if (!overrideCursorActive_)
        return;

    QGuiApplication::restoreOverrideCursor();
    overrideCursorActive_ = false;
}

void installEditorCursorController(QQmlContext *context)
{
    if (!context)
        return;

    context->setContextProperty(QStringLiteral("anykeepCursor"), new EditorCursorController(context));
}

} // namespace AnyKeep
