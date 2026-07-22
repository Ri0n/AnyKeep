#ifndef QTNOTE_SETTINGSPROVIDERINTERFACE_H
#define QTNOTE_SETTINGSPROVIDERINTERFACE_H

#include <QUrl>
#include <QtPlugin>

class QObject;

namespace QtNote {

class SettingsController;

class SettingsProviderInterface {
public:
    virtual ~SettingsProviderInterface()                                  = default;
    virtual QUrl                settingsComponent() const                 = 0;
    virtual SettingsController *createSettingsController(QObject *parent) = 0;
};

} // namespace QtNote

Q_DECLARE_INTERFACE(QtNote::SettingsProviderInterface, "com.rion-soft.QtNote.SettingsProviderInterface/1.0")

#endif // QTNOTE_SETTINGSPROVIDERINTERFACE_H
