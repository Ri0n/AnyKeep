#include "xmppworker.h"

#include "private.h"

#include "privatenotespubsubitem.h"
#include "xmpperror.h"
#include "xmppkeysyncextension.h"
#include "xmppnotecodec.h"
#include "xmppomemopubsubitems.h"
#include "xmppomemostorage.h"
#include "xmpppepextension.h"
#include "xmpppersistenttruststorage.h"
#include "xmppxmllog.h"

#include <QCoroFuture>
#include <QCoroSignal>
#include <QFutureInterface>
#include <QPointer>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTimer>
#include <QUuid>
#include <QXmppClient.h>
#include <QXmppConfiguration.h>
#include <QXmppDiscoveryIq.h>
#include <QXmppDiscoveryManager.h>
#include <QXmppE2eeMetadata.h>
#include <QXmppError.h>
#include <QXmppGlobal.h>
#include <QXmppLogger.h>
#include <QXmppOmemoManager.h>
#include <QXmppPubSubManager.h>
#include <QXmppPubSubNodeConfig.h>
#include <QXmppPubSubPublishOptions.h>
#include <QXmppRosterManager.h>
#include <QXmppStanza.h>
#include <QXmppTask.h>
#include <QXmppTrustManager.h>
#include <QXmppUtils.h>

#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace AnyKeep {

namespace {

    constexpr auto PublishOptionsFeature = "http://jabber.org/protocol/pubsub#publish-options";

    bool sameConfig(const XmppConfig &left, const XmppConfig &right)
    {
        return left.jid == right.jid && left.password == right.password && left.host == right.host
            && left.port == right.port && left.resource == right.resource && left.nodeName == right.nodeName
            && left.originId == right.originId && left.timeoutMs == right.timeoutMs && left.masterKey == right.masterKey
            && left.omemoStateKey == right.omemoStateKey && left.omemoStatePath == right.omemoStatePath;
    }

    QXmppPubSubNodeConfig privateNodeConfig()
    {
        QXmppPubSubNodeConfig config;
        config.setAccessModel(QXmppPubSubNodeConfig::Allowlist);
        config.setPersistItems(true);
        config.setMaxItems(QXmppPubSubNodeConfig::Max {});
        config.setIncludePayloads(true);
        config.setRetractNotificationsEnabled(true);
        config.setNodeType(QXmppPubSubNodeConfig::Leaf);
        config.setPayloadType(PrivateNotesPubSubItem::payloadNamespace);
        return config;
    }

    bool nodeConfigIsPrivate(const QXmppPubSubNodeConfig &config)
    {
        return config.accessModel() == QXmppPubSubNodeConfig::Allowlist && config.persistItems().value_or(false);
    }

    bool hasStanzaCondition(const QXmppError &error, QXmppStanza::Error::Condition condition)
    {
        const auto stanzaError = error.value<QXmppStanza::Error>();
        return stanzaError && stanzaError->condition() == condition;
    }

} // namespace

using namespace XmppWorkerPrivate;

struct XmppWorker::ConnectionAttempt {
    bool                  finished { false };
    quint64               generation { 0 };
    QPointer<QObject>     guard;
    QPointer<QTimer>      timer;
    QList<StatusCallback> callbacks;
};

XmppWorker::XmppWorker(QObject *parent) : XmppBackend(parent) { qRegisterMetaType<XmppRemoteNote>(); }

XmppWorker::~XmppWorker()
{
    acceptingWork_ = false;
    resetClient();
}

void XmppWorker::start() { acceptingWork_ = true; }

void XmppWorker::setConfig(const XmppConfig &config)
{
    if (sameConfig(config_, config)) {
        return;
    }
    config_ = config;
    resetClient();
}

void XmppWorker::shutdown()
{
    acceptingWork_ = false;
    resetClient();
}

