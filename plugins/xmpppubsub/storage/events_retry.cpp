#include "xmppstorage.h"

#include "private.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "localdatakeystore.h"
#include "securekeystore.h"
#include "xmppbackend.h"
#include "xmppdialogpresenter.h"
#include "xmppkeyresolutioncontroller.h"
#include "xmppsettingscontroller.h"

#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace AnyKeep {

using namespace XmppStoragePrivate;

void XmppStorage::onRemoteNotePublished(const XmppRemoteNote &remote)
{
    if (errorState_) {
        return;
    }
    if (remote.id.isEmpty()) {
        return;
    }

    const auto previous = cache_.value(remote.id);
    QString    previousRevision;
    QString    previousParentRevision;
    if (!previous.isNull()) {
        previousRevision       = previous.backendValue(QStringLiteral("revision")).toString();
        previousParentRevision = previous.backendValue(QStringLiteral("parentRevision")).toString();
    }

    if (!previous.isNull() && previousRevision == remote.revision) {
        qInfo().noquote() << "Conflict trace: XMPP event duplicate note=" << remote.id
                          << "revision=" << remote.revision;
        return;
    }

    const bool siblingConflict = !previous.isNull() && !previousParentRevision.isEmpty()
        && previousParentRevision == remote.parentRevision && previousRevision != remote.revision;

    reconcileRemoteFolders({ remote });
    auto       incoming               = fromRemote(remote);
    const auto previousOrigin         = previous.backendValue(QStringLiteral("originId")).toString();
    const bool ownedDisplacedRevision = previousOrigin == config_.originId;
    qInfo().noquote() << "Conflict trace: XMPP event note=" << remote.id << "previous=" << previousRevision
                      << "previous-parent=" << previousParentRevision << "previous-origin=" << previousOrigin
                      << "previous-loaded=" << previous.isLoaded() << "incoming=" << remote.revision
                      << "incoming-parent=" << remote.parentRevision << "incoming-origin=" << remote.originId
                      << "sibling=" << siblingConflict << "owned-displaced=" << ownedDisplacedRevision;

    // XEP-0060 has no atomic compare-and-swap. Two writers can therefore both
    // pass the revision check and publish sibling revisions. Only the device
    // that authored the displaced local revision creates a conflict copy;
    // other devices merely converge on the latest server item.
    if (siblingConflict && previous.isLoaded() && ownedDisplacedRevision) {
        qInfo().noquote() << "Conflict trace: XMPP sibling resolver triggered note=" << remote.id
                          << "displaced=" << previousRevision << "winner=" << remote.revision;
        DraftManager::instance()->resolveConcurrentEdit(
            previous, incoming, tr("Parallel XMPP note revisions were detected after publication."));
    } else if (siblingConflict) {
        qInfo().noquote() << "Conflict trace: XMPP sibling resolver skipped note=" << remote.id << "reason="
                          << (!previous.isLoaded() ? QStringLiteral("previous-not-loaded")
                                                   : QStringLiteral("different-origin"));
    }
    cache_.insert(remote.id, incoming);
    cacheValid_ = true;
    persistCache();
    if (!incoming.isLoaded())
        startBodyPrefetch({ remote.id });

    if (previous.isNull()) {
        emit noteAdded(incoming);
    } else {
        emit noteModified(incoming);
    }

    if (siblingConflict) {
        reportError(tr("Parallel XMPP note revisions were detected. "
                       "The latest server item is displayed; an automatic merge is not available."),
                    true);
    }
}

void XmppStorage::onRemoteNoteRetracted(const QString &id)
{
    if (errorState_) {
        return;
    }
    const auto removed = cache_.take(id);
    persistCache();
    if (!removed.isNull()) {
        emit noteRemoved(removed);
    } else {
        emit invalidated();
    }
}

void XmppStorage::onRemoteNodeInvalidated()
{
    if (errorState_) {
        return;
    }
    cacheValid_ = false;
    emit invalidated();
}

