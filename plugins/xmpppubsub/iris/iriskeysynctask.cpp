#include "iriskeysynctask.h"

#include <iris/xmpp_client.h>
#include <iris/xmpp_omemo.h>

#include <QDomDocument>
#include <QJsonDocument>
#include <QJsonObject>

namespace AnyKeep {
namespace {

    QString localName(const QDomElement &element)
    {
        return element.localName().isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : element.localName();
    }

    QDomElement keySyncElement(const QDomElement &iq)
    {
        for (auto child = iq.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            const auto ns
                = child.namespaceURI().isEmpty() ? child.attribute(QStringLiteral("xmlns")) : child.namespaceURI();
            if (localName(child) == QStringLiteral("key-sync") && ns == IrisKeySyncTask::feature)
                return child;
        }
        return {};
    }

    QJsonObject parsePayload(const QDomElement &element)
    {
        if (element.isNull())
            return {};
        QJsonParseError error;
        const auto      document = QJsonDocument::fromJson(element.text().toUtf8(), &error);
        return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject {};
    }

    QDomElement appendPayload(QDomDocument *document, QDomElement iq, const QJsonObject &payload)
    {
        auto element = document->createElementNS(IrisKeySyncTask::feature, QStringLiteral("key-sync"));
        element.appendChild(
            document->createTextNode(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))));
        iq.appendChild(element);
        return iq;
    }

    QDomElement makeIq(QDomDocument *document, const QString &type, const QString &to, const QString &id)
    {
        auto iq = document->createElement(QStringLiteral("iq"));
        iq.setAttribute(QStringLiteral("type"), type);
        iq.setAttribute(QStringLiteral("id"), id);
        if (!to.isEmpty())
            iq.setAttribute(QStringLiteral("to"), to);
        return iq;
    }

} // namespace

const QString IrisKeySyncTask::feature = QStringLiteral("urn:xmpp:private-notes:key-sync:0");

IrisKeySyncTask::IrisKeySyncTask(XMPP::Client *client) : XMPP::Task(client->rootTask()) {}

bool IrisKeySyncTask::take(const QDomElement &stanza)
{
    if (localName(stanza) != QStringLiteral("iq") || stanza.attribute(QStringLiteral("type")) != QStringLiteral("set"))
        return false;

    const auto element = keySyncElement(stanza);
    if (element.isNull())
        return false;

    const auto payload   = parsePayload(element);
    const auto requestId = payload.value(QStringLiteral("requestId")).toString();
    const auto type      = payload.value(QStringLiteral("type")).toString();
    const auto metadata  = encryptionMetadata();

    if (type == QStringLiteral("trust-request") && !requestId.isEmpty() && !metadata) {
        const auto senderKey = QByteArray::fromBase64(payload.value(QStringLiteral("senderKey")).toString().toLatin1());
        if (senderKey.isEmpty())
            return true;
        pendingRequests_.insert(
            requestId, { stanza.attribute(QStringLiteral("from")), stanza.attribute(QStringLiteral("id")), {} });
        emit trustRequestReceived(requestId, stanza.attribute(QStringLiteral("from")), senderKey);
        return true;
    }

    if (type != QStringLiteral("request") || requestId.isEmpty() || !metadata)
        return true;

    pendingRequests_.insert(
        requestId, { stanza.attribute(QStringLiteral("from")), stanza.attribute(QStringLiteral("id")), *metadata });
    emit requestReceived(requestId, stanza.attribute(QStringLiteral("from")), metadata->senderKey);
    return true;
}

