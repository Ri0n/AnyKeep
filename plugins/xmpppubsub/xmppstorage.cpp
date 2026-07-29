#include "xmppstorage.h"

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
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QNetworkInformation>
#endif
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <utility>

namespace QtNote {

namespace {
    constexpr int MinimumRetryDelaySeconds  = 30;
    constexpr int MaximumRetryDelaySeconds  = 300;
    const QString QtNoteKeychainService     = QStringLiteral("com.github.ri0n.qtnote");
    const QString PsiKeychainService        = QStringLiteral("xmpp");
    const QString IndexRecordTemplateKey    = QStringLiteral("xmpp.xml.v1.index-template");
    const QString ContentRecordTemplateKey  = QStringLiteral("xmpp.xml.v1.content-template");
    const QString ContentRevisionBackendKey = QStringLiteral("xmpp.xml.v1.content-revision");
    const QString FolderPathBackendKey      = QStringLiteral("xmpp.xml.v1.folder-path");

    QString passwordKeyName(const QString &jid)
    {
        return QStringLiteral("xmpp-password-v1:%1").arg(jid.trimmed().section(QLatin1Char('/'), 0, 0));
    }

    QString storageKeyName(const QString &jid)
    {
        return QStringLiteral("xmpp-storage-master-key-v1:%1").arg(jid.trimmed().section(QLatin1Char('/'), 0, 0));
    }

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

    StorageError storageError(const XmppStatusResult &result, StorageError::Code fallback)
    {
        auto code = fallback;
        if (result.errorKind == XmppErrorKind::Authentication)
            code = StorageError::Authentication;
        else if (result.errorKind == XmppErrorKind::Configuration || result.errorKind == XmppErrorKind::Security)
            code = StorageError::Unavailable;
        return { code, result.error, result.retryable() };
    }

} // namespace

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
    const auto keyId = SecureEnvelope::keyId(key);
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
                        this);

                    keyResolutionController_ = controller;
                    connect(controller, &XmppKeyResolutionController::finished, this,
                            [this, controller, jid, settingsGuard, epoch](bool accepted) {
                                const auto rekeyed   = controller->rekeyResult();
                                const auto canonical = controller->canonicalKey();
                                if (keyResolutionController_ == controller)
                                    keyResolutionController_.clear();
                                keyResolutionInProgress_ = false;
                                controller->deleteLater();

                                if (shuttingDown_ || epoch != configEpoch_ || !accepted)
                                    return;
                                if (!rekeyed.ok) {
                                    if (settingsGuard)
                                        settingsGuard->setKeyState(SecureEnvelope::keyId(canonical), rekeyed.error);
                                    return;
                                }
                                installReceivedStorageKey(jid, canonical);
                                if (settingsGuard) {
                                    settingsGuard->setKeyState(
                                        SecureEnvelope::keyId(canonical),
                                        tr("Recovery complete: %1 notes use the canonical key").arg(rekeyed.migrated));
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
        const auto ownPassword = SecureKeyStore::readPassword(QtNoteKeychainService, passwordKeyName(config.jid));
        if (ownPassword) {
            config.password = ownPassword.value;
            settings.remove(QStringLiteral("storage.xmpppubsub.password"));
        } else {
            // Psi uses service "xmpp" and the bare JID as its key. Importing it
            // also keeps QtNote usable on keychain backends that restrict
            // cross-application access later.
            const auto psiPassword = SecureKeyStore::readPassword(PsiKeychainService, config.jid);
            if (psiPassword) {
                config.password = psiPassword.value;
                if (!SecureKeyStore::writePassword(QtNoteKeychainService, passwordKeyName(config.jid), config.password))
                    settings.remove(QStringLiteral("storage.xmpppubsub.password"));
            } else {
                const auto legacy = settings.value(QStringLiteral("storage.xmpppubsub.password")).toString();
                if (!legacy.isEmpty()) {
                    config.password = legacy;
                    if (!SecureKeyStore::writePassword(QtNoteKeychainService, passwordKeyName(config.jid), legacy))
                        settings.remove(QStringLiteral("storage.xmpppubsub.password"));
                }
            }
        }
    }
    config.host = settings.value(QStringLiteral("storage.xmpppubsub.host")).toString();
    config.port = settings.value(QStringLiteral("storage.xmpppubsub.port"), 0).toInt();

    const QString defaultResource = QStringLiteral("QtNote-") + config.originId.left(8);
    config.resource = settings.value(QStringLiteral("storage.xmpppubsub.resource"), defaultResource).toString();
    const auto storedNodeName = settings.value(QStringLiteral("storage.xmpppubsub.node")).toString().trimmed();
    config.nodeName           = storedNodeName.isEmpty() || storedNodeName == QStringLiteral("urn:xmpp:qtnote:notes:0")
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
        config.omemoStatePath  = Utils::qtnoteDataDir() + QStringLiteral("/xmpp-omemo-")
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

Note XmppStorage::fromRemote(const XmppRemoteNote &remote)
{
    Note note(new NoteData(this));
    applyRemote(note, remote);
    return note;
}

void XmppStorage::applyRemote(Note &note, const XmppRemoteNote &remote)
{
    note.setId(remote.id);
    note.setTitle(remote.title);
    note.setFormat(Note::Markdown);
    note.setLastChangeUTC(remote.modified);
    note.setBackendValue(QStringLiteral("revision"), remote.revision);
    note.setBackendValue(ContentRevisionBackendKey, remote.contentRevision);
    note.setBackendValue(QStringLiteral("parentRevision"), remote.parentRevision);
    note.setBackendValue(QStringLiteral("originId"), remote.originId);
    note.setBackendValue(IndexRecordTemplateKey, remote.indexRecordTemplate);
    note.setBackendValue(ContentRecordTemplateKey, remote.contentRecordTemplate);
    note.setBackendValue(FolderPathBackendKey, remote.folderPath);
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
        note.setFolderId(folderCatalogManager_->catalog().folderForNote(systemName(), remote.id));
    } else {
        note.setFolderId({});
    }
    if (remote.contentPresent)
        note.setText(remote.content, Note::Markdown);
    else
        note.unload();
    note.setTags(remote.tags);
}

bool XmppStorage::folderPathForFolder(const QUuid &folderId, QStringList *path, QString *error) const
{
    if (path)
        path->clear();
    if (error)
        error->clear();
    if (folderId.isNull())
        return true;
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable()) {
        if (error)
            *error = tr("The folder catalog is unavailable");
        return false;
    }

    const auto folderPath = folderCatalogManager_->catalog().pathForFolder(folderId);
    if (folderPath.isEmpty()) {
        if (error)
            *error = tr("The selected folder no longer exists");
        return false;
    }
    if (path)
        *path = folderPath;
    return true;
}