void XmppStorage::onConnectionChanged(bool connected)
{
    if (errorState_) {
        return;
    }
    accessible_ = connected;
    cacheValid_ = false;
    emit invalidated();
    if (!connected) {
        scheduleRetry();
    } else if (retryTimer_ && retryTimer_->isActive()) {
        retryTimer_->stop();
        QTimer::singleShot(0, this, &XmppStorage::retryInitialization);
    }
    if (connected)
        scheduleFolderPathSynchronization();
}

void XmppStorage::reportError(const QString &error, bool invalidate)
{
    if (!error.isEmpty() && error != lastReportedError_) {
        lastReportedError_ = error;
        emit storageErorr(tr("XMPP private notes error: %1").arg(error));
    }
    if (invalidate) {
        cacheValid_ = false;
        emit invalidated();
    }
}

void XmppStorage::enterErrorState(const QString &error, bool invalidate)
{
    const bool keyMismatch = error.contains(QStringLiteral("storage key mismatch"), Qt::CaseInsensitive);
    if (errorState_ && error == errorStateMessage_) {
        if (keyMismatch)
            QTimer::singleShot(0, this, [this]() { resolveStorageKeys(config_.jid); });
        return;
    }

    qCritical().noquote() << "XMPP storage stopped after a non-retryable error:" << error;
    errorState_        = true;
    errorStateMessage_ = error;
    accessible_        = false;
    cacheValid_        = false;
    if (retryTimer_)
        retryTimer_->stop();

    if (!keyMismatch)
        backend_->shutdown();

    if (keyMismatch) {
        if (invalidate) {
            cacheValid_ = false;
            emit invalidated();
        }
        QTimer::singleShot(0, this, [this]() { resolveStorageKeys(config_.jid); });
    } else {
        reportError(error, invalidate);
    }
}

void XmppStorage::clearErrorState()
{
    errorState_ = false;
    errorStateMessage_.clear();
    lastReportedError_.clear();
}

void XmppStorage::handleTransientFailure(const QString &error, bool invalidate)
{
    accessible_ = false;
    cacheValid_ = false;
    reportError(error, invalidate);
    scheduleRetry();
}

void XmppStorage::scheduleRetry()
{
    if (shuttingDown_ || errorState_ || retryInProgress_ || !retryTimer_ || retryTimer_->isActive())
        return;

    QString validationError;
    if (!configIsValid(config_, &validationError))
        return;

    const int delay    = retryDelaySeconds_;
    retryDelaySeconds_ = qMin(retryDelaySeconds_ * 2, MaximumRetryDelaySeconds);
    qInfo() << "XMPP storage reconnect scheduled in" << delay << "seconds";
    retryTimer_->start(delay * 1000);
}

void XmppStorage::retryInitialization()
{
    if (shuttingDown_ || errorState_ || retryInProgress_)
        return;

    QString validationError;
    if (!configIsValid(config_, &validationError))
        return;

    retryInProgress_    = true;
    auto      *job      = initAsync(this);
    const auto handled  = std::make_shared<bool>(false);
    const auto finished = [this, job, handled]() {
        if (std::exchange(*handled, true))
            return;
        retryInProgress_ = false;
        if (job->state() == StorageJob::Succeeded) {
            resetRetryBackoff();
            emit invalidated();
        } else if (job->error().retryable) {
            scheduleRetry();
        }
        job->deleteLater();
    };
    connect(job, &StorageJob::finished, this, finished);
    if (job->isFinished())
        finished();
}

void XmppStorage::resetRetryBackoff()
{
    if (retryTimer_)
        retryTimer_->stop();
    retryDelaySeconds_ = MinimumRetryDelaySeconds;
}

