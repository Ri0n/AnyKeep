#ifndef BUNDLEDPLUGININTERFACE_H
#define BUNDLEDPLUGININTERFACE_H

#include <QtPlugin>

namespace AnyKeep {

// UI-neutral lifecycle used by plugins that are compiled into a platform
// application instead of discovered as desktop shared libraries. Implementations
// may register storages, providers, and actions through the common AnyKeep core,
// but must not depend on QWidget, QDialog, DBus, or the desktop Main shell.
class BundledPluginInterface {
public:
    virtual ~BundledPluginInterface() = default;

    virtual bool initialize() = 0;
    virtual void shutdown() {}
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::BundledPluginInterface, "com.rion-soft.AnyKeep.BundledPluginInterface/1.0")

#endif // BUNDLEDPLUGININTERFACE_H
