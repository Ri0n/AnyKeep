#ifndef ANYKEEP_SETTINGSPROVIDERINTERFACE_H
#define ANYKEEP_SETTINGSPROVIDERINTERFACE_H

#include <QUrl>
#include <QtPlugin>

class QObject;

namespace AnyKeep {

class SettingsController;

class SettingsProviderInterface {
public:
    virtual ~SettingsProviderInterface()                                  = default;
    virtual QUrl                settingsComponent() const                 = 0;
    virtual SettingsController *createSettingsController(QObject *parent) = 0;
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::SettingsProviderInterface, "com.rion-soft.AnyKeep.SettingsProviderInterface/1.0")

#endif // ANYKEEP_SETTINGSPROVIDERINTERFACE_H
