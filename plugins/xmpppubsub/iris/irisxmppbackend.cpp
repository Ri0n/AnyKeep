#include "irisxmppbackend.h"

#include "iriskeysynctask.h"
#include "irisomemostorage.h"
#include "iristruststorage.h"
#include "secureenvelope.h"
#include "xmppnotecodec.h"
#include "xmpppayloadxml.h"
#include "xmppxmllog.h"

#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_discoinfotask.h>
#include <iris/xmpp_encryption.h>
#include <iris/xmpp_features.h>
#include <iris/xmpp_omemo.h>
#include <iris/xmpp_pubsub.h>
#include <iris/xmpp_pubsubitem.h>
#include <iris/xmpp_resourcelist.h>
#include <iris/xmpp_status.h>
#include <iris/xmpp_tasks.h>

#include <QCryptographicHash>
#include <QtCrypto>

#include <QDomDocument>
#include <QLoggingCategory>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <utility>

Q_LOGGING_CATEGORY(lcIrisXmpp, "anykeep.xmpp.iris")

namespace AnyKeep {
namespace {

    constexpr auto PublishOptionsFeature = "http://jabber.org/protocol/pubsub#publish-options";
    constexpr int  BatchSize             = 50;

    XMPP::Jid bareJid(const XmppConfig &config) { return XMPP::Jid(config.jid).withResource({}); }

    XMPP::PubSubOptions privateNodeOptions()
    {
        return {
            { QStringLiteral("pubsub#access_model"), { QStringLiteral("whitelist") } },
            { QStringLiteral("pubsub#persist_items"), { QStringLiteral("1") } },
            { QStringLiteral("pubsub#max_items"), { QStringLiteral("max") } },
            { QStringLiteral("pubsub#deliver_payloads"), { QStringLiteral("1") } },
            { QStringLiteral("pubsub#notify_retract"), { QStringLiteral("1") } },
            { QStringLiteral("pubsub#node_type"), { QStringLiteral("leaf") } },
            { QStringLiteral("pubsub#type"), { XmppPayloadXml::payloadNamespace } },
        };
    }

    XMPP::PubSubOptions privatePublishOptions()
    {
        return { { QStringLiteral("pubsub#access_model"), { QStringLiteral("whitelist") } },
                 { QStringLiteral("pubsub#persist_items"), { QStringLiteral("1") } } };
    }

    bool optionTrue(const XMPP::PubSubOptions &options, const QString &key)
    {
        const auto values = options.value(key);
        if (values.isEmpty())
            return false;
        const auto value = values.constFirst().trimmed().toLower();
        return value == QStringLiteral("1") || value == QStringLiteral("true");
    }

    bool nodeConfigIsPrivate(const XMPP::PubSubOptions &options)
    {
        const auto access = options.value(QStringLiteral("pubsub#access_model"));
        return !access.isEmpty() && access.constFirst() == QStringLiteral("whitelist")
            && optionTrue(options, QStringLiteral("pubsub#persist_items"));
    }

    bool isItemNotFound(const XMPP::Task *task)
    {
        return task && !task->success() && task->error().condition == XMPP::Stanza::Error::ErrorCond::ItemNotFound;
    }

    bool isConflict(const XMPP::Task *task)
    {
        return task && !task->success() && task->error().condition == XMPP::Stanza::Error::ErrorCond::Conflict;
    }

    XmppTrustLevel toBackendTrust(XMPP::EncryptionTrustLevel trust)
    {
        switch (trust) {
        case XMPP::EncryptionTrustLevel::AutomaticallyTrusted:
            return XmppTrustLevel::AutomaticallyTrusted;
        case XMPP::EncryptionTrustLevel::ManuallyTrusted:
            return XmppTrustLevel::ManuallyTrusted;
        case XMPP::EncryptionTrustLevel::Authenticated:
            return XmppTrustLevel::Authenticated;
        case XMPP::EncryptionTrustLevel::Distrusted:
            return XmppTrustLevel::Distrusted;
        case XMPP::EncryptionTrustLevel::Undecided:
            return XmppTrustLevel::Undecided;
        }
        return XmppTrustLevel::Undecided;
    }

    bool trustedForRecovery(XMPP::EncryptionTrustLevel trust)
    {
        return trust == XMPP::EncryptionTrustLevel::ManuallyTrusted
            || trust == XMPP::EncryptionTrustLevel::Authenticated;
    }

    template <typename TaskType, typename Callback>
    void runIrisTask(TaskType *task, QObject *context, int timeoutMs, Callback callback)
    {
        if (!task) {
            callback(static_cast<TaskType *>(nullptr));
            return;
        }
        task->setTimeout(qMax(1, (timeoutMs + 999) / 1000));
        QObject::connect(task, &XMPP::Task::finished, context,
                         [task, callback = std::move(callback)]() mutable { callback(task); });
        task->go(true);
    }

    template <typename Callback> void runEncryptionJob(XMPP::EncryptionJob *job, QObject *context, Callback callback)
    {
        if (!job) {
            callback(static_cast<XMPP::EncryptionJob *>(nullptr));
            return;
        }
        if (job->isFinished()) {
            QTimer::singleShot(0, context, [job, callback = std::move(callback)]() mutable {
                callback(job);
                job->deleteLater();
            });
            return;
        }
        QObject::connect(job, &XMPP::EncryptionJob::finished, context, [job, callback = std::move(callback)]() mutable {
            callback(job);
            job->deleteLater();
        });
    }

    XmppStatusResult encryptionFailure(const XMPP::EncryptionJob *job, const QString &fallback)
    {
        XmppStatusResult result;
        result.error     = job && !job->errorString().isEmpty() ? job->errorString() : fallback;
        result.errorKind = XmppErrorKind::Security;
        if (job
            && (job->error() == XMPP::EncryptionJob::Error::NetworkError
                || job->error() == XMPP::EncryptionJob::Error::Cancelled))
            result.errorKind = XmppErrorKind::Transient;
        return result;
    }

    XMPP::PubSubItem pubSubItem(const XmppEncryptedPayload &payload)
    {
        QDomDocument document;
        auto         element = XmppPayloadXml::serialize(document, payload);
        document.appendChild(element);
        return XMPP::PubSubItem(payload.id, document.documentElement());
    }

    XmppPayloadParseResult parsePubSubItem(const XMPP::PubSubItem &item)
    {
        return XmppPayloadXml::parse(item.id(), item.payload());
    }

