#include "xmppstorage.h"

#include "private.h"

#include "draftmanager.h"
#include "fileremotecachestore.h"
#include "foldercatalogmanager.h"
#include "iconutils.h"
#include "localdatakeystore.h"
#include "notedata.h"
#include "remotecachestore.h"
#include "securekeystore.h"
#include "utils.h"
#include "xmppbackend.h"
#include "xmppdialogpresenter.h"
#include "xmppkeyresolutioncontroller.h"
#include "xmppsettingscontroller.h"
#include "xmppworker.h"

#include <QCryptographicHash>
#include <QMetaObject>
#include <QNetworkInformation>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <utility>

namespace AnyKeep {

namespace {
    QIcon xmppStorageIcon()
    {
        // Prefer the symbolic name so the icon follows light/dark palettes.
        // Resolve it while the storage is constructed on the GUI thread; the
        // QML image provider can then render the cached QIcon reliably.
        auto icon = QIcon::fromTheme(QStringLiteral("im-jabber-symbolic"));
        if (icon.isNull())
            icon = QIcon::fromTheme(QStringLiteral("im-jabber"));
        if (icon.isNull())
            icon = QIcon(QStringLiteral(":/icons/xmpp-logo"));
        return icon;
    }

} // namespace

using namespace XmppStoragePrivate;

XmppStorage::XmppStorage(QObject *parent, XmppBackend *backend, FolderCatalogManager *folderCatalogManager) :
    NoteStorage(parent), icon_(xmppStorageIcon()),
    folderCatalogManager_(folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance())
{
    dialogPresenter_ = new XmppDialogPresenter(this);
    backend_         = backend ? backend : new XmppWorker;
    backend_->setParent(this);
    connect(backend_, &XmppBackend::remoteNotePublished, this, &XmppStorage::onRemoteNotePublished);
    connect(backend_, &XmppBackend::remoteNoteRetracted, this, &XmppStorage::onRemoteNoteRetracted);
    connect(backend_, &XmppBackend::remoteNodeInvalidated, this, &XmppStorage::onRemoteNodeInvalidated);
    connect(backend_, &XmppBackend::connectionChanged, this, &XmppStorage::onConnectionChanged);
    connect(backend_, &XmppBackend::backendError, this, [this](const QString &error) {
        if (error.contains(QStringLiteral("storage key mismatch"), Qt::CaseInsensitive))
            enterErrorState(error, true);
        else
            reportError(error);
    });
    connect(backend_, &XmppBackend::keySyncTrustRequested, this,
            [this](const QString &requestId, const QByteArray &keyId) {
                dialogPresenter_->presentTrustRequest(
                    requestId, keyId,
                    [this, requestId]() {
                        QMetaObject::invokeMethod(backend_,
                                                  [this, requestId]() { backend_->approveKeySyncRequest(requestId); });
                    },
                    [this, requestId]() {
                        QMetaObject::invokeMethod(backend_,
                                                  [this, requestId]() { backend_->rejectKeySyncRequest(requestId); });
                    });
            });

    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &XmppStorage::retryInitialization);

    if (folderCatalogManager_) {
        connect(folderCatalogManager_, &FolderCatalogManager::availabilityChanged, this, [this](bool available) {
            if (!available)
                return;
            reconcileCachedFolders();
            scheduleFolderPathSynchronization();
            emit invalidated();
        });
        connect(folderCatalogManager_, &FolderCatalogManager::catalogChanged, this,
                &XmppStorage::scheduleFolderPathSynchronization);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    QNetworkInformation::loadDefaultBackend();
    if (auto *network = QNetworkInformation::instance()) {
        connect(network, &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability reachability) {
                    if (reachability == QNetworkInformation::Reachability::Disconnected
                        || reachability == QNetworkInformation::Reachability::Unknown || errorState_) {
                        return;
                    }
                    if (retryTimer_->isActive())
                        retryTimer_->stop();
                    retryDelaySeconds_ = MinimumRetryDelaySeconds;
                    QTimer::singleShot(0, this, &XmppStorage::retryInitialization);
                });
    }
#endif

    // Make the last confirmed snapshot available before the network probe.
    config_ = readConfig();
    openPersistentCache(config_);
}

