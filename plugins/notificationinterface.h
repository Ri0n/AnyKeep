#ifndef NOTIFICATIONINTERFACE_H
#define NOTIFICATIONINTERFACE_H

#include <QObject>

namespace AnyKeep {

class Main;

class NotificationInterface {
public:
    virtual void notifyError(const QString &message) = 0;
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::NotificationInterface, "com.rion-soft.AnyKeep.NotificationInterface/1.0")

#endif // NOTIFICATIONINTERFACE_H
