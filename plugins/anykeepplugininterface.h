#ifndef ANYKEEPPLUGININTERFACE_H
#define ANYKEEPPLUGININTERFACE_H

#include <QString>

#include "pluginhostinterface.h"

namespace AnyKeep {

class PluginInterface {
public:
    virtual ~PluginInterface()                      = default;
    virtual void setHost(PluginHostInterface *host) = 0;
};

class RegularPluginInterface {
public:
    virtual ~RegularPluginInterface() = default;
    virtual bool initialize()         = 0;
    virtual void shutdown() { }
};

class PluginOptionsTooltipInterface {
public:
    virtual ~PluginOptionsTooltipInterface() = default;
    virtual QString tooltip() const          = 0;
};

} // namespace AnyKeep

#define ANYKEEP_PLUGIN_INTERFACE_IID "com.rion-soft.AnyKeep.PluginInterface/4.0"
Q_DECLARE_INTERFACE(AnyKeep::PluginInterface, ANYKEEP_PLUGIN_INTERFACE_IID)
Q_DECLARE_INTERFACE(AnyKeep::RegularPluginInterface, "com.rion-soft.AnyKeep.RegularPluginInterface/2.0")
Q_DECLARE_INTERFACE(AnyKeep::PluginOptionsTooltipInterface, "com.rion-soft.AnyKeep.PluginOptionsTooltipInterface/2.0")

#endif // ANYKEEPPLUGININTERFACE_H
