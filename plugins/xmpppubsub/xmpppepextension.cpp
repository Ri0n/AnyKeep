#include "xmpppepextension.h"

#include "qtnotepubsubitem.h"

#include <QDebug>
#include <QDomElement>
#include <QXmppPubSubEvent.h>
#include <QXmppUtils.h>

namespace QtNote {

XmppPepExtension::XmppPepExtension() = default;

void XmppPepExtension::setOwnBareJid(const QString &jid) { ownBareJid_ = QXmppUtils::jidToBareJid(jid); }

void XmppPepExtension::setNodeName(const QString &nodeName) { nodeName_ = nodeName; }

QStringList XmppPepExtension::discoveryFeatures() const
{
    if (nodeName_.isEmpty()) {
        return {};
    }
    return { nodeName_ + QStringLiteral("+notify") };
}

bool XmppPepExtension::handlePubSubEvent(const QDomElement &element, const QString &pubSubService,
                                         const QString &nodeName)
{
    if (nodeName != nodeName_) {
        return false;
    }

    // XEP-0223 requires private-data events to originate from our own PEP
    // service. Some servers omit the service JID, which is valid here.
    if (!pubSubService.isEmpty() && QXmppUtils::jidToBareJid(pubSubService) != ownBareJid_) {
        qWarning().noquote() << "Ignoring QtNote PEP event from unexpected service" << pubSubService << "for"
                             << ownBareJid_;
        return true;
    }

    if (!QXmppPubSubEvent<QtNotePubSubItem>::isPubSubEvent(element)) {
        const auto error = QStringLiteral("Malformed QtNote PubSub event for node %1").arg(nodeName);
        qWarning().noquote() << error;
        emit malformedItem(error);
        emit nodeInvalidated();
        return true;
    }

    QXmppPubSubEvent<QtNotePubSubItem> event;
    event.parse(element);

    switch (event.eventType()) {
    case QXmppPubSubEventBase::Items: {
        qInfo().noquote() << "QtNote PEP items event received: node=" << nodeName << "items=" << event.items().size();
        bool refreshRequired = event.items().isEmpty();
        for (const auto &item : event.items()) {
            if (item.isValid()) {
                emit payloadPublished(item.payload());
            } else {
                refreshRequired = true;
                qWarning().noquote() << "Ignoring unreadable QtNote PEP item" << item.id() << ':' << item.parseError();
                emit malformedItem(item.parseError());
            }
        }
        if (refreshRequired)
            emit nodeInvalidated();
        break;
    }
    case QXmppPubSubEventBase::Retract:
        for (const auto &id : event.retractIds()) {
            emit noteRetracted(id);
        }
        break;
    case QXmppPubSubEventBase::Delete:
    case QXmppPubSubEventBase::Purge:
        emit nodeInvalidated();
        break;
    case QXmppPubSubEventBase::Configuration:
    case QXmppPubSubEventBase::Subscription:
        break;
    }

    return true;
}

} // namespace QtNote
