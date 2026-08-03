#ifndef XMPPPEPEXTENSION_H
#define XMPPPEPEXTENSION_H

#include "xmppdto.h"

#include <QXmppClientExtension.h>
#include <QXmppPubSubEventHandler.h>

namespace AnyKeep {

/**
 * @brief Receives and validates AnyKeep PEP notifications.
 *
 * Only events from the configured own bare JID and note index node are
 * accepted. Valid encrypted items are converted to backend DTOs; ambiguous or
 * malformed events invalidate the cache instead of applying partial state.
 */
class XmppPepExtension final : public QXmppClientExtension, public QXmppPubSubEventHandler {
    Q_OBJECT

public:
    XmppPepExtension();

    void setOwnBareJid(const QString &jid);
    void setNodeName(const QString &nodeName);

    QStringList discoveryFeatures() const override;
    bool handlePubSubEvent(const QDomElement &element, const QString &pubSubService, const QString &nodeName) override;

signals:
    void payloadPublished(const AnyKeep::XmppEncryptedPayload &payload);
    void noteRetracted(const QString &id);
    void nodeInvalidated();
    void malformedItem(const QString &error);

private:
    QString ownBareJid_;
    QString nodeName_;
};

} // namespace AnyKeep

#endif // XMPPPEPEXTENSION_H