void XmppStorage::installReceivedStorageKey(const QString &jid, const QByteArray &key)
{
    if (auto error = SecureKeyStore::write(storageKeyName(jid), key)) {
        emit encryptionKeyChanged({}, error.message);
        reportError(error.message);
        return;
    }
    // The storage key is part of the backend configuration. Invalidate every
    // operation started with the previous (keyless) snapshot before reconnecting.
    cancelRefreshAttempt();
    ++configEpoch_;
    config_ = readConfig();
    clearErrorState();
    const auto keyId = SecureEnvelope::keyId(key, KeyDerivationProfile::PrivateNotes);
    qInfo().noquote() << "XMPP storage key installed from a trusted device: key="
                      << QString::fromLatin1(keyId.left(8).toHex());
    emit encryptionKeyChanged(keyId, tr("Storage key received from a trusted device"));

    backend_->start();

    // Reinitialize with the recovered key, then let the normal storage model
    // invalidation path perform exactly one shared index refresh. Previously an
    // explicit refresh here raced the refresh triggered by connectionChanged(),
    // producing duplicate PubSub IQs and leaving one cancelled UI job waiting
    // if the backend was reset during the race.
    auto      *initJob       = initAsync(this);
    const auto finishInitJob = [this, initJob]() {
        if (initJob->state() != StorageJob::Succeeded) {
            qWarning().noquote() << "XMPP storage key was installed, but storage initialization failed:"
                                 << initJob->error().message;
            initJob->deleteLater();
            emit invalidated();
            return;
        }
        initJob->deleteLater();
        qInfo() << "XMPP recovery initialization completed; refreshing the note model";
        emit invalidated();
    };
    connect(initJob, &StorageJob::finished, this, finishInitJob);
    if (initJob->isFinished())
        finishInitJob();
}

