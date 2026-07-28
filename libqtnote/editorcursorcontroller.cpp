#include "editorcursorcontroller.h"

#include <QCursor>
#include <QGuiApplication>
#include <QQmlContext>
#include <QString>

namespace QtNote {

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

    context->setContextProperty(QStringLiteral("qtnoteCursor"), new EditorCursorController(context));
}

} // namespace QtNote
