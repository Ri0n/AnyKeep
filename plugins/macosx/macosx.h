#ifndef MACOSXPLUGIN_H
#define MACOSXPLUGIN_H

#include <QObject>

#include "anykeepplugininterface.h"
#include "deintegrationinterface.h"
#include "notificationinterface.h"
#include "trayinterface.h"

namespace AnyKeep {

class MacOSXTray;
class PluginHostInterface;

class MacOSXPlugin : public QObject, public PluginInterface, public TrayInterface, public NotificationInterface {
    Q_OBJECT
#include "macosx_plugin_metadata.inc"
    Q_INTERFACES(AnyKeep::PluginInterface AnyKeep::TrayInterface AnyKeep::NotificationInterface)
public:
    explicit MacOSXPlugin(QObject *parent = 0);
    void setHost(PluginHostInterface *host);

    TrayImpl *initTray(Main *anykeep);
    void      notifyError(const QString &msg);

private:
    MacOSXTray          *_tray;
    PluginHostInterface *host;
};

} // namespace AnyKeep

#endif // MACOSXPLUGIN_H