    QString firstTaskError(const XMPP::Task *task)
    {
        if (!task)
            return QStringLiteral("Iris could not start the XMPP operation");
        if (!task->statusString().isEmpty())
            return task->statusString();
        const auto description = task->error().description();
        if (!description.second.isEmpty())
            return description.second;
        if (!task->error().text.isEmpty())
            return task->error().text;
        return QStringLiteral("Unknown XMPP error");
    }

} // namespace

struct IrisXmppBackend::ConnectionAttempt {
    bool                  finished { false };
    quint64               generation { 0 };
    QPointer<QObject>     guard;
    QPointer<QTimer>      timer;
    QList<StatusCallback> callbacks;
};

struct IrisXmppBackend::ReadyAttempt {
    bool                  finished { false };
    quint64               generation { 0 };
    QList<StatusCallback> callbacks;
};

IrisXmppBackend::IrisXmppBackend(QObject *parent) : XmppBackend(parent) { qRegisterMetaType<XmppRemoteNote>(); }

IrisXmppBackend::~IrisXmppBackend()
{
    acceptingWork_ = false;
    resetClient();
}

void IrisXmppBackend::start() { acceptingWork_ = true; }

bool IrisXmppBackend::sameConfig(const XmppConfig &left, const XmppConfig &right)
{
    return left.jid == right.jid && left.password == right.password && left.host == right.host
        && left.port == right.port && left.resource == right.resource && left.nodeName == right.nodeName
        && left.originId == right.originId && left.timeoutMs == right.timeoutMs && left.masterKey == right.masterKey
        && left.omemoStateKey == right.omemoStateKey && left.omemoStatePath == right.omemoStatePath;
}

void IrisXmppBackend::setConfig(const XmppConfig &config)
{
    if (sameConfig(config_, config))
        return;
    config_ = config;
    resetClient();
}

void IrisXmppBackend::shutdown()
{
    acceptingWork_ = false;
    resetClient();
}

XmppStatusResult IrisXmppBackend::cancelledResult() const
{
    return { false, false,
             false, QStringLiteral("The XMPP configuration changed during initialization"),
             {},    XmppErrorKind::Configuration };
}

void IrisXmppBackend::resetClient()
{
    ++generation_;
    connected_  = false;
    omemoReady_ = false;
    prepared_   = false;

    auto cancelAttempt = [this](auto &attempt) {
        if (!attempt)
            return;
        auto old = std::exchange(attempt, {});
        if (old->finished)
            return;
        old->finished     = true;
        const auto result = cancelledResult();
        for (auto &callback : old->callbacks)
            callback(result);
    };
    cancelAttempt(connectionAttempt_);
    cancelAttempt(omemoAttempt_);
    cancelAttempt(readyAttempt_);

    destroyClientObjects();
}

void IrisXmppBackend::destroyClientObjects()
{
    pendingInboundKeyRequests_.clear();
    keySyncTask_ = nullptr;
    pubSub_      = nullptr;
    omemo_       = nullptr;

    if (client_) {
        client_->close(true);
        delete client_;
        client_ = nullptr;
    }
    delete stream_;
    stream_ = nullptr;
    delete tlsHandler_;
    tlsHandler_ = nullptr;
    delete connector_;
    connector_ = nullptr;
    delete trustStorage_;
    trustStorage_ = nullptr;
    delete omemoStorage_;
    omemoStorage_        = nullptr;
    freshClientRequired_ = false;
}

void IrisXmppBackend::markDisconnected()
{
    const bool wasConnected = connected_;
    connected_              = false;
    omemoReady_             = false;
    prepared_               = false;
    freshClientRequired_    = client_ != nullptr;
    if (wasConnected)
        emit connectionChanged(false);
}

void IrisXmppBackend::createClient()
{
    if (client_)
        return;

    connector_ = new XMPP::AdvancedConnector(this);
    if (!config_.host.isEmpty() || config_.port > 0) {
        const auto explicitHost = config_.host.isEmpty() ? XMPP::Jid(config_.jid).domain() : config_.host;
        connector_->setOptHostPort(explicitHost, config_.port > 0 ? quint16(config_.port) : quint16(5222));
    }
    connector_->setOptSSL(false);

    tlsHandler_ = XMPP::QCATLSHandler::createOwned(this);
    // QCA::TLS starts with an empty trust collection.  Match Psi's TLS setup
    // by loading the platform trust store before the handshake.  systemStore()
    // starts the default QCA keystore provider and waits until it is ready.
    tlsHandler_->tls()->setTrustedCertificates(QCA::systemStore());
    tlsHandler_->setXMPPCertCheck(true);
    stream_ = new XMPP::ClientStream(connector_, tlsHandler_, this);
    stream_->setAllowPlain(XMPP::ClientStream::AllowPlainOverTLS);

    client_ = new XMPP::Client(this);
    // PEP implicit subscriptions are driven by XEP-0115 entity capabilities.
    // Iris only puts a <c/> element into presence when a caps node is configured.
    client_->setCaps(XMPP::CapsSpec(QStringLiteral("https://anykeep.net"), QCryptographicHash::Sha1));
    if (XmppXmlLog::isEnabled()) {
        connect(client_, &XMPP::Client::xmlIncoming, this,
                [](const QString &xml) { qInfo().noquote() << "XMPP <<" << XmppXmlLog::sanitized(xml); });
        connect(client_, &XMPP::Client::xmlOutgoing, this,
                [](const QString &xml) { qInfo().noquote() << "XMPP >>" << XmppXmlLog::sanitized(xml); });
    }
    pubSub_ = client_->pubSubManager();

    const auto irisStatePath = config_.omemoStatePath + QStringLiteral(".iris");
    omemoStorage_            = new IrisOmemoStorage(irisStatePath, config_.omemoStateKey, config_.jid);
    trustStorage_ = new IrisTrustStorage(irisStatePath + QStringLiteral(".trust"), config_.omemoStateKey, config_.jid);
    omemo_        = new XMPP::OmemoEncryption(client_, omemoStorage_, trustStorage_, client_);
    omemo_->setAcceptedSessionBuildingTrustLevels(XMPP::EncryptionTrustLevel::ManuallyTrusted
                                                  | XMPP::EncryptionTrustLevel::Authenticated);
    omemo_->setNewIdentityTrustLevel(XMPP::EncryptionTrustLevel::Undecided);
    client_->encryptionManager()->registerMethod(omemo_);

    auto features = client_->features();
    features += client_->encryptionManager()->features();
    features.addFeature(IrisKeySyncTask::feature);
    features.addFeature(config_.indexNodeName() + QStringLiteral("+notify"));
    client_->setFeatures(features);

    keySyncTask_ = new IrisKeySyncTask(client_);
    connect(keySyncTask_, &IrisKeySyncTask::requestReceived, this, &IrisXmppBackend::handleKeySyncRequest,
            Qt::QueuedConnection);
    connect(keySyncTask_, &IrisKeySyncTask::trustRequestReceived, this, &IrisXmppBackend::handleKeySyncTrustRequest,
            Qt::QueuedConnection);

    connect(tlsHandler_, &XMPP::QCATLSHandler::tlsHandshaken, this, [this]() {
        const bool identityValid = tlsHandler_->peerIdentityValid();
        const bool hostnameValid = tlsHandler_->certMatchesHostname();
        if (!identityValid || !hostnameValid) {
            const auto *tls = tlsHandler_->tls();
            qCWarning(lcIrisXmpp) << "Rejecting XMPP TLS certificate for" << XMPP::Jid(config_.jid).domain()
                                  << "peerIdentityResult=" << (tls ? tls->peerIdentityResult() : -1)
                                  << "certificateValidity="
                                  << (tls ? static_cast<int>(tls->peerCertificateValidity()) : -1)
                                  << "hostnameMatch=" << hostnameValid;
            if (connectionAttempt_ && !connectionAttempt_->finished) {
                auto attempt      = connectionAttempt_;
                attempt->finished = true;
                if (attempt->timer)
                    attempt->timer->stop();
                if (attempt->guard)
                    attempt->guard->deleteLater();
                connectionAttempt_.reset();
                XmppStatusResult result { false, false,
                                          false, QStringLiteral("The XMPP server TLS certificate is invalid"),
                                          {},    XmppErrorKind::Security };
                for (auto &callback : attempt->callbacks)
                    callback(result);
            }
            stream_->close();
            return;
        }
        tlsHandler_->continueAfterHandshake();
    });

    connect(stream_, &XMPP::ClientStream::needAuthParams, this, [this](bool user, bool pass, bool realm) {
        const XMPP::Jid jid(config_.jid);
        if (user)
            stream_->setUsername(jid.node());
        if (pass)
            stream_->setPassword(config_.password);
        if (realm)
            stream_->setRealm(jid.domain());
        stream_->continueAfterParams();
    });
    connect(stream_, &XMPP::ClientStream::warning, this, [this](int warning) {
        if (warning == XMPP::ClientStream::WarnNoTLS) {
            if (connectionAttempt_ && !connectionAttempt_->finished) {
                auto attempt      = connectionAttempt_;
                attempt->finished = true;
                if (attempt->timer)
                    attempt->timer->stop();
                if (attempt->guard)
                    attempt->guard->deleteLater();
                connectionAttempt_.reset();
                const XmppStatusResult result { false, false,
                                                false, QStringLiteral("The XMPP server does not offer TLS"),
                                                {},    XmppErrorKind::Security };
                for (auto &callback : attempt->callbacks)
                    callback(result);
            }
            stream_->close();
            return;
        }
        stream_->continueAfterWarning();
    });
    connect(stream_, &XMPP::ClientStream::authenticated, this, [this]() {
        const XMPP::Jid jid(config_.jid);
        client_->start(jid.domain(), jid.node(), config_.password, stream_->jid().resource());
        client_->setPresence(XMPP::Status());
        connected_ = true;
        prepared_  = false;
        emit connectionChanged(true);

        if (connectionAttempt_ && !connectionAttempt_->finished) {
            auto attempt      = connectionAttempt_;
            attempt->finished = true;
            if (attempt->timer)
                attempt->timer->stop();
            if (attempt->guard)
                attempt->guard->deleteLater();
            connectionAttempt_.reset();
            for (auto &callback : attempt->callbacks)
                callback({ true });
        }
    });
    connect(stream_, &XMPP::ClientStream::connectionClosed, this, [this]() {
        markDisconnected();
        // ClientStream does not forward a clean peer close through Client::streamError().
        // Explicitly close the Client side so every in-flight Task observes disconnected()
        // and completes with ErrDisc instead of waiting for its timeout.
        if (client_ && client_->hasStream())
            client_->close(true);
        if (connectionAttempt_ && !connectionAttempt_->finished) {
            auto attempt      = connectionAttempt_;
            attempt->finished = true;
            if (attempt->timer)
                attempt->timer->stop();
            if (attempt->guard)
                attempt->guard->deleteLater();
            connectionAttempt_.reset();
            const XmppStatusResult result { false, false,
                                            false, QStringLiteral("XMPP connection closed before authentication"),
                                            {},    XmppErrorKind::Transient };
            for (auto &callback : attempt->callbacks)
                callback(result);
        }
    });
    connect(stream_, qOverload<int>(&XMPP::ClientStream::error), this, [this](int code) {
        markDisconnected();
        const auto errorKind
            = code == XMPP::ClientStream::ErrAuth ? XmppErrorKind::Authentication : XmppErrorKind::Transient;
        const auto message = code == XMPP::ClientStream::ErrAuth
            ? QStringLiteral("XMPP authentication failed")
            : QStringLiteral("Iris XMPP stream error %1").arg(code);
        if (connectionAttempt_ && !connectionAttempt_->finished) {
            auto attempt      = connectionAttempt_;
            attempt->finished = true;
            if (attempt->timer)
                attempt->timer->stop();
            if (attempt->guard)
                attempt->guard->deleteLater();
            connectionAttempt_.reset();
            const XmppStatusResult result { false, false, false, message, {}, errorKind };
            for (auto &callback : attempt->callbacks)
                callback(result);
        } else {
            emit backendError(message);
        }
    });

    connect(pubSub_, &XMPP::PubSubManager::itemPublished, this,
            [this](const XMPP::Jid &service, const QString &node, const XMPP::PubSubItem &item) {
                if (service.bare() != bareJid(config_).bare() || node != config_.indexNodeName())
                    return;
                const auto parsed = parsePubSubItem(item);
                if (!parsed.valid) {
                    qCWarning(lcIrisXmpp).noquote() << "Ignoring malformed private-note PEP item:" << parsed.error;
                    emit remoteNodeInvalidated();
                    return;
                }
                auto note = XmppNoteCodec::decodeIndex(parsed.payload, config_.masterKey, config_.indexNodeName());
                if (note)
                    emit remoteNotePublished(note.value);
                else
                    emit remoteNodeInvalidated();
            });
    connect(pubSub_, &XMPP::PubSubManager::itemRetracted, this,
            [this](const XMPP::Jid &service, const QString &node, const QString &id) {
                if (service.bare() == bareJid(config_).bare() && node == config_.indexNodeName())
                    emit remoteNoteRetracted(id);
            });
    connect(pubSub_, &XMPP::PubSubManager::nodePurged, this, [this](const XMPP::Jid &service, const QString &node) {
        if (service.bare() == bareJid(config_).bare()
            && (node == config_.indexNodeName() || node == config_.contentNodeName()))
            emit remoteNodeInvalidated();
    });
    connect(pubSub_, &XMPP::PubSubManager::nodeDeleted, this, [this](const XMPP::Jid &service, const QString &node) {
        if (service.bare() == bareJid(config_).bare()
            && (node == config_.indexNodeName() || node == config_.contentNodeName()))
            emit remoteNodeInvalidated();
    });
}

void IrisXmppBackend::connectToServerAsync(StatusCallback callback)
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
    const XMPP::Jid jid(config_.jid);
    if (!jid.isValid() || jid.node().isEmpty()) {
        callback(
            { false, false, false, QStringLiteral("A valid XMPP JID is required"), {}, XmppErrorKind::Configuration });
        return;
    }
    if (config_.password.isEmpty()) {
        callback(
            { false, false, false, QStringLiteral("The XMPP password is empty"), {}, XmppErrorKind::Authentication });
        return;
    }

    // Iris persistent push/server Tasks become terminal after Client::disconnected().
    // Reusing Client::start() would install a second set of those tasks, so use a
    // fresh Iris client stack for every non-resumed application-level reconnect.
    if (freshClientRequired_)
        destroyClientObjects();
    createClient();
    if (connected_ && stream_ && stream_->isAuthenticated()) {
        callback({ true });
        return;
    }
    if (connectionAttempt_ && connectionAttempt_->generation == generation_ && !connectionAttempt_->finished) {
        connectionAttempt_->callbacks.append(std::move(callback));
        return;
    }

    const auto attempt  = std::make_shared<ConnectionAttempt>();
    attempt->generation = generation_;
    attempt->callbacks.append(std::move(callback));
    auto *guard = new QObject(this);
    auto *timer = new QTimer(guard);
    timer->setSingleShot(true);
    attempt->guard     = guard;
    attempt->timer     = timer;
    connectionAttempt_ = attempt;

    connect(timer, &QTimer::timeout, guard, [this, attempt]() {
        if (attempt->finished)
            return;
        attempt->finished = true;
        if (connectionAttempt_ == attempt)
            connectionAttempt_.reset();
        const XmppStatusResult result {
            false, false,
            false, QStringLiteral("XMPP connection timed out after %1 ms").arg(config_.timeoutMs),
            {},    XmppErrorKind::Transient
        };
        for (auto &callback : attempt->callbacks)
            callback(result);
        if (stream_)
            stream_->close();
    });
    timer->start(qMax(1000, config_.timeoutMs));