void XmppStorage::applyConfig(const XmppConfig &config)
{
    abortKeyResolution();
    cancelRefreshAttempt();
    folderPathUpdateQueue_.clear();
    folderPathUpdateQueued_.clear();
    folderPathUpdateInFlight_.clear();
    folderPathUpdateRunning_   = false;
    folderPathUpdateScheduled_ = false;
    ++configEpoch_;
    shuttingDown_ = false;
    QSettings  settings;
    const bool endpointChanged = config_.jid != config.jid || config_.nodeName != config.nodeName;
    const auto instanceId      = endpointChanged || config.instanceId.isEmpty()
             ? QUuid::createUuid().toString(QUuid::WithoutBraces)
             : config.instanceId;
    settings.setValue(QStringLiteral("storage.xmpppubsub.instanceId"), instanceId);
    settings.setValue(QStringLiteral("storage.xmpppubsub.jid"), config.jid);
    const auto passwordError
        = SecureKeyStore::writePassword(AnyKeepKeychainService, passwordKeyName(config.jid), config.password);
    if (passwordError) {
        // Preserve compatibility on systems without a usable keychain. The
        // validation path will continue to report an empty password normally.
        settings.setValue(QStringLiteral("storage.xmpppubsub.password"), config.password);
        qWarning().noquote() << "Could not store XMPP password in the system keychain:" << passwordError.message;
    } else {
        settings.remove(QStringLiteral("storage.xmpppubsub.password"));
    }
    settings.setValue(QStringLiteral("storage.xmpppubsub.host"), config.host);
    settings.setValue(QStringLiteral("storage.xmpppubsub.port"), config.port);
    settings.setValue(QStringLiteral("storage.xmpppubsub.resource"), config.resource);
    settings.setValue(QStringLiteral("storage.xmpppubsub.node"), config.nodeName);
    settings.setValue(QStringLiteral("storage.xmpppubsub.timeoutMs"), config.timeoutMs);
    settings.setValue(QStringLiteral("storage.xmpppubsub.originId"), config.originId);

    clearErrorState();
    resetRetryBackoff();
    cache_.clear();
    cacheValid_ = false;
    accessible_ = false;
    config_     = readConfig();
    backend_->start();
    init();
    emit invalidated();
}

QUrl XmppStorage::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/XmppSettings.qml")); }

