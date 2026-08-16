#ifndef ACTIONNOTIFICATIONINTERFACE_H
#define ACTIONNOTIFICATIONINTERFACE_H

#include <QString>
#include <functional>

namespace AnyKeep {

class ActionNotificationInterface {
public:
    virtual ~ActionNotificationInterface()            = default;
    virtual void notify(const QString &title, const QString &message, const QString &actionText,
                        std::function<void()> action) = 0;
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::ActionNotificationInterface, "com.rion-soft.AnyKeep.ActionNotificationInterface/1.0")

#endif // ACTIONNOTIFICATIONINTERFACE_H