bool XmppStorage::folderPathForNote(const Note &note, QStringList *path, QString *error) const
{
    if (!note.folderId().isNull())
        return folderPathForFolder(note.folderId(), path, error);

    if (path)
        path->clear();
    if (error)
        error->clear();
    if (note.id().isEmpty())
        return true;
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
        const auto assignedFolder = folderCatalogManager_->catalog().folderForNote(systemName(), note.id());
        if (!assignedFolder.isNull())
            return folderPathForFolder(assignedFolder, path, error);
        const auto *assignment = folderCatalogManager_->catalog().assignment(systemName(), note.id());
        if (assignment && assignment->tombstone)
            return true;
    }

    if (path)
        *path = note.backendValue(FolderPathBackendKey).toStringList();
    return true;
}

bool XmppStorage::toRemote(const Note &note, XmppRemoteNote *remote, QString *error) const
{
    if (!remote)
        return false;

    XmppRemoteNote result;
    result.id              = note.id();
    result.revision        = note.backendValue(QStringLiteral("revision")).toString();
    result.contentRevision = note.backendValue(ContentRevisionBackendKey).toString();
    result.parentRevision  = note.backendValue(QStringLiteral("parentRevision")).toString();
    result.originId        = note.backendValue(QStringLiteral("originId")).toString();
    result.title           = note.title();
    result.content         = note.text();
    result.modified        = note.lastChangeUTC();
    const auto requestedModified
        = note.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).toDateTime();
    result.preserveModified = requestedModified.isValid();
    if (result.preserveModified)
        result.modified = requestedModified;
    result.format                = QStringLiteral("markdown");
    result.tags                  = note.tags();
    result.contentPresent        = note.isLoaded();
    result.indexRecordTemplate   = note.backendValue(IndexRecordTemplateKey).toByteArray();
    result.contentRecordTemplate = note.backendValue(ContentRecordTemplateKey).toByteArray();
    if (!folderPathForNote(note, &result.folderPath, error))
        return false;
    *remote = std::move(result);
    return true;
}

