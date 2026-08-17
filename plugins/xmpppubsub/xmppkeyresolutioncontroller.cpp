#include "xmppkeyresolutioncontroller.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>

#include <utility>

namespace AnyKeep {
namespace {

    bool isTrusted(const XmppDeviceInfo &device)
    {
        return device.trustLevel == XmppTrustLevel::ManuallyTrusted
            || device.trustLevel == XmppTrustLevel::Authenticated;
    }

    QString deviceFingerprint(const QByteArray &keyId)
    {
        if (keyId.isEmpty())
            return XmppKeyResolutionController::tr("Fingerprint unavailable");

        const auto  hex = keyId.toHex();
        QStringList groups;
        groups.reserve((hex.size() + 7) / 8);
        for (qsizetype offset = 0; offset < hex.size(); offset += 8)
            groups.append(QString::fromLatin1(hex.mid(offset, 8)));
        return groups.join(QLatin1Char(' '));
    }

} // namespace

class XmppDeviceSelectionModel final : public QAbstractListModel {
public:
    enum Role {
        LabelRole = Qt::UserRole + 1,
        FingerprintRole,
        TrustTextRole,
        SelectedRole,
        SelectableRole,
        TrustedRole,
        DeviceIdRole,
    };

    explicit XmppDeviceSelectionModel(QList<XmppDeviceInfo> devices, QObject *parent = nullptr) :
        QAbstractListModel(parent), devices_(std::move(devices))
    {
        selected_.fill(false, devices_.size());
    }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : devices_.size(); }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= devices_.size())
            return {};
        const auto &device  = devices_.at(index.row());
        const bool  trusted = isTrusted(device);
        switch (role) {
        case LabelRole:
            return device.label.isEmpty() ? tr("Unnamed device") : device.label;
        case FingerprintRole:
            return deviceFingerprint(device.keyId);
        case TrustTextRole:
            return device.keyId.isEmpty() ? tr("Cannot be trusted yet")
                : trusted                 ? tr("Trusted")
                                          : tr("Needs confirmation");
        case SelectedRole:
            return trusted || selected_.at(index.row());
        case SelectableRole:
            return !trusted && !device.keyId.isEmpty();
        case TrustedRole:
            return trusted;
        case DeviceIdRole:
            return device.deviceId;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return { { LabelRole, "label" },       { FingerprintRole, "fingerprint" }, { TrustTextRole, "trustText" },
                 { SelectedRole, "selected" }, { SelectableRole, "selectable" },   { TrustedRole, "trusted" },
                 { DeviceIdRole, "deviceId" } };
    }

    void setSelected(int row, bool selected)
    {
        if (row < 0 || row >= devices_.size() || isTrusted(devices_.at(row)) || devices_.at(row).keyId.isEmpty()
            || selected_.at(row) == selected) {
            return;
        }
        selected_[row]        = selected;
        const auto modelIndex = index(row, 0);
        emit       dataChanged(modelIndex, modelIndex, { SelectedRole });
    }

    QList<QByteArray> selectedDeviceKeys() const
    {
        QList<QByteArray> result;
        for (int row = 0; row < devices_.size(); ++row) {
            const auto &device = devices_.at(row);
            if (!isTrusted(device) && selected_.at(row) && !device.keyId.isEmpty())
                result.append(device.keyId);
        }
        return result;
    }

    bool hasTrustedOrSelectedDevice() const
    {
        for (int row = 0; row < devices_.size(); ++row) {
            if (isTrusted(devices_.at(row)) || selected_.at(row))
                return true;
        }
        return false;
    }

    quint32 deviceId(int row) const { return row >= 0 && row < devices_.size() ? devices_.at(row).deviceId : 0; }

    void removeDevice(int row)
    {
        if (row < 0 || row >= devices_.size())
            return;
        beginRemoveRows({}, row, row);
        devices_.removeAt(row);
        selected_.removeAt(row);
        endRemoveRows();
    }

private:
    QList<XmppDeviceInfo> devices_;
    QList<bool>           selected_;
};

class XmppStorageKeyModel final : public QAbstractListModel {
public:
    enum Role {
        FingerprintRole = Qt::UserRole + 1,
        SourceRole,
        NoteCountRole,
        StatusRole,
        AvailableRole,
    };

