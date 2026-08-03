#ifndef EDITORCURSORCONTROLLER_H
#define EDITORCURSORCONTROLLER_H

#include "anykeep_export.h"

#include <QObject>

class QQmlContext;

namespace AnyKeep {

class ANYKEEP_EXPORT EditorCursorController : public QObject {
    Q_OBJECT
public:
    explicit EditorCursorController(QObject *parent = nullptr);
    ~EditorCursorController() override;

    Q_INVOKABLE void setOverrideCursor(int shape);
    Q_INVOKABLE void setTrashCursor();
    Q_INVOKABLE void restoreOverrideCursor();

private:
    bool overrideCursorActive_ = false;
};

ANYKEEP_EXPORT void installEditorCursorController(QQmlContext *context);

} // namespace AnyKeep

#endif // EDITORCURSORCONTROLLER_H