void XmppStorage::reconcileRemoteFolders(const QList<XmppRemoteNote> &notes)
{
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return;

    QList<ProviderFolderPathAssignment> assignments;
    assignments.reserve(notes.size());
    for (const auto &remote : notes) {
        if (remote.id.isEmpty())
            continue;
        ProviderFolderPathAssignment assignment;
        assignment.noteId     = remote.id;
        assignment.path       = remote.folderPath;
        assignment.modifiedAt = remote.modified;
        assignments.append(std::move(assignment));
    }
    if (assignments.isEmpty())
        return;
    if (const auto result = folderCatalogManager_->reconcileProviderFolderPaths(systemName(), assignments))
        reportError(tr("Could not merge XMPP folders: %1").arg(result.message));
}

void XmppStorage::reconcileCachedFolders()
{
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return;

    QList<XmppRemoteNote> notes;
    notes.reserve(cache_.size());
    for (const auto &cached : std::as_const(cache_)) {
        if (!cached.backendData().contains(FolderPathBackendKey))
            continue;
        XmppRemoteNote remote;
        remote.id         = cached.id();
        remote.modified   = cached.lastChangeUTC();
        remote.folderPath = cached.backendValue(FolderPathBackendKey).toStringList();
        notes.append(std::move(remote));
    }
    reconcileRemoteFolders(notes);

    bool changed = false;
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        const auto folderId = folderCatalogManager_->catalog().folderForNote(systemName(), it.key());
        if (it.value().folderId() == folderId)
            continue;
        it.value().setFolderId(folderId);
        changed = true;
    }
    if (changed)
        persistCache();
}

void XmppStorage::scheduleFolderPathSynchronization()
{
    if (folderPathUpdateScheduled_ || shuttingDown_ || errorState_ || !accessible_ || !folderCatalogManager_
        || !folderCatalogManager_->isAvailable()) {
        return;
    }
    folderPathUpdateScheduled_ = true;
    QTimer::singleShot(0, this, [this]() {
        folderPathUpdateScheduled_ = false;
        enqueueFolderPathUpdates();
    });
}

void XmppStorage::enqueueFolderPathUpdates()
{
    if (shuttingDown_ || errorState_ || !accessible_ || !folderCatalogManager_
        || !folderCatalogManager_->isAvailable()) {
        return;
    }

    const auto &catalog = folderCatalogManager_->catalog();
    for (const auto &cached : std::as_const(cache_)) {
        if (cached.isNull() || cached.id().isEmpty() || folderPathUpdateInFlight_.contains(cached.id()))
            continue;

        Note desired = cached;
        if (const auto *assignment = catalog.assignment(systemName(), cached.id()))
            desired.setFolderId(assignment->tombstone ? QUuid {} : assignment->folderId);
        else
            desired.setFolderId({});

        QStringList path;
        QString     error;
        if (!folderPathForNote(desired, &path, &error)) {
            reportError(tr("Could not resolve the XMPP folder path for a note: %1").arg(error));
            continue;
        }
        if (path == cached.backendValue(FolderPathBackendKey).toStringList())
            continue;
        if (!folderPathUpdateQueued_.contains(cached.id())) {
            folderPathUpdateQueued_.insert(cached.id());
            folderPathUpdateQueue_.append(cached.id());
        }
    }
    publishNextFolderPathUpdate();
}