    client_->connectToServer(stream_, jid.withResource(config_.resource));
}

XmppStatusResult IrisXmppBackend::taskFailure(const XMPP::Task *task, QString context) const
{
    XmppStatusResult result;
    const auto       detail = firstTaskError(task);
    result.error            = context.isEmpty() ? detail : QStringLiteral("%1: %2").arg(context, detail);
    if (!task) {
        result.errorKind = XmppErrorKind::Protocol;
        return result;
    }
    if (task->statusCode() == XMPP::Task::ErrTimeout || task->statusCode() == XMPP::Task::ErrDisc) {
        result.errorKind = XmppErrorKind::Transient;
        return result;
    }
    switch (task->error().condition) {
    case XMPP::Stanza::Error::ErrorCond::NotAuthorized:
        result.errorKind = XmppErrorKind::Authentication;
        break;
    case XMPP::Stanza::Error::ErrorCond::RemoteServerNotFound:
    case XMPP::Stanza::Error::ErrorCond::RemoteServerTimeout:
    case XMPP::Stanza::Error::ErrorCond::ResourceConstraint:
    case XMPP::Stanza::Error::ErrorCond::InternalServerError:
        result.errorKind = XmppErrorKind::Transient;
        break;
    case XMPP::Stanza::Error::ErrorCond::Forbidden:
    case XMPP::Stanza::Error::ErrorCond::NotAllowed:
    case XMPP::Stanza::Error::ErrorCond::RegistrationRequired:
        result.errorKind = XmppErrorKind::Configuration;
        break;
    default:
        result.errorKind = XmppErrorKind::Protocol;
        break;
    }
    return result;
}

void IrisXmppBackend::completeStatusForTask(XMPP::Task *task, StatusCallback callback, QString context)
{
    if (task && task->success())
        callback({ true });
    else
        callback(taskFailure(task, std::move(context)));
}

void IrisXmppBackend::ensureOmemoReadyAsync(StatusCallback callback)
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
    if (connected_ && omemoReady_ && omemo_ && omemo_->isReady()) {
        callback({ true });
        return;
    }
    if (omemoAttempt_ && omemoAttempt_->generation == generation_ && !omemoAttempt_->finished) {
        omemoAttempt_->callbacks.append(std::move(callback));
        return;
    }

    const auto attempt  = std::make_shared<ReadyAttempt>();
    attempt->generation = generation_;
    attempt->callbacks.append(std::move(callback));
    omemoAttempt_     = attempt;
    const auto finish = [this, attempt](XmppStatusResult result) {
        if (attempt->finished)
            return;
        attempt->finished = true;
        if (attempt->generation != generation_)
            result = cancelledResult();
        if (omemoAttempt_ == attempt)
            omemoAttempt_.reset();
        for (auto &callback : attempt->callbacks)
            callback(result);
    };

    connectToServerAsync([this, attempt, finish](XmppStatusResult status) mutable {
        if (!status.ok) {
            finish(std::move(status));
            return;
        }
        if (attempt->generation != generation_) {
            finish(cancelledResult());
            return;
        }
        if (!omemoStorage_ || !omemoStorage_->isValid()) {
            finish({ false,
                     false,
                     false,
                     omemoStorage_ ? omemoStorage_->errorString() : QStringLiteral("Iris OMEMO storage is unavailable"),
                     {},
                     XmppErrorKind::Security });
            return;
        }
        if (!trustStorage_ || !trustStorage_->isValid()) {
            finish({ false,
                     false,
                     false,
                     trustStorage_ ? trustStorage_->errorString() : QStringLiteral("Iris trust storage is unavailable"),
                     {},
                     XmppErrorKind::Security });
            return;
        }
        if (omemo_->isReady()) {
            omemoReady_ = true;
            finish({ true });
            return;
        }

        runEncryptionJob(
            omemo_->setUp(config_.resource), this, [this, attempt, finish](XMPP::EncryptionJob *job) mutable {
                if (attempt->generation != generation_) {
                    finish(cancelledResult());
                    return;
                }
                if (!job || !job->success()) {
                    finish(encryptionFailure(job, QStringLiteral("Could not initialize the OMEMO device")));
                    return;
                }
                omemoReady_ = omemo_->isReady();
                finish(omemoReady_
                           ? XmppStatusResult { true }
                           : XmppStatusResult { false,
                                                false,
                                                false,
                                                QStringLiteral("Iris OMEMO setup completed without a ready device"),
                                                {},
                                                XmppErrorKind::Security });
            });
    });
}

void IrisXmppBackend::verifyPrivateStorageSupportAsync(StatusCallback callback)
{
    auto *task = new XMPP::DiscoInfoTask(client_->rootTask());
    task->get(bareJid(config_));
    runIrisTask(task, this, config_.timeoutMs,
                [this, callback = std::move(callback)](XMPP::DiscoInfoTask *task) mutable {
                    if (!task || !task->success()) {
                        callback(taskFailure(task, QStringLiteral("Could not discover the PEP service")));
                        return;
                    }
                    const auto &item = task->item();
                    const bool  hasPepIdentity
                        = std::any_of(item.identities().cbegin(), item.identities().cend(), [](const auto &id) {
                              return id.category == QStringLiteral("pubsub") && id.type == QStringLiteral("pep");
                          });
                    if (!hasPepIdentity) {
                        callback({ false,
                                   false,
                                   false,
                                   QStringLiteral("The XMPP server does not advertise a pubsub/pep identity"),
                                   {},
                                   XmppErrorKind::Configuration });
                        return;
                    }
                    if (!item.features().test(QLatin1String(PublishOptionsFeature))) {
                        callback({ false,
                                   false,
                                   false,
                                   QStringLiteral("The server does not advertise PubSub publish-options; the client "
                                                  "will not store private notes there"),
                                   {},
                                   XmppErrorKind::Configuration });
                        return;
                    }
                    callback({ true });
                });
}

void IrisXmppBackend::verifyNodeAsync(QString nodeName, StatusCallback callback)
{
    auto *task = pubSub_->nodeConfig(bareJid(config_), nodeName);
    runIrisTask(task, this, config_.timeoutMs,
                [this, callback = std::move(callback)](XMPP::PubSubNodeConfigTask *task) mutable {
                    if (!task || !task->success()) {
                        callback(taskFailure(task, QStringLiteral("Could not verify the private-note PEP node")));
                        return;
                    }
                    if (!nodeConfigIsPrivate(task->options())) {
                        callback({ false,
                                   false,
                                   false,
                                   QStringLiteral(
                                       "The private-note PEP node is not persistent and private after configuration"),
                                   {},
                                   XmppErrorKind::Configuration });
                        return;
                    }
                    callback({ true });
                });
}

void IrisXmppBackend::ensureNodeAsync(QString nodeName, StatusCallback callback)
{
    auto *task = pubSub_->nodeConfig(bareJid(config_), nodeName);
    runIrisTask(
        task, this, config_.timeoutMs,
        [this, nodeName = std::move(nodeName),
         callback = std::move(callback)](XMPP::PubSubNodeConfigTask *task) mutable {
            if (!task) {
                callback({ false,
                           false,
                           false,
                           QStringLiteral("Could not inspect the private-note PEP node"),
                           {},
                           XmppErrorKind::Protocol });
                return;
            }
            if (!task->success()) {
                if (!isItemNotFound(task)) {
                    callback(
                        taskFailure(task, QStringLiteral("Could not read the private-note PEP node configuration")));
                    return;
                }
                auto *create = pubSub_->createNode(bareJid(config_), nodeName, privateNodeOptions());
                runIrisTask(
                    create, this, config_.timeoutMs,
                    [this, nodeName, callback = std::move(callback)](XMPP::PubSubCreateTask *create) mutable {
                        if (create && !create->success() && !isConflict(create)) {
                            callback(taskFailure(create, QStringLiteral("Could not create the private-note PEP node")));
                            return;
                        }
                        verifyNodeAsync(nodeName, std::move(callback));
                    });
                return;
            }
            if (nodeConfigIsPrivate(task->options())) {
                callback({ true });
                return;
            }
            auto       options  = task->options();
            const auto required = privateNodeOptions();
            for (auto it = required.cbegin(); it != required.cend(); ++it)
                options.insert(it.key(), it.value());
            auto *configure = pubSub_->configureNode(bareJid(config_), nodeName, options);
            runIrisTask(configure, this, config_.timeoutMs,
                        [this, nodeName, callback = std::move(callback)](XMPP::PubSubConfigureTask *configure) mutable {
                            if (!configure || !configure->success()) {
                                callback(taskFailure(configure,
                                                     QStringLiteral("Could not configure the private-note PEP node")));
                                return;
                            }
                            verifyNodeAsync(nodeName, std::move(callback));
                        });
        });
}

void IrisXmppBackend::ensureReadyAsync(StatusCallback callback)
{
    if (prepared_) {
        callback({ true });
        return;
    }
    if (readyAttempt_ && readyAttempt_->generation == generation_ && !readyAttempt_->finished) {
        readyAttempt_->callbacks.append(std::move(callback));
        return;
    }

    const auto attempt  = std::make_shared<ReadyAttempt>();
    attempt->generation = generation_;
    attempt->callbacks.append(std::move(callback));
    readyAttempt_     = attempt;
    const auto finish = [this, attempt](XmppStatusResult result) {
        if (attempt->finished)
            return;
        attempt->finished = true;
        if (attempt->generation != generation_)
            result = cancelledResult();
        if (readyAttempt_ == attempt)
            readyAttempt_.reset();
        for (auto &callback : attempt->callbacks)
            callback(result);
    };

    ensureOmemoReadyAsync([this, attempt, finish](XmppStatusResult status) mutable {
        if (!status.ok) {
            finish(std::move(status));
            return;
        }
        verifyPrivateStorageSupportAsync([this, attempt, finish](XmppStatusResult status) mutable {
            if (!status.ok) {
                finish(std::move(status));
                return;
            }
            ensureNodeAsync(config_.indexNodeName(), [this, attempt, finish](XmppStatusResult status) mutable {
                if (!status.ok) {
                    finish(std::move(status));
                    return;
                }
                ensureNodeAsync(config_.contentNodeName(), [this, attempt, finish](XmppStatusResult status) mutable {
                    if (status.ok)
                        prepared_ = true;
                    finish(std::move(status));
                });
            });
        });
    });
}

void IrisXmppBackend::probeAsync(StatusCallback callback) { ensureReadyAsync(std::move(callback)); }

QString IrisXmppBackend::newUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