    explicit XmppStorageKeyModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : candidates_.size(); }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= candidates_.size())
            return {};
        const auto &candidate = candidates_.at(index.row());
        switch (role) {
        case FingerprintRole:
            return QString::fromLatin1(candidate.keyId.left(8).toHex());
        case SourceRole:
            return candidate.resource.isEmpty() ? tr("Key not received") : candidate.resource;
        case NoteCountRole:
            return candidate.indexItemCount;
        case StatusRole:
            return candidate.key.isEmpty() ? tr("Unavailable")
                : candidate.local          ? tr("Current device")
                                           : tr("Available");
        case AvailableRole:
            return !candidate.key.isEmpty();
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return { { FingerprintRole, "fingerprint" },
                 { SourceRole, "source" },
                 { NoteCountRole, "noteCount" },
                 { StatusRole, "status" },
                 { AvailableRole, "available" } };
    }

    void setCandidates(QList<XmppStorageKeyCandidate> candidates)
    {
        beginResetModel();
        candidates_ = std::move(candidates);
        endResetModel();
    }

    const XmppStorageKeyCandidate *candidate(int row) const
    {
        return row >= 0 && row < candidates_.size() ? &candidates_.at(row) : nullptr;
    }

    const QList<XmppStorageKeyCandidate> &candidates() const { return candidates_; }

private:
    QList<XmppStorageKeyCandidate> candidates_;
};

XmppKeyResolutionController::XmppKeyResolutionController(bool localKeyMissing, const QList<XmppDeviceInfo> &devices,
                                                         const QString &deviceError, TrustDevices trustDevices,
                                                         RemoveDevice removeDevice, AuditKeys auditKeys,
                                                         RekeyStorage rekeyStorage, int operationTimeoutMs,
                                                         CreateStorageKey createStorageKey, QObject *parent) :
    QObject(parent), devicesModel_(new XmppDeviceSelectionModel(devices, this)),
    keysModel_(new XmppStorageKeyModel(this)), trustDevices_(std::move(trustDevices)),
    removeDevice_(std::move(removeDevice)), auditKeys_(std::move(auditKeys)), rekeyStorage_(std::move(rekeyStorage)),
    createStorageKey_(std::move(createStorageKey)), localKeyMissing_(localKeyMissing),
    operationTimeoutMs_(qMax(1, operationTimeoutMs))
{
    operationTimer_.setSingleShot(true);
    connect(&operationTimer_, &QTimer::timeout, this, [this]() {
        if (!busy_ || completed_)
            return;
        ++operationToken_;
        setBusy(false);
        setDeviceStatus(operationTimeoutMessage_);
    });
    setDeviceStatus(deviceError.isEmpty() ? tr("Found %1 OMEMO device(s) for this account.").arg(devices.size())
                                          : tr("Found %1 OMEMO device(s). %2").arg(devices.size()).arg(deviceError));
}

QAbstractItemModel *XmppKeyResolutionController::devicesModel() const { return devicesModel_; }
QAbstractItemModel *XmppKeyResolutionController::keysModel() const { return keysModel_; }

bool XmppKeyResolutionController::canGoBack() const
{
    return !busy_ && !completed_ && currentPage_ > ProblemPage && currentPage_ < ResultPage;
}

bool XmppKeyResolutionController::canGoNext() const
{
    if (busy_ || completed_)
        return false;
    if (currentPage_ == KeysPage)
        return !canonicalKey().isEmpty();
    return true;
}

bool XmppKeyResolutionController::canCreateNewKey() const { return canStartFresh() && audit_.totalIndexItems == 0; }

bool XmppKeyResolutionController::canStartFresh() const
{
    return !busy_ && !completed_ && currentPage_ == KeysPage && localKeyMissing_ && audit_.ok
        && keysModel_->rowCount() == 0 && bool(createStorageKey_);
}

QString XmppKeyResolutionController::nextText() const
{
    if (currentPage_ == ReviewPage)
        return tr("Repair");
    if (currentPage_ == ResultPage)
        return tr("Finish");
    return tr("Next");
}

void XmppKeyResolutionController::setDeviceSelected(int row, bool selected)
{
    devicesModel_->setSelected(row, selected);
    emit navigationChanged();
}

void XmppKeyResolutionController::removeDevice(int row)
{
    if (busy_ || completed_ || !removeDevice_)
        return;
    const auto deviceId = devicesModel_->deviceId(row);
    if (!deviceId)
        return;

    setDeviceStatus(tr("Removing OMEMO device %1…").arg(deviceId));
    setBusy(true);
    const auto token = beginDeviceOperation(
        tr("Timed out while removing OMEMO device %1. Check the connection and retry.").arg(deviceId));
    QPointer<XmppKeyResolutionController> guard(this);
    removeDevice_(deviceId, [guard, token, row, deviceId](XmppStatusResult result) {
        if (!guard || !guard->deviceOperationIsCurrent(token))
            return;
        guard->finishDeviceOperation(token);
        if (!result.ok && !result.notFound) {
            guard->setDeviceStatus(tr("Could not remove OMEMO device %1: %2").arg(deviceId).arg(result.error));
            return;
        }
        guard->devicesModel_->removeDevice(row);
        guard->setDeviceStatus(result.error.isEmpty() ? tr("OMEMO device %1 was removed.").arg(deviceId)
                                                      : result.error);
        emit guard->navigationChanged();
    });
}