void XmppWorker::resetClient()
{
    ++clientGeneration_;

    QList<StatusCallback> cancelledConnectionCallbacks;
    if (connectionAttempt_) {
        auto attempt = std::exchange(connectionAttempt_, std::shared_ptr<ConnectionAttempt> {});
        if (!attempt->finished) {
            attempt->finished = true;
            if (attempt->timer)
                attempt->timer->stop();
            if (attempt->guard)
                attempt->guard->deleteLater();
            cancelledConnectionCallbacks = std::move(attempt->callbacks);
        }
    }

    omemoReadinessAttempt_.reset();
    readinessAttempt_.reset();
    prepared_         = false;
    omemoReady_       = false;
    discovery_        = nullptr;
    roster_           = nullptr;
    pubSub_           = nullptr;
    pepExtension_     = nullptr;
    keySyncExtension_ = nullptr;
    trustManager_     = nullptr;
    omemoManager_     = nullptr;
    pendingInboundKeyRequests_.clear();
    cachedOwnOmemoBundle_.reset();
    consumedOwnPreKeyIds_.clear();
    ownBundleRepairScheduled_ = false;

    if (client_) {
        client_->disconnectFromServer();
        delete client_;
        client_ = nullptr;
    }
    delete trustStorage_;
    trustStorage_ = nullptr;
    delete omemoStorage_;
    omemoStorage_ = nullptr;

    if (acceptingWork_ && !cancelledConnectionCallbacks.isEmpty()) {
        const auto result = configurationChangedResult<XmppStatusResult>();
        for (auto &callback : cancelledConnectionCallbacks)
            callback(result);
    }
}

void XmppWorker::createClient()
{
    if (client_) {
        return;
    }

    client_ = new QXmppClient(QXmppClient::BasicExtensions, this);
    if (XmppXmlLog::isEnabled()) {
        auto *logger = new QXmppLogger(client_);
        logger->setMessageTypes(QXmppLogger::SentMessage | QXmppLogger::ReceivedMessage | QXmppLogger::WarningMessage);
        logger->setLoggingType(QXmppLogger::SignalLogging);
        connect(logger, &QXmppLogger::message, this, [](QXmppLogger::MessageType type, const QString &message) {
            const auto direction = type == QXmppLogger::SentMessage ? QStringLiteral("XMPP >>")
                : type == QXmppLogger::ReceivedMessage              ? QStringLiteral("XMPP <<")
                                                                    : QStringLiteral("XMPP !!");
            qInfo().noquote() << direction << XmppXmlLog::sanitized(message);
        });
        client_->setLogger(logger);
    }
    discovery_        = client_->findExtension<QXmppDiscoveryManager>();
    roster_           = client_->findExtension<QXmppRosterManager>();
    pubSub_           = client_->addNewExtension<QXmppPubSubManager>();
    pepExtension_     = client_->addNewExtension<XmppPepExtension>();
    keySyncExtension_ = client_->addNewExtension<XmppKeySyncExtension>();
    omemoStorage_     = new XmppOmemoStorage(config_.omemoStatePath, config_.omemoStateKey, config_.jid);
    omemoStorage_->setPreKeyRemovedHandler([this](uint32_t id) { scheduleOwnOmemoBundleRepair(id); });
    if (!trustStorage_) {
        auto *persistentTrust = new XmppPersistentTrustStorage(config_.omemoStatePath + QStringLiteral(".trust"),
                                                               config_.omemoStateKey, config_.jid);
        if (!persistentTrust->isValid())
            qWarning().noquote() << "Could not load persistent OMEMO trust:" << persistentTrust->errorString();
        trustStorage_ = persistentTrust;
    }
    trustManager_ = client_->addNewExtension<QXmppTrustManager>(trustStorage_);
    omemoManager_ = client_->addNewExtension<QXmppOmemoManager>(omemoStorage_);
    client_->setEncryptionExtension(omemoManager_);
    pepExtension_->setOwnBareJid(config_.jid);
    pepExtension_->setNodeName(config_.indexNodeName());

    connect(pepExtension_, &XmppPepExtension::payloadPublished, this, [this](const XmppEncryptedPayload &payload) {
        auto note = XmppNoteCodec::decodeIndex(payload, config_.masterKey, config_.indexNodeName());
        if (note) {
            qInfo().noquote() << "Decoded private-note PEP index item" << payload.id
                              << "revision=" << note.value.revision;
            emit remoteNotePublished(note.value);
            return;
        }

        // A single malformed, obsolete, or concurrently replaced event item
        // is not a storage-wide failure. Ask the storage layer to refresh the
        // authoritative index; list refresh isolates unreadable items and can
        // still apply every valid record.
        qWarning().noquote() << "Could not decode private-note PEP index item" << payload.id << ':'
                             << note.error.message << "-- scheduling a full index refresh";
        emit remoteNodeInvalidated();
    });
    connect(pepExtension_, &XmppPepExtension::noteRetracted, this, &XmppWorker::remoteNoteRetracted);
    connect(pepExtension_, &XmppPepExtension::nodeInvalidated, this, &XmppWorker::remoteNodeInvalidated);
    connect(pepExtension_, &XmppPepExtension::malformedItem, this, [](const QString &error) {
        // Per-item event damage is already followed by nodeInvalidated(). Do
        // not feed it into the storage-wide error-state machinery.
        qWarning().noquote() << "Private-note PEP event item ignored:" << error;
    });
    // The handlers perform synchronous-looking waits for additional PubSub IQs.
    // Do not run them from inside QXmppClient's stanza dispatch: an IQ response
    // may need to reach an extension that is later in the dispatch chain.
    connect(keySyncExtension_, &XmppKeySyncExtension::requestReceived, this, &XmppWorker::handleKeySyncRequest,
            Qt::QueuedConnection);
    connect(keySyncExtension_, &XmppKeySyncExtension::trustRequestReceived, this,
            &XmppWorker::handleKeySyncTrustRequest, Qt::QueuedConnection);

    connect(client_, &QXmppClient::connected, this, [this]() {
        prepared_ = false;
        emit connectionChanged(true);
    });
    connect(client_, &QXmppClient::disconnected, this, [this]() {
        prepared_ = false;
        emit connectionChanged(false);
    });
    connect(client_, &QXmppClient::errorOccurred, this,
            [this](const QXmppError &error) { emit backendError(errorText(error)); });
}