void IrisXmppBackend::listNodeItemIdsAsync(QString                                            nodeName,
                                           std::function<void(QStringList, XmppStatusResult)> callback)
{
    auto *task = new XMPP::JT_DiscoItems(client_->rootTask());
    task->get(bareJid(config_), nodeName);
    runIrisTask(task, this, config_.timeoutMs, [callback = std::move(callback)](XMPP::JT_DiscoItems *task) mutable {
        if (!task || !task->success()) {
            XmppStatusResult status;
            status.error = firstTaskError(task);
            status.errorKind
                = task && (task->statusCode() == XMPP::Task::ErrTimeout || task->statusCode() == XMPP::Task::ErrDisc)
                ? XmppErrorKind::Transient
                : XmppErrorKind::Protocol;
            callback({}, std::move(status));
            return;
        }
        QStringList ids;
        for (const auto &item : task->items()) {
            // XEP-0060 section 5.5 carries a published item ID in
            // the disco#items result's name attribute.
            if (!item.name().isEmpty())
                ids.append(item.name());
        }
        callback(std::move(ids), { true });
    });
}

void IrisXmppBackend::fetchPayloadAsync(
    QString nodeName, QString id, std::function<void(std::optional<XmppEncryptedPayload>, XmppStatusResult)> callback)
{
    auto *task = pubSub_->items(bareJid(config_), nodeName, { id });
    runIrisTask(task, this, config_.timeoutMs,
                [this, id = std::move(id), callback = std::move(callback)](XMPP::PubSubItemsTask *task) mutable {
                    if (!task || !task->success()) {
                        auto status     = taskFailure(task);
                        status.notFound = isItemNotFound(task);
                        callback({}, std::move(status));
                        return;
                    }
                    const auto &items = task->items();
                    if (items.isEmpty()) {
                        callback({}, { false, false, true, QStringLiteral("The XMPP PubSub item was not found") });
                        return;
                    }
                    const auto parsed = parsePubSubItem(items.constFirst());
                    if (!parsed.valid) {
                        callback({}, { false, false, false, parsed.error, {}, XmppErrorKind::Protocol });
                        return;
                    }
                    callback(parsed.payload, { true });
                });
}

void IrisXmppBackend::publishPayloadAsync(QString nodeName, XmppEncryptedPayload payload, StatusCallback callback)
{
    auto *task = pubSub_->publish(bareJid(config_), nodeName, pubSubItem(payload), privatePublishOptions());
    runIrisTask(
        task, this, config_.timeoutMs, [this, callback = std::move(callback)](XMPP::PubSubPublishTask *task) mutable {
            completeStatusForTask(task, std::move(callback), QStringLiteral("Could not publish the XMPP PubSub item"));
        });
}

void IrisXmppBackend::retractItemAsync(QString nodeName, QString id, StatusCallback callback)
{
    auto *task = pubSub_->retract(bareJid(config_), nodeName, id, true);
    runIrisTask(
        task, this, config_.timeoutMs, [this, callback = std::move(callback)](XMPP::PubSubRetractTask *task) mutable {
            if (task && (!task->success() && isItemNotFound(task))) {
                callback({ true });
                return;
            }
            completeStatusForTask(task, std::move(callback), QStringLiteral("Could not retract the XMPP PubSub item"));
        });
}

void IrisXmppBackend::listNotesAsync(ListCallback callback)
{
    const auto generation = generation_;
    ensureReadyAsync([this, generation, callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (generation != generation_) {
            XmppListResult result;
            static_cast<XmppStatusResult &>(result) = cancelledResult();
            callback(std::move(result));
            return;
        }
        if (!ready.ok) {
            XmppListResult result;
            static_cast<XmppStatusResult &>(result) = std::move(ready);
            callback(std::move(result));
            return;
        }

        const auto decode = [this](const QList<XMPP::PubSubItem> &items, XmppListResult &output) {
            const auto configuredKeyId = SecureEnvelope::keyId(config_.masterKey, KeyDerivationProfile::PrivateNotes);
            for (const auto &item : items) {
                const auto parsed = parsePubSubItem(item);
                if (!parsed.valid) {
                    output.partial = true;
                    qCWarning(lcIrisXmpp).noquote()
                        << "Skipping unreadable XMPP index item" << item.id() << ':' << parsed.error;
                    continue;
                }
                if (parsed.payload.keyId != configuredKeyId) {
                    output.partial = true;
                    continue;
                }
                auto note = XmppNoteCodec::decodeIndex(parsed.payload, config_.masterKey, config_.indexNodeName());
                if (!note) {
                    output.partial = true;
                    continue;
                }
                output.notes.append(std::move(note.value));
            }
        };

        listNodeItemIdsAsync(
            config_.indexNodeName(),
            [this, generation, decode, callback = std::move(callback)](QStringList      ids,
                                                                       XmppStatusResult idsStatus) mutable {
                if (generation != generation_) {
                    XmppListResult result;
                    static_cast<XmppStatusResult &>(result) = cancelledResult();
                    callback(std::move(result));
                    return;
                }
                if (!idsStatus.ok) {
                    // Some PEP implementations do not expose disco#items. Ask for all
                    // items directly as the compatibility path.
                    auto *all = pubSub_->items(bareJid(config_), config_.indexNodeName());
                    runIrisTask(all, this, config_.timeoutMs,
                                [this, decode, callback = std::move(callback)](XMPP::PubSubItemsTask *task) mutable {
                                    XmppListResult output;
                                    if (!task || !task->success()) {
                                        static_cast<XmppStatusResult &>(output) = taskFailure(
                                            task, QStringLiteral("Could not load the private-note index"));
                                        callback(std::move(output));
                                        return;
                                    }
                                    decode(task->items(), output);
                                    output.ok = true;
                                    callback(std::move(output));
                                });
                    return;
                }

                auto                                       output       = std::make_shared<XmppListResult>();
                auto                                       offset       = std::make_shared<int>(0);
                auto                                       loadNext     = std::make_shared<std::function<void()>>();
                const std::weak_ptr<std::function<void()>> weakLoadNext = loadNext;
                *loadNext = [this, generation, ids = std::move(ids), decode, output, offset, weakLoadNext,
                             callback = std::move(callback)]() mutable {
                    if (generation != generation_) {
                        static_cast<XmppStatusResult &>(*output) = cancelledResult();
                        callback(std::move(*output));
                        return;
                    }
                    if (*offset >= ids.size()) {
                        output->ok = true;
                        callback(std::move(*output));
                        return;
                    }
                    const auto batch = ids.mid(*offset, BatchSize);
                    *offset += batch.size();
                    auto      *items = pubSub_->items(bareJid(config_), config_.indexNodeName(), batch);
                    const auto next  = weakLoadNext.lock();
                    if (!next) {
                        XmppListResult result;
                        static_cast<XmppStatusResult &>(result) = cancelledResult();
                        callback(std::move(result));
                        return;
                    }
                    runIrisTask(items, this, config_.timeoutMs,
                                [this, decode, output, next, callback](XMPP::PubSubItemsTask *task) mutable {
                                    if (!task || !task->success()) {
                                        static_cast<XmppStatusResult &>(*output) = taskFailure(
                                            task, QStringLiteral("Could not load the private-note index"));
                                        callback(std::move(*output));
                                        return;
                                    }
                                    decode(task->items(), *output);
                                    (*next)();
                                });
                };
                (*loadNext)();
            });
    });
}

void IrisXmppBackend::requestIndexAsync(QString id, quint64 generation, NoteCallback callback)
{
    fetchPayloadAsync(config_.indexNodeName(), id,
                      [this, generation, callback = std::move(callback)](std::optional<XmppEncryptedPayload> payload,
                                                                         XmppStatusResult status) mutable {
                          XmppNoteResult output;
                          if (generation != generation_) {
                              static_cast<XmppStatusResult &>(output) = cancelledResult();
                              callback(std::move(output));
                              return;
                          }
                          if (!status.ok || !payload) {
                              static_cast<XmppStatusResult &>(output) = std::move(status);
                              callback(std::move(output));
                              return;
                          }
                          auto decoded
                              = XmppNoteCodec::decodeIndex(*payload, config_.masterKey, config_.indexNodeName());
                          if (!decoded) {
                              output.error     = decoded.error.message;
                              output.errorKind = decoded.error.code == CryptoError::Unsupported
                                  ? XmppErrorKind::Protocol
                                  : XmppErrorKind::Security;
                              callback(std::move(output));
                              return;
                          }
                          output.note = std::move(decoded.value);
                          output.ok   = true;
                          callback(std::move(output));
                      });
}

void IrisXmppBackend::requestNoteAsync(QString id, quint64 generation, NoteCallback callback, int attempt)
{
    const auto indexId = id;
    requestIndexAsync(
        indexId, generation,
        [this, id = std::move(id), generation, callback = std::move(callback), attempt](XmppNoteResult index) mutable {
            if (!index.ok) {
                callback(std::move(index));
                return;
            }
            fetchPayloadAsync(config_.contentNodeName(), id,
                              [this, id, generation, callback = std::move(callback), attempt, index = std::move(index)](
                                  std::optional<XmppEncryptedPayload> payload, XmppStatusResult status) mutable {
                                  XmppNoteResult output;
                                  if (generation != generation_) {
                                      static_cast<XmppStatusResult &>(output) = cancelledResult();
                                      callback(std::move(output));
                                      return;
                                  }
                                  if (!status.ok || !payload) {
                                      static_cast<XmppStatusResult &>(output) = std::move(status);
                                      callback(std::move(output));
                                      return;
                                  }
                                  auto content = XmppNoteCodec::decodeContent(*payload, config_.masterKey,
                                                                              config_.contentNodeName(), index.note);
                                  if (!content) {
                                      const auto inconsistent
                                          = QStringLiteral("private-note content does not match its index revision");
                                      if (content.error.message == inconsistent && attempt < 3) {
                                          requestNoteAsync(id, generation, std::move(callback), attempt + 1);
                                          return;
                                      }
                                      output.error = content.error.message;
                                      if (content.error.message == inconsistent)
                                          output.errorKind = XmppErrorKind::Transient;
                                      callback(std::move(output));
                                      return;
                                  }
                                  output.note = std::move(content.value);
                                  output.ok   = true;
                                  callback(std::move(output));
                              });
        });
}

void IrisXmppBackend::getNoteAsync(QString id, NoteCallback callback)
{
    const auto generation = generation_;
    ensureReadyAsync(
        [this, generation, id = std::move(id), callback = std::move(callback)](XmppStatusResult ready) mutable {
            if (!ready.ok) {
                XmppNoteResult output;
                static_cast<XmppStatusResult &>(output) = std::move(ready);
                callback(std::move(output));
                return;
            }
            requestNoteAsync(std::move(id), generation, std::move(callback));
        });
}

