#include "xmppworker.h"

#include "qtnotepubsubitem.h"
#include "xmpperror.h"
#include "xmppkeysyncextension.h"
#include "xmppnotecodec.h"
#include "xmppomemopubsubitems.h"
#include "xmppomemostorage.h"
#include "xmpppepextension.h"
#include "xmpppersistenttruststorage.h"

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

namespace QtNote {

namespace {

    constexpr auto PublishOptionsFeature = "http://jabber.org/protocol/pubsub#publish-options";

    bool sameConfig(const XmppConfig &left, const XmppConfig &right)
    {
        return left.jid == right.jid && left.password == right.password && left.host == right.host
            && left.port == right.port && left.resource == right.resource && left.nodeName == right.nodeName
            && left.originId == right.originId && left.timeoutMs == right.timeoutMs && left.masterKey == right.masterKey
            && left.omemoStateKey == right.omemoStateKey && left.omemoStatePath == right.omemoStatePath;
    }

    QString sanitizedXmppLog(QString xml)
    {
        if (xml.contains(QStringLiteral("<bundle")) || xml.contains(QStringLiteral("<prekeys"))
            || xml.contains(QRegularExpression(QStringLiteral("<pk\\s")))) {
            const auto itemMatch = QRegularExpression(QStringLiteral("<item\\s[^>]*id=['\"]([^'\"]+)['\"]")).match(xml);
            const auto identityMatch = QRegularExpression(QStringLiteral("<ik\\b[^>]*>([^<]+)</ik>")).match(xml);
            QStringList details;
            if (itemMatch.hasMatch())
                details.append(QStringLiteral("device=%1").arg(itemMatch.captured(1)));
            if (identityMatch.hasMatch())
                details.append(QStringLiteral("identity-key=%1").arg(identityMatch.captured(1)));
            return details.isEmpty()
                ? QStringLiteral("[OMEMO public prekeys omitted]")
                : QStringLiteral("[OMEMO bundle %1; public prekeys omitted]").arg(details.join(QLatin1Char(' ')));
        }
        if (xml.contains(QRegularExpression(QStringLiteral("<encrypted\\b[^>]*xmlns=['\"]urn:xmpp:omemo:2['\"]")))) {
            const auto sid
                = QRegularExpression(QStringLiteral("<header\\s[^>]*sid=['\"]([^'\"]+)['\"]")).match(xml).captured(1);
            QStringList recipients;
            auto        keys = QRegularExpression(QStringLiteral("<key\\s([^>]*)>")).globalMatch(xml);
            while (keys.hasNext()) {
                const auto attributes = keys.next().captured(1);
                const auto rid
                    = QRegularExpression(QStringLiteral("rid=['\"]([^'\"]+)['\"]")).match(attributes).captured(1);
                const bool kex = QRegularExpression(QStringLiteral("kex=['\"]true['\"]")).match(attributes).hasMatch();
                if (!rid.isEmpty())
                    recipients.append(QStringLiteral("%1%2").arg(rid, kex ? QStringLiteral("/kex") : QString {}));
            }
            return QStringLiteral("[OMEMO encrypted sid=%1 recipients=%2]")
                .arg(sid.isEmpty() ? QStringLiteral("?") : sid,
                     recipients.isEmpty() ? QStringLiteral("?") : recipients.join(QLatin1Char(',')));
        }
        static const QRegularExpression sensitiveElement(
            QStringLiteral("(<(?:auth|response|encrypted)\\b[^>]*>).*?(</(?:auth|response|encrypted)>)"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        xml.replace(sensitiveElement, QStringLiteral("\\1[redacted]\\2"));
        return xml;
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
        config.setPayloadType(QtNotePubSubItem::payloadNamespace);
        return config;
    }

    QXmppPubSubPublishOptions privatePublishOptions()
    {
        QXmppPubSubPublishOptions options;
        options.setAccessModel(QXmppPubSubNodeConfig::Allowlist);
        options.setPersistItems(true);
        return options;
    }

    bool nodeConfigIsPrivate(const QXmppPubSubNodeConfig &config)
    {
        return config.accessModel() == QXmppPubSubNodeConfig::Allowlist && config.persistItems().value_or(false);
    }

    template <typename Result> const QXmppError *resultError(const Result &result)
    {
        return std::get_if<QXmppError>(&result);
    }

    bool hasStanzaCondition(const QXmppError &error, QXmppStanza::Error::Condition condition)
    {
        const auto stanzaError = error.value<QXmppStanza::Error>();
        return stanzaError && stanzaError->condition() == condition;
    }

    bool isItemNotFound(const QXmppError &error)
    {
        return hasStanzaCondition(error, QXmppStanza::Error::ItemNotFound)
            || error.description.contains(QStringLiteral("No such item"), Qt::CaseInsensitive)
            || error.description.contains(QStringLiteral("item-not-found"), Qt::CaseInsensitive);
    }

    template <typename Result> void setXmppFailure(Result &result, const QXmppError &error, const QString &message)
    {
        result.error     = message;
        result.errorKind = classifyXmppError(error);
    }

    template <typename Result> Result configurationChangedResult()
    {
        Result result;
        result.error     = QStringLiteral("The XMPP configuration changed while the operation was running");
        result.errorKind = XmppErrorKind::Configuration;
        return result;
    }

} // namespace

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
    if (qEnvironmentVariableIntValue("QTNOTE_XMPP_XML_LOG") > 0) {
        auto *logger = new QXmppLogger(client_);
        logger->setMessageTypes(QXmppLogger::SentMessage | QXmppLogger::ReceivedMessage | QXmppLogger::WarningMessage);
        logger->setLoggingType(QXmppLogger::SignalLogging);
        connect(logger, &QXmppLogger::message, this, [](QXmppLogger::MessageType type, const QString &message) {
            const auto direction = type == QXmppLogger::SentMessage ? QStringLiteral("XMPP >>")
                : type == QXmppLogger::ReceivedMessage              ? QStringLiteral("XMPP <<")
                                                                    : QStringLiteral("XMPP !!");
            qInfo().noquote() << direction << sanitizedXmppLog(message);
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
            qInfo().noquote() << "Decoded QtNote PEP index item" << payload.id << "revision=" << note.value.revision;
            emit remoteNotePublished(note.value);
            return;
        }

        // A single malformed, obsolete, or concurrently replaced event item
        // is not a storage-wide failure. Ask the storage layer to refresh the
        // authoritative index; list refresh isolates unreadable items and can
        // still apply every valid record.
        qWarning().noquote() << "Could not decode QtNote PEP index item" << payload.id << ':' << note.error.message
                             << "-- scheduling a full index refresh";
        emit remoteNodeInvalidated();
    });
    connect(pepExtension_, &XmppPepExtension::noteRetracted, this, &XmppWorker::remoteNoteRetracted);
    connect(pepExtension_, &XmppPepExtension::nodeInvalidated, this, &XmppWorker::remoteNodeInvalidated);
    connect(pepExtension_, &XmppPepExtension::malformedItem, this, [](const QString &error) {
        // Per-item event damage is already followed by nodeInvalidated(). Do
        // not feed it into the storage-wide error-state machinery.
        qWarning().noquote() << "QtNote PEP event item ignored:" << error;
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
                                                    "QtNote will not store private notes there") };
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::verifyNodeTask(QString nodeName)
{
    auto result = co_await pubSub_->requestOwnPepNodeConfiguration(nodeName).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        co_return XmppStatusResult {
            false, false,
            false, QStringLiteral("Could not verify the QtNote PEP node: %1").arg(errorText(*error)),
            {},    classifyXmppError(*error)
        };
    }
    if (!nodeConfigIsPrivate(std::get<QXmppPubSubNodeConfig>(result))) {
        co_return XmppStatusResult {
            false, false, false, QStringLiteral("The QtNote PEP node is not persistent and private after configuration")
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
                QStringLiteral("Could not read the QtNote PEP node configuration: %1").arg(errorText(*requestError)),
                {},
                classifyXmppError(*requestError)
            };
        }

        auto created = co_await pubSub_->createOwnPepNode(nodeName, privateNodeConfig()).toFuture(this);
        if (const auto *error = resultError(created);
            error && !hasStanzaCondition(*error, QXmppStanza::Error::Conflict)) {
            co_return XmppStatusResult {
                false, false,
                false, QStringLiteral("Could not create the private QtNote PEP node: %1").arg(errorText(*error)),
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
    nodeConfig.setPayloadType(QtNotePubSubItem::payloadNamespace);

    auto configured = co_await pubSub_->configureOwnPepNode(nodeName, nodeConfig).toFuture(this);
    if (const auto *error = resultError(configured)) {
        co_return XmppStatusResult {
            false, false,
            false, QStringLiteral("Could not configure the private QtNote PEP node: %1").arg(errorText(*error)),
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

void XmppWorker::cacheOwnOmemoBundle() { cacheOwnOmemoBundleTask(); }

void XmppWorker::scheduleOwnOmemoBundleRepair(uint32_t consumedPreKeyId)
{
    consumedOwnPreKeyIds_.insert(consumedPreKeyId);
    if (ownBundleRepairScheduled_)
        return;
    ownBundleRepairScheduled_ = true;
    // QXmpp publishes its incomplete in-memory bundle after returning from the pre-key storage callback.
    QTimer::singleShot(350, this, [this]() { repairOwnOmemoBundleAfterPreKeyUse(); });
}

void XmppWorker::repairOwnOmemoBundleAfterPreKeyUse(int attemptsRemaining)
{
    repairOwnOmemoBundleAfterPreKeyUseTask(attemptsRemaining);
}

void XmppWorker::probeAsync(StatusCallback callback) { ensureReadyTask().then(std::move(callback)); }

void XmppWorker::listNotesAsync(ListCallback callback) { listNotesTask().then(std::move(callback)); }

void XmppWorker::getNoteAsync(QString id, NoteCallback callback)
{
    getNoteTask(std::move(id)).then(std::move(callback));
}

void XmppWorker::saveNoteAsync(XmppRemoteNote note, NoteCallback callback)
{
    saveNoteTask(std::move(note)).then(std::move(callback));
}

void XmppWorker::deleteNoteAsync(QString id, StatusCallback callback)
{
    deleteNoteTask(std::move(id)).then(std::move(callback));
}

XmppDeviceInfo XmppWorker::ownOmemoDevice() const
{
    if (!omemoStorage_)
        return {};
    return { omemoStorage_->ownDeviceLabel(), omemoStorage_->ownDeviceId(), omemoStorage_->ownIdentityKey(),
             int(QXmpp::TrustLevel::Authenticated) };
}

void XmppWorker::ownOmemoDevicesAsync(DevicesCallback callback)
{
    ownOmemoDevicesTask().then([callback = std::move(callback)](auto result) mutable {
        callback(std::move(result.first), std::move(result.second));
    });
}

void XmppWorker::ownOmemoBundleValidAsync(StatusCallback callback)
{
    ownOmemoBundleValidTask().then(std::move(callback));
}

void XmppWorker::repairOwnOmemoDeviceAsync(StatusCallback callback)
{
    repairOwnOmemoDeviceTask().then(std::move(callback));
}

void XmppWorker::trustOwnOmemoDeviceAsync(QByteArray keyId, StatusCallback callback)
{
    trustOwnOmemoDeviceTask(std::move(keyId)).then(std::move(callback));
}

void XmppWorker::trustOwnOmemoDevicesAsync(QList<QByteArray> keyIds, StatusCallback callback)
{
    trustOwnOmemoDevicesTask(std::move(keyIds)).then(std::move(callback));
}

void XmppWorker::auditStorageKeysAsync(AuditCallback callback) { auditStorageKeysTask().then(std::move(callback)); }

void XmppWorker::rekeyStorageAsync(QList<QByteArray> keys, QByteArray canonicalKey, RekeyCallback callback)
{
    rekeyStorageTask(std::move(keys), std::move(canonicalKey)).then(std::move(callback));
}

void XmppWorker::scanObsoleteItemsAsync(CleanupCallback callback) { scanObsoleteItemsTask().then(std::move(callback)); }

void XmppWorker::deleteObsoleteItemsAsync(QStringList indexItemIds, QStringList contentItemIds,
                                          CleanupCallback callback)
{
    deleteObsoleteItemsTask(std::move(indexItemIds), std::move(contentItemIds)).then(std::move(callback));
}

void XmppWorker::approveKeySyncRequest(QString requestId) { approveKeySyncRequestTask(std::move(requestId)); }

void XmppWorker::rejectKeySyncRequest(QString requestId)
{
    pendingInboundKeyRequests_.remove(requestId);
    if (keySyncExtension_)
        keySyncExtension_->reject(requestId);
}

void XmppWorker::handleKeySyncRequest(const QString &requestId, const QString &from, const QByteArray &senderKey)
{
    handleKeySyncRequestTask(requestId, from, senderKey);
}

void XmppWorker::handleKeySyncTrustRequest(const QString &requestId, const QString &from, const QByteArray &senderKey)
{
    if (QXmppUtils::jidToBareJid(from) != QXmppUtils::jidToBareJid(config_.jid)) {
        keySyncExtension_->reject(requestId);
        return;
    }

    // This runs in reaction to an incoming stanza. Keep the complete lookup
    // asynchronous: nested event loops can receive a PubSub IQ response while
    // preventing QXmppTask's continuation from being dispatched.
    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto listTask = pubSub_->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                                  QStringLiteral("current"));
    listTask.then(this, [this, bareJid, requestId, senderKey](auto &&listResult) {
        if (const auto *requestError = std::get_if<QXmppError>(&listResult)) {
            keySyncExtension_->reject(requestId);
            emit backendError(errorText(*requestError));
            return;
        }

        struct LookupState {
            int     remaining { 0 };
            bool    matched { false };
            QString lastError;
        };
        const auto state   = std::make_shared<LookupState>();
        const auto ownId   = omemoStorage_->ownDeviceId();
        const auto devices = std::get<XmppOmemoDeviceListItem>(listResult).devices();
        for (const auto &listed : devices) {
            if (listed.id.toUInt() == ownId)
                continue;
            ++state->remaining;
            auto bundleTask = pubSub_->requestItem<XmppOmemoBundleItem>(
                bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), listed.id);
            bundleTask.then(this, [this, requestId, senderKey, listed, state](auto &&bundleResult) {
                if (state->matched)
                    return;
                if (const auto *requestError = std::get_if<QXmppError>(&bundleResult)) {
                    state->lastError = errorText(*requestError);
                } else {
                    const auto keyId = std::get<XmppOmemoBundleItem>(bundleResult).identityKey();
                    qInfo().noquote() << "Trust bootstrap OMEMO bundle: id=" << listed.id
                                      << "identity-key-size=" << keyId.size();
                    if (keyId == senderKey) {
                        state->matched = true;
                        finishKeySyncTrustRequest(requestId, senderKey);
                        return;
                    }
                }
                if (--state->remaining == 0) {
                    keySyncExtension_->reject(requestId);
                    emit backendError(state->lastError.isEmpty()
                                          ? QStringLiteral("Ignored a trust request from an unknown OMEMO device")
                                          : state->lastError);
                }
            });
        }
        if (state->remaining == 0) {
            keySyncExtension_->reject(requestId);
            emit backendError(QStringLiteral("Ignored a trust request because no other OMEMO device is published"));
        }
    });
}

void XmppWorker::finishKeySyncTrustRequest(const QString &requestId, const QByteArray &senderKey)
{
    auto trustTask = omemoManager_->trustLevel(QXmppUtils::jidToBareJid(config_.jid), senderKey);
    trustTask.then(this, [this, requestId, senderKey](QXmpp::TrustLevel trust) {
        if (trust == QXmpp::TrustLevel::ManuallyTrusted || trust == QXmpp::TrustLevel::Authenticated) {
            keySyncExtension_->replyTrustApproved(requestId);
            return;
        }
        pendingInboundKeyRequests_.insert(requestId, { senderKey, true });
        qInfo() << "Key-sync trust bootstrap needs user approval: id=" << requestId;
        emit keySyncTrustRequested(requestId, senderKey);
    });
}

QCoro::Task<XmppListResult> XmppWorker::listNotesTask()
{
    XmppListResult output;
    const auto     generation = clientGeneration_;
    const auto     ready      = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppListResult>();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }

    struct DecodeSummary {
        int     obsoleteItems { 0 };
        int     malformedItems { 0 };
        int     unsupportedItems { 0 };
        int     protectedUnreadableItems { 0 };
        int     keyMismatchItems { 0 };
        QString firstKeyMismatch;

        int skippedItems() const
        {
            return obsoleteItems + malformedItems + unsupportedItems + protectedUnreadableItems + keyMismatchItems;
        }
    } decodeSummary;
    const auto configuredKeyId = SecureEnvelope::keyId(config_.masterKey);
    const auto decodeItems     = [this, &decodeSummary, &configuredKeyId](const auto &items, XmppListResult &result) {
        for (const auto &item : items) {
            if (!item.isValid()) {
                switch (item.parseFailure()) {
                case QtNotePubSubItem::ParseFailure::ObsoleteFormat:
                    ++decodeSummary.obsoleteItems;
                    break;
                case QtNotePubSubItem::ParseFailure::UnsupportedFormat:
                    ++decodeSummary.unsupportedItems;
                    break;
                case QtNotePubSubItem::ParseFailure::Malformed:
                    ++decodeSummary.malformedItems;
                    break;
                case QtNotePubSubItem::ParseFailure::None:
                    ++decodeSummary.protectedUnreadableItems;
                    break;
                }
                qWarning().noquote() << "Skipping unreadable XMPP index item" << item.id() << ':' << item.parseError();
                continue;
            }
            if (item.payload().keyId != configuredKeyId) {
                ++decodeSummary.keyMismatchItems;
                if (decodeSummary.firstKeyMismatch.isEmpty()) {
                    decodeSummary.firstKeyMismatch
                        = QStringLiteral("Encrypted QtNote storage key mismatch (item %1, configured %2)")
                              .arg(QString::fromLatin1(item.payload().keyId.left(8).toHex()),
                                       QString::fromLatin1(configuredKeyId.left(8).toHex()));
                }
                qWarning().noquote() << "Skipping XMPP index item encrypted with another storage key" << item.id();
                continue;
            }
            auto note = XmppNoteCodec::decodeIndex(item.payload(), config_.masterKey, config_.indexNodeName());
            if (!note) {
                if (note.error.code == CryptoError::Unsupported)
                    ++decodeSummary.unsupportedItems;
                else
                    ++decodeSummary.protectedUnreadableItems;
                qWarning().noquote() << "Skipping unreadable XMPP index item" << item.id() << ':' << note.error.message;
                continue;
            }
            result.notes.append(std::move(note.value));
        }
    };

    auto idsResult = co_await pubSub_->requestOwnPepItemIds(config_.indexNodeName()).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppListResult>();
    if (std::holds_alternative<QVector<QString>>(idsResult)) {
        const auto   &ids       = std::get<QVector<QString>>(idsResult);
        constexpr int BatchSize = 50;
        for (int offset = 0; offset < ids.size(); offset += BatchSize) {
            QStringList batch;
            const int   end = qMin(offset + BatchSize, ids.size());
            for (int i = offset; i < end; ++i)
                batch.append(ids.at(i));

            auto result = co_await pubSub_
                              ->requestItems<QtNotePubSubItem>(QXmppUtils::jidToBareJid(config_.jid),
                                                               config_.indexNodeName(), batch)
                              .toFuture(this);
            if (generation != clientGeneration_)
                co_return configurationChangedResult<XmppListResult>();
            if (const auto *error = std::get_if<QXmppError>(&result)) {
                setXmppFailure(output, *error, errorText(*error));
                co_return output;
            }
            decodeItems(std::get<QXmppPubSubManager::Items<QtNotePubSubItem>>(result).items, output);
        }
        if (output.notes.isEmpty() && decodeSummary.keyMismatchItems > 0) {
            output.error     = decodeSummary.firstKeyMismatch;
            output.errorKind = XmppErrorKind::Security;
            co_return output;
        }
        if (decodeSummary.skippedItems() > 0) {
            output.partial = true;
            qWarning() << "XMPP index refresh skipped" << decodeSummary.skippedItems()
                       << "unreadable item(s): obsolete=" << decodeSummary.obsoleteItems
                       << "malformed=" << decodeSummary.malformedItems
                       << "unsupported=" << decodeSummary.unsupportedItems
                       << "other-key=" << decodeSummary.keyMismatchItems
                       << "protected=" << decodeSummary.protectedUnreadableItems;
        }
        output.ok = true;
        co_return output;
    }

    // Compatibility fallback for servers without disco item IDs.
    auto result = co_await pubSub_
                      ->requestItems<QtNotePubSubItem>(QXmppUtils::jidToBareJid(config_.jid), config_.indexNodeName())
                      .toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppListResult>();
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    const auto &items = std::get<QXmppPubSubManager::Items<QtNotePubSubItem>>(result);
    output.partial    = items.continuation.has_value();
    decodeItems(items.items, output);
    if (output.notes.isEmpty() && decodeSummary.keyMismatchItems > 0) {
        output.error     = decodeSummary.firstKeyMismatch;
        output.errorKind = XmppErrorKind::Security;
        co_return output;
    }
    if (decodeSummary.skippedItems() > 0) {
        output.partial = true;
        qWarning() << "XMPP index refresh skipped" << decodeSummary.skippedItems()
                   << "unreadable item(s): obsolete=" << decodeSummary.obsoleteItems
                   << "malformed=" << decodeSummary.malformedItems << "unsupported=" << decodeSummary.unsupportedItems
                   << "other-key=" << decodeSummary.keyMismatchItems
                   << "protected=" << decodeSummary.protectedUnreadableItems;
    }
    output.ok = true;
    co_return output;
}

QCoro::Task<XmppNoteResult> XmppWorker::requestNoteTask(QString id, quint64 clientGeneration)
{
    // Content and index live in separate PubSub nodes and therefore cannot be
    // replaced atomically. A reader may briefly observe an old index together
    // with new content while another client is publishing a note.
    constexpr int snapshotAttempts          = 3;
    const auto    inconsistentSnapshotError = QStringLiteral("QtNote content does not match its index revision");

    for (int attempt = 1; attempt <= snapshotAttempts; ++attempt) {
        XmppNoteResult output;
        auto           indexResult
            = co_await pubSub_
                  ->requestItem<QtNotePubSubItem>(QXmppUtils::jidToBareJid(config_.jid), config_.indexNodeName(), id)
                  .toFuture(this);
        if (clientGeneration != clientGeneration_)
            co_return configurationChangedResult<XmppNoteResult>();
        if (const auto *error = std::get_if<QXmppError>(&indexResult)) {
            setXmppFailure(output, *error, errorText(*error));
            output.notFound = isItemNotFound(*error);
            co_return output;
        }
        const auto &indexItem = std::get<QtNotePubSubItem>(indexResult);
        if (!indexItem.isValid()) {
            output.error = indexItem.parseError();
            co_return output;
        }
        auto index = XmppNoteCodec::decodeIndex(indexItem.payload(), config_.masterKey, config_.indexNodeName());
        if (!index) {
            output.error = index.error.message;
            co_return output;
        }

        auto contentResult
            = co_await pubSub_
                  ->requestItem<QtNotePubSubItem>(QXmppUtils::jidToBareJid(config_.jid), config_.contentNodeName(), id)
                  .toFuture(this);
        if (clientGeneration != clientGeneration_)
            co_return configurationChangedResult<XmppNoteResult>();
        if (const auto *error = std::get_if<QXmppError>(&contentResult)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        const auto &contentItem = std::get<QtNotePubSubItem>(contentResult);
        if (!contentItem.isValid()) {
            output.error = contentItem.parseError();
            co_return output;
        }
        auto content = XmppNoteCodec::decodeContent(contentItem.payload(), config_.masterKey, config_.contentNodeName(),
                                                    index.value);
        if (!content) {
            if (content.error.message == inconsistentSnapshotError) {
                qInfo().noquote() << "Conflict trace: XMPP inconsistent snapshot note=" << id << "attempt=" << attempt
                                  << "index-revision=" << index.value.revision;
                if (attempt < snapshotAttempts)
                    continue;
                output.errorKind = XmppErrorKind::Transient;
            }
            output.error = content.error.message;
            co_return output;
        }
        output.note = std::move(content.value);
        output.ok   = true;
        co_return output;
    }

    Q_UNREACHABLE();
}

QCoro::Task<XmppNoteResult> XmppWorker::getNoteTask(QString id)
{
    const auto generation = clientGeneration_;
    const auto ready      = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (!ready.ok) {
        XmppNoteResult output;
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    co_return co_await requestNoteTask(std::move(id), generation);
}

QCoro::Task<XmppNoteResult> XmppWorker::publishNoteTask(XmppRemoteNote note, quint64 clientGeneration)
{
    XmppNoteResult output;
    note.parentRevision = note.revision;
    note.revision       = newUuid();
    note.originId       = config_.originId;
    qInfo().noquote() << "Conflict trace: XMPP publish note=" << note.id << "revision=" << note.revision
                      << "parent=" << note.parentRevision << "origin=" << note.originId
                      << "generation=" << clientGeneration;
    note.modified       = QDateTime::currentDateTimeUtc();
    note.format         = QStringLiteral("markdown");
    note.contentPresent = true;

    auto contentPayload = XmppNoteCodec::encodeContent(note, config_.masterKey, config_.contentNodeName());
    auto indexPayload   = XmppNoteCodec::encodeIndex(note, config_.masterKey, config_.indexNodeName());
    if (!contentPayload || !indexPayload) {
        output.error = !contentPayload ? contentPayload.error.message : indexPayload.error.message;
        co_return output;
    }

    auto published = co_await pubSub_
                         ->publishOwnPepItem(config_.contentNodeName(), QtNotePubSubItem(contentPayload.value),
                                             privatePublishOptions())
                         .toFuture(this);
    if (clientGeneration != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (const auto *error = resultError(published)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    published = co_await pubSub_
                    ->publishOwnPepItem(config_.indexNodeName(), QtNotePubSubItem(indexPayload.value),
                                        privatePublishOptions())
                    .toFuture(this);
    if (clientGeneration != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (const auto *error = resultError(published)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    output.note = std::move(note);
    output.ok   = true;
    co_return output;
}

QCoro::Task<XmppNoteResult> XmppWorker::saveNoteTask(XmppRemoteNote note)
{
    const auto generation = clientGeneration_;
    qInfo().noquote() << "Conflict trace: XMPP save begin note=" << note.id << "local-revision=" << note.revision
                      << "local-parent=" << note.parentRevision << "local-origin=" << note.originId
                      << "generation=" << generation;
    const auto ready = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (!ready.ok) {
        XmppNoteResult output;
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    if (note.id.isEmpty()) {
        note.id = newUuid();
    } else {
        auto server = co_await requestNoteTask(note.id, generation);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppNoteResult>();
        if (!server.ok)
            co_return server;
        qInfo().noquote() << "Conflict trace: XMPP revision check note=" << note.id << "local=" << note.revision
                          << "server=" << server.note.revision << "server-parent=" << server.note.parentRevision
                          << "server-origin=" << server.note.originId;
        if (server.note.revision != note.revision) {
            qInfo().noquote() << "Conflict trace: XMPP optimistic conflict note=" << note.id
                              << "local=" << note.revision << "server=" << server.note.revision;
            XmppNoteResult conflict;
            conflict.conflict         = true;
            conflict.remoteOnConflict = std::move(server.note);
            conflict.error
                = QStringLiteral("The note was modified on another XMPP resource; the local version was not published");
            co_return conflict;
        }
    }
    co_return co_await publishNoteTask(std::move(note), generation);
}

QCoro::Task<XmppStatusResult> XmppWorker::deleteNoteTask(QString id)
{
    const auto generation = clientGeneration_;
    auto       status     = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppStatusResult>();
    if (!status.ok)
        co_return status;

    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto       result  = co_await pubSub_->retractItem(bareJid, config_.indexNodeName(), id, true).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppStatusResult>();
    if (const auto *error = resultError(result); error && !isItemNotFound(*error))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    result = co_await pubSub_->retractItem(bareJid, config_.contentNodeName(), id, true).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppStatusResult>();
    if (const auto *error = resultError(result); error && !isItemNotFound(*error))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    co_return XmppStatusResult { true };
}

QCoro::Task<std::pair<QList<XmppDeviceInfo>, QString>> XmppWorker::ownOmemoDevicesTask()
{
    auto ready = co_await ensureOmemoReadyTask();
    if (!ready.ok)
        co_return std::make_pair(QList<XmppDeviceInfo> {}, ready.error);

    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto       list    = co_await pubSub_
                    ->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                           QStringLiteral("current"))
                    .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&list))
        co_return std::make_pair(QList<XmppDeviceInfo> {}, errorText(*error));

    const auto            ownDeviceId = omemoStorage_->ownDeviceId();
    const auto            ownKey      = omemoStorage_->ownIdentityKey();
    QList<XmppDeviceInfo> devices;
    int                   missingFingerprints = 0;
    for (const auto &listed : std::get<XmppOmemoDeviceListItem>(list).devices()) {
        if (listed.id.toUInt() == ownDeviceId)
            continue;
        auto bundle
            = co_await pubSub_
                  ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), listed.id)
                  .toFuture(this);
        QByteArray keyId;
        if (const auto *error = std::get_if<QXmppError>(&bundle)) {
            qWarning().noquote() << "Could not fetch OMEMO bundle: id=" << listed.id << "label=" << listed.label
                                 << "error=" << errorText(*error);
        } else {
            keyId = std::get<XmppOmemoBundleItem>(bundle).identityKey();
        }
        if (!keyId.isEmpty() && keyId == ownKey)
            continue;
        const auto label = listed.label.isEmpty() ? QStringLiteral("OMEMO device %1").arg(listed.id)
                                                  : QStringLiteral("%1 (OMEMO %2)").arg(listed.label, listed.id);
        if (keyId.isEmpty()) {
            ++missingFingerprints;
            devices.append({ label, listed.id.toUInt(), {}, int(QXmpp::TrustLevel::Undecided) });
            continue;
        }
        const auto trust = co_await omemoManager_->trustLevel(bareJid, keyId).toFuture(this);
        devices.append({ label, listed.id.toUInt(), keyId, int(trust) });
    }
    const auto error = missingFingerprints
        ? QStringLiteral("Could not obtain the OMEMO fingerprint for %1 device(s)").arg(missingFingerprints)
        : QString {};
    co_return std::make_pair(std::move(devices), error);
}

QCoro::Task<XmppStatusResult> XmppWorker::ownOmemoBundleValidTask()
{
    auto ready = co_await ensureOmemoReadyTask();
    if (!ready.ok)
        co_return ready;
    const auto own = ownOmemoDevice();
    if (!own.deviceId || own.keyId.isEmpty())
        co_return XmppStatusResult { false, false, false, QStringLiteral("The local OMEMO device is not initialized") };
    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto       bundle  = co_await pubSub_
                      ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"),
                                                         QString::number(own.deviceId))
                      .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&bundle))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    const auto publishedKey = std::get<XmppOmemoBundleItem>(bundle).identityKey();
    if (publishedKey != own.keyId) {
        co_return XmppStatusResult { false, false, false,
                                     publishedKey.isEmpty()
                                         ? QStringLiteral("The published OMEMO bundle has no identity key")
                                         : QStringLiteral("The published OMEMO identity key does not match") };
    }
    auto list = co_await pubSub_
                    ->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                           QStringLiteral("current"))
                    .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&list))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    const auto &devices   = std::get<XmppOmemoDeviceListItem>(list).devices();
    const bool  announced = std::any_of(devices.cbegin(), devices.cend(),
                                        [&own](const auto &device) { return device.id.toUInt() == own.deviceId; });
    if (!announced)
        co_return XmppStatusResult {
            false, false, false, QStringLiteral("The local OMEMO device is missing from the published device list")
        };
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::trustOwnOmemoDeviceTask(QByteArray keyId)
{
    if (keyId.isEmpty())
        co_return XmppStatusResult { false, false, false, QStringLiteral("No OMEMO device was selected") };
    auto [devices, error] = co_await ownOmemoDevicesTask();
    const bool belongsToSelf
        = std::any_of(devices.cbegin(), devices.cend(), [&keyId](const auto &device) { return device.keyId == keyId; });
    if (!belongsToSelf)
        co_return XmppStatusResult { false, false, false,
                                     error.isEmpty() ? QStringLiteral("The OMEMO key does not belong to an own device")
                                                     : error };
    QMultiHash<QString, QByteArray> keys;
    keys.insert(QXmppUtils::jidToBareJid(config_.jid), keyId);
    co_await omemoManager_->setTrustLevel(keys, QXmpp::TrustLevel::ManuallyTrusted).toFuture(this);
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::trustOwnOmemoDevicesTask(QList<QByteArray> keyIds)
{
    for (const auto &keyId : keyIds) {
        const auto result = co_await trustOwnOmemoDeviceTask(keyId);
        if (!result.ok)
            co_return result;
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::repairOwnOmemoDeviceTask()
{
    auto ready = co_await ensureOmemoReadyTask();
    if (!ready.ok)
        co_return ready;

    const auto bareJid      = QXmppUtils::jidToBareJid(config_.jid);
    const auto oldDeviceId  = omemoStorage_->ownDeviceId();
    const auto oldDeviceKey = omemoStorage_->ownIdentityKey();
    auto       oldBundle    = co_await pubSub_
                         ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"),
                                                            QString::number(oldDeviceId))
                         .toFuture(this);
    const bool bundleValid = std::holds_alternative<XmppOmemoBundleItem>(oldBundle)
        && std::get<XmppOmemoBundleItem>(oldBundle).identityKey() == oldDeviceKey;

    auto list = co_await pubSub_
                    ->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                           QStringLiteral("current"))
                    .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&list))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    auto item    = std::get<XmppOmemoDeviceListItem>(list);
    auto devices = item.devices();

