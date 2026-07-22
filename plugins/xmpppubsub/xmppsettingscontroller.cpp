#include "xmppsettingscontroller.h"

#include "secureenvelope.h"
#include "xmppstorage.h"

namespace QtNote {

XmppSettingsController::XmppSettingsController(XmppStorage *storage, const XmppConfig &config, QObject *parent) :
    SettingsController(parent), storage_(storage), originId_(config.originId), instanceId_(config.instanceId)
{
    QList<Field> fields;

    Field jid;
    jid.key         = QStringLiteral("jid");
    jid.label       = tr("JID");
    jid.value       = config.jid;
    jid.placeholder = QStringLiteral("user@example.org");
    fields.append(jid);

    Field password;
    password.key   = QStringLiteral("password");
    password.label = tr("Password");
    password.type  = Password;
    password.value = config.password;
    fields.append(password);

    Field host;
    host.key         = QStringLiteral("host");
    host.label       = tr("Host override");
    host.value       = config.host;
    host.placeholder = tr("leave empty to use DNS SRV");
    fields.append(host);

    Field port;
    port.key     = QStringLiteral("port");
    port.label   = tr("Port");
    port.type    = Integer;
    port.value   = config.port;
    port.minimum = 0;
    port.maximum = 65535;
    fields.append(port);

    Field resource;
    resource.key         = QStringLiteral("resource");
    resource.label       = tr("Resource");
    resource.value       = config.resource;
    resource.placeholder = QStringLiteral("QtNote-device");
    fields.append(resource);

    Field node;
    node.key         = QStringLiteral("nodeName");
    node.label       = tr("PEP node");
    node.value       = config.nodeName;
    node.placeholder = QStringLiteral("urn:xmpp:qtnote:notes:0");
    fields.append(node);

    Field timeout;
    timeout.key     = QStringLiteral("timeoutSeconds");
    timeout.label   = tr("Operation timeout");
    timeout.type    = Integer;
    timeout.value   = qBound(2, config.timeoutMs / 1000, 120);
    timeout.minimum = 2;
    timeout.maximum = 120;
    fields.append(timeout);

    setFields(std::move(fields));
    setKeyState(SecureEnvelope::keyId(config.masterKey));
}

XmppConfig XmppSettingsController::config() const
{
    XmppConfig result;
    result.instanceId = instanceId_;
    result.originId   = originId_;
    result.jid        = value(QStringLiteral("jid")).toString().trimmed().section(QLatin1Char('/'), 0, 0);
    result.password   = value(QStringLiteral("password")).toString();
    result.host       = value(QStringLiteral("host")).toString().trimmed();
    result.port       = value(QStringLiteral("port")).toInt();
    result.resource   = value(QStringLiteral("resource")).toString().trimmed();
    result.nodeName   = value(QStringLiteral("nodeName")).toString().trimmed();
    result.timeoutMs  = value(QStringLiteral("timeoutSeconds")).toInt() * 1000;
    return result;
}

QString XmppSettingsController::jid() const { return config().jid; }

void XmppSettingsController::setKeyState(const QByteArray &keyId, const QString &message)
{
    const QString next = !message.isEmpty() ? message
        : keyId.isEmpty()                   ? tr("Not configured")
                                            : QString::fromLatin1(keyId.left(8).toHex());
    if (keyState_ == next)
        return;
    keyState_ = next;
    emit keyStateChanged();
}

void XmppSettingsController::setRecoveryKey(const QString &key)
{
    if (recoveryKey_ == key)
        return;
    recoveryKey_ = key;
    emit recoveryKeyChanged();
}

void XmppSettingsController::setOmemoDevices(const XmppDeviceInfo &ownDevice, bool ownBundleValid,
                                             const QList<XmppDeviceInfo> &devices, const QString &message)
{
    if (ownDevice.deviceId) {
        const auto fingerprint
            = ownDevice.keyId.isEmpty() ? tr("fingerprint unavailable") : QString::fromLatin1(ownDevice.keyId.toHex());
        const auto label = ownDevice.label.isEmpty() ? tr("unnamed") : ownDevice.label;
        ownOmemoDevice_
            = tr("%1 (OMEMO label) — ID %2\nIdentity key: %3").arg(label).arg(ownDevice.deviceId).arg(fingerprint);
    } else {
        ownOmemoDevice_ = tr("Not initialized");
    }
    repairAvailable_ = ownDevice.deviceId && !ownBundleValid;
    omemoDeviceLabels_.clear();
    omemoDeviceKeys_.clear();
    for (const auto &device : devices) {
        if (device.keyId.isEmpty())
            continue;
        const auto label = device.label.isEmpty() ? tr("Unnamed device") : device.label;
        omemoDeviceLabels_.append(QStringLiteral("OMEMO %1 — %2 — %3 — trust %4")
                                      .arg(device.deviceId)
                                      .arg(label, QString::fromLatin1(device.keyId.left(8).toHex()))
                                      .arg(device.trustLevel));
        omemoDeviceKeys_.append(device.keyId);
    }
    if (!message.isEmpty())
        setKeyState({}, message);
    emit omemoDevicesChanged();
}

void XmppSettingsController::requestCreateKey() { emit createKeyRequested(jid()); }
void XmppSettingsController::requestImportKey() { emit importKeyRequested(jid(), recoveryKey_); }
void XmppSettingsController::requestExportKey() { emit exportKeyRequested(jid()); }
void XmppSettingsController::requestOmemoSync() { emit omemoSyncRequested(jid()); }
void XmppSettingsController::requestOmemoDevices() { emit omemoDevicesRequested(jid()); }
void XmppSettingsController::requestRepairOmemoDevice() { emit repairOmemoDeviceRequested(jid()); }

void XmppSettingsController::requestTrustOmemoDevice(int index)
{
    if (index < 0 || index >= omemoDeviceKeys_.size())
        return;
    emit trustOmemoDeviceRequested(jid(), omemoDeviceKeys_.at(index));
}

bool XmppSettingsController::applyValues(const QVariantMap &, QString *error)
{
    const auto next = config();
    if (!storage_ || !storage_->configIsValid(next, error))
        return false;
    emit applyConfigRequested(next);
    return true;
}

} // namespace QtNote