SettingsController *XmppStorage::createSettingsController(QObject *parent)
{
    const auto current = readConfig();
    auto      *widget  = new XmppSettingsController(this, current, parent);
    widget->setKeyState(SecureEnvelope::keyId(current.masterKey, KeyDerivationProfile::PrivateNotes));
    connect(this, &XmppStorage::encryptionKeyChanged, widget, &XmppSettingsController::setKeyState);
    connect(widget, &XmppSettingsController::applyConfigRequested, this, &XmppStorage::applyConfig);
    connect(widget, &XmppSettingsController::createKeyRequested, this, [this, widget](const QString &jid) {
        if (jid.isEmpty()) {
            widget->setKeyState({}, tr("Enter the XMPP JID first"));
            return;
        }
        auto existing = SecureKeyStore::read(storageKeyName(jid));
        if (existing) {
            widget->setKeyState(SecureEnvelope::keyId(existing.value, KeyDerivationProfile::PrivateNotes),
                                tr("A key already exists"));
            return;
        }
        const auto key   = SecureEnvelope::generateMasterKey();
        const auto error = SecureKeyStore::write(storageKeyName(jid), key);
        if (error) {
            widget->setKeyState({}, error.message);
            return;
        }
        widget->setKeyState(SecureEnvelope::keyId(key, KeyDerivationProfile::PrivateNotes));
        clearErrorState();
    });
    connect(widget, &XmppSettingsController::importKeyRequested, this,
            [this, widget](const QString &jid, const QString &encoded) {
                if (jid.isEmpty()) {
                    widget->setKeyState({}, tr("Enter the XMPP JID first"));
                    return;
                }
                auto imported = SecureEnvelope::decodeRecoveryKey(encoded, KeyDerivationProfile::PrivateNotes);
                if (!imported) {
                    widget->setKeyState({}, imported.error.message);
                    return;
                }
                auto existing = SecureKeyStore::read(storageKeyName(jid));
                if (existing && existing.value != imported.value) {
                    widget->setKeyState(SecureEnvelope::keyId(existing.value, KeyDerivationProfile::PrivateNotes),
                                        tr("A different key already exists; it was not replaced"));
                    return;
                }
                const auto error = SecureKeyStore::write(storageKeyName(jid), imported.value);
                if (error) {
                    widget->setKeyState({}, error.message);
                    return;
                }
                widget->setKeyState(SecureEnvelope::keyId(imported.value, KeyDerivationProfile::PrivateNotes));
                clearErrorState();
            });
    connect(widget, &XmppSettingsController::exportKeyRequested, this, [widget](const QString &jid) {
        auto key = SecureKeyStore::read(storageKeyName(jid));
        if (!key) {
            widget->setKeyState({}, key.error.message);
            return;
        }
        widget->setRecoveryKey(SecureEnvelope::encodeRecoveryKey(key.value, KeyDerivationProfile::PrivateNotes));
        widget->setKeyState(SecureEnvelope::keyId(key.value, KeyDerivationProfile::PrivateNotes));
    });
    connect(widget, &XmppSettingsController::omemoSyncRequested, this, [this, widget](const QString &jid) {
        if (jid != config_.jid) {
            widget->setKeyState({}, tr("Apply the account settings before synchronizing the storage key"));
            return;
        }
        resolveStorageKeys(jid, widget);
    });
    connect(widget, &XmppSettingsController::omemoDevicesRequested, this, [this, widget](const QString &jid) {
        if (jid.isEmpty()) {
            widget->setOmemoStatus(tr("Enter the XMPP JID before querying OMEMO devices"));
            return;
        }
        if (jid != config_.jid) {
            widget->setOmemoStatus(tr("Apply the account settings before querying OMEMO devices"));
            return;
        }
        const auto                       config = config_;
        const auto                       epoch  = configEpoch_;
        QPointer<XmppSettingsController> guard(widget);
        QMetaObject::invokeMethod(backend_, [this, guard, config, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_)
                return;
            backend_->setConfig(config);
            backend_->ownOmemoDevicesAsync([this, guard, epoch](auto devices, QString error) mutable {
                if (shuttingDown_ || epoch != configEpoch_)
                    return;
                const auto ownDevice = backend_->ownOmemoDevice();
                backend_->ownOmemoBundleValidAsync([this, guard, ownDevice, devices = std::move(devices),
                                                    error = std::move(error),
                                                    epoch](XmppStatusResult validity) mutable {
                    if (shuttingDown_ || epoch != configEpoch_)
                        return;
                    if (!validity.ok)
                        error = validity.error;
                    QMetaObject::invokeMethod(
                        this,
                        [guard, ownDevice, valid = validity.ok, devices = std::move(devices),
                         error = std::move(error)]() mutable {
                            if (guard)
                                guard->setOmemoDevices(ownDevice, valid, devices, error);
                        },
                        Qt::QueuedConnection);
                });
            });
        });
    });
    connect(widget, &XmppSettingsController::repairOmemoDeviceRequested, this, [this, widget](const QString &jid) {
        if (jid != config_.jid) {
            widget->setOmemoStatus(tr("Apply the account settings before repairing the OMEMO device"));
            return;
        }
        const auto                       config = config_;
        const auto                       epoch  = configEpoch_;
        QPointer<XmppSettingsController> guard(widget);
        QMetaObject::invokeMethod(backend_, [this, guard, config, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_)
                return;
            backend_->setConfig(config);
            backend_->repairOwnOmemoDeviceAsync([this, guard, epoch](XmppStatusResult result) {
                if (shuttingDown_ || epoch != configEpoch_)
                    return;
                QMetaObject::invokeMethod(
                    this,
                    [guard, result]() {
                        if (!guard)
                            return;
                        guard->setOmemoStatus(result.ok ? XmppSettingsController::tr("OMEMO device repaired")
                                                        : result.error);
                        if (result.ok)
                            emit guard->omemoDevicesRequested(guard->config().jid);
                    },
                    Qt::QueuedConnection);
            });
        });
    });
    connect(widget, &XmppSettingsController::trustOmemoDeviceRequested, this,
            [this, widget](const QString &jid, const QByteArray &keyId) {
                if (jid != config_.jid) {
                    widget->setOmemoStatus(tr("Apply the account settings before changing OMEMO trust"));
                    return;
                }
                const auto                       config = config_;
                const auto                       epoch  = configEpoch_;
                QPointer<XmppSettingsController> guard(widget);
                QMetaObject::invokeMethod(backend_, [this, guard, config, keyId, epoch]() {
                    if (shuttingDown_ || epoch != configEpoch_)
                        return;
                    backend_->setConfig(config);
                    backend_->trustOwnOmemoDeviceAsync(keyId, [this, guard, epoch](XmppStatusResult result) {
                        if (shuttingDown_ || epoch != configEpoch_)
                            return;
                        QMetaObject::invokeMethod(
                            this,
                            [guard, result]() {
                                if (guard)
                                    guard->setOmemoStatus(result.ok ? XmppSettingsController::tr("OMEMO device trusted")
                                                                    : result.error);
                            },
                            Qt::QueuedConnection);
                    });
                });
            });
    connect(widget, &XmppSettingsController::scanObsoleteItemsRequested, this, [this, widget](const QString &jid) {
        if (jid != config_.jid) {
            XmppCleanupResult result;
            result.error = tr("Apply the account settings before scanning PubSub data");
            widget->setCleanupScanResult(std::move(result));
            return;
        }
        const auto                       config = config_;
        const auto                       epoch  = configEpoch_;
        QPointer<XmppSettingsController> guard(widget);
        QMetaObject::invokeMethod(backend_, [this, guard, config, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_)
                return;
            backend_->setConfig(config);
            backend_->scanObsoleteItemsAsync([this, guard, epoch](XmppCleanupResult result) mutable {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, epoch, result = std::move(result)]() mutable {
                        if (!guard || shuttingDown_ || epoch != configEpoch_)
                            return;
                        guard->setCleanupScanResult(std::move(result));
                    },
                    Qt::QueuedConnection);
            });
        });
    });
    connect(widget, &XmppSettingsController::deleteObsoleteItemsRequested, this,
            [this, widget](const QString &jid, const QStringList &indexIds, const QStringList &contentIds) {
                if (jid != config_.jid) {
                    XmppCleanupResult result;
                    result.error = tr("Apply the account settings before deleting PubSub data");
                    widget->setCleanupDeleteResult(std::move(result));
                    return;
                }
                const auto                       config = config_;
                const auto                       epoch  = configEpoch_;
                QPointer<XmppSettingsController> guard(widget);
                QMetaObject::invokeMethod(backend_, [this, guard, config, indexIds, contentIds, epoch]() mutable {
                    if (shuttingDown_ || epoch != configEpoch_)
                        return;
                    backend_->setConfig(config);
                    backend_->deleteObsoleteItemsAsync(
                        std::move(indexIds), std::move(contentIds),
                        [this, guard, epoch](XmppCleanupResult result) mutable {
                            QMetaObject::invokeMethod(
                                this,
                                [this, guard, epoch, result = std::move(result)]() mutable {
                                    if (!guard || shuttingDown_ || epoch != configEpoch_)
                                        return;
                                    guard->setCleanupDeleteResult(std::move(result));
                                },
                                Qt::QueuedConnection);
                        });
                });
            });

    if (!current.jid.isEmpty()) {
        QTimer::singleShot(0, widget, [widget, jid = current.jid]() { emit widget->omemoDevicesRequested(jid); });
    }
    return widget;
}

QString XmppStorage::tooltip()
{
    if (errorState_) {
        return tr("XMPP private notes is stopped after an error:\n%1\n\nOpen storage settings and apply the "
                  "configuration to retry.")
            .arg(errorStateMessage_);
    }
    if (config_.jid.isEmpty()) {
        config_ = readConfig();
    }
    if (config_.jid.isEmpty()) {
        return tr("XMPP private notes is not configured.");
    }
    return tr("Account: %1\nPEP nodes: %2\nEncryption: end-to-end, key %3")
        .arg(config_.jid, config_.nodeName,
             QString::fromLatin1(
                 SecureEnvelope::keyId(config_.masterKey, KeyDerivationProfile::PrivateNotes).left(8).toHex()));
}

QString XmppStorage::storageId = QStringLiteral("xmpp-pubsub");

} // namespace AnyKeep
