#include "xmppkeyresolutioncontroller.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>
#include <QTime>

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
                                                         CreateStorageKey createStorageKey, QObject *parent,
                                                         DiscoverResources discoverResources,
                                                         int presencePollIntervalMs) :
    QObject(parent), devicesModel_(new XmppDeviceSelectionModel(devices, this)),
    keysModel_(new XmppStorageKeyModel(this)), trustDevices_(std::move(trustDevices)),
    removeDevice_(std::move(removeDevice)), auditKeys_(std::move(auditKeys)),
    discoverResources_(std::move(discoverResources)), rekeyStorage_(std::move(rekeyStorage)),
    createStorageKey_(std::move(createStorageKey)), localKeyMissing_(localKeyMissing),
    operationTimeoutMs_(qMax(1, operationTimeoutMs)),
    presencePollIntervalMs_(qMax(1, presencePollIntervalMs))
{
    presenceTimer_.setInterval(presencePollIntervalMs_);
    connect(&presenceTimer_, &QTimer::timeout, this, &XmppKeyResolutionController::refreshOnlineResources);
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
    if (waitingForDevices_ && currentPage_ == DevicesPage)
        return false;
    if (currentPage_ == KeysPage)
        return !canonicalKey().isEmpty();
    return true;
}

bool XmppKeyResolutionController::canCreateNewKey() const
{
    return currentPage_ == KeysPage && canStartFresh() && audit_.totalIndexItems == 0;
}

bool XmppKeyResolutionController::canStartFresh() const
{
    if (busy_ || completed_ || !localKeyMissing_ || !createStorageKey_)
        return false;
    // First-install onboarding is explicitly user initiated and can create
    // a local key without OMEMO trust/key-sync. Existing remote notes are
    // never rewritten by this route, and their count remains unknown.
    if (currentPage_ == ProblemPage)
        return true;
    if (currentPage_ == DevicesPage && waitingForDevices_)
        return true;
    return currentPage_ == KeysPage && audit_.ok && keysModel_->rowCount() == 0;
}