void IrisXmppBackend::publishNoteAsync(XmppRemoteNote note, quint64 generation, NoteCallback callback)
{
    XmppNoteResult output;
    note.parentRevision  = note.revision;
    note.revision        = newUuid();
    note.contentRevision = note.revision;
    note.originId        = config_.originId;
    if (!note.preserveModified || !note.modified.isValid())
        note.modified = QDateTime::currentDateTimeUtc();
    note.format         = QStringLiteral("markdown");
    note.contentPresent = true;

    auto content = XmppNoteCodec::encodeContent(note, config_.masterKey, config_.contentNodeName());
    auto index   = XmppNoteCodec::encodeIndex(note, config_.masterKey, config_.indexNodeName());
    if (!content || !index) {
        output.error = !content ? content.error.message : index.error.message;
        callback(std::move(output));
        return;
    }

    publishPayloadAsync(config_.contentNodeName(), content.value,
                        [this, generation, note = std::move(note), index = std::move(index.value),
                         callback = std::move(callback)](XmppStatusResult status) mutable {
                            if (!status.ok) {
                                XmppNoteResult output;
                                static_cast<XmppStatusResult &>(output) = std::move(status);
                                callback(std::move(output));
                                return;
                            }
                            if (generation != generation_) {
                                XmppNoteResult output;
                                static_cast<XmppStatusResult &>(output) = cancelledResult();
                                callback(std::move(output));
                                return;
                            }
                            publishPayloadAsync(config_.indexNodeName(), std::move(index),
                                                [note     = std::move(note),
                                                 callback = std::move(callback)](XmppStatusResult status) mutable {
                                                    XmppNoteResult output;
                                                    if (!status.ok) {
                                                        static_cast<XmppStatusResult &>(output) = std::move(status);
                                                        callback(std::move(output));
                                                        return;
                                                    }
                                                    output.note = std::move(note);
                                                    output.ok   = true;
                                                    callback(std::move(output));
                                                });
                        });
}

void IrisXmppBackend::saveNoteAsync(XmppRemoteNote note, NoteCallback callback)
{
    const auto generation = generation_;
    ensureReadyAsync([this, generation, note = std::move(note),
                      callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            XmppNoteResult output;
            static_cast<XmppStatusResult &>(output) = std::move(ready);
            callback(std::move(output));
            return;
        }
        if (note.id.isEmpty()) {
            note.id = newUuid();
            publishNoteAsync(std::move(note), generation, std::move(callback));
            return;
        }

        const auto noteId = note.id;
        requestNoteAsync(
            noteId, generation,
            [this, generation, note = std::move(note), callback = std::move(callback)](XmppNoteResult server) mutable {
                if (!server.ok) {
                    callback(std::move(server));
                    return;
                }
                if (server.note.revision != note.revision) {
                    const auto localContentRevision
                        = note.contentRevision.isEmpty() ? note.revision : note.contentRevision;
                    const auto serverContentRevision
                        = server.note.contentRevision.isEmpty() ? server.note.revision : server.note.contentRevision;
                    const bool ownIndexOnlyUpdate = server.note.originId == config_.originId
                        && server.note.parentRevision == note.revision && serverContentRevision == localContentRevision;
                    if (!ownIndexOnlyUpdate) {
                        XmppNoteResult conflict;
                        conflict.conflict         = true;
                        conflict.remoteOnConflict = std::move(server.note);
                        conflict.error            = QStringLiteral(
                            "The note was modified on another XMPP resource; the local version was not published");
                        callback(std::move(conflict));
                        return;
                    }
                    note.revision        = server.note.revision;
                    note.contentRevision = serverContentRevision;
                }
                publishNoteAsync(std::move(note), generation, std::move(callback));
            });
    });
}

void IrisXmppBackend::updateNoteIndexAsync(XmppRemoteNote note, NoteCallback callback)
{
    const auto generation = generation_;
    ensureReadyAsync([this, generation, note = std::move(note),
                      callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            XmppNoteResult output;
            static_cast<XmppStatusResult &>(output) = std::move(ready);
            callback(std::move(output));
            return;
        }
        if (note.id.isEmpty()) {
            XmppNoteResult output;
            output.error = QStringLiteral("A saved XMPP note ID is required for an index update");
            callback(std::move(output));
            return;
        }
        const auto noteId = note.id;
        requestIndexAsync(
            noteId, generation,
            [this, generation, note = std::move(note), callback = std::move(callback)](XmppNoteResult server) mutable {
                if (!server.ok) {
                    callback(std::move(server));
                    return;
                }
                if (server.note.revision != note.revision) {
                    XmppNoteResult conflict;
                    conflict.conflict         = true;
                    conflict.remoteOnConflict = std::move(server.note);
                    conflict.error            = QStringLiteral(
                        "The note was modified on another XMPP resource; the folder was not published");
                    callback(std::move(conflict));
                    return;
                }
                auto updated           = std::move(server.note);
                updated.folderPath     = std::move(note.folderPath);
                updated.parentRevision = updated.revision;
                updated.revision       = newUuid();
                updated.originId       = config_.originId;
                updated.modified       = QDateTime::currentDateTimeUtc();
                updated.format         = QStringLiteral("markdown");
                updated.contentPresent = false;
                auto payload = XmppNoteCodec::encodeIndex(updated, config_.masterKey, config_.indexNodeName());
                if (!payload) {
                    XmppNoteResult output;
                    output.error = payload.error.message;
                    callback(std::move(output));
                    return;
                }
                publishPayloadAsync(
                    config_.indexNodeName(), std::move(payload.value),
                    [updated = std::move(updated), callback = std::move(callback)](XmppStatusResult status) mutable {
                        XmppNoteResult output;
                        if (!status.ok) {
                            static_cast<XmppStatusResult &>(output) = std::move(status);
                        } else {
                            output.note = std::move(updated);
                            output.ok   = true;
                        }
                        callback(std::move(output));
                    });
            });
    });
}

void IrisXmppBackend::deleteNoteAsync(QString id, StatusCallback callback)
{
    ensureReadyAsync([this, id = std::move(id), callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            callback(std::move(ready));
            return;
        }
        const auto noteId = id;
        retractItemAsync(config_.indexNodeName(), noteId,
                         [this, id = std::move(id), callback = std::move(callback)](XmppStatusResult status) mutable {
                             if (!status.ok) {
                                 callback(std::move(status));
                                 return;
                             }
                             retractItemAsync(config_.contentNodeName(), id, std::move(callback));
                         });
    });
}

XmppDeviceInfo IrisXmppBackend::ownOmemoDevice() const
{
    if (!omemo_ || !omemo_->isReady())
        return {};
    return { omemo_->ownDeviceLabel(), omemo_->ownDeviceId(), omemo_->ownIdentityKey(), XmppTrustLevel::Authenticated };
}

void IrisXmppBackend::refreshOwnOmemoFingerprintsAsync(std::function<void(QSet<quint32>, XmppStatusResult)> callback)
{
    runEncryptionJob(
        omemo_->refreshDevices(bareJid(config_), XMPP::OmemoProtocol::Omemo2, false), this,
        [this, callback = std::move(callback)](XMPP::EncryptionJob *listJob) mutable {
            if (!listJob || !listJob->success()) {
                callback({}, encryptionFailure(listJob, QStringLiteral("Could not refresh own OMEMO devices")));
                return;
            }

            QList<quint32> deviceIds;
            const auto     ownId = omemo_->ownDeviceId();
            for (const auto &device : omemo_->devices(bareJid(config_))) {
                if (device.protocol == XMPP::OmemoProtocol::Omemo2 && device.active && device.id != ownId)
                    deviceIds.append(device.id);
            }

            auto                                       failed   = std::make_shared<QSet<quint32>>();
            auto                                       offset   = std::make_shared<int>(0);
            auto                                       next     = std::make_shared<std::function<void()>>();
            const std::weak_ptr<std::function<void()>> weakNext = next;
            *next = [this, deviceIds = std::move(deviceIds), failed, offset, weakNext,
                     callback = std::move(callback)]() mutable {
                if (*offset >= deviceIds.size()) {
                    callback(std::move(*failed), { true });
                    return;
                }
                const auto deviceId = deviceIds.at((*offset)++);
                const auto next     = weakNext.lock();
                if (!next) {
                    callback(std::move(*failed), cancelledResult());
                    return;
                }
                // Device discovery must not build a session: an Undecided identity has to be
                // visible to the trust UI before it can be accepted.
                runEncryptionJob(omemo_->refreshBundle(bareJid(config_), deviceId, XMPP::OmemoProtocol::Omemo2, false),
                                 this, [deviceId, failed, next](XMPP::EncryptionJob *bundleJob) mutable {
                                     if (!bundleJob || !bundleJob->success())
                                         failed->insert(deviceId);
                                     (*next)();
                                 });
            };
            (*next)();
        });
}

void IrisXmppBackend::ownOmemoDevicesAsync(DevicesCallback callback)
{
    ensureOmemoReadyAsync([this, callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            callback({}, ready.error);
            return;
        }
        refreshOwnOmemoFingerprintsAsync([this, callback = std::move(callback)](QSet<quint32>    failedBundles,
                                                                                XmppStatusResult status) mutable {
            if (!status.ok) {
                callback({}, status.error);
                return;
            }
            const auto            ownId  = omemo_->ownDeviceId();
            const auto            ownKey = omemo_->ownIdentityKey();
            QList<XmppDeviceInfo> output;
            for (const auto &device : omemo_->devices(bareJid(config_))) {
                if (device.protocol != XMPP::OmemoProtocol::Omemo2 || !device.active || device.id == ownId)
                    continue;
                const auto key = failedBundles.contains(device.id) ? QByteArray {} : device.identityKey;
                if (!key.isEmpty() && key == ownKey)
                    continue;
                const auto label = device.label.isEmpty() ? QStringLiteral("Unnamed device") : device.label;
                output.append({ label, device.id, key,
                                key.isEmpty() ? XmppTrustLevel::Undecided : toBackendTrust(device.trust) });
            }
            const auto warning = failedBundles.isEmpty()
                ? QString {}
                : QStringLiteral("Could not obtain the OMEMO fingerprint for %1 device(s)").arg(failedBundles.size());
            callback(std::move(output), warning);
        });
    });
}

