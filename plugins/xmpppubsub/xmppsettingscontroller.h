#ifndef XMPPSETTINGSCONTROLLER_H
#define XMPPSETTINGSCONTROLLER_H

#include "settingscontroller.h"
#include "xmppdto.h"

#include <QByteArray>
#include <QPointer>
#include <QStringList>

namespace QtNote {

class XmppStorage;

class XmppSettingsController final : public SettingsController {
    Q_OBJECT
    Q_PROPERTY(QString keyState READ keyState NOTIFY keyStateChanged)
    Q_PROPERTY(QString recoveryKey READ recoveryKey WRITE setRecoveryKey NOTIFY recoveryKeyChanged)
    Q_PROPERTY(QString ownOmemoDevice READ ownOmemoDevice NOTIFY omemoDevicesChanged)
    Q_PROPERTY(QStringList omemoDevices READ omemoDevices NOTIFY omemoDevicesChanged)
    Q_PROPERTY(bool repairAvailable READ repairAvailable NOTIFY omemoDevicesChanged)

public:
    explicit XmppSettingsController(XmppStorage *storage, const XmppConfig &config, QObject *parent = nullptr);

    XmppConfig  config() const;
    QString     jid() const;
    QString     keyState() const { return keyState_; }
    QString     recoveryKey() const { return recoveryKey_; }
    QString     ownOmemoDevice() const { return ownOmemoDevice_; }
    QStringList omemoDevices() const { return omemoDeviceLabels_; }
    bool        repairAvailable() const { return repairAvailable_; }

    void setKeyState(const QByteArray &keyId, const QString &message = {});
    void setRecoveryKey(const QString &key);
    void setOmemoDevices(const XmppDeviceInfo &ownDevice, bool ownBundleValid, const QList<XmppDeviceInfo> &devices,
                         const QString &message = {});

    Q_INVOKABLE void requestCreateKey();
    Q_INVOKABLE void requestImportKey();
    Q_INVOKABLE void requestExportKey();
    Q_INVOKABLE void requestOmemoSync();
    Q_INVOKABLE void requestOmemoDevices();
    Q_INVOKABLE void requestRepairOmemoDevice();
    Q_INVOKABLE void requestTrustOmemoDevice(int index);

signals:
    void keyStateChanged();
    void recoveryKeyChanged();
    void omemoDevicesChanged();
    void applyConfigRequested(const QtNote::XmppConfig &config);
    void createKeyRequested(const QString &jid);
    void importKeyRequested(const QString &jid, const QString &recoveryKey);
    void exportKeyRequested(const QString &jid);
    void omemoSyncRequested(const QString &jid);
    void omemoDevicesRequested(const QString &jid);
    void trustOmemoDeviceRequested(const QString &jid, const QByteArray &keyId);
    void repairOmemoDeviceRequested(const QString &jid);

protected:
    bool applyValues(const QVariantMap &values, QString *error) override;

private:
    QPointer<XmppStorage> storage_;
    QString               originId_;
    QString               instanceId_;
    QString               keyState_;
    QString               recoveryKey_;
    QString               ownOmemoDevice_;
    QStringList           omemoDeviceLabels_;
    QList<QByteArray>     omemoDeviceKeys_;
    bool                  repairAvailable_ { false };
};

} // namespace QtNote

#endif // XMPPSETTINGSCONTROLLER_H