void XmppWorker::connectToServerAsync(StatusCallback callback)
{
    if (!acceptingWork_) {
        callback({ false,
                   false,
                   false,
                   QStringLiteral("The XMPP backend is shutting down"),
                   {},
                   XmppErrorKind::Configuration });
        return;
    }
    if (!QSslSocket::supportsSsl()) {
        const auto error = QStringLiteral("TLS support is unavailable. The Android package is missing a compatible "
                                          "OpenSSL runtime or Qt could not load its TLS backend.");
        qCritical().noquote() << error;
        callback({ false, false, false, error, {}, XmppErrorKind::Configuration });
        return;
    }
    createClient();

    if (client_->isConnected()) {
        callback({ true });
        return;
    }

    // Settings, automatic key recovery and storage initialization may all need
    // the same connection at once. QXmppClient does not accept a second
    // connectToServer() call while DNS lookup or authentication is in progress,
    // so every concurrent caller joins the current attempt instead.
    if (connectionAttempt_ && connectionAttempt_->generation == clientGeneration_ && !connectionAttempt_->finished) {
        connectionAttempt_->callbacks.append(std::move(callback));
        return;
    }

    QXmppConfiguration configuration;
    configuration.setJid(config_.jid);
    configuration.setPassword(config_.password);
    configuration.setResource(config_.resource);
    // XmppStorage owns the retry policy and reacts to system reachability.
    // Enabling QXmpp's independent reconnect loop here creates competing
    // connection attempts and bypasses permanent/transient error handling.
    configuration.setAutoReconnectionEnabled(false);
    configuration.setStreamSecurityMode(QXmppConfiguration::TLSRequired);
    configuration.setIgnoreSslErrors(false);
    if (!config_.host.isEmpty())
        configuration.setHost(config_.host);
    if (config_.port > 0)
        configuration.setPort(config_.port);

    const auto attempt  = std::make_shared<ConnectionAttempt>();
    attempt->generation = clientGeneration_;
    attempt->callbacks.append(std::move(callback));
    auto *guard = new QObject(this);
    auto *timer = new QTimer(guard);
    timer->setSingleShot(true);
    attempt->guard     = guard;
    attempt->timer     = timer;
    connectionAttempt_ = attempt;

    const auto finish = [this, attempt](XmppStatusResult result) {
        if (attempt->finished)
            return;
        attempt->finished = true;
        if (attempt->timer)
            attempt->timer->stop();
        if (attempt->guard)
            attempt->guard->deleteLater();

        const bool currentGeneration = attempt->generation == clientGeneration_;
        if (!currentGeneration)
            result = configurationChangedResult<XmppStatusResult>();
        else if (!result.ok && client_)
            client_->disconnectFromServer();

        if (connectionAttempt_ == attempt)
            connectionAttempt_.reset();

        auto callbacks = std::move(attempt->callbacks);
        for (auto &queuedCallback : callbacks)
            queuedCallback(result);
    };

    connect(client_, &QXmppClient::connected, guard, [finish]() mutable { finish({ true }); });
    connect(client_, &QXmppClient::errorOccurred, guard, [finish](const QXmppError &error) mutable {
        finish({ false, false, false, XmppWorker::errorText(error), {}, classifyXmppError(error) });
    });
    connect(client_, &QXmppClient::disconnected, guard, [finish]() mutable {
        finish({ false,
                 false,
                 false,
                 QStringLiteral("XMPP connection closed before authentication"),
                 {},
                 XmppErrorKind::Transient });
    });
    connect(timer, &QTimer::timeout, guard, [finish, timeoutMs = config_.timeoutMs]() mutable {
        finish({ false,
                 false,
                 false,
                 QStringLiteral("XMPP connection timed out after %1 ms").arg(timeoutMs),
                 {},
                 XmppErrorKind::Transient });
    });

    timer->start(qMax(1000, config_.timeoutMs));
    client_->connectToServer(configuration);
    if (client_->isConnected())
        finish({ true });
}

