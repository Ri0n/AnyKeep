#ifndef DEINTEGRATIONINTERFACE_H
#define DEINTEGRATIONINTERFACE_H

#include <QString>

class QWindow;

namespace QtNote {

enum class WindowGeometryRestoreResult { Unsupported, Restored, Pending };

class DEIntegrationInterface {
public:
    virtual void activateWindow(QWindow *window) = 0;

    // Return false when the desktop integration does not support the operation.
    // The caller can then use its platform-independent fallback.
    virtual WindowGeometryRestoreResult restoreWindowGeometry(QWindow *, const QString &)
    {
        return WindowGeometryRestoreResult::Unsupported;
    }
    virtual bool    saveWindowGeometry(QWindow *, const QString &) { return false; }
    virtual bool    removeWindowGeometry(const QString &) { return false; }
    virtual QString takePendingWindowGeometryKey() { return {}; }
    virtual void    windowGeometryBridgeReady() { }
};

} // namespace QtNote

Q_DECLARE_INTERFACE(QtNote::DEIntegrationInterface, "com.rion-soft.QtNote.DEIntegrationInterface/2.0")

#endif