void XmppStorage::resolveStorageKeys(const QString &jid, XmppSettingsController *settings)
{
    if (jid.isEmpty() || keyResolutionInProgress_)
        return;
    keyResolutionInProgress_               = true;
    auto config                            = readConfig();
    config.jid                             = jid;
    const auto                       epoch = configEpoch_;
    QPointer<XmppSettingsController> settingsGuard(settings);
    QMetaObject::invokeMethod(backend_, [this, config, jid, settingsGuard, epoch]() {
        if (shuttingDown_ || epoch != configEpoch_) {
            keyResolutionInProgress_ = false;
            return;
        }
        backend_->setConfig(config);
        backend_->ownOmemoDevicesAsync([this, jid, settingsGuard, epoch](auto devices, QString deviceError) mutable {
            QMetaObject::invokeMethod(
                this,
                [this, devices = std::move(devices), deviceError = std::move(deviceError), jid, settingsGuard,
                 epoch]() mutable {
                    if (shuttingDown_ || epoch != configEpoch_) {
                        keyResolutionInProgress_ = false;
                        return;
                    }

                    const bool localKeyMissing = readConfig().masterKey.size() != SecureEnvelope::MasterKeySize;
                    auto      *controller      = new XmppKeyResolutionController(
                        localKeyMissing, devices, deviceError,
                        [this, epoch](const QList<QByteArray>                      &keyIds,
                                      XmppKeyResolutionController::StatusCompletion completion) {
                            QMetaObject::invokeMethod(
                                backend_, [this, epoch, keyIds, completion = std::move(completion)]() mutable {
                                    if (shuttingDown_ || epoch != configEpoch_)
                                        return;
                                    backend_->trustOwnOmemoDevicesAsync(
                                        keyIds,
                                        [this, epoch,
                                         completion = std::move(completion)](XmppStatusResult result) mutable {
                                            QMetaObject::invokeMethod(
                                                this,
                                                [this, epoch, completion = std::move(completion),
                                                 result = std::move(result)]() mutable {
                                                    if (shuttingDown_ || epoch != configEpoch_)
                                                        return;
                                                    completion(std::move(result));
                                                },
                                                Qt::QueuedConnection);
                                        });
                                });
                        },
                        [this, epoch](quint32 deviceId, XmppKeyResolutionController::StatusCompletion completion) {
                            QMetaObject::invokeMethod(
                                backend_, [this, epoch, deviceId, completion = std::move(completion)]() mutable {
                                    if (shuttingDown_ || epoch != configEpoch_)
                                        return;
                                    backend_->removeOwnOmemoDeviceAsync(
                                        deviceId,
                                        [this, epoch,
                                         completion = std::move(completion)](XmppStatusResult result) mutable {
                                            QMetaObject::invokeMethod(
                                                this,
                                                [this, epoch, completion = std::move(completion),
                                                 result = std::move(result)]() mutable {
                                                    if (shuttingDown_ || epoch != configEpoch_)
                                                        return;
                                                    completion(std::move(result));
                                                },
                                                Qt::QueuedConnection);
                                        });
                                });
                        },
                        [this, epoch](XmppKeyResolutionController::AuditCompletion completion) {
                            QMetaObject::invokeMethod(
                                backend_, [this, epoch, completion = std::move(completion)]() mutable {
                                    if (shuttingDown_ || epoch != configEpoch_)
                                        return;
                                    backend_->auditStorageKeysAsync([this, epoch, completion = std::move(completion)](
                                                                        XmppKeyAuditResult result) mutable {
                                        QMetaObject::invokeMethod(
                                            this,
                                            [this, epoch, completion = std::move(completion),
                                             result = std::move(result)]() mutable {
                                                if (shuttingDown_ || epoch != configEpoch_)
                                                    return;
                                                completion(std::move(result));
                                            },
                                            Qt::QueuedConnection);
                                    });
                                });
                        },
                        [this, epoch](const QList<QByteArray> &keys, const QByteArray &canonical,
                                      XmppKeyResolutionController::RekeyCompletion completion) {
                            QMetaObject::invokeMethod(
                                backend_, [this, epoch, keys, canonical, completion = std::move(completion)]() mutable {
                                    if (shuttingDown_ || epoch != configEpoch_)
                                        return;
                                    backend_->rekeyStorageAsync(
                                        keys, canonical,
                                        [this, epoch,
                                         completion = std::move(completion)](XmppRekeyResult result) mutable {
                                            QMetaObject::invokeMethod(
                                                this,
                                                [this, epoch, completion = std::move(completion),
                                                 result = std::move(result)]() mutable {
                                                    if (shuttingDown_ || epoch != configEpoch_)
                                                        return;
                                                    completion(std::move(result));
                                                },
                                                Qt::QueuedConnection);
                                        });
                                });
                        },
                        qBound(15000, readConfig().timeoutMs * 3, 120000),
                        [](XmppKeyResolutionController::KeyCompletion completion) {
                            XmppStatusResult result;
                            result.ok      = true;
                            const auto key = SecureEnvelope::generateMasterKey();
                            completion(std::move(result), key,
                                       SecureEnvelope::keyId(key, KeyDerivationProfile::PrivateNotes));
                        },
                        this);

                    keyResolutionController_ = controller;
                    connect(controller, &XmppKeyResolutionController::finished, this,
                            [this, controller, jid, settingsGuard, epoch](bool accepted) {
                                const auto rekeyed    = controller->rekeyResult();
                                const auto canonical  = controller->canonicalKey();
                                const auto freshStart = controller->freshStart();
                                if (keyResolutionController_ == controller)
                                    keyResolutionController_.clear();
                                keyResolutionInProgress_ = false;
                                controller->deleteLater();

                                if (shuttingDown_ || epoch != configEpoch_ || !accepted)
                                    return;
                                if (!rekeyed.ok) {
                                    if (settingsGuard)
                                        settingsGuard->setKeyState(
                                            SecureEnvelope::keyId(canonical, KeyDerivationProfile::PrivateNotes),
                                            rekeyed.error);
                                    return;
                                }
                                installReceivedStorageKey(jid, canonical);
                                if (settingsGuard) {
                                    settingsGuard->setKeyState(
                                        SecureEnvelope::keyId(canonical, KeyDerivationProfile::PrivateNotes),
                                        freshStart
                                            ? tr("New empty storage created; old encrypted notes were left unchanged")
                                            : tr("Recovery complete: %1 notes use the canonical key")
                                                  .arg(rekeyed.migrated));
                                }
                            });
                    dialogPresenter_->presentKeyResolution(controller);
                },
                Qt::QueuedConnection);
        });
    });
}

void XmppStorage::abortKeyResolution()
{
    if (dialogPresenter_)
        dialogPresenter_->cancelAll();
    else if (keyResolutionController_)
        keyResolutionController_->abort();
    keyResolutionController_.clear();
    keyResolutionInProgress_ = false;
}