QCoro::Task<XmppStatusResult> XmppWorker::connectToServerTask()
{
    // Connection establishment has three competing completion signals. Keep that
    // fan-in in the existing Qt callback helper; all sequential protocol work is
    // expressed as coroutines below.
    QFutureInterface<XmppStatusResult> promise;
    auto                               future = promise.future();
    connectToServerAsync([promise](XmppStatusResult result) mutable {
        promise.reportResult(std::move(result));
        promise.reportFinished();
    });
    co_return co_await future;
}

QCoro::Task<XmppStatusResult> XmppWorker::verifyPrivateStorageSupportTask()
{
    if (!discovery_)
        co_return XmppStatusResult { false, false, false, QStringLiteral("QXmpp discovery manager is unavailable") };

    const auto pepService = QXmppUtils::jidToBareJid(config_.jid);
#if QXMPP_VERSION >= QT_VERSION_CHECK(1, 12, 0)
    auto result = co_await discovery_->info(pepService).toFuture(this);
#else
    auto result = co_await discovery_->requestDiscoInfo(pepService).toFuture(this);
#endif
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        co_return XmppStatusResult {
            false, false,
            false, QStringLiteral("Could not discover the PEP service at %1: %2").arg(pepService, errorText(*error)),
            {},    classifyXmppError(*error)
        };
    }

    const auto &info = std::get<0>(result);
    const bool  hasPepIdentity
        = std::any_of(info.identities().cbegin(), info.identities().cend(), [](const auto &identity) {
              return identity.category() == QStringLiteral("pubsub") && identity.type() == QStringLiteral("pep");
          });
    if (!hasPepIdentity) {
        co_return XmppStatusResult { false, false, false,
                                     QStringLiteral("The XMPP server does not advertise a pubsub/pep identity") };
    }
    if (!info.features().contains(QString::fromLatin1(PublishOptionsFeature))) {
        co_return XmppStatusResult { false, false, false,
                                     QStringLiteral("The server does not advertise PubSub publish-options; "
                                                    "the client will not store private notes there") };
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::verifyNodeTask(QString nodeName)
{
    auto result = co_await pubSub_->requestOwnPepNodeConfiguration(nodeName).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        co_return XmppStatusResult {
            false, false,
            false, QStringLiteral("Could not verify the private-note PEP node: %1").arg(errorText(*error)),
            {},    classifyXmppError(*error)
        };
    }
    if (!nodeConfigIsPrivate(std::get<QXmppPubSubNodeConfig>(result))) {
        co_return XmppStatusResult {
            false, false, false,
            QStringLiteral("The private-note PEP node is not persistent and private after configuration")
        };
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::ensureNodeTask(QString nodeName)
{
    auto request = co_await pubSub_->requestOwnPepNodeConfiguration(nodeName).toFuture(this);
    if (const auto *requestError = std::get_if<QXmppError>(&request)) {
        if (!isItemNotFound(*requestError)) {
            co_return XmppStatusResult {
                false,
                false,
                false,
                QStringLiteral("Could not read the private-note PEP node configuration: %1").arg(errorText(*requestError)),
                {},
                classifyXmppError(*requestError)
            };
        }

        auto created = co_await pubSub_->createOwnPepNode(nodeName, privateNodeConfig()).toFuture(this);
        if (const auto *error = resultError(created);
            error && !hasStanzaCondition(*error, QXmppStanza::Error::Conflict)) {
            co_return XmppStatusResult {
                false, false,
                false, QStringLiteral("Could not create the private-notes PEP node: %1").arg(errorText(*error)),
                {},    classifyXmppError(*error)
            };
        }
        co_return co_await verifyNodeTask(nodeName);
    }

    auto nodeConfig = std::get<QXmppPubSubNodeConfig>(request);
    if (nodeConfigIsPrivate(nodeConfig))
        co_return XmppStatusResult { true };

    nodeConfig.setAccessModel(QXmppPubSubNodeConfig::Allowlist);
    nodeConfig.setPersistItems(true);
    nodeConfig.setMaxItems(QXmppPubSubNodeConfig::Max {});
    nodeConfig.setIncludePayloads(true);
    nodeConfig.setRetractNotificationsEnabled(true);
    nodeConfig.setNodeType(QXmppPubSubNodeConfig::Leaf);
    nodeConfig.setPayloadType(PrivateNotesPubSubItem::payloadNamespace);

    auto configured = co_await pubSub_->configureOwnPepNode(nodeName, nodeConfig).toFuture(this);
    if (const auto *error = resultError(configured)) {
        co_return XmppStatusResult {
            false, false,
            false, QStringLiteral("Could not configure the private-notes PEP node: %1").arg(errorText(*error)),
            {},    classifyXmppError(*error)
        };
    }
    co_return co_await verifyNodeTask(nodeName);
}

QCoro::Task<XmppStatusResult> XmppWorker::ensureOmemoTask()
{
    if (omemoReady_)
        co_return XmppStatusResult { true };
    if (!omemoStorage_ || !omemoStorage_->isValid()) {
        co_return XmppStatusResult { false, false, false,
                                     omemoStorage_ ? omemoStorage_->errorString()
                                                   : QStringLiteral("OMEMO storage is unavailable") };
    }

    omemoManager_->setAcceptedSessionBuildingTrustLevels(QXmpp::TrustLevel::ManuallyTrusted
                                                         | QXmpp::TrustLevel::Authenticated);
    omemoManager_->setNewDeviceAutoSessionBuildingEnabled(false);
    const bool loaded = co_await omemoManager_->load().toFuture(this);
    if (!loaded) {
        const bool setup = co_await omemoManager_->setUp(config_.resource).toFuture(this);
        if (!setup) {
            co_return XmppStatusResult { false, false, false, QStringLiteral("Could not initialize the OMEMO device") };
        }
    }
    omemoReady_ = true;
    cacheOwnOmemoBundle();
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::ensureOmemoReadyTask()
{
    if (!acceptingWork_)
        co_return XmppStatusResult { false, false,
                                     false, QStringLiteral("The XMPP backend is shutting down"),
                                     {},    XmppErrorKind::Configuration };
    if (client_ && client_->isConnected() && omemoReady_)
        co_return XmppStatusResult { true };

    if (omemoReadinessAttempt_)
        co_return co_await omemoReadinessAttempt_->future();

    const auto attempt     = std::make_shared<QFutureInterface<XmppStatusResult>>();
    omemoReadinessAttempt_ = attempt;
    const auto generation  = clientGeneration_;
    const auto finish      = [this, attempt](XmppStatusResult result) {
        attempt->reportResult(result);
        attempt->reportFinished();
        if (omemoReadinessAttempt_ == attempt)
            omemoReadinessAttempt_.reset();
        return result;
    };
    const auto configurationChanged
        = [generation, this]() { return generation != clientGeneration_ || !acceptingWork_; };
    const auto cancelled = []() {
        return XmppStatusResult { false, false,
                                  false, QStringLiteral("The XMPP configuration changed during initialization"),
                                  {},    XmppErrorKind::Configuration };
    };

    auto status = co_await connectToServerTask();
    if (configurationChanged())
        co_return finish(cancelled());
    if (!status.ok)
        co_return finish(std::move(status));
    status = co_await ensureOmemoTask();
    if (configurationChanged())
        co_return finish(cancelled());
    co_return finish(std::move(status));
}

QCoro::Task<XmppStatusResult> XmppWorker::ensureReadyTask()
{
    if (!acceptingWork_)
        co_return XmppStatusResult { false, false,
                                     false, QStringLiteral("The XMPP backend is shutting down"),
                                     {},    XmppErrorKind::Configuration };
    if (prepared_)
        co_return XmppStatusResult { true };

    if (readinessAttempt_)
        co_return co_await readinessAttempt_->future();

    const auto attempt    = std::make_shared<QFutureInterface<XmppStatusResult>>();
    readinessAttempt_     = attempt;
    const auto generation = clientGeneration_;
    const auto finish     = [this, attempt](XmppStatusResult result) {
        attempt->reportResult(result);
        attempt->reportFinished();
        if (readinessAttempt_ == attempt)
            readinessAttempt_.reset();
        return result;
    };
    const auto configurationChanged
        = [generation, this]() { return generation != clientGeneration_ || !acceptingWork_; };
    const auto cancelled = []() {
        return XmppStatusResult { false, false,
                                  false, QStringLiteral("The XMPP configuration changed during initialization"),
                                  {},    XmppErrorKind::Configuration };
    };

    auto status = co_await ensureOmemoReadyTask();
    if (configurationChanged())
        co_return finish(cancelled());
    if (!status.ok)
        co_return finish(std::move(status));
    status = co_await verifyPrivateStorageSupportTask();
    if (configurationChanged())
        co_return finish(cancelled());
    if (!status.ok)
        co_return finish(std::move(status));
    status = co_await ensureNodeTask(config_.indexNodeName());
    if (configurationChanged())
        co_return finish(cancelled());
    if (!status.ok)
        co_return finish(std::move(status));
    status = co_await ensureNodeTask(config_.contentNodeName());
    if (configurationChanged())
        co_return finish(cancelled());
    if (!status.ok)
        co_return finish(std::move(status));
    prepared_ = true;
    co_return finish(XmppStatusResult { true });
}

} // namespace AnyKeep