    if (bundleValid) {
        if (std::none_of(devices.cbegin(), devices.cend(),
                         [oldDeviceId](const auto &device) { return device.id.toUInt() == oldDeviceId; })) {
            devices.append({ config_.resource, QString::number(oldDeviceId), {} });
            item.setDevices(std::move(devices));
            auto published = co_await pubSub_->publishItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"), item)
                                 .toFuture(this);
            if (const auto *error = std::get_if<QXmppError>(&published))
                co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
        }
        co_return XmppStatusResult { true };
    }

    co_await omemoManager_->resetOwnDeviceLocally().toFuture(this);
    omemoReady_      = false;
    const bool setup = co_await omemoManager_->setUp(config_.resource).toFuture(this);
    if (!setup)
        co_return XmppStatusResult { false, false, false,
                                     QStringLiteral("QXmpp could not create a replacement OMEMO device") };
    omemoReady_ = true;
    devices.removeIf([oldDeviceId](const auto &device) { return device.id.toUInt() == oldDeviceId; });
    const auto newDeviceId = omemoStorage_->ownDeviceId();
    if (std::none_of(devices.cbegin(), devices.cend(),
                     [newDeviceId](const auto &device) { return device.id.toUInt() == newDeviceId; }))
        devices.append({ config_.resource, QString::number(newDeviceId), {} });
    item.setDevices(std::move(devices));
    auto published
        = co_await pubSub_->publishItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"), item).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&published))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    if (oldDeviceId) {
        auto retracted
            = co_await pubSub_
                  ->retractItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), QString::number(oldDeviceId))
                  .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&retracted))
            qWarning().noquote() << "Could not retract old OMEMO bundle" << oldDeviceId << errorText(*error);
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<std::pair<QStringList, QString>> XmppWorker::onlineQtNoteResourcesTask()
{
    if (!roster_ || !discovery_)
        co_return std::make_pair(QStringList {}, QStringLiteral("XMPP resource discovery is unavailable"));
    const auto  bareJid     = QXmppUtils::jidToBareJid(config_.jid);
    const auto  ownResource = client_->configuration().resource();
    QStringList resources;
    QStringList failures;
    for (const auto &resource : roster_->getResources(bareJid)) {
        if (resource.isEmpty() || resource == ownResource)
            continue;
        const auto fullJid = bareJid + QLatin1Char('/') + resource;
#if QXMPP_VERSION >= QT_VERSION_CHECK(1, 12, 0)
        auto info = co_await discovery_->info(fullJid).toFuture(this);
#else
        auto info = co_await discovery_->requestDiscoInfo(fullJid).toFuture(this);
#endif
        if (const auto *error = std::get_if<QXmppError>(&info)) {
            failures.append(QStringLiteral("%1: %2").arg(fullJid, errorText(*error)));
            continue;
        }
        if (std::get<0>(info).features().contains(XmppKeySyncExtension::feature))
            resources.append(resource);
    }
    co_return std::make_pair(std::move(resources), failures.join(QLatin1Char('\n')));
}