QString XmppKeyResolutionController::nextText() const
{
    if (currentPage_ == ReviewPage)
        return freshStart_ ? tr("Continue") : tr("Sync notes");
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

void XmppKeyResolutionController::checkAgain()
{
    if (completed_ || busy_ || !waitingForDevices_ || currentPage_ != DevicesPage)
        return;
    requestKeyAudit();
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
    const bool fromWelcome = currentPage_ == ProblemPage;
    setWaitingForDevices(false);
    freshStartFromWelcome_ = fromWelcome;
    freshStart_            = true;

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
        if (guard->freshStartFromWelcome_)
            guard->setCurrentPage(ReviewPage);
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
        // Abandon any locally generated onboarding key when the user chooses
        // to recover existing notes instead. It has not been installed yet.
        if (freshStartFromWelcome_) {
            freshStartFromWelcome_ = false;
            freshStart_            = false;
            keysModel_->setCandidates({});
            audit_            = {};
            selectedKeyIndex_ = -1;
            emit selectedKeyIndexChanged();
            setKeyStatus({});
        }
        setCurrentPage(DevicesPage);
        return;
    case DevicesPage: {
        if (devicesComplete_) {
            setCurrentPage(KeysPage);
            return;
        }
        if (waitingForDevices_) {
            checkAgain();
            return;
        }
        if (!devicesModel_->hasTrustedOrSelectedDevice()) {
            if (onlineResources_.isEmpty() && localKeyMissing_) {
                // The selected peer might be offline, or its OMEMO device
                // listing may not yet have reached this account.
                setWaitingForDevices(true);
                setDeviceStatus(tr("No other AnyKeep XMPP devices are online. Waiting for one to connect. "
                                   "Open AnyKeep on the device that has your notes."));
            } else {
                setDeviceStatus(tr("Select an AnyKeep device whose fingerprint you recognize before requesting its "
                                   "storage key."));
            }
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
            guard->requestKeyAudit();
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
            resultText_           = freshStartFromWelcome_
                ? tr("A new local storage key is ready. Existing XMPP notes, if any, remain encrypted with "
                     "their original key and have not been changed. Save your new recovery key from settings.")
                : tr("A new empty storage was created. %1 existing encrypted note(s) were left unchanged "
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
        recordSyncProgress({ tr("Preparing note synchronization…") });
        const auto                            keys      = availableKeys();
        const auto                            canonical = canonicalKey();
        QPointer<XmppKeyResolutionController> guard(this);
        rekeyStorage_(keys, canonical, [guard](XmppRekeyResult result) {
            if (!guard || guard->completed_)
                return;
            guard->rekeyResult_ = std::move(result);
            if (guard->rekeyResult_.ok) {
                guard->resultText_ = tr("Synchronization completed. %1 of %2 note(s) use the selected key.\n"
                                        "Verified without changes: %3\nRe-encrypted: %4\n\n"
                                        "The local storage key will be updated when you finish this flow.")
                                         .arg(guard->rekeyResult_.migrated + guard->rekeyResult_.unchanged)
                                         .arg(guard->rekeyResult_.total)
                                         .arg(guard->rekeyResult_.unchanged)
                                         .arg(guard->rekeyResult_.migrated);
            } else {
                guard->resultText_ = tr("Recovery is incomplete: %1\n\nVerified without changes: %2\n"
                                        "Re-encrypted: %3\nNotes found: %4\n\nThe local storage key has not been "
                                        "changed. You can safely run this recovery flow again.")
                                         .arg(guard->rekeyResult_.error)
                                         .arg(guard->rekeyResult_.unchanged)
                                         .arg(guard->rekeyResult_.migrated)
                                         .arg(guard->rekeyResult_.total);
            }
            emit guard->resultTextChanged();
            guard->rekeyComplete_ = true;
            guard->setBusy(false);
            const auto &finished   = guard->rekeyResult_;
            const bool  totalKnown = finished.ok || finished.total > 0;
            guard->recordSyncProgress(
                { finished.ok ? guard->tr("Note synchronization completed")
                              : guard->tr("Note synchronization stopped: %1").arg(finished.error),
                  totalKnown ? finished.migrated + finished.unchanged + int(finished.inaccessibleNoteIds.size()) : -1,
                  totalKnown ? finished.total : -1 });
            guard->setCurrentPage(ResultPage);
        });
        return;
    }
    case ResultPage:
        finish(rekeyResult_.ok);
        return;
    }
}

void XmppKeyResolutionController::setWaitingForDevices(bool waiting)
{
    if (waitingForDevices_ == waiting)
        return;
    waitingForDevices_ = waiting;
    waitingPollCount_  = 0;
    emit waitingForDevicesChanged();
    emit navigationChanged();
}

void XmppKeyResolutionController::requestKeyAudit()
{
    if (completed_ || busy_ || currentPage_ != DevicesPage || !auditKeys_)
        return;
    setDeviceStatus(tr("Checking online resources for an available storage key…"));
    setBusy(true);
    const auto token = beginDeviceOperation(
        tr("The key request timed out. Waiting for an available device; check AnyKeep on the other device."));
    QPointer<XmppKeyResolutionController> guard(this);
    auditKeys_([guard, token](XmppKeyAuditResult audit) {
        if (!guard || !guard->deviceOperationIsCurrent(token))
            return;
        guard->finishDeviceOperation(token);
        if (!audit.ok) {
            guard->setDeviceStatus(tr("The last key request failed: %1").arg(audit.error));
            if (guard->waitingForDevices_) {
                // Retry on the next presence check, but do not mark the
                // authenticated XMPP connection as a failed login.
                guard->waitingPollCount_ = 3;
            }
            return;
        }

        // The audit returns the discovered online resources independently
        // from OMEMO device ID/label mapping.
        if (!audit.onlineResources.isEmpty() && audit.onlineResources != guard->onlineResources_) {
            guard->onlineResources_ = audit.onlineResources;
            guard->presenceKnown_ = true;
            guard->presenceStatus_ = tr("Online AnyKeep XMPP resources:");
            emit guard->onlineResourcesChanged();
        }

        const bool hasUsableKey = std::any_of(audit.candidates.cbegin(), audit.candidates.cend(),
                                             [](const auto &candidate) { return !candidate.key.isEmpty(); });
        if (!hasUsableKey) {
            guard->audit_ = std::move(audit);
            guard->setWaitingForDevices(true);
            guard->setDeviceStatus(guard->onlineResources_.isEmpty()
                                       ? tr("No other AnyKeep XMPP device is online. Waiting; open AnyKeep on "
                                            "another device to automatically retry key synchronization.")
                                       : tr("Other XMPP resources are online, but none has provided a trusted "
                                            "storage key yet. Waiting for approval or another device."));
            return;
        }
        guard->setWaitingForDevices(false);
        guard->populateKeys(audit);
        guard->devicesComplete_ = true;
        guard->setCurrentPage(KeysPage);
    });
}

void XmppKeyResolutionController::refreshOnlineResources()
{
    if (completed_ || currentPage_ != DevicesPage || presenceProbeInFlight_)
        return;
    if (!discoverResources_) {
        if (waitingForDevices_ && !busy_)
            requestKeyAudit();
        return;
    }

    presenceProbeInFlight_ = true;
    const auto generation = ++presenceGeneration_;
    QPointer<XmppKeyResolutionController> guard(this);
    // The backend normally completes each disco request with its own timeout.
    // Protect the wizard from a callback that never returns after a disconnect.
    QTimer::singleShot(operationTimeoutMs_, this, [guard, generation]() {
        if (!guard || generation != guard->presenceGeneration_ || !guard->presenceProbeInFlight_)
            return;
        guard->presenceProbeInFlight_ = false;
        guard->presenceStatus_ = tr("Could not check XMPP presence; retrying…");
        emit guard->onlineResourcesChanged();
    });
    discoverResources_([guard, generation](QStringList resources, QString error) {
        if (!guard || guard->completed_ || generation != guard->presenceGeneration_
            || guard->currentPage_ != DevicesPage)
            return;
        guard->presenceProbeInFlight_ = false;
        resources.removeDuplicates();
        resources.sort();
        const bool changed = resources != guard->onlineResources_;
        guard->onlineResources_ = std::move(resources);
        guard->presenceKnown_   = error.isEmpty();
        guard->presenceStatus_ = error.isEmpty()
            ? (guard->onlineResources_.isEmpty()
                   ? guard->tr("No other AnyKeep XMPP resources are online.")
                   : guard->tr("Online AnyKeep XMPP resources:"))
            : guard->tr("Could not determine all online resources: %1").arg(error);
        emit guard->onlineResourcesChanged();
        if (!guard->waitingForDevices_ || guard->busy_ || !guard->presenceKnown_)
            return;
        // A newly online resource triggers an immediate retry. While it stays
        // online but awaits trust approval, retry at most once per minute.
        if (!guard->onlineResources_.isEmpty()
            && (changed || ++guard->waitingPollCount_ >= 4)) {
            guard->waitingPollCount_ = 0;
            guard->requestKeyAudit();
        }
    });
}

void XmppKeyResolutionController::back()
{
    if (!canGoBack())
        return;
    if (currentPage_ == ReviewPage && freshStartFromWelcome_) {
        // The key is not installed until Finish, so returning to the welcome
        // screen is safe. Never reuse this transient new key for recovery.
        freshStartFromWelcome_ = false;
        freshStart_            = false;
        keysModel_->setCandidates({});
        audit_            = {};
        selectedKeyIndex_ = -1;
        emit selectedKeyIndexChanged();
        setKeyStatus({});
        setCurrentPage(ProblemPage);
        return;
    }
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
    ++progressGeneration_;
    if (busy) {
        syncCompleted_ = syncTotal_ = -1;
        emit syncProgressChanged();
    }
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
    if (!status.isEmpty())
        recordSyncProgress({ status });
    emit deviceStatusChanged();
}

void XmppKeyResolutionController::setKeyStatus(const QString &status)
{
    if (keyStatus_ == status)
        return;
    keyStatus_ = status;
    if (!status.isEmpty())
        recordSyncProgress({ status });
    emit keyStatusChanged();
}

XmppProgressCallback XmppKeyResolutionController::progressCallback()
{
    QPointer<XmppKeyResolutionController> guard(this);
    const auto                            generation = progressGeneration_;
    return [guard, generation](XmppSyncProgress progress) {
        if (!guard || guard->completed_ || !guard->busy_ || guard->progressGeneration_ != generation)
            return;
        guard->recordSyncProgress(progress);
    };
}

void XmppKeyResolutionController::recordSyncProgress(const XmppSyncProgress &progress)
{
    if (progress.message.isEmpty())
        return;
    syncSummary_   = progress.message;
    syncTotal_     = progress.total < 0 ? -1 : progress.total;
    syncCompleted_ = syncTotal_ < 0 ? -1 : qBound(0, progress.completed, syncTotal_);
    syncEvents_.append(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ")
                       + progress.message);
    constexpr int MaximumSyncEvents = 40;
    while (syncEvents_.size() > MaximumSyncEvents)
        syncEvents_.removeFirst();
    emit syncProgressChanged();
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
    } else if (freshStartFromWelcome_) {
        nextSummary = tr("New local key: %1\n\n"
                         "AnyKeep will start with a new empty storage without searching other devices. "
                         "Existing XMPP notes, if any, remain encrypted with their original key and will NOT be "
                         "deleted or overwritten. The new key cannot read them.\n\n"
                         "After finishing, save your new recovery key in XMPP settings.")
                          .arg(QString::fromLatin1(canonical->keyId.left(8).toHex()));
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
                 "AnyKeep will download and verify accessible notes. Records already encrypted with the selected "
                 "key will remain unchanged. Only records using another key will be re-encrypted and published, "
                 "content before index when both need updating. "
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
    if (accepted && installKey_ && !keyInstalled_) {
        setBusy(true);
        recordSyncProgress({ tr("Saving the selected storage key…"), syncCompleted_, syncTotal_ });
        QPointer<XmppKeyResolutionController> guard(this);
        installKey_(canonicalKey(), freshStart_, [guard](XmppStatusResult status) {
            if (!guard || guard->completed_)
                return;
            guard->setBusy(false);
            if (!status.ok) {
                guard->resultText_ = guard
                                         ->tr("Could not install the selected local storage key: %1\n\n"
                                              "Retry Finish to save the selected key.")
                                         .arg(status.error);
                emit guard->resultTextChanged();
                guard->recordSyncProgress(
                    { guard->tr("Could not save the selected storage key"), guard->syncCompleted_, guard->syncTotal_ });
                return;
            }
            guard->keyInstalled_ = true;
            guard->finish(true);
        });
        return;
    }
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