void IrisXmppBackend::ownOmemoBundleValidAsync(StatusCallback callback)
{
    ensureOmemoReadyAsync([this, callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            callback(std::move(ready));
            return;
        }
        const auto ownId = omemo_->ownDeviceId();
        if (!ownId || omemo_->ownIdentityKey().isEmpty()) {
            callback({ false,
                       false,
                       false,
                       QStringLiteral("The local OMEMO device is not initialized"),
                       {},
                       XmppErrorKind::Security });
            return;
        }
        runEncryptionJob(
            omemo_->refreshDevices(bareJid(config_), XMPP::OmemoProtocol::Omemo2, false), this,
            [this, ownId, callback = std::move(callback)](XMPP::EncryptionJob *listJob) mutable {
                if (!listJob || !listJob->success()) {
                    callback(encryptionFailure(listJob,
                                               QStringLiteral("Could not refresh the published OMEMO device list")));
                    return;
                }
                const auto listed    = omemo_->devices(bareJid(config_));
                const bool announced = std::any_of(listed.cbegin(), listed.cend(), [ownId](const auto &device) {
                    return device.id == ownId && device.protocol == XMPP::OmemoProtocol::Omemo2 && device.active;
                });
                if (!announced) {
                    callback({ false,
                               false,
                               false,
                               QStringLiteral("The local OMEMO device is missing from the published device list"),
                               {},
                               XmppErrorKind::Security });
                    return;
                }
                runEncryptionJob(
                    omemo_->refreshBundle(bareJid(config_), ownId, XMPP::OmemoProtocol::Omemo2, false), this,
                    [this, ownId, callback = std::move(callback)](XMPP::EncryptionJob *bundleJob) mutable {
                        if (!bundleJob || !bundleJob->success()) {
                            callback(encryptionFailure(
                                bundleJob, QStringLiteral("The published OMEMO bundle is missing or invalid")));
                            return;
                        }
                        const auto devices = omemo_->devices(bareJid(config_));
                        const auto it
                            = std::find_if(devices.cbegin(), devices.cend(), [this, ownId](const auto &device) {
                                  return device.id == ownId && device.protocol == XMPP::OmemoProtocol::Omemo2
                                      && device.active && device.identityKey == omemo_->ownIdentityKey();
                              });
                        callback(it == devices.cend()
                                     ? XmppStatusResult { false,
                                                          false,
                                                          false,
                                                          QStringLiteral(
                                                              "The published OMEMO bundle does not match this device"),
                                                          {},
                                                          XmppErrorKind::Security }
                                     : XmppStatusResult { true });
                    });
            });
    });
}

void IrisXmppBackend::repairOwnOmemoDeviceAsync(StatusCallback callback)
{
    ensureOmemoReadyAsync([this, callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            callback(std::move(ready));
            return;
        }
        runEncryptionJob(
            omemo_->sanitizeOwnPep(), this, [callback = std::move(callback)](XMPP::EncryptionJob *job) mutable {
                callback(job && job->success()
                             ? XmppStatusResult { true }
                             : encryptionFailure(job, QStringLiteral("Could not repair the OMEMO publication")));
            });
    });
}

void IrisXmppBackend::removeOwnOmemoDeviceAsync(quint32 deviceId, StatusCallback callback)
{
    ensureOmemoReadyAsync([this, deviceId, callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            callback(std::move(ready));
            return;
        }
        if (deviceId == omemo_->ownDeviceId()) {
            callback({ false,
                       false,
                       false,
                       QStringLiteral("The active OMEMO device cannot retire itself"),
                       {},
                       XmppErrorKind::Configuration });
            return;
        }
        runEncryptionJob(omemo_->retireOwnDevice(deviceId, XMPP::OmemoProtocol::Omemo2), this,
                         [callback = std::move(callback)](XMPP::EncryptionJob *job) mutable {
                             callback(
                                 job && job->success()
                                     ? XmppStatusResult { true }
                                     : encryptionFailure(job, QStringLiteral("Could not remove the OMEMO device")));
                         });
    });
}

void IrisXmppBackend::trustOwnOmemoDeviceAsync(QByteArray keyId, StatusCallback callback)
{
    if (keyId.isEmpty()) {
        callback({ false, false, false, QStringLiteral("No OMEMO device was selected"), {}, XmppErrorKind::Security });
        return;
    }
    trustOwnOmemoDevicesAsync({ std::move(keyId) }, std::move(callback));
}

void IrisXmppBackend::trustOwnOmemoDevicesAsync(QList<QByteArray> keyIds, StatusCallback callback)
{
    ensureOmemoReadyAsync([this, keyIds = std::move(keyIds),
                           callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            callback(std::move(ready));
            return;
        }
        refreshOwnOmemoFingerprintsAsync([this, keyIds = std::move(keyIds), callback = std::move(callback)](
                                             QSet<quint32> failedBundles, XmppStatusResult status) mutable {
            if (!status.ok) {
                callback(std::move(status));
                return;
            }
            QSet<QByteArray> ownKeys;
            const auto       ownId  = omemo_->ownDeviceId();
            const auto       ownKey = omemo_->ownIdentityKey();
            for (const auto &device : omemo_->devices(bareJid(config_))) {
                if (device.protocol == XMPP::OmemoProtocol::Omemo2 && device.active && device.id != ownId
                    && !failedBundles.contains(device.id) && !device.identityKey.isEmpty()
                    && device.identityKey != ownKey) {
                    ownKeys.insert(device.identityKey);
                }
            }
            for (const auto &keyId : keyIds) {
                if (!ownKeys.contains(keyId)) {
                    const auto error = failedBundles.isEmpty()
                        ? QStringLiteral("The OMEMO key does not belong to an own device")
                        : QStringLiteral("Could not verify every own OMEMO device because %1 bundle(s) are unavailable")
                              .arg(failedBundles.size());
                    callback({ false, false, false, error, {}, XmppErrorKind::Security });
                    return;
                }
            }
            for (const auto &keyId : keyIds) {
                if (!omemo_->setTrustLevel(bareJid(config_), keyId, XMPP::EncryptionTrustLevel::ManuallyTrusted)) {
                    callback({ false,
                               false,
                               false,
                               QStringLiteral("Could not persist OMEMO trust"),
                               {},
                               XmppErrorKind::Security });
                    return;
                }
            }
            callback({ true });
        });
    });
}

void IrisXmppBackend::onlinePrivateNotesResourcesAsync(std::function<void(QStringList, QString)> callback)
{
    if (!client_) {
        callback({}, QStringLiteral("XMPP resource discovery is unavailable"));
        return;
    }

    QStringList candidates;
    const auto  ownResource = client_->resource();
    for (const auto &resource : client_->resourceList()) {
        if (!resource.name().isEmpty() && resource.name() != ownResource)
            candidates.append(resource.name());
    }
    candidates.removeDuplicates();

    auto                                       output   = std::make_shared<QStringList>();
    auto                                       failures = std::make_shared<QStringList>();
    auto                                       offset   = std::make_shared<int>(0);
    auto                                       next     = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weakNext = next;
    *next = [this, candidates = std::move(candidates), output, failures, offset, weakNext,
             callback = std::move(callback)]() mutable {
        if (*offset >= candidates.size()) {
            callback(std::move(*output), failures->join(QLatin1Char('\n')));
            return;
        }
        const auto name = candidates.at((*offset)++);
        const auto full = bareJid(config_).withResource(name);
        auto      *task = new XMPP::DiscoInfoTask(client_->rootTask());
        task->get(full);
        const auto next = weakNext.lock();
        if (!next) {
            callback(std::move(*output), QStringLiteral("XMPP resource discovery was cancelled"));
            return;
        }
        runIrisTask(task, this, config_.timeoutMs,
                    [full, name, output, failures, next](XMPP::DiscoInfoTask *task) mutable {
                        if (!task || !task->success()) {
                            failures->append(QStringLiteral("%1: %2").arg(full.full(), firstTaskError(task)));
                        } else if (task->item().features().test(IrisKeySyncTask::feature)) {
                            output->append(name);
                        }
                        (*next)();
                    });
    };
    (*next)();
}

void IrisXmppBackend::requestStorageKeyFromResourceAsync(QString fullJid, AuditCallback callback)
{
    const auto target = XMPP::Jid(fullJid);
    if (!target.isValid() || target.resource().isEmpty()) {
        XmppKeyAuditResult output;
        output.error = QStringLiteral("Invalid full JID for key recovery");
        callback(std::move(output));
        return;
    }

    const auto trustId   = newUuid();
    auto      *trustTask = new IrisKeySyncRequestTask(client_->rootTask());
    trustTask->request(target, IrisKeySyncRequestTask::RequestType::TrustBootstrap, trustId, omemo_->ownIdentityKey());
    runIrisTask(trustTask, this, config_.timeoutMs,
                [this, target, callback = std::move(callback)](IrisKeySyncRequestTask *trustTask) mutable {
                    if (!trustTask || !trustTask->success() || !trustTask->trustApproved()) {
                        XmppKeyAuditResult output;
                        output.error     = QStringLiteral("%1: OMEMO trust bootstrap failed: %2")
                                               .arg(target.full(), firstTaskError(trustTask));
                        output.errorKind = XmppErrorKind::Security;
                        callback(std::move(output));
                        return;
                    }

                    const auto requestId = newUuid();
                    auto      *request   = new IrisKeySyncRequestTask(client_->rootTask());
                    request->request(target, IrisKeySyncRequestTask::RequestType::StorageKey, requestId);
                    runIrisTask(
                        request, this, config_.timeoutMs,
                        [target, callback = std::move(callback)](IrisKeySyncRequestTask *request) mutable {
                            XmppKeyAuditResult output;
                            if (!request || !request->success()) {
                                output.error     = QStringLiteral("%1: %2").arg(target.full(), firstTaskError(request));
                                output.errorKind = XmppErrorKind::Security;
                                callback(std::move(output));
                                return;
                            }
                            auto key = SecureEnvelope::decodeRecoveryKey(request->recoveryKey(),
                                                                         KeyDerivationProfile::PrivateNotes);
                            if (!key) {
                                output.error = QStringLiteral("%1: invalid storage key response").arg(target.full());
                                output.errorKind = XmppErrorKind::Security;
                                callback(std::move(output));
                                return;
                            }
                            output.candidates.append(
                                { target.full(), key.value,
                                  SecureEnvelope::keyId(key.value, KeyDerivationProfile::PrivateNotes), 0, false });
                            output.ok = true;
                            callback(std::move(output));
                        });
                });
}

