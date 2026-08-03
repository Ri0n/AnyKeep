#include "xmppkeysyncextension.h"

#include <QDebug>
#include <QDomElement>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmppClient.h>
#include <QXmppElement.h>
#include <QXmppTask.h>

namespace AnyKeep {
namespace {
    QDomElement keySyncElement(const QDomElement &iq)
    {
        const auto child = iq.firstChildElement();
        const auto name  = child.localName().isEmpty() ? child.tagName() : child.localName();
        const auto ns
            = child.namespaceURI().isEmpty() ? child.attribute(QStringLiteral("xmlns")) : child.namespaceURI();
        return ns == XmppKeySyncExtension::feature && name == QStringLiteral("key-sync") ? child : QDomElement {};
    }

    QXmppElement payloadElement(const QJsonObject &payload)
    {
        QXmppElement element;
        element.setTagName(QStringLiteral("key-sync"));
        element.setAttribute(QStringLiteral("xmlns"), XmppKeySyncExtension::feature);
        element.setValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        return element;
    }

    QJsonObject parsePayload(const QDomElement &element)
    {
        QJsonParseError error;
        const auto      document = QJsonDocument::fromJson(element.text().toUtf8(), &error);
        return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject {};
    }
}

const QString XmppKeySyncExtension::feature = QStringLiteral("urn:xmpp:private-notes:key-sync:0");

QStringList XmppKeySyncExtension::discoveryFeatures() const { return { feature }; }

bool XmppKeySyncExtension::handleStanza(const QDomElement &stanza, const std::optional<QXmppE2eeMetadata> &metadata)
{
    if (stanza.tagName() != QStringLiteral("iq") || stanza.attribute(QStringLiteral("type")) != QStringLiteral("set"))
        return false;
    const auto element = keySyncElement(stanza);
    if (element.isNull())
        return false;
    const auto payload   = parsePayload(element);
    const auto requestId = payload.value(QStringLiteral("requestId")).toString();
    const auto type      = payload.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("trust-request") && !requestId.isEmpty() && !metadata) {
        const auto senderKey = QByteArray::fromBase64(payload.value(QStringLiteral("senderKey")).toString().toLatin1());
        if (senderKey.isEmpty()) {
            qWarning() << "Ignoring key-sync trust bootstrap without a sender key: id=" << requestId;
            return true;
        }
        pendingRequests_.insert(
            requestId, { stanza.attribute(QStringLiteral("from")), stanza.attribute(QStringLiteral("id")), {} });
        qInfo().noquote() << "Key-sync trust bootstrap received: id=" << requestId
                          << "from=" << stanza.attribute(QStringLiteral("from"));
        emit trustRequestReceived(requestId, stanza.attribute(QStringLiteral("from")), senderKey);
        return true;
    }
    if (type != QStringLiteral("request") || requestId.isEmpty() || !metadata) {
        qWarning() << "Ignoring malformed or unencrypted key-sync request: id=" << requestId
                   << "has-e2ee-metadata=" << metadata.has_value();
        return true;
    }
    qInfo().noquote() << "Key-sync request decrypted: id=" << requestId
                      << "from=" << stanza.attribute(QStringLiteral("from"))
                      << "sender-key-size=" << metadata->senderKey().size();
    pendingRequests_.insert(
        requestId, { stanza.attribute(QStringLiteral("from")), stanza.attribute(QStringLiteral("id")), metadata });
    emit requestReceived(requestId, stanza.attribute(QStringLiteral("from")), metadata->senderKey());
    return true;
}

QXmppIq XmppKeySyncExtension::makeTrustRequest(const QString &to, const QString &requestId,
                                               const QByteArray &senderKey) const
{
    QXmppIq iq(QXmppIq::Set);
    iq.setTo(to);
    iq.setId(requestId);
    iq.setExtensions(
        { payloadElement({ { QStringLiteral("type"), QStringLiteral("trust-request") },
                           { QStringLiteral("requestId"), requestId },
                           { QStringLiteral("senderKey"), QString::fromLatin1(senderKey.toBase64()) } }) });
    return iq;
}

QXmppIq XmppKeySyncExtension::makeRequest(const QString &to, const QString &requestId) const
{
    QXmppIq iq(QXmppIq::Set);
    iq.setTo(to);
    iq.setId(requestId);
    iq.setExtensions({ payloadElement(
        { { QStringLiteral("type"), QStringLiteral("request") }, { QStringLiteral("requestId"), requestId } }) });
    return iq;
}

void XmppKeySyncExtension::replyWithKey(const QString &requestId, const QString &recoveryKey)
{
    const auto pending = pendingRequests_.take(requestId);
    if (pending.iqId.isEmpty()) {
        qWarning() << "Cannot reply to key-sync request because it is no longer pending: id=" << requestId;
        return;
    }
    QXmppIq response(QXmppIq::Result);
    response.setTo(pending.from);
    response.setId(pending.iqId);
    response.setExtensions({ payloadElement({ { QStringLiteral("type"), QStringLiteral("response") },
                                              { QStringLiteral("requestId"), requestId },
                                              { QStringLiteral("recoveryKey"), recoveryKey } }) });
    qInfo().noquote() << "Sending encrypted key-sync response: id=" << requestId << "to=" << pending.from;
    client()->reply(std::move(response), pending.metadata);
}

void XmppKeySyncExtension::replyTrustApproved(const QString &requestId)
{
    const auto pending = pendingRequests_.take(requestId);
    if (pending.iqId.isEmpty())
        return;
    QXmppIq response(QXmppIq::Result);
    response.setTo(pending.from);
    response.setId(pending.iqId);
    response.setExtensions({ payloadElement({ { QStringLiteral("type"), QStringLiteral("trust-approved") },
                                              { QStringLiteral("requestId"), requestId } }) });
    qInfo().noquote() << "Sending key-sync trust bootstrap acknowledgement: id=" << requestId << "to=" << pending.from;
    client()->reply(std::move(response), {});
}

void XmppKeySyncExtension::reject(const QString &requestId) { pendingRequests_.remove(requestId); }

QString XmppKeySyncExtension::responseRecoveryKey(const QDomElement &iq, const QString &requestId)
{
    const auto payload = parsePayload(keySyncElement(iq));
    if (payload.value(QStringLiteral("type")).toString() != QStringLiteral("response")
        || payload.value(QStringLiteral("requestId")).toString() != requestId) {
        return {};
    }
    return payload.value(QStringLiteral("recoveryKey")).toString();
}

bool XmppKeySyncExtension::isTrustApproved(const QDomElement &iq, const QString &requestId)
{
    const auto payload = parsePayload(keySyncElement(iq));
    return payload.value(QStringLiteral("type")).toString() == QStringLiteral("trust-approved")
        && payload.value(QStringLiteral("requestId")).toString() == requestId;
}

} // namespace AnyKeep