void XmppKeyResolutionController::selectKey(int row)
{
    const auto *candidate = keysModel_->candidate(row);
    if (!candidate || candidate->key.isEmpty() || selectedKeyIndex_ == row)
        return;
    selectedKeyIndex_ = row;
    emit selectedKeyIndexChanged();
    updateSummary();
    emit navigationChanged();
}

void XmppKeyResolutionController::retryKeySearch()
{
    if (busy_ || completed_ || currentPage_ != KeysPage)
        return;
    devicesComplete_ = false;
    keysModel_->setCandidates({});
    audit_            = {};
    selectedKeyIndex_ = -1;
    emit selectedKeyIndexChanged();
    setKeyStatus({});
    setDeviceStatus(tr("Select a known device and request storage keys again."));
    setCurrentPage(DevicesPage);
}

void XmppKeyResolutionController::createNewKey()
{
    if (!canCreateNewKey())
        return;
    freshStart_ = false;

    setKeyStatus(tr("Creating a new storage key…"));
    setBusy(true);
    QPointer<XmppKeyResolutionController> guard(this);
    createStorageKey_([guard](XmppStatusResult result, QByteArray key, QByteArray keyId) {
        if (!guard || guard->completed_)
            return;
        guard->setBusy(false);
        if (!result.ok || key.isEmpty() || keyId.isEmpty()) {
            guard->setKeyStatus(result.error.isEmpty() ? guard->tr("Could not create a new storage key.")
                                                       : result.error);
            return;
        }
        guard->audit_.candidates.append({ guard->tr("This device (new key)"), key, keyId, 0, true });
        guard->keysModel_->setCandidates(guard->audit_.candidates);
        guard->selectedKeyIndex_ = 0;
        emit guard->selectedKeyIndexChanged();
        guard->setKeyStatus(
            guard->freshStart_
                ? guard->tr("A new storage key is ready. Existing encrypted notes will be left unchanged.")
                : guard->tr("No published notes were found. A new storage key is ready to use."));
        guard->updateSummary();
        emit guard->navigationChanged();
    });
}

void XmppKeyResolutionController::startFresh()
{
    if (!canStartFresh())
        return;
    freshStart_ = true;

    setKeyStatus(tr("Creating a new storage key…"));
    setBusy(true);
    QPointer<XmppKeyResolutionController> guard(this);
    createStorageKey_([guard](XmppStatusResult result, QByteArray key, QByteArray keyId) {
        if (!guard || guard->completed_)
            return;
        guard->setBusy(false);
        if (!result.ok || key.isEmpty() || keyId.isEmpty()) {
            guard->setKeyStatus(result.error.isEmpty() ? guard->tr("Could not create a new storage key.")
                                                       : result.error);
            return;
        }
        guard->audit_.candidates.append({ guard->tr("This device (new key)"), key, keyId, 0, true });
        guard->keysModel_->setCandidates(guard->audit_.candidates);
        guard->selectedKeyIndex_ = 0;
        emit guard->selectedKeyIndexChanged();
        guard->setKeyStatus(guard->tr("A new storage key is ready. Existing encrypted notes will be left unchanged."));
        guard->updateSummary();
        emit guard->navigationChanged();
    });
}

QByteArray XmppKeyResolutionController::canonicalKey() const
{
    const auto *candidate = keysModel_->candidate(selectedKeyIndex_);
    return candidate ? candidate->key : QByteArray {};
}

QList<QByteArray> XmppKeyResolutionController::availableKeys() const
{
    QList<QByteArray> result;
    for (const auto &candidate : keysModel_->candidates()) {
        if (!candidate.key.isEmpty() && !result.contains(candidate.key))
            result.append(candidate.key);
    }
    return result;
}

