#ifndef EDITORCURSORCONTROLLER_H
#define EDITORCURSORCONTROLLER_H

#include "qtnote_export.h"

#include <QObject>

class QQmlContext;

namespace QtNote {

class QTNOTE_EXPORT EditorCursorController : public QObject {
    Q_OBJECT
public:
    explicit EditorCursorController(QObject *parent = nullptr);
    ~EditorCursorController() override;

    Q_INVOKABLE void setOverrideCursor(int shape);
    Q_INVOKABLE void restoreOverrideCursor();

private:
    bool overrideCursorActive_ = false;
};

QTNOTE_EXPORT void installEditorCursorController(QQmlContext *context);

} // namespace QtNote

#endif // EDITORCURSORCONTROLLER_H