QCoro::Task<XmppKeyAuditResult> XmppWorker::auditStorageKeysTask()
{
    XmppKeyAuditResult output;
    auto               ready = co_await ensureOmemoReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    if (client_->encryptionExtension() != omemoManager_ || !keySyncExtension_) {
        output.error = QStringLiteral("OMEMO key synchronization is unavailable");
        co_return output;
    }
    if (config_.masterKey.size() == SecureEnvelope::MasterKeySize)
        output.candidates.append({ client_->configuration().resource(), config_.masterKey,
                                   SecureEnvelope::keyId(config_.masterKey), 0, true });

    const auto bareJid               = QXmppUtils::jidToBareJid(config_.jid);
    auto [resources, discoveryError] = co_await onlineQtNoteResourcesTask();
    QStringList errors;
    if (!discoveryError.isEmpty())
        errors.append(discoveryError);
    co_await omemoStorage_->resetAllSessions().toFuture(this);

    for (const auto &name : resources) {
        const auto resource = bareJid + QLatin1Char('/') + name;
        const auto trustId  = newUuid();
        auto       trustResult
            = co_await client_
                  ->sendIq(keySyncExtension_->makeTrustRequest(resource, trustId, omemoStorage_->ownIdentityKey()), {})
                  .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&trustResult)) {
            errors.append(QStringLiteral("%1: OMEMO trust bootstrap failed: %2").arg(resource, errorText(*error)));
            continue;
        }
        if (!XmppKeySyncExtension::isTrustApproved(std::get<QDomElement>(trustResult), trustId)) {
            errors.append(QStringLiteral("%1: invalid OMEMO trust bootstrap response").arg(resource));
            continue;
        }

        auto requestId = newUuid();
        auto result
            = co_await client_->sendSensitiveIq(keySyncExtension_->makeRequest(resource, requestId), {}).toFuture(this);
        if (std::holds_alternative<QXmppError>(result)) {
            requestId = newUuid();
            result    = co_await client_->sendSensitiveIq(keySyncExtension_->makeRequest(resource, requestId), {})
                         .toFuture(this);
        }
        if (const auto *error = std::get_if<QXmppError>(&result)) {
            errors.append(QStringLiteral("%1: %2").arg(resource, errorText(*error)));
            continue;
        }
        const auto encoded = XmppKeySyncExtension::responseRecoveryKey(std::get<QDomElement>(result), requestId);
        auto       key     = SecureEnvelope::decodeRecoveryKey(encoded);
        if (!key) {
            errors.append(QStringLiteral("%1: invalid storage key response").arg(resource));
            continue;
        }
        const auto keyId    = SecureEnvelope::keyId(key.value);
        auto       existing = std::find_if(output.candidates.begin(), output.candidates.end(),
                                           [&keyId](const auto &candidate) { return candidate.keyId == keyId; });
        if (existing == output.candidates.end())
            output.candidates.append({ resource, key.value, keyId, 0, false });
        else if (!existing->resource.split(QStringLiteral(", ")).contains(resource))
            existing->resource += QStringLiteral(", ") + resource;
    }

    auto idsResult = co_await pubSub_->requestOwnPepItemIds(config_.indexNodeName()).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&idsResult)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    const auto ids          = std::get<QVector<QString>>(idsResult);
    output.totalIndexItems  = ids.size();
    constexpr int BatchSize = 50;
    for (int offset = 0; offset < ids.size(); offset += BatchSize) {
        QStringList batch;
        for (int i = offset; i < qMin(offset + BatchSize, ids.size()); ++i)
            batch.append(ids.at(i));
        auto items
            = co_await pubSub_->requestItems<QtNotePubSubItem>(bareJid, config_.indexNodeName(), batch).toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&items)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        for (const auto &item : std::get<QXmppPubSubManager::Items<QtNotePubSubItem>>(items).items) {
            if (!item.isValid()) {
                qWarning().noquote() << "Skipping unreadable XMPP index item during storage-key audit" << item.id()
                                     << ':' << item.parseError();
                continue;
            }
            const auto keyId     = item.payload().keyId;
            auto       candidate = std::find_if(output.candidates.begin(), output.candidates.end(),
                                                [&keyId](const auto &entry) { return entry.keyId == keyId; });
            if (candidate == output.candidates.end())
                output.candidates.append({ {}, {}, keyId, 1, false });
            else
                ++candidate->indexItemCount;
        }
    }
    output.ok = true;
    if (!errors.isEmpty())
        output.error = QStringLiteral("Some QtNote resources failed:\n%1").arg(errors.join('\n'));
    co_return output;
}