XmppStorage::~XmppStorage() { shutdown(); }

void XmppStorage::shutdown()
{
    if (shuttingDown_)
        return;
    shuttingDown_ = true;
    abortKeyResolution();
    cancelRefreshAttempt();
    folderPathUpdateQueue_.clear();
    folderPathUpdateQueued_.clear();
    folderPathUpdateInFlight_.clear();
    folderPathUpdateRunning_   = false;
    folderPathUpdateScheduled_ = false;
    ++configEpoch_;
    if (retryTimer_)
        retryTimer_->stop();
    if (backend_)
        backend_->shutdown();
}

XmppConfig XmppStorage::readConfig() const
{
    QSettings  settings;
    XmppConfig config;
    config.instanceId = settings.value(QStringLiteral("storage.xmpppubsub.instanceId")).toString();
    if (config.instanceId.isEmpty()) {
        config.instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("storage.xmpppubsub.instanceId"), config.instanceId);
    }
    config.originId = settings.value(QStringLiteral("storage.xmpppubsub.originId")).toString();
    if (config.originId.isEmpty()) {
        config.originId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("storage.xmpppubsub.originId"), config.originId);
    }

    config.jid
        = settings.value(QStringLiteral("storage.xmpppubsub.jid")).toString().trimmed().section(QLatin1Char('/'), 0, 0);
    if (!config.jid.isEmpty()) {
        const auto ownPassword = SecureKeyStore::readPassword(AnyKeepKeychainService, passwordKeyName(config.jid));
        if (ownPassword) {
            config.password = ownPassword.value;
            settings.remove(QStringLiteral("storage.xmpppubsub.password"));
        } else {
            // Psi uses service "xmpp" and the bare JID as its key. Importing it
            // also keeps AnyKeep usable on keychain backends that restrict
            // cross-application access later.
            const auto psiPassword = SecureKeyStore::readPassword(PsiKeychainService, config.jid);
            if (psiPassword) {
                config.password = psiPassword.value;
                if (!SecureKeyStore::writePassword(AnyKeepKeychainService, passwordKeyName(config.jid),
                                                   config.password))
                    settings.remove(QStringLiteral("storage.xmpppubsub.password"));
            } else {
                const auto legacy = settings.value(QStringLiteral("storage.xmpppubsub.password")).toString();
                if (!legacy.isEmpty()) {
                    config.password = legacy;
                    if (!SecureKeyStore::writePassword(AnyKeepKeychainService, passwordKeyName(config.jid), legacy))
                        settings.remove(QStringLiteral("storage.xmpppubsub.password"));
                }
            }
        }
    }
    config.host = settings.value(QStringLiteral("storage.xmpppubsub.host")).toString();
    config.port = settings.value(QStringLiteral("storage.xmpppubsub.port"), 0).toInt();

    const QString defaultResource = QStringLiteral("private-notes-") + config.originId.left(8);
    config.resource = settings.value(QStringLiteral("storage.xmpppubsub.resource"), defaultResource).toString();
    const auto storedNodeName = settings.value(QStringLiteral("storage.xmpppubsub.node")).toString().trimmed();
    config.nodeName           = storedNodeName.isEmpty() || storedNodeName == QStringLiteral("urn:xmpp:private-notes:0")
        ? XmppConfig {}.nodeName
        : storedNodeName;
    config.timeoutMs          = settings.value(QStringLiteral("storage.xmpppubsub.timeoutMs"), 15000).toInt();
    if (!config.jid.isEmpty()) {
        const auto key = SecureKeyStore::read(storageKeyName(config.jid));
        if (key)
            config.masterKey = key.value;
        const auto omemoKey
            = SecureKeyStore::loadOrCreate(QStringLiteral("xmpp-omemo-state-key-v1:%1").arg(config.jid));
        if (omemoKey)
            config.omemoStateKey = omemoKey.value;
        const auto accountHash = QCryptographicHash::hash(config.jid.toUtf8(), QCryptographicHash::Sha256).toHex();
        config.omemoStatePath  = Utils::anykeepDataDir() + QStringLiteral("/xmpp-omemo-")
            + QString::fromLatin1(accountHash.left(16)) + QStringLiteral(".state");
    }
    return config;
}

