#ifndef QTNOTEPLUGININTERFACE_H
#define QTNOTEPLUGININTERFACE_H

#include <QString>

#include "pluginhostinterface.h"

namespace QtNote {

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

} // namespace QtNote

#define QTNOTE_PLUGIN_INTERFACE_IID "com.rion-soft.QtNote.PluginInterface/4.0"
Q_DECLARE_INTERFACE(QtNote::PluginInterface, QTNOTE_PLUGIN_INTERFACE_IID)
Q_DECLARE_INTERFACE(QtNote::RegularPluginInterface, "com.rion-soft.QtNote.RegularPluginInterface/2.0")
Q_DECLARE_INTERFACE(QtNote::PluginOptionsTooltipInterface, "com.rion-soft.QtNote.PluginOptionsTooltipInterface/2.0")

#endif // QTNOTEPLUGININTERFACE_H