QCoro::Task<XmppCleanupResult> XmppWorker::scanNodeForObsoleteItemsTask(QString                    nodeName,
                                                                        XmppEncryptedPayload::Kind expectedKind)
{
    XmppCleanupResult output;
    const auto        generation = clientGeneration_;
    auto              idsResult  = co_await pubSub_->requestOwnPepItemIds(nodeName).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppCleanupResult>();
    if (const auto *error = std::get_if<QXmppError>(&idsResult)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }

    const auto    ids       = std::get<QVector<QString>>(idsResult);
    const auto    bareJid   = QXmppUtils::jidToBareJid(config_.jid);
    constexpr int BatchSize = 50;
    for (int offset = 0; offset < ids.size(); offset += BatchSize) {
        QStringList batch;
        for (int i = offset; i < qMin(offset + BatchSize, ids.size()); ++i)
            batch.append(ids.at(i));
        auto items = co_await pubSub_->requestItems<QtNotePubSubItem>(bareJid, nodeName, batch).toFuture(this);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppCleanupResult>();
        if (const auto *error = std::get_if<QXmppError>(&items)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        for (const auto &item : std::get<QXmppPubSubManager::Items<QtNotePubSubItem>>(items).items) {
            bool obsolete = false;
            if (!item.isValid()) {
                obsolete = item.isObsoleteOrMalformed();
            } else {
                const auto decodeError
                    = XmppNoteCodec::validatePayload(item.payload(), expectedKind, config_.masterKey, nodeName);
                if (!decodeError)
                    ++output.validItems;
                else if (decodeError.code == CryptoError::Corrupt)
                    obsolete = true;
                else
                    ++output.protectedUnreadableItems;
            }
            if (obsolete) {
                if (expectedKind == XmppEncryptedPayload::Index)
                    output.obsoleteIndexItemIds.append(item.id());
                else
                    output.obsoleteContentItemIds.append(item.id());
            } else if (!item.isValid()) {
                ++output.protectedUnreadableItems;
            }
        }
    }
    output.ok = true;
    co_return output;
}

QCoro::Task<XmppCleanupResult> XmppWorker::scanObsoleteItemsTask()
{
    XmppCleanupResult output;
    const auto        ready = co_await ensureReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }

    auto index = co_await scanNodeForObsoleteItemsTask(config_.indexNodeName(), XmppEncryptedPayload::Index);
    if (!index.ok)
        co_return index;
    auto content = co_await scanNodeForObsoleteItemsTask(config_.contentNodeName(), XmppEncryptedPayload::Content);
    if (!content.ok)
        co_return content;

    output.ok                       = true;
    output.obsoleteIndexItemIds     = std::move(index.obsoleteIndexItemIds);
    output.obsoleteContentItemIds   = std::move(content.obsoleteContentItemIds);
    output.protectedUnreadableItems = index.protectedUnreadableItems + content.protectedUnreadableItems;
    output.validItems               = index.validItems + content.validItems;
    co_return output;
}