void IrisKeySyncTask::replyWithKey(const QString &requestId, const QString &recoveryKey)
{
    const auto pending = pendingRequests_.take(requestId);
    if (pending.iqId.isEmpty() || !pending.metadata)
        return;

    auto  iq  = appendPayload(doc(), makeIq(doc(), QStringLiteral("result"), pending.from, pending.iqId),
                              { { QStringLiteral("type"), QStringLiteral("response") },
                                { QStringLiteral("requestId"), requestId },
                                { QStringLiteral("recoveryKey"), recoveryKey } });
    auto *job = client()->replyEncrypted(iq, *pending.metadata);
    if (!job)
        return;
    if (job->isFinished())
        job->deleteLater();
    else
        connect(job, &XMPP::EncryptionJob::finished, job, &QObject::deleteLater);
}

void IrisKeySyncTask::replyTrustApproved(const QString &requestId)
{
    const auto pending = pendingRequests_.take(requestId);
    if (pending.iqId.isEmpty())
        return;

    auto iq = appendPayload(
        doc(), makeIq(doc(), QStringLiteral("result"), pending.from, pending.iqId),
        { { QStringLiteral("type"), QStringLiteral("trust-approved") }, { QStringLiteral("requestId"), requestId } });
    client()->send(iq);
}

void IrisKeySyncTask::reject(const QString &requestId) { pendingRequests_.remove(requestId); }

IrisKeySyncRequestTask::IrisKeySyncRequestTask(XMPP::Task *parent) : XMPP::Task(parent) {}

void IrisKeySyncRequestTask::request(const XMPP::Jid &to, RequestType type, const QString &requestId,
                                     const QByteArray &senderKey)
{
    to_        = to;
    type_      = type;
    requestId_ = requestId;
    senderKey_ = senderKey;
    recoveryKey_.clear();
    trustApproved_ = false;
}

void IrisKeySyncRequestTask::onGo()
{
    QJsonObject payload { { QStringLiteral("requestId"), requestId_ } };
    if (type_ == RequestType::TrustBootstrap) {
        payload.insert(QStringLiteral("type"), QStringLiteral("trust-request"));
        payload.insert(QStringLiteral("senderKey"), QString::fromLatin1(senderKey_.toBase64()));
    } else {
        payload.insert(QStringLiteral("type"), QStringLiteral("request"));
    }

    auto iq = appendPayload(doc(), makeIq(doc(), QStringLiteral("set"), to_.full(), id()), payload);
    if (type_ == RequestType::TrustBootstrap) {
        send(iq);
        return;
    }

    XMPP::EncryptionContext context;
    context.recipients.append(to_);
    auto *job = client()->sendEncrypted(iq, XMPP::OmemoEncryption::methodId(), context);
    if (!job) {
        setError(1, QStringLiteral("Could not start encryption for the key-sync request"));
        return;
    }
    const auto complete = [this, job]() {
        if (!job->success())
            setError(1,
                     job->errorString().isEmpty() ? QStringLiteral("Could not encrypt the key-sync request")
                                                  : job->errorString());
        job->deleteLater();
    };
    if (job->isFinished())
        complete();
    else
        connect(job, &XMPP::EncryptionJob::finished, this, complete);
}

bool IrisKeySyncRequestTask::take(const QDomElement &stanza)
{
    if (!iqVerify(stanza, to_, id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) != QStringLiteral("result")) {
        setError(stanza);
        return true;
    }

    const auto payload = parsePayload(keySyncElement(stanza));
    if (payload.value(QStringLiteral("requestId")).toString() != requestId_) {
        setError(1, QStringLiteral("Mismatching key-sync response"));
        return true;
    }
    const auto type = payload.value(QStringLiteral("type")).toString();
    if (type_ == RequestType::TrustBootstrap) {
        trustApproved_ = type == QStringLiteral("trust-approved");
        if (!trustApproved_) {
            setError(1, QStringLiteral("The remote resource did not approve OMEMO trust bootstrap"));
            return true;
        }
    } else {
        if (type != QStringLiteral("response")) {
            setError(1, QStringLiteral("Invalid key-sync response"));
            return true;
        }
        recoveryKey_ = payload.value(QStringLiteral("recoveryKey")).toString();
    }
    setSuccess();
    return true;
}

} // namespace AnyKeep