void XmppKeyResolutionController::next()
{
    if (!canGoNext())
        return;

    switch (currentPage_) {
    case ProblemPage:
        setCurrentPage(DevicesPage);
        return;
    case DevicesPage: {
        if (devicesComplete_) {
            setCurrentPage(KeysPage);
            return;
        }
        if (!devicesModel_->hasTrustedOrSelectedDevice()) {
            setDeviceStatus(
                tr("Select at least one AnyKeep device you recognize, then continue. If no expected device is shown, "
                   "start AnyKeep on the other machine and reopen this recovery flow."));
            return;
        }
        if (!trustDevices_ || !auditKeys_) {
            setDeviceStatus(tr("The XMPP recovery backend is unavailable."));
            return;
        }

        setDeviceStatus(tr("Establishing trusted OMEMO sessions…"));
        setBusy(true);
        const auto selected   = devicesModel_->selectedDeviceKeys();
        const auto trustToken = beginDeviceOperation(
            tr("Timed out while establishing OMEMO trust. Check the XMPP connection and retry."));
        QPointer<XmppKeyResolutionController> guard(this);
        trustDevices_(selected, [guard, trustToken](XmppStatusResult trusted) {
            if (!guard || !guard->deviceOperationIsCurrent(trustToken))
                return;
            guard->finishDeviceOperation(trustToken);
            if (!trusted.ok) {
                guard->setDeviceStatus(tr("Could not trust the selected device: %1").arg(trusted.error));
                return;
            }
            guard->setDeviceStatus(tr("Requesting storage keys from online AnyKeep resources…"));
            guard->setBusy(true);
            const auto auditToken = guard->beginDeviceOperation(
                tr("Timed out while requesting storage keys. XMPP does not provide a reliable mapping from an "
                   "unresponsive resource to an OMEMO device ID, so no fingerprint can be identified. Start "
                   "AnyKeep on another device and retry."));
            guard->auditKeys_([guard, auditToken](XmppKeyAuditResult audit) {
                if (!guard || !guard->deviceOperationIsCurrent(auditToken))
                    return;
                guard->finishDeviceOperation(auditToken);
                if (!audit.ok) {
                    guard->setDeviceStatus(tr("Could not collect storage keys: %1").arg(audit.error));
                    return;
                }
                guard->populateKeys(audit);
                guard->devicesComplete_ = true;
                guard->setCurrentPage(KeysPage);
            });
        });
        return;
    }
    case KeysPage:
        if (canonicalKey().isEmpty()) {
            setKeyStatus(tr("Select an available key before continuing."));
            return;
        }
        updateSummary();
        setCurrentPage(ReviewPage);
        return;
    case ReviewPage: {
        if (freshStart_) {
            rekeyResult_.ok       = true;
            rekeyResult_.total    = audit_.totalIndexItems;
            rekeyResult_.migrated = 0;
            resultText_ = tr("A new empty storage was created. %1 existing encrypted note(s) were left unchanged "
                             "in XMPP because their old key is unavailable.")
                              .arg(audit_.totalIndexItems);
            emit resultTextChanged();
            rekeyComplete_ = true;
            setCurrentPage(ResultPage);
            return;
        }
        if (rekeyComplete_) {
            setCurrentPage(ResultPage);
            return;
        }
        if (!rekeyStorage_) {
            rekeyResult_.error = tr("The XMPP recovery backend is unavailable.");
            resultText_        = rekeyResult_.error;
            emit resultTextChanged();
            rekeyComplete_ = true;
            setCurrentPage(ResultPage);
            return;
        }
        setBusy(true);
        const auto                            keys      = availableKeys();
        const auto                            canonical = canonicalKey();
        QPointer<XmppKeyResolutionController> guard(this);
        rekeyStorage_(keys, canonical, [guard](XmppRekeyResult result) {
            if (!guard || guard->completed_)
                return;
            guard->rekeyResult_ = std::move(result);
            if (guard->rekeyResult_.ok) {
                guard->resultText_ = tr("Recovery completed successfully. %1 of %2 note(s) now use the canonical "
                                        "key.\n\nThe local storage key will be updated when you finish this flow.")
                                         .arg(guard->rekeyResult_.migrated)
                                         .arg(guard->rekeyResult_.total);
            } else {
                guard->resultText_ = tr("Recovery is incomplete: %1\n\nMigrated %2 of %3 note(s). The local storage "
                                        "key has not been changed. You can safely run this recovery flow again.")
                                         .arg(guard->rekeyResult_.error)
                                         .arg(guard->rekeyResult_.migrated)
                                         .arg(guard->rekeyResult_.total);
            }
            emit guard->resultTextChanged();
            guard->rekeyComplete_ = true;
            guard->setBusy(false);
            guard->setCurrentPage(ResultPage);
        });
        return;
    }
    case ResultPage:
        finish(true);
        return;
    }
}

void XmppKeyResolutionController::back()
{
    if (!canGoBack())
        return;
    setCurrentPage(static_cast<Page>(int(currentPage_) - 1));
}

void XmppKeyResolutionController::cancel()
{
    if (canCancel())
        finish(false);
}