QCoro::Task<XmppCleanupResult> XmppWorker::deleteObsoleteItemsTask(QStringList indexItemIds, QStringList contentItemIds)
{
    XmppCleanupResult output;
    const auto        ready = co_await ensureReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }

    struct Candidate {
        QString                    node;
        QString                    id;
        XmppEncryptedPayload::Kind kind;
    };
    QList<Candidate> candidates;
    candidates.reserve(indexItemIds.size() + contentItemIds.size());
    for (const auto &id : std::as_const(indexItemIds))
        candidates.append({ config_.indexNodeName(), id, XmppEncryptedPayload::Index });
    for (const auto &id : std::as_const(contentItemIds))
        candidates.append({ config_.contentNodeName(), id, XmppEncryptedPayload::Content });

    const auto bareJid    = QXmppUtils::jidToBareJid(config_.jid);
    const auto generation = clientGeneration_;
    for (const auto &candidate : std::as_const(candidates)) {
        auto current
            = co_await pubSub_->requestItem<QtNotePubSubItem>(bareJid, candidate.node, candidate.id).toFuture(this);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppCleanupResult>();
        if (const auto *error = std::get_if<QXmppError>(&current)) {
            if (isItemNotFound(*error))
                continue;
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }

        const auto &item      = std::get<QtNotePubSubItem>(current);
        bool        removable = !item.isValid() && item.isObsoleteOrMalformed();
        if (item.isValid()) {
            const auto decodeError
                = XmppNoteCodec::validatePayload(item.payload(), candidate.kind, config_.masterKey, candidate.node);
            removable = decodeError.code == CryptoError::Corrupt;
        }
        if (!removable) {
            ++output.protectedUnreadableItems;
            continue;
        }

        auto retracted = co_await pubSub_->retractItem(bareJid, candidate.node, candidate.id, true).toFuture(this);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppCleanupResult>();
        if (const auto *error = std::get_if<QXmppError>(&retracted)) {
            if (isItemNotFound(*error))
                continue;
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        ++output.removedItems;
    }
    output.ok = true;
    if (output.removedItems)
        emit remoteNodeInvalidated();
    co_return output;
}