void IrisXmppBackend::auditStorageKeysAsync(AuditCallback callback)
{
    ensureOmemoReadyAsync([this, callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            XmppKeyAuditResult output;
            static_cast<XmppStatusResult &>(output) = std::move(ready);
            callback(std::move(output));
            return;
        }

        auto output = std::make_shared<XmppKeyAuditResult>();
        if (config_.masterKey.size() == SecureEnvelope::MasterKeySize) {
            output->candidates.append({ client_->resource(), config_.masterKey,
                                        SecureEnvelope::keyId(config_.masterKey, KeyDerivationProfile::PrivateNotes), 0,
                                        true });
        }
        if (omemoStorage_)
            omemoStorage_->resetSessions();

        onlinePrivateNotesResourcesAsync([this, output, callback = std::move(callback)](
                                             QStringList resources, QString discoveryError) mutable {
            auto errors = std::make_shared<QStringList>();
            if (!discoveryError.isEmpty())
                errors->append(discoveryError);
            auto                                       index            = std::make_shared<int>(0);
            auto                                       nextResource     = std::make_shared<std::function<void()>>();
            const std::weak_ptr<std::function<void()>> weakNextResource = nextResource;
            *nextResource = [this, resources = std::move(resources), output, errors, index, weakNextResource,
                             callback = std::move(callback)]() mutable {
                if (*index >= resources.size()) {
                    listNodeItemIdsAsync(
                        config_.indexNodeName(),
                        [this, output, errors, callback = std::move(callback)](QStringList      ids,
                                                                               XmppStatusResult status) mutable {
                            if (!status.ok) {
                                static_cast<XmppStatusResult &>(*output) = std::move(status);
                                callback(std::move(*output));
                                return;
                            }
                            output->totalIndexItems = ids.size();
                            auto offset             = std::make_shared<int>(0);
                            auto countNext          = std::make_shared<std::function<void()>>();
                            const std::weak_ptr<std::function<void()>> weakCountNext = countNext;
                            *countNext = [this, ids = std::move(ids), output, errors, offset, weakCountNext,
                                          callback = std::move(callback)]() mutable {
                                if (*offset >= ids.size()) {
                                    output->ok = true;
                                    if (!errors->isEmpty())
                                        output->error = QStringLiteral("Some private-note resources failed:\n%1")
                                                            .arg(errors->join(QLatin1Char('\n')));
                                    callback(std::move(*output));
                                    return;
                                }
                                const auto batch = ids.mid(*offset, BatchSize);
                                *offset += batch.size();
                                auto      *items     = pubSub_->items(bareJid(config_), config_.indexNodeName(), batch);
                                const auto countNext = weakCountNext.lock();
                                if (!countNext) {
                                    static_cast<XmppStatusResult &>(*output) = cancelledResult();
                                    callback(std::move(*output));
                                    return;
                                }
                                runIrisTask(
                                    items, this, config_.timeoutMs,
                                    [this, output, errors, countNext, callback](XMPP::PubSubItemsTask *task) mutable {
                                        if (!task || !task->success()) {
                                            static_cast<XmppStatusResult &>(*output) = taskFailure(
                                                task, QStringLiteral("Could not audit the private-note index"));
                                            callback(std::move(*output));
                                            return;
                                        }
                                        for (const auto &item : task->items()) {
                                            const auto parsed = parsePubSubItem(item);
                                            if (!parsed.valid)
                                                continue;
                                            const auto keyId     = parsed.payload.keyId;
                                            auto       candidate = std::find_if(
                                                output->candidates.begin(), output->candidates.end(),
                                                [&keyId](const auto &entry) { return entry.keyId == keyId; });
                                            if (candidate == output->candidates.end())
                                                output->candidates.append({ {}, {}, keyId, 1, false });
                                            else
                                                ++candidate->indexItemCount;
                                        }
                                        (*countNext)();
                                    });
                            };
                            (*countNext)();
                        });
                    return;
                }

                const auto resource     = resources.at((*index)++);
                const auto full         = bareJid(config_).withResource(resource).full();
                const auto nextResource = weakNextResource.lock();
                if (!nextResource) {
                    static_cast<XmppStatusResult &>(*output) = cancelledResult();
                    callback(std::move(*output));
                    return;
                }
                requestStorageKeyFromResourceAsync(
                    full, [output, errors, nextResource, full](XmppKeyAuditResult remote) mutable {
                        if (remote.ok && !remote.candidates.isEmpty()) {
                            const auto candidate = remote.candidates.constFirst();
                            auto       existing  = std::find_if(
                                output->candidates.begin(), output->candidates.end(),
                                [&candidate](const auto &entry) { return entry.keyId == candidate.keyId; });
                            if (existing == output->candidates.end()) {
                                output->candidates.append(candidate);
                            } else if (!existing->resource.split(QStringLiteral(", ")).contains(full)) {
                                if (!existing->resource.isEmpty())
                                    existing->resource += QStringLiteral(", ");
                                existing->resource += full;
                            }
                        } else if (!remote.error.isEmpty()) {
                            errors->append(remote.error);
                        }
                        (*nextResource)();
                    });
            };
            (*nextResource)();
        });
    });
}

void IrisXmppBackend::scanNodeForObsoleteItemsAsync(QString nodeName, XmppEncryptedPayload::Kind kind,
                                                    CleanupCallback callback)
{
    const auto queriedNode = nodeName;
    listNodeItemIdsAsync(
        queriedNode,
        [this, nodeName = std::move(nodeName), kind, callback = std::move(callback)](QStringList      ids,
                                                                                     XmppStatusResult status) mutable {
            auto output = std::make_shared<XmppCleanupResult>();
            if (!status.ok) {
                static_cast<XmppStatusResult &>(*output) = std::move(status);
                callback(std::move(*output));
                return;
            }
            auto                                       offset   = std::make_shared<int>(0);
            auto                                       next     = std::make_shared<std::function<void()>>();
            const std::weak_ptr<std::function<void()>> weakNext = next;
            *next = [this, nodeName, kind, ids = std::move(ids), output, offset, weakNext,
                     callback = std::move(callback)]() mutable {
                if (*offset >= ids.size()) {
                    output->ok = true;
                    callback(std::move(*output));
                    return;
                }
                const auto batch = ids.mid(*offset, BatchSize);
                *offset += batch.size();
                auto      *items = pubSub_->items(bareJid(config_), nodeName, batch);
                const auto next  = weakNext.lock();
                if (!next) {
                    static_cast<XmppStatusResult &>(*output) = cancelledResult();
                    callback(std::move(*output));
                    return;
                }
                runIrisTask(items, this, config_.timeoutMs,
                            [this, nodeName, kind, output, next, callback](XMPP::PubSubItemsTask *task) mutable {
                                if (!task || !task->success()) {
                                    static_cast<XmppStatusResult &>(*output)
                                        = taskFailure(task, QStringLiteral("Could not scan private-note PubSub items"));
                                    callback(std::move(*output));
                                    return;
                                }
                                for (const auto &item : task->items()) {
                                    const auto parsed   = parsePubSubItem(item);
                                    bool       obsolete = false;
                                    if (!parsed.valid) {
                                        obsolete = parsed.failure == XmppPayloadParseFailure::ObsoleteFormat
                                            || parsed.failure == XmppPayloadParseFailure::Malformed;
                                        if (!obsolete)
                                            ++output->protectedUnreadableItems;
                                    } else {
                                        const auto error = XmppNoteCodec::validatePayload(parsed.payload, kind,
                                                                                          config_.masterKey, nodeName);
                                        if (!error)
                                            ++output->validItems;
                                        else if (error.code == CryptoError::Corrupt)
                                            obsolete = true;
                                        else
                                            ++output->protectedUnreadableItems;
                                    }
                                    if (obsolete) {
                                        if (kind == XmppEncryptedPayload::Index)
                                            output->obsoleteIndexItemIds.append(item.id());
                                        else
                                            output->obsoleteContentItemIds.append(item.id());
                                    }
                                }
                                (*next)();
                            });
            };
            (*next)();
        });
}

void IrisXmppBackend::scanObsoleteItemsAsync(CleanupCallback callback)
{
    ensureReadyAsync([this, callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            XmppCleanupResult output;
            static_cast<XmppStatusResult &>(output) = std::move(ready);
            callback(std::move(output));
            return;
        }
        scanNodeForObsoleteItemsAsync(
            config_.indexNodeName(), XmppEncryptedPayload::Index,
            [this, callback = std::move(callback)](XmppCleanupResult index) mutable {
                if (!index.ok) {
                    callback(std::move(index));
                    return;
                }
                scanNodeForObsoleteItemsAsync(
                    config_.contentNodeName(), XmppEncryptedPayload::Content,
                    [index = std::move(index), callback = std::move(callback)](XmppCleanupResult content) mutable {
                        if (!content.ok) {
                            callback(std::move(content));
                            return;
                        }
                        XmppCleanupResult output;
                        output.ok                     = true;
                        output.obsoleteIndexItemIds   = std::move(index.obsoleteIndexItemIds);
                        output.obsoleteContentItemIds = std::move(content.obsoleteContentItemIds);
                        output.protectedUnreadableItems
                            = index.protectedUnreadableItems + content.protectedUnreadableItems;
                        output.validItems = index.validItems + content.validItems;
                        callback(std::move(output));
                    });
            });
    });
}

void IrisXmppBackend::deleteObsoleteItemsAsync(QStringList indexItemIds, QStringList contentItemIds,
                                               CleanupCallback callback)
{
    ensureReadyAsync([this, indexItemIds = std::move(indexItemIds), contentItemIds = std::move(contentItemIds),
                      callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            XmppCleanupResult output;
            static_cast<XmppStatusResult &>(output) = std::move(ready);
            callback(std::move(output));
            return;
        }
        struct Candidate {
            QString                    node;
            QString                    id;
            XmppEncryptedPayload::Kind kind;
        };
        auto candidates = std::make_shared<QList<Candidate>>();
        for (const auto &id : indexItemIds)
            candidates->append({ config_.indexNodeName(), id, XmppEncryptedPayload::Index });
        for (const auto &id : contentItemIds)
            candidates->append({ config_.contentNodeName(), id, XmppEncryptedPayload::Content });

        auto                                       output   = std::make_shared<XmppCleanupResult>();
        auto                                       offset   = std::make_shared<int>(0);
        auto                                       next     = std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> weakNext = next;
        *next = [this, candidates, output, offset, weakNext, callback = std::move(callback)]() mutable {
            if (*offset >= candidates->size()) {
                output->ok = true;
                if (output->removedItems)
                    emit remoteNodeInvalidated();
                callback(std::move(*output));
                return;
            }
            const auto candidate = candidates->at((*offset)++);
            auto      *items     = pubSub_->items(bareJid(config_), candidate.node, { candidate.id });
            const auto next      = weakNext.lock();
            if (!next) {
                static_cast<XmppStatusResult &>(*output) = cancelledResult();
                callback(std::move(*output));
                return;
            }
            runIrisTask(items, this, config_.timeoutMs,
                        [this, candidate, output, next, callback](XMPP::PubSubItemsTask *task) mutable {
                            if (!task || !task->success()) {
                                if (isItemNotFound(task)) {
                                    (*next)();
                                    return;
                                }
                                static_cast<XmppStatusResult &>(*output) = taskFailure(
                                    task, QStringLiteral("Could not re-check the private-note PubSub item"));
                                callback(std::move(*output));
                                return;
                            }
                            if (task->items().isEmpty()) {
                                (*next)();
                                return;
                            }

                            const auto parsed    = parsePubSubItem(task->items().constFirst());
                            bool       removable = !parsed.valid && parsed.isObsoleteOrMalformed();
                            if (parsed.valid) {
                                const auto error = XmppNoteCodec::validatePayload(parsed.payload, candidate.kind,
                                                                                  config_.masterKey, candidate.node);
                                removable        = error.code == CryptoError::Corrupt;
                            }
                            if (!removable) {
                                ++output->protectedUnreadableItems;
                                (*next)();
                                return;
                            }
                            retractItemAsync(candidate.node, candidate.id,
                                             [output, next, callback](XmppStatusResult status) mutable {
                                                 if (!status.ok) {
                                                     static_cast<XmppStatusResult &>(*output) = std::move(status);
                                                     callback(std::move(*output));
                                                     return;
                                                 }
                                                 ++output->removedItems;
                                                 (*next)();
                                             });
                        });
        };
        (*next)();
    });
}