void XmppStorage::publishNextFolderPathUpdate()
{
    if (folderPathUpdateRunning_ || shuttingDown_ || errorState_ || !accessible_)
        return;

    while (!folderPathUpdateQueue_.isEmpty()) {
        const auto noteId = folderPathUpdateQueue_.takeFirst();
        folderPathUpdateQueued_.remove(noteId);
        if (folderPathUpdateInFlight_.contains(noteId))
            continue;

        const auto cached = cache_.value(noteId);
        if (cached.isNull())
            continue;

        Note desired = cached;
        if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
            if (const auto *assignment = folderCatalogManager_->catalog().assignment(systemName(), noteId))
                desired.setFolderId(assignment->tombstone ? QUuid {} : assignment->folderId);
            else
                desired.setFolderId({});
        }

        QStringList path;
        QString     error;
        if (!folderPathForNote(desired, &path, &error)) {
            reportError(tr("Could not resolve the XMPP folder path for a note: %1").arg(error));
            continue;
        }
        if (path == cached.backendValue(FolderPathBackendKey).toStringList())
            continue;

        XmppRemoteNote local;
        if (!toRemote(desired, &local, &error)) {
            reportError(tr("Could not prepare an XMPP folder update: %1").arg(error));
            continue;
        }
        if (local.revision.isEmpty())
            continue;
        local.folderPath = std::move(path);

        folderPathUpdateRunning_ = true;
        folderPathUpdateInFlight_.insert(noteId);
        const auto config = config_;
        const auto epoch  = configEpoch_;
        QMetaObject::invokeMethod(
            backend_,
            [this, config, local = std::move(local), noteId, epoch]() {
                if (shuttingDown_ || epoch != configEpoch_)
                    return;
                backend_->setConfig(config);
                backend_->updateNoteIndexAsync(local, [this, noteId, epoch](XmppNoteResult result) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, noteId, epoch, result = std::move(result)]() {
                            folderPathUpdateInFlight_.remove(noteId);
                            folderPathUpdateRunning_ = false;
                            if (shuttingDown_ || epoch != configEpoch_)
                                return;

                            if (!result.ok) {
                                if (result.remoteOnConflict) {
                                    reconcileRemoteFolders({ *result.remoteOnConflict });
                                    cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                                    persistCache();
                                }
                                if (result.retryable())
                                    handleTransientFailure(result.error, false);
                                else
                                    reportError(
                                        tr("Could not synchronize an XMPP folder change: %1").arg(result.error));
                            } else {
                                reconcileRemoteFolders({ result.note });
                                auto       changed  = fromRemote(result.note);
                                const auto previous = cache_.value(noteId);
                                if (!previous.isNull() && previous.isLoaded()) {
                                    changed.setText(previous.text(), previous.format());
                                    changed.setMedia(previous.media());
                                }
                                cache_.insert(noteId, changed);
                                cacheValid_ = accessible_ = true;
                                persistCache();
                                emit noteModified(changed);
                            }

                            QTimer::singleShot(0, this, &XmppStorage::publishNextFolderPathUpdate);
                        },
                        Qt::QueuedConnection);
                });
            },
            Qt::QueuedConnection);
        return;
    }
}

bool XmppStorage::openPersistentCache(const XmppConfig &config)
{
    if (config.instanceId.isEmpty())
        return false;
    const auto nodeHash
        = QCryptographicHash::hash(config.nodeName.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    const auto cacheScope = config.instanceId + QLatin1Char(':') + QString::fromLatin1(nodeHash);
    if (!persistentCache_ || persistentCacheInstanceId_ != cacheScope) {
        QString keyError;
        auto    localKey = LocalDataKeyStore::loadOrCreateMasterKey(&keyError);
        if (localKey.isEmpty()) {
            reportError(tr("The local note cache could not be opened: %1").arg(keyError));
            return false;
        }
        const auto path = Utils::qtnoteDataDir() + QStringLiteral("/remote-cache/xmpppubsub/") + config.instanceId
            + QLatin1Char('-') + QString::fromLatin1(nodeHash) + QStringLiteral(".cache");
        persistentCache_           = std::make_unique<FileRemoteCacheStore>(path, cacheScope, std::move(localKey));
        persistentCacheInstanceId_ = cacheScope;
    }

    const auto records = persistentCache_->records();
    if (!records) {
        reportError(tr("The local note cache could not be read: %1").arg(records.error.message));
        return false;
    }
    cache_.clear();
    for (const auto &record : records.value) {
        Note note(new NoteData(this));
        note.setId(record.id);
        note.setTitle(record.title);
        note.setFormat(record.format);
        note.setLastChangeUTC(record.modified);
        note.setFolderId(record.folderId);
        note.setBackendData(record.backendData);
        note.setMedia(record.media);
        if (record.bodyPresent)
            note.setText(record.body, record.format);
        else
            note.unload();
        note.setTags(record.tags);
        cache_.insert(record.id, note);
    }
    cacheAvailable_ = !records.value.isEmpty();
    cacheValid_     = cacheAvailable_;
    reconcileCachedFolders();
    return true;
}

void XmppStorage::persistCache()
{
    if (!persistentCache_)
        return;
    QList<RemoteCacheRecord> records;
    records.reserve(cache_.size());
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &note : std::as_const(cache_)) {
        RemoteCacheRecord record;
        record.id          = note.id();
        record.title       = note.title();
        record.tags        = note.tags();
        record.modified    = note.lastChangeUTC();
        record.format      = note.format();
        record.body        = note.text();
        record.bodyPresent = note.isLoaded();
        record.folderId    = note.folderId();
        record.backendData = note.backendData();
        record.media       = note.media();
        record.syncState   = RemoteCacheRecord::Synced;
        record.cachedAt    = now;
        records.append(std::move(record));
    }
    if (const auto error = persistentCache_->replaceRecords(records)) {
        reportError(tr("The local note cache could not be written: %1").arg(error.message));
        return;
    }
    cacheAvailable_ = !records.isEmpty();
}

