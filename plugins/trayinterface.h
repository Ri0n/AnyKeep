#ifndef TRAYINTERFACE_H
#define TRAYINTERFACE_H

#include <QObject>

namespace AnyKeep {

class Main;
class TrayImpl;

class TrayInterface {
public:
    virtual TrayImpl *initTray(Main *anykeep) = 0;
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::TrayInterface, "com.rion-soft.AnyKeep.TrayInterface/1.1")

#endif // TRAYINTERFACE_H