void XmppKeyResolutionController::abort() { finish(false); }

void XmppKeyResolutionController::setCurrentPage(Page page)
{
    if (currentPage_ == page || completed_)
        return;
    currentPage_ = page;
    if (currentPage_ == KeysPage || currentPage_ == ReviewPage)
        updateSummary();
    emit currentPageChanged();
    emit navigationChanged();
}

void XmppKeyResolutionController::setBusy(bool busy)
{
    if (busy_ == busy)
        return;
    busy_ = busy;
    emit busyChanged();
    emit navigationChanged();
}

quint64 XmppKeyResolutionController::beginDeviceOperation(const QString &timeoutMessage)
{
    operationTimeoutMessage_ = timeoutMessage;
    const auto token         = ++operationToken_;
    operationTimer_.start(operationTimeoutMs_);
    return token;
}

bool XmppKeyResolutionController::deviceOperationIsCurrent(quint64 token) const
{
    return !completed_ && token == operationToken_;
}

void XmppKeyResolutionController::finishDeviceOperation(quint64 token)
{
    if (token != operationToken_)
        return;
    operationTimer_.stop();
    setBusy(false);
}

void XmppKeyResolutionController::setDeviceStatus(const QString &status)
{
    if (deviceStatus_ == status)
        return;
    deviceStatus_ = status;
    emit deviceStatusChanged();
}

void XmppKeyResolutionController::setKeyStatus(const QString &status)
{
    if (keyStatus_ == status)
        return;
    keyStatus_ = status;
    emit keyStatusChanged();
}

void XmppKeyResolutionController::populateKeys(const XmppKeyAuditResult &audit)
{
    audit_ = audit;
    keysModel_->setCandidates(audit.candidates);

    int preferredRow   = -1;
    int preferredScore = -1;
    for (int row = 0; row < audit.candidates.size(); ++row) {
        const auto &candidate = audit.candidates.at(row);
        const int   score     = candidate.key.isEmpty() ? -1 : candidate.indexItemCount;
        if (score > preferredScore) {
            preferredScore = score;
            preferredRow   = row;
        }
    }
    if (selectedKeyIndex_ != preferredRow) {
        selectedKeyIndex_ = preferredRow;
        emit selectedKeyIndexChanged();
    }
    if (audit.candidates.isEmpty() && audit.totalIndexItems == 0) {
        setKeyStatus(tr("No storage key and no published notes were found."));
    } else if (audit.candidates.isEmpty()) {
        setKeyStatus(tr("No storage key was received from the responding AnyKeep devices."));
    } else {
        setKeyStatus(audit.error.isEmpty() ? tr("All responding AnyKeep devices were queried successfully.")
                                           : tr("Some devices could not provide a key:\n%1").arg(audit.error));
    }
    updateSummary();
    emit navigationChanged();
}

void XmppKeyResolutionController::updateSummary()
{
    const auto *canonical = keysModel_->candidate(selectedKeyIndex_);
    QString     nextSummary;
    if (!canonical || canonical->key.isEmpty()) {
        nextSummary = tr("No available canonical key is selected.");
    } else if (freshStart_) {
        nextSummary = tr("New key: %1\nEncrypted notes left unchanged: %2\n\n"
                         "The old key is unavailable. AnyKeep will start with an empty storage and will not "
                         "overwrite or delete the encrypted notes already in XMPP.")
                          .arg(QString::fromLatin1(canonical->keyId.left(8).toHex()))
                          .arg(audit_.totalIndexItems);
    } else {
        int inaccessible = 0;
        for (const auto &candidate : audit_.candidates) {
            if (candidate.key.isEmpty())
                inaccessible += candidate.indexItemCount;
        }
        nextSummary
            = tr("Canonical key: %1\nNotes found: %2\nNotes whose key is unavailable: %3\n\n"
                 "For each accessible note the client will publish encrypted content first and its index second. "
                 "Inaccessible notes are never overwritten or deleted. The operation can be safely resumed "
                 "after interruption.")
                  .arg(QString::fromLatin1(canonical->keyId.left(8).toHex()))
                  .arg(audit_.totalIndexItems)
                  .arg(inaccessible);
    }
    if (summary_ == nextSummary)
        return;
    summary_ = nextSummary;
    emit summaryChanged();
}

void XmppKeyResolutionController::finish(bool accepted)
{
    if (completed_)
        return;
    completed_ = true;
    operationTimer_.stop();
    ++operationToken_;
    busy_ = false;
    emit busyChanged();
    emit completedChanged();
    emit navigationChanged();
    emit finished(accepted);
}

} // namespace AnyKeep