void IrisXmppBackend::rekeyStorageAsync(QList<QByteArray> keys, QByteArray canonicalKey, RekeyCallback callback)
{
    ensureReadyAsync([this, keys = std::move(keys), canonicalKey = std::move(canonicalKey),
                      callback = std::move(callback)](XmppStatusResult ready) mutable {
        if (!ready.ok) {
            XmppRekeyResult output;
            static_cast<XmppStatusResult &>(output) = std::move(ready);
            callback(std::move(output));
            return;
        }
        if (canonicalKey.size() != SecureEnvelope::MasterKeySize) {
            XmppRekeyResult output;
            output.error = QStringLiteral("The selected canonical XMPP storage key is invalid");
            callback(std::move(output));
            return;
        }
        auto keyring = std::make_shared<QHash<QByteArray, QByteArray>>();
        for (const auto &key : keys) {
            if (key.size() == SecureEnvelope::MasterKeySize)
                keyring->insert(SecureEnvelope::keyId(key, KeyDerivationProfile::PrivateNotes), key);
        }
        keyring->insert(SecureEnvelope::keyId(canonicalKey, KeyDerivationProfile::PrivateNotes), canonicalKey);

        listNodeItemIdsAsync(
            config_.indexNodeName(),
            [this, canonicalKey = std::move(canonicalKey), keyring,
             callback = std::move(callback)](QStringList ids, XmppStatusResult status) mutable {
                auto output = std::make_shared<XmppRekeyResult>();
                if (!status.ok) {
                    static_cast<XmppStatusResult &>(*output) = std::move(status);
                    callback(std::move(*output));
                    return;
                }
                output->total                                       = ids.size();
                auto                                       offset   = std::make_shared<int>(0);
                auto                                       next     = std::make_shared<std::function<void()>>();
                const std::weak_ptr<std::function<void()>> weakNext = next;
                *next = [this, ids = std::move(ids), canonicalKey, keyring, output, offset, weakNext,
                         callback = std::move(callback)]() mutable {
                    if (*offset >= ids.size()) {
                        output->ok = output->inaccessibleNoteIds.isEmpty();
                        if (!output->ok)
                            output->error = QStringLiteral("Some notes use storage keys that are not available");
                        callback(std::move(*output));
                        return;
                    }
                    const auto id   = ids.at((*offset)++);
                    const auto next = weakNext.lock();
                    if (!next) {
                        static_cast<XmppStatusResult &>(*output) = cancelledResult();
                        callback(std::move(*output));
                        return;
                    }
                    fetchPayloadAsync(
                        config_.indexNodeName(), id,
                        [this, id, canonicalKey, keyring, output, next, callback](
                            std::optional<XmppEncryptedPayload> indexPayload, XmppStatusResult indexStatus) mutable {
                            if (!indexStatus.ok || !indexPayload) {
                                if (indexStatus.notFound) {
                                    output->inaccessibleNoteIds.append(id);
                                    (*next)();
                                    return;
                                }
                                static_cast<XmppStatusResult &>(*output) = std::move(indexStatus);
                                callback(std::move(*output));
                                return;
                            }
                            fetchPayloadAsync(
                                config_.contentNodeName(), id,
                                [this, id, canonicalKey, keyring, output, next, callback,
                                 indexPayload
                                 = std::move(indexPayload)](std::optional<XmppEncryptedPayload> contentPayload,
                                                            XmppStatusResult                    contentStatus) mutable {
                                    if (!contentStatus.ok || !contentPayload) {
                                        if (contentStatus.notFound) {
                                            output->inaccessibleNoteIds.append(id);
                                            (*next)();
                                            return;
                                        }
                                        static_cast<XmppStatusResult &>(*output) = std::move(contentStatus);
                                        callback(std::move(*output));
                                        return;
                                    }
                                    const auto indexKey   = keyring->value(indexPayload->keyId);
                                    const auto contentKey = keyring->value(contentPayload->keyId);
                                    if (indexKey.isEmpty() || contentKey.isEmpty()) {
                                        output->inaccessibleNoteIds.append(id);
                                        (*next)();
                                        return;
                                    }
                                    auto index
                                        = XmppNoteCodec::decodeIndex(*indexPayload, indexKey, config_.indexNodeName());
                                    if (!index) {
                                        output->inaccessibleNoteIds.append(id);
                                        (*next)();
                                        return;
                                    }
                                    auto note = XmppNoteCodec::decodeContent(*contentPayload, contentKey,
                                                                             config_.contentNodeName(), index.value);
                                    if (!note) {
                                        output->inaccessibleNoteIds.append(id);
                                        (*next)();
                                        return;
                                    }
                                    auto newContent = XmppNoteCodec::encodeContent(note.value, canonicalKey,
                                                                                   config_.contentNodeName());
                                    auto newIndex
                                        = XmppNoteCodec::encodeIndex(note.value, canonicalKey, config_.indexNodeName());
                                    if (!newContent || !newIndex) {
                                        output->error = !newContent ? newContent.error.message : newIndex.error.message;
                                        callback(std::move(*output));
                                        return;
                                    }
                                    publishPayloadAsync(
                                        config_.contentNodeName(), std::move(newContent.value),
                                        [this, output, next, callback,
                                         newIndex = std::move(newIndex.value)](XmppStatusResult status) mutable {
                                            if (!status.ok) {
                                                static_cast<XmppStatusResult &>(*output) = std::move(status);
                                                callback(std::move(*output));
                                                return;
                                            }
                                            publishPayloadAsync(
                                                config_.indexNodeName(), std::move(newIndex),
                                                [output, next, callback](XmppStatusResult status) mutable {
                                                    if (!status.ok) {
                                                        static_cast<XmppStatusResult &>(*output) = std::move(status);
                                                        callback(std::move(*output));
                                                        return;
                                                    }
                                                    ++output->migrated;
                                                    (*next)();
                                                });
                                        });
                                });
                        });
                };
                (*next)();
            });
    });
}

void IrisXmppBackend::handleKeySyncTrustRequest(const QString &requestId, const QString &from,
                                                const QByteArray &senderKey)
{
    if (!keySyncTask_ || XMPP::Jid(from).bare() != bareJid(config_).bare()) {
        if (keySyncTask_)
            keySyncTask_->reject(requestId);
        return;
    }

    refreshOwnOmemoFingerprintsAsync([this, requestId, senderKey](QSet<quint32>    failedBundles,
                                                                  XmppStatusResult status) {
        if (!status.ok) {
            keySyncTask_->reject(requestId);
            emit backendError(status.error);
            return;
        }
        const auto ownId   = omemo_->ownDeviceId();
        const auto devices = omemo_->devices(bareJid(config_));
        const bool known
            = std::any_of(devices.cbegin(), devices.cend(), [ownId, &senderKey, &failedBundles](const auto &device) {
                  return device.protocol == XMPP::OmemoProtocol::Omemo2 && device.active && device.id != ownId
                      && !failedBundles.contains(device.id) && device.identityKey == senderKey;
              });
        if (!known) {
            keySyncTask_->reject(requestId);
            emit backendError(
                failedBundles.isEmpty()
                    ? QStringLiteral("Ignored a trust request from an unknown OMEMO device")
                    : QStringLiteral("Could not verify a trust request because %1 OMEMO bundle(s) are unavailable")
                          .arg(failedBundles.size()));
            return;
        }
        finishKeySyncTrustRequest(requestId, senderKey);
    });
}

void IrisXmppBackend::finishKeySyncTrustRequest(const QString &requestId, const QByteArray &senderKey)
{
    const auto trust = omemo_->trustLevel(bareJid(config_), senderKey);
    if (trustedForRecovery(trust)) {
        keySyncTask_->replyTrustApproved(requestId);
        return;
    }
    pendingInboundKeyRequests_.insert(requestId, { senderKey, true });
    emit keySyncTrustRequested(requestId, senderKey);
}

void IrisXmppBackend::handleKeySyncRequest(const QString &requestId, const QString &from, const QByteArray &senderKey)
{
    if (!keySyncTask_ || XMPP::Jid(from).bare() != bareJid(config_).bare()) {
        if (keySyncTask_)
            keySyncTask_->reject(requestId);
        return;
    }
    const auto trust = omemo_->trustLevel(bareJid(config_), senderKey);
    if (trustedForRecovery(trust)) {
        if (config_.masterKey.size() == SecureEnvelope::MasterKeySize) {
            keySyncTask_->replyWithKey(
                requestId, SecureEnvelope::encodeRecoveryKey(config_.masterKey, KeyDerivationProfile::PrivateNotes));
        } else {
            keySyncTask_->reject(requestId);
        }
        return;
    }

    refreshOwnOmemoFingerprintsAsync(
        [this, requestId, senderKey](QSet<quint32> failedBundles, XmppStatusResult status) {
            if (!status.ok) {
                keySyncTask_->reject(requestId);
                emit backendError(status.error);
                return;
            }
            const auto devices = omemo_->devices(bareJid(config_));
            const bool known
                = std::any_of(devices.cbegin(), devices.cend(), [&senderKey, &failedBundles](const auto &device) {
                      return device.protocol == XMPP::OmemoProtocol::Omemo2 && device.active
                          && !failedBundles.contains(device.id) && device.identityKey == senderKey;
                  });
            if (!known) {
                keySyncTask_->reject(requestId);
                emit backendError(
                    failedBundles.isEmpty()
                        ? QStringLiteral("Ignored a storage-key request from an unknown OMEMO device")
                        : QStringLiteral(
                              "Could not verify a storage-key request because %1 OMEMO bundle(s) are unavailable")
                              .arg(failedBundles.size()));
                return;
            }
            pendingInboundKeyRequests_.insert(requestId, { senderKey, false });
            emit keySyncTrustRequested(requestId, senderKey);
        });
}

void IrisXmppBackend::approveKeySyncRequest(QString requestId)
{
    const auto pending = pendingInboundKeyRequests_.take(requestId);
    if (pending.senderKey.isEmpty() || !keySyncTask_ || !omemo_)
        return;
    if (!omemo_->setTrustLevel(bareJid(config_), pending.senderKey, XMPP::EncryptionTrustLevel::ManuallyTrusted)) {
        emit backendError(QStringLiteral("Could not persist OMEMO trust for the key-sync request"));
        keySyncTask_->reject(requestId);
        return;
    }
    if (pending.trustBootstrap) {
        keySyncTask_->replyTrustApproved(requestId);
        return;
    }
    if (config_.masterKey.size() == SecureEnvelope::MasterKeySize) {
        keySyncTask_->replyWithKey(
            requestId, SecureEnvelope::encodeRecoveryKey(config_.masterKey, KeyDerivationProfile::PrivateNotes));
    } else {
        keySyncTask_->reject(requestId);
    }
}

void IrisXmppBackend::rejectKeySyncRequest(QString requestId)
{
    pendingInboundKeyRequests_.remove(requestId);
    if (keySyncTask_)
        keySyncTask_->reject(requestId);
}

} // namespace AnyKeep
