#ifndef DEINTEGRATIONINTERFACE_H
#define DEINTEGRATIONINTERFACE_H

#include <QString>

class QWindow;

namespace AnyKeep {

enum class WindowGeometryRestoreResult { Unsupported, Restored, Pending };

class DEIntegrationInterface {
public:
    virtual void activateWindow(QWindow *window) = 0;

    // Note windows are identified by a stable window id (see
    // NoteWindowGeometryStore) and the caller owns their stored geometry.
    // restoreWindowGeometry() is called before the window is shown:
    // - Unsupported: the caller applies the stored geometry itself;
    // - Pending: a compositor helper places the window, the caller only sizes it;
    // - Restored: the integration applied the geometry.
    virtual WindowGeometryRestoreResult restoreWindowGeometry(QWindow *, const QString &)
    {
        return WindowGeometryRestoreResult::Unsupported;
    }
    // True when a compositor helper stores the geometry of this window.
    virtual bool saveWindowGeometry(QWindow *, const QString &) { return false; }
    virtual bool removeWindowGeometry(const QString &) { return false; }
    // The window's id changed while it is shown, e.g. its draft was published.
    virtual void setWindowIdentity(QWindow *, const QString &) {}
    // Bridges that identify windows by the order they appear in (GNOME,
    // Cinnamon) hand out the ids of windows being shown, oldest first.
    virtual QString takePendingWindowGeometryKey() { return {}; }
    virtual void    windowGeometryBridgeReady() {}
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::DEIntegrationInterface, "com.rion-soft.AnyKeep.DEIntegrationInterface/2.1")

#endif
