#ifndef QTNOTE_MOBILEEDITORPLATFORMBACKEND_H
#define QTNOTE_MOBILEEDITORPLATFORMBACKEND_H

#include "editorplatformbackend.h"

#include <QPointer>

namespace QtNote {

class AndroidPlatformServices;

class MobileEditorPlatformBackend final : public EditorPlatformBackend {
    Q_OBJECT

public:
    explicit MobileEditorPlatformBackend(AndroidPlatformServices *services, QObject *parent = nullptr);

    Q_INVOKABLE bool insertImage(int row = -1) override;

private:
    AndroidPlatformServices *services_ { nullptr };
    QPointer<NoteEditor>     pendingEditor_;
    int                      pendingRow_ { -1 };
};

} // namespace QtNote

#endif // QTNOTE_MOBILEEDITORPLATFORMBACKEND_H