bool XmppStorage::connectionConfigIsValid(const XmppConfig &config, QString *error) const
{
    const QString bareJid = config.jid.section(QLatin1Char('/'), 0, 0);
    const int     at      = bareJid.indexOf(QLatin1Char('@'));
    if (at <= 0 || at == bareJid.size() - 1) {
        if (error)
            *error = tr("Enter a valid XMPP JID such as user@example.org.");
        return false;
    }
    if (config.password.isEmpty()) {
        if (error)
            *error = tr("Enter the XMPP account password.");
        return false;
    }
    if (config.resource.isEmpty()) {
        if (error)
            *error = tr("Enter an XMPP resource name.");
        return false;
    }
    if (config.nodeName.isEmpty()) {
        if (error)
            *error = tr("Enter a PubSub node name.");
        return false;
    }
    return true;
}

bool XmppStorage::configIsValid(const XmppConfig &config, QString *error) const
{
    if (!connectionConfigIsValid(config, error))
        return false;
    if (config.masterKey.size() != SecureEnvelope::MasterKeySize) {
        if (error)
            *error = tr("Create, import, or synchronize the XMPP storage encryption key.");
        return false;
    }
    if (config.omemoStateKey.size() != SecureEnvelope::MasterKeySize || config.omemoStatePath.isEmpty()) {
        if (error)
            *error = tr("The local OMEMO state cannot be protected by the system keychain.");
        return false;
    }
    return true;
}

bool XmppStorage::init()
{
    if (errorState_) {
        return false;
    }

    config_ = readConfig();

    QString validationError;
    if (!configIsValid(config_, &validationError)) {
        accessible_           = false;
        cacheValid_           = false;
        auto recoverable      = config_;
        recoverable.masterKey = QByteArray(SecureEnvelope::MasterKeySize, '\0');
        QString otherError;
        if (config_.masterKey.size() != SecureEnvelope::MasterKeySize && configIsValid(recoverable, &otherError)) {
            QTimer::singleShot(0, this, [this, jid = config_.jid]() { resolveStorageKeys(jid); });
        }
        return false;
    }

    auto *job = initAsync(this);
    connect(job, &StorageJob::finished, job, &QObject::deleteLater);
    return accessible_;
}

StorageInitJob *XmppStorage::initAsync(QObject *owner)
{
    auto *job = new StorageInitJob(owner ? owner : this);
    job->start();
    if (errorState_) {
        job->fail({ StorageError::Unavailable, errorStateMessage_, false });
        return job;
    }
    config_ = readConfig();
    QString validationError;
    if (!configIsValid(config_, &validationError)) {
        auto recoverable      = config_;
        recoverable.masterKey = QByteArray(SecureEnvelope::MasterKeySize, '\0');
        QString otherError;
        if (config_.masterKey.size() != SecureEnvelope::MasterKeySize && configIsValid(recoverable, &otherError)) {
            QTimer::singleShot(0, this, [this, jid = config_.jid]() { resolveStorageKeys(jid); });
        }
        job->fail({ StorageError::NotConfigured, validationError, false });
        return job;
    }
    openPersistentCache(config_);
    if (cacheAvailable_)
        emit invalidated();
    const auto               config = config_;
    const auto               epoch  = configEpoch_;
    QPointer<StorageInitJob> guard(job);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->probeAsync([this, guard, epoch](XmppStatusResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, epoch, result = std::move(result)]() {
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        accessible_ = result.ok;
                        cacheValid_ = false;
                        if (result.ok) {
                            resetRetryBackoff();
                            scheduleFolderPathSynchronization();
                            guard->complete();
                        } else {
                            if (result.retryable())
                                handleTransientFailure(result.error);
                            else
                                enterErrorState(result.error, true);
                            guard->fail(storageError(result, StorageError::Network));
                        }
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

const QString XmppStorage::systemName() const { return storageId; }

const QString XmppStorage::name() const { return tr("XMPP Private Notes"); }

QIcon XmppStorage::storageIcon() const { return icon_; }

QIcon XmppStorage::noteIcon() const { return icon_; }

bool XmppStorage::isAccessible() const { return accessible_ || cacheAvailable_; }

bool XmppStorage::canAcceptWrites() const
{
    if (shuttingDown_ || errorState_)
        return false;
    return configIsValid(config_, nullptr);
}

QList<Note::Format> XmppStorage::availableFormats() const { return { Note::Markdown }; }

} // namespace AnyKeep