QCoro::Task<XmppRekeyResult> XmppWorker::rekeyStorageTask(QList<QByteArray> keys, QByteArray canonicalKey)
{
    XmppRekeyResult output;
    const auto      ready = co_await ensureReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    if (canonicalKey.size() != SecureEnvelope::MasterKeySize) {
        output.error = QStringLiteral("The selected canonical XMPP storage key is invalid");
        co_return output;
    }
    QHash<QByteArray, QByteArray> keyring;
    for (const auto &key : keys) {
        if (key.size() == SecureEnvelope::MasterKeySize)
            keyring.insert(SecureEnvelope::keyId(key), key);
    }
    keyring.insert(SecureEnvelope::keyId(canonicalKey), canonicalKey);

    auto idsResult = co_await pubSub_->requestOwnPepItemIds(config_.indexNodeName()).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&idsResult)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    const auto ids     = std::get<QVector<QString>>(idsResult);
    output.total       = ids.size();
    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    for (const auto &id : ids) {
        auto indexResult
            = co_await pubSub_->requestItem<QtNotePubSubItem>(bareJid, config_.indexNodeName(), id).toFuture(this);
        auto contentResult
            = co_await pubSub_->requestItem<QtNotePubSubItem>(bareJid, config_.contentNodeName(), id).toFuture(this);
        const auto *indexError   = std::get_if<QXmppError>(&indexResult);
        const auto *contentError = std::get_if<QXmppError>(&contentResult);
        if (indexError || contentError) {
            const auto &error = indexError ? *indexError : *contentError;
            setXmppFailure(output, error, errorText(error));
            co_return output;
        }
        const auto &indexItem   = std::get<QtNotePubSubItem>(indexResult);
        const auto &contentItem = std::get<QtNotePubSubItem>(contentResult);
        if (!indexItem.isValid() || !contentItem.isValid()) {
            qWarning() << "Skipping unreadable XMPP note during rekey:" << id
                       << (!indexItem.isValid() ? indexItem.parseError() : contentItem.parseError());
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        const auto indexKey   = keyring.value(indexItem.payload().keyId);
        const auto contentKey = keyring.value(contentItem.payload().keyId);
        if (indexKey.isEmpty() || contentKey.isEmpty()) {
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        auto note = XmppNoteCodec::decodeIndex(indexItem.payload(), indexKey, config_.indexNodeName());
        if (!note) {
            qWarning() << "Skipping inaccessible XMPP note index during rekey:" << id << note.error.message;
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        auto content
            = XmppNoteCodec::decodeContent(contentItem.payload(), contentKey, config_.contentNodeName(), note.value);
        if (!content) {
            qWarning() << "Skipping inaccessible XMPP note content during rekey:" << id << content.error.message;
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        note.value      = std::move(content.value);
        auto newContent = XmppNoteCodec::encodeContent(note.value, canonicalKey, config_.contentNodeName());
        auto newIndex   = XmppNoteCodec::encodeIndex(note.value, canonicalKey, config_.indexNodeName());
        if (!newContent || !newIndex) {
            output.error = !newContent ? newContent.error.message : newIndex.error.message;
            co_return output;
        }
        auto published = co_await pubSub_
                             ->publishOwnPepItem(config_.contentNodeName(), QtNotePubSubItem(newContent.value),
                                                 privatePublishOptions())
                             .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&published)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        published = co_await pubSub_
                        ->publishOwnPepItem(config_.indexNodeName(), QtNotePubSubItem(newIndex.value),
                                            privatePublishOptions())
                        .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&published)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        ++output.migrated;
    }
    output.ok = output.inaccessibleNoteIds.isEmpty();
    if (!output.ok)
        output.error = QStringLiteral("Some notes use storage keys that are not available");
    co_return output;
}

QCoro::Task<> XmppWorker::approveKeySyncRequestTask(QString requestId)
{
    const auto pending = pendingInboundKeyRequests_.take(requestId);
    if (pending.senderKey.isEmpty()) {
        qWarning() << "Key-sync approval has no pending request: id=" << requestId;
        co_return;
    }
    if (pending.trustBootstrap) {
        QMultiHash<QString, QByteArray> keys;
        keys.insert(QXmppUtils::jidToBareJid(config_.jid), pending.senderKey);
        co_await omemoManager_->setTrustLevel(keys, QXmpp::TrustLevel::ManuallyTrusted).toFuture(this);
        keySyncExtension_->replyTrustApproved(requestId);
        co_return;
    }
    const auto trusted = co_await trustOwnOmemoDeviceTask(pending.senderKey);
    if (!trusted.ok) {
        emit backendError(trusted.error);
        co_return;
    }
    if (config_.masterKey.size() == SecureEnvelope::MasterKeySize)
        keySyncExtension_->replyWithKey(requestId, SecureEnvelope::encodeRecoveryKey(config_.masterKey));
}

QCoro::Task<> XmppWorker::handleKeySyncRequestTask(QString requestId, QString from, QByteArray senderKey)
{
    qInfo().noquote() << "Handling key-sync request: id=" << requestId << "from=" << from
                      << "sender-key-size=" << senderKey.size();
    if (QXmppUtils::jidToBareJid(from) != QXmppUtils::jidToBareJid(config_.jid)) {
        keySyncExtension_->reject(requestId);
        co_return;
    }
    const auto trust
        = co_await omemoManager_->trustLevel(QXmppUtils::jidToBareJid(config_.jid), senderKey).toFuture(this);
    if (trust != QXmpp::TrustLevel::ManuallyTrusted && trust != QXmpp::TrustLevel::Authenticated) {
        auto [devices, error] = co_await ownOmemoDevicesTask();
        const bool ownDevice  = std::any_of(devices.cbegin(), devices.cend(),
                                            [&senderKey](const auto &device) { return device.keyId == senderKey; });
        if (!ownDevice) {
            keySyncExtension_->reject(requestId);
            emit backendError(
                error.isEmpty() ? QStringLiteral("Ignored a storage-key request from an unknown OMEMO device") : error);
            co_return;
        }
        pendingInboundKeyRequests_.insert(requestId, { senderKey, false });
        emit keySyncTrustRequested(requestId, senderKey);
        co_return;
    }
    if (config_.masterKey.size() != SecureEnvelope::MasterKeySize) {
        keySyncExtension_->reject(requestId);
        co_return;
    }
    keySyncExtension_->replyWithKey(requestId, SecureEnvelope::encodeRecoveryKey(config_.masterKey));
}

QCoro::Task<> XmppWorker::cacheOwnOmemoBundleTask()
{
    if (!pubSub_ || !omemoStorage_ || !omemoStorage_->ownDeviceId())
        co_return;
    const auto deviceId = omemoStorage_->ownDeviceId();
    auto       result
        = co_await pubSub_
              ->requestItem<XmppOmemoBundleItem>(QXmppUtils::jidToBareJid(config_.jid),
                                                 QStringLiteral("urn:xmpp:omemo:2:bundles"), QString::number(deviceId))
              .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        qWarning().noquote() << "Could not cache own OMEMO bundle:" << errorText(*error);
        co_return;
    }
    auto bundle = std::get<XmppOmemoBundleItem>(std::move(result));
    if (!omemoStorage_ || deviceId != omemoStorage_->ownDeviceId()
        || bundle.identityKey() != omemoStorage_->ownIdentityKey()) {
        qWarning() << "Own OMEMO bundle was not cached because its identity key is invalid";
        co_return;
    }
    cachedOwnOmemoBundle_ = std::move(bundle);
}