void XmppStorage::startBodyPrefetch(const QStringList &ids)
{
    for (const auto &id : ids) {
        if (!id.isEmpty() && !bodyPrefetchQueue_.contains(id))
            bodyPrefetchQueue_.append(id);
    }
    prefetchNextBody();
}

void XmppStorage::prefetchNextBody()
{
    if (bodyPrefetchRunning_ || bodyPrefetchQueue_.isEmpty() || shuttingDown_ || !accessible_)
        return;
    bodyPrefetchRunning_ = true;
    const auto id        = bodyPrefetchQueue_.takeFirst();
    auto      *job       = loadNoteAsync(id, this);
    const auto finish    = [this, job]() {
        bodyPrefetchRunning_ = false;
        job->deleteLater();
        QTimer::singleShot(0, this, &XmppStorage::prefetchNextBody);
    };
    connect(job, &StorageJob::finished, this, finish);
    if (job->isFinished())
        finish();
}

QList<Note> XmppStorage::noteList(int limit)
{
    auto notes = cache_.values();
    std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
    return limit > 0 ? notes.mid(0, limit) : notes;
}

NoteListJob *XmppStorage::refreshNotesAsync(int limit, QObject *owner)
{
    auto *job = new NoteListJob(owner ? owner : this);
    job->start();
    if (cacheValid_ || (cacheAvailable_ && !accessible_)) {
        auto notes = cache_.values();
        std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
        QPointer<NoteListJob> guard(job);
        QTimer::singleShot(0, this, [guard, notes = limit > 0 ? notes.mid(0, limit) : notes]() {
            if (guard && !guard->isFinished())
                guard->complete(notes);
        });
        return job;
    }
    if (errorState_) {
        job->fail({ StorageError::Unavailable, errorStateMessage_, false });
        return job;
    }

    if (refreshAttempt_ && refreshAttempt_->epoch == configEpoch_) {
        refreshAttempt_->waiters.append(RefreshWaiter { job, limit });
        return job;
    }

    const auto attempt = std::make_shared<RefreshAttempt>();
    attempt->epoch     = configEpoch_;
    attempt->waiters.append(RefreshWaiter { job, limit });
    refreshAttempt_ = attempt;

    const auto config = config_;
    const auto epoch  = attempt->epoch;
    QMetaObject::invokeMethod(
        backend_,
        [this, attempt, config, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                for (const auto &waiter : attempt->waiters) {
                    if (waiter.job && !waiter.job->isFinished())
                        waiter.job->cancel();
                }
                if (refreshAttempt_ == attempt)
                    refreshAttempt_.reset();
                return;
            }
            backend_->setConfig(config);
            backend_->listNotesAsync([this, attempt, epoch](XmppListResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, attempt, result = std::move(result), epoch]() {
                        if (refreshAttempt_ != attempt)
                            return;
                        refreshAttempt_.reset();
                        if (shuttingDown_ || epoch != configEpoch_) {
                            for (const auto &waiter : attempt->waiters) {
                                if (waiter.job && !waiter.job->isFinished())
                                    waiter.job->cancel();
                            }
                            return;
                        }
                        if (!result.ok) {
                            qWarning().noquote() << "XMPP index refresh failed:" << result.error;
                            if (result.retryable())
                                handleTransientFailure(result.error);
                            else
                                enterErrorState(result.error, true);
                            const auto error = storageError(result, StorageError::Network);
                            for (const auto &waiter : attempt->waiters) {
                                if (waiter.job && !waiter.job->isFinished())
                                    waiter.job->fail(error);
                            }
                            return;
                        }
                        reconcileRemoteFolders(result.notes);
                        QHash<QString, Note> refreshed = result.partial ? cache_ : QHash<QString, Note> {};
                        QStringList          missingBodies;
                        for (const auto &remote : result.notes) {
                            const auto old = cache_.constFind(remote.id);
                            if (old != cache_.cend()
                                && old.value().backendValue(QStringLiteral("revision")).toString() == remote.revision) {
                                auto cached = old.value();
                                if (folderCatalogManager_ && folderCatalogManager_->isAvailable()) {
                                    cached.setFolderId(
                                        folderCatalogManager_->catalog().folderForNote(systemName(), remote.id));
                                }
                                refreshed.insert(remote.id, cached);
                                if (!old.value().isLoaded())
                                    missingBodies.append(remote.id);
                            } else {
                                refreshed.insert(remote.id, fromRemote(remote));
                                missingBodies.append(remote.id);
                            }
                        }
                        cache_      = std::move(refreshed);
                        cacheValid_ = accessible_ = true;
                        resetRetryBackoff();
                        persistCache();
                        scheduleFolderPathSynchronization();
                        auto notes = cache_.values();
                        std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
                        qInfo() << "XMPP index refresh loaded" << notes.size() << "note(s) for"
                                << attempt->waiters.size() << "caller(s)"
                                << (result.partial ? "from a partial remote result" : "from a complete remote result");
                        for (const auto &waiter : attempt->waiters) {
                            if (!waiter.job || waiter.job->isFinished())
                                continue;
                            waiter.job->complete(waiter.limit > 0 ? notes.mid(0, waiter.limit) : notes);
                        }
                        startBodyPrefetch(missingBodies);
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

void XmppStorage::cancelRefreshAttempt()
{
    const auto attempt = std::exchange(refreshAttempt_, std::shared_ptr<RefreshAttempt> {});
    if (!attempt)
        return;
    for (const auto &waiter : attempt->waiters) {
        if (waiter.job && !waiter.job->isFinished())
            waiter.job->cancel();
    }
}

Note XmppStorage::note(const QString &id) { return cache_.value(id); }

NoteLoadJob *XmppStorage::loadNoteAsync(const QString &id, QObject *owner)
{
    auto *job = new NoteLoadJob(owner ? owner : this);
    job->start();
    if (id.isEmpty()) {
        job->fail({ StorageError::NotFound, tr("Note was not found"), false });
        return job;
    }
    const auto cached = cache_.value(id);
    if (!cached.isNull() && cached.isLoaded()) {
        QPointer<NoteLoadJob> guard(job);
        QTimer::singleShot(0, this, [guard, cached]() {
            if (guard && !guard->isFinished())
                guard->complete(cached);
        });
        return job;
    }
    if (errorState_ || !accessible_) {
        job->fail({ StorageError::Unavailable,
                    errorState_ ? errorStateMessage_ : tr("The remote note body is not available offline."), false });
        return job;
    }
    const auto            config = config_;
    const auto            epoch  = configEpoch_;
    QPointer<NoteLoadJob> guard(job);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, id, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->getNoteAsync(id, [this, guard, id, epoch](XmppNoteResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), id, epoch]() {
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok) {
                            if (result.notFound) {
                                cache_.remove(id);
                                persistCache();
                            }
                            guard->fail(
                                storageError(result, result.notFound ? StorageError::NotFound : StorageError::Network));
                            return;
                        }
                        reconcileRemoteFolders({ result.note });
                        auto loaded = fromRemote(result.note);
                        cache_.insert(id, loaded);
                        accessible_ = true;
                        persistCache();
                        guard->complete(loaded);
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

Note XmppStorage::createNote()
{
    Note note(new NoteData(this));
    note.setText(QString(), Note::Markdown);
    note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
    return note;
}

bool XmppStorage::loadNote(Note &note)
{
    const QString id = note.id();
    if (id.isEmpty())
        return true;
    const auto cached = cache_.value(id);
    if (cached.isNull() || !cached.isLoaded())
        return false;
    note = cached;
    return true;
}

bool XmppStorage::saveNote(const Note &note)
{
    if (note.isNull() || note.storage() != this || !note.isLoaded())
        return false;
    const auto draftId = QUuid::createUuid();
    auto       error   = DraftManager::instance()->saveEditing(draftId, note, note.title(), note.text(), note.format());
    if (!error)
        error = DraftManager::instance()->markReady(draftId);
    return !error;
}

NoteSaveJob *XmppStorage::saveNoteAsync(const Note &note, QObject *owner)
{
    auto *job = new NoteSaveJob(owner ? owner : this);
    job->start();
    if (note.isNull() || note.storage() != this || !note.isLoaded() || errorState_) {
        job->fail({ StorageError::Other,
                    errorState_ ? errorStateMessage_ : tr("The note cannot be saved in its current state."), false });
        return job;
    }
    XmppRemoteNote local;
    QString        folderError;
    if (!toRemote(note, &local, &folderError)) {
        job->fail({ StorageError::Other, folderError, false });
        return job;
    }
    const auto            config = config_;
    const auto            epoch  = configEpoch_;
    const auto            oldId  = note.id();
    QPointer<NoteSaveJob> guard(job);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, local, oldId, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->saveNoteAsync(local, [this, guard, oldId, epoch](XmppNoteResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), oldId, epoch]() {
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok) {
                            auto error = storageError(result,
                                                      result.conflict ? StorageError::Conflict : StorageError::Network);
                            if (result.remoteOnConflict) {
                                reconcileRemoteFolders({ *result.remoteOnConflict });
                                cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                            }
                            guard->fail(error);
                            return;
                        }
                        reconcileRemoteFolders({ result.note });
                        auto       saved   = fromRemote(result.note);
                        const bool existed = !oldId.isEmpty() && cache_.contains(oldId);
                        if (!oldId.isEmpty() && oldId != saved.id())
                            cache_.remove(oldId);
                        cache_.insert(saved.id(), saved);
                        cacheValid_ = accessible_ = true;
                        persistCache();
                        guard->complete(saved);
                        if (!oldId.isEmpty() && oldId != saved.id())
                            emit noteIdChanged(saved, oldId);
                        if (existed || !oldId.isEmpty())
                            emit noteModified(saved);
                        else
                            emit noteAdded(saved);
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

NoteFolderChangeJob *XmppStorage::changeNoteFolderAsync(const Note &note, QObject *owner)
{
    auto *job = new NoteFolderChangeJob(owner ? owner : this);
    job->start();
    if (note.isNull() || note.storage() != this) {
        job->fail({ StorageError::Other, tr("Attempted to move a note owned by another storage."), false });
        return job;
    }
    if (note.id().isEmpty()) {
        job->fail({ StorageError::NotFound, tr("The note must be saved before it can be moved."), false });
        return job;
    }
    if (errorState_) {
        job->fail({ StorageError::Unavailable, errorStateMessage_, false });
        return job;
    }
    if (folderPathUpdateInFlight_.contains(note.id())) {
        job->fail({ StorageError::Network, tr("Another folder update for this note is already in progress."), true });
        return job;
    }

    QStringList folderPath;
    QString     folderError;
    if (!folderPathForFolder(note.folderId(), &folderPath, &folderError)) {
        job->fail({ StorageError::Other, folderError, false });
        return job;
    }

    XmppRemoteNote local;
    QString        remoteError;
    if (!toRemote(note, &local, &remoteError)) {
        job->fail({ StorageError::Other, remoteError, false });
        return job;
    }
    local.folderPath = std::move(folderPath);

    const auto                    config   = config_;
    const auto                    epoch    = configEpoch_;
    const auto                    noteId   = note.id();
    const auto                    folderId = note.folderId();
    QPointer<NoteFolderChangeJob> guard(job);
    folderPathUpdateInFlight_.insert(noteId);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, local, noteId, folderId, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->updateNoteIndexAsync(local, [this, guard, noteId, folderId, epoch](XmppNoteResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), noteId, folderId, epoch]() {
                        folderPathUpdateInFlight_.remove(noteId);
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok) {
                            if (result.remoteOnConflict) {
                                reconcileRemoteFolders({ *result.remoteOnConflict });
                                cache_.insert(result.remoteOnConflict->id, fromRemote(*result.remoteOnConflict));
                                persistCache();
                            }
                            guard->fail(
                                storageError(result, result.conflict ? StorageError::Conflict : StorageError::Network));
                            return;
                        }

                        reconcileRemoteFolders({ result.note });
                        auto       changed = fromRemote(result.note);
                        const auto cached  = cache_.value(noteId);
                        if (!cached.isNull() && cached.isLoaded()) {
                            changed.setText(cached.text(), cached.format());
                            changed.setMedia(cached.media());
                        }
                        // The folder controller already recorded the local
                        // assignment before this request. Keep the summary
                        // aligned with that intent if a catalog merge is
                        // temporarily delayed by a timestamp tie.
                        if (changed.folderId() != folderId)
                            changed.setFolderId(folderId);
                        cache_.insert(noteId, changed);
                        cacheValid_ = accessible_ = true;
                        persistCache();
                        emit noteModified(changed);
                        guard->complete(changed);
                        scheduleFolderPathSynchronization();
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

void XmppStorage::removeNote(const QString &noteId)
{
    if (!noteId.isEmpty())
        DraftManager::instance()->queueRemoval(systemName(), noteId);
}

NoteRemoveJob *XmppStorage::removeNoteAsync(const QString &noteId, QObject *owner)
{
    auto *job = new NoteRemoveJob(owner ? owner : this);
    job->start();
    if (noteId.isEmpty() || errorState_) {
        job->fail({ noteId.isEmpty() ? StorageError::NotFound : StorageError::Unavailable,
                    noteId.isEmpty() ? tr("Note was not found") : errorStateMessage_, false });
        return job;
    }
    const auto              config  = config_;
    const auto              epoch   = configEpoch_;
    const auto              removed = cache_.value(noteId);
    QPointer<NoteRemoveJob> guard(job);
    QMetaObject::invokeMethod(
        backend_,
        [this, guard, config, noteId, removed, epoch]() {
            if (shuttingDown_ || epoch != configEpoch_) {
                if (guard)
                    guard->cancel();
                return;
            }
            backend_->setConfig(config);
            backend_->deleteNoteAsync(noteId, [this, guard, noteId, removed, epoch](XmppStatusResult result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, guard, result = std::move(result), noteId, removed, epoch]() {
                        if (!guard || guard->isFinished())
                            return;
                        if (shuttingDown_ || epoch != configEpoch_) {
                            guard->cancel();
                            return;
                        }
                        if (!result.ok && !result.notFound) {
                            guard->fail(storageError(result, StorageError::Network));
                            return;
                        }
                        cache_.remove(noteId);
                        persistCache();
                        if (!removed.isNull())
                            emit noteRemoved(removed);
                        guard->complete();
                    },
                    Qt::QueuedConnection);
            });
        },
        Qt::QueuedConnection);
    return job;
}

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
        = SecureKeyStore::writePassword(QtNoteKeychainService, passwordKeyName(config.jid), config.password);
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
    widget->setKeyState(SecureEnvelope::keyId(current.masterKey));
    connect(this, &XmppStorage::encryptionKeyChanged, widget, &XmppSettingsController::setKeyState);
    connect(widget, &XmppSettingsController::applyConfigRequested, this, &XmppStorage::applyConfig);
    connect(widget, &XmppSettingsController::createKeyRequested, this, [this, widget](const QString &jid) {
        if (jid.isEmpty()) {
            widget->setKeyState({}, tr("Enter the XMPP JID first"));
            return;
        }
        auto existing = SecureKeyStore::read(storageKeyName(jid));
        if (existing) {
            widget->setKeyState(SecureEnvelope::keyId(existing.value), tr("A key already exists"));
            return;
        }
        const auto key   = SecureEnvelope::generateMasterKey();
        const auto error = SecureKeyStore::write(storageKeyName(jid), key);
        if (error) {
            widget->setKeyState({}, error.message);
            return;
        }
        widget->setKeyState(SecureEnvelope::keyId(key));
        clearErrorState();
    });
    connect(widget, &XmppSettingsController::importKeyRequested, this,
            [this, widget](const QString &jid, const QString &encoded) {
                if (jid.isEmpty()) {
                    widget->setKeyState({}, tr("Enter the XMPP JID first"));
                    return;
                }
                auto imported = SecureEnvelope::decodeRecoveryKey(encoded);
                if (!imported) {
                    widget->setKeyState({}, imported.error.message);
                    return;
                }
                auto existing = SecureKeyStore::read(storageKeyName(jid));
                if (existing && existing.value != imported.value) {
                    widget->setKeyState(SecureEnvelope::keyId(existing.value),
                                        tr("A different key already exists; it was not replaced"));
                    return;
                }
                const auto error = SecureKeyStore::write(storageKeyName(jid), imported.value);
                if (error) {
                    widget->setKeyState({}, error.message);
                    return;
                }
                widget->setKeyState(SecureEnvelope::keyId(imported.value));
                clearErrorState();
            });
    connect(widget, &XmppSettingsController::exportKeyRequested, this, [widget](const QString &jid) {
        auto key = SecureKeyStore::read(storageKeyName(jid));
        if (!key) {
            widget->setKeyState({}, key.error.message);
            return;
        }
        widget->setRecoveryKey(SecureEnvelope::encodeRecoveryKey(key.value));
        widget->setKeyState(SecureEnvelope::keyId(key.value));
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
             QString::fromLatin1(SecureEnvelope::keyId(config_.masterKey).left(8).toHex()));
}

QString XmppStorage::storageId = QStringLiteral("xmpp-pubsub");

} // namespace QtNote
