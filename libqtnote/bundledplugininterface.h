#ifndef BUNDLEDPLUGININTERFACE_H
#define BUNDLEDPLUGININTERFACE_H

#include <QtPlugin>

namespace QtNote {

// UI-neutral lifecycle used by plugins that are compiled into a platform
// application instead of discovered as desktop shared libraries. Implementations
// may register storages, providers, and actions through the common QtNote core,
// but must not depend on QWidget, QDialog, DBus, or the desktop Main shell.
class BundledPluginInterface {
public:
    virtual ~BundledPluginInterface() = default;

    virtual bool initialize() = 0;
    virtual void shutdown() { }
};

} // namespace QtNote

Q_DECLARE_INTERFACE(QtNote::BundledPluginInterface, "com.rion-soft.QtNote.BundledPluginInterface/1.0")

#endif // BUNDLEDPLUGININTERFACE_H