QCoro::Task<> XmppWorker::repairOwnOmemoBundleAfterPreKeyUseTask(int attemptsRemaining)
{
    ownBundleRepairScheduled_ = false;
    if (!cachedOwnOmemoBundle_ || !pubSub_ || !client_ || !client_->isConnected())
        co_return;
    const auto bareJid  = QXmppUtils::jidToBareJid(config_.jid);
    const auto deviceId = omemoStorage_->ownDeviceId();
    auto       result   = co_await pubSub_
                      ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"),
                                                         QString::number(deviceId))
                      .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        qWarning().noquote() << "Could not inspect own OMEMO bundle after pre-key use:" << errorText(*error);
        co_return;
    }
    auto published = std::get<XmppOmemoBundleItem>(std::move(result));
    if (published.identityKey() == omemoStorage_->ownIdentityKey()) {
        bool containsConsumed = false;
        for (const auto id : consumedOwnPreKeyIds_)
            containsConsumed |= published.publicPreKeys().contains(id);
        if (containsConsumed && attemptsRemaining > 0) {
            ownBundleRepairScheduled_ = true;
            QTimer::singleShot(
                250, this, [this, attemptsRemaining]() { repairOwnOmemoBundleAfterPreKeyUse(attemptsRemaining - 1); });
            co_return;
        }
        cachedOwnOmemoBundle_ = std::move(published);
        consumedOwnPreKeyIds_.clear();
        co_return;
    }
    if (!published.identityKey().isEmpty()) {
        qWarning() << "Refusing to repair own OMEMO bundle with a mismatching identity key";
        co_return;
    }
    auto repaired = published.repairedFrom(*cachedOwnOmemoBundle_, consumedOwnPreKeyIds_);
    auto outcome
        = co_await pubSub_->publishItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), repaired).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&outcome)) {
        qWarning().noquote() << "Could not repair QXmpp's incomplete own OMEMO bundle:" << errorText(*error);
        co_return;
    }
    cachedOwnOmemoBundle_ = std::move(repaired);
    consumedOwnPreKeyIds_.clear();
}

QString XmppWorker::newUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString XmppWorker::errorText(const QXmppError &error)
{
    return error.description.isEmpty() ? QStringLiteral("Unknown XMPP error") : error.description;
}

} // namespace QtNote
