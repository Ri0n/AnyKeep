#ifndef XMPPOMEMOPUBSUBITEMS_H
#define XMPPOMEMOPUBSUBITEMS_H

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QXmppPubSubBaseItem.h>

namespace AnyKeep {

/// One entry in the XEP-0384 OMEMO device list.
struct XmppOmemoListedDevice {
    QString label;
    QString id;
    QString labelSignature;
};

/** @brief Parser/serializer for an XEP-0384 OMEMO device-list PubSub item. */
class XmppOmemoDeviceListItem final : public QXmppPubSubBaseItem {
public:
    static bool                         isItem(const QDomElement &element);
    const QList<XmppOmemoListedDevice> &devices() const { return devices_; }
    void setDevices(QList<XmppOmemoListedDevice> devices) { devices_ = std::move(devices); }

protected:
    void parsePayload(const QDomElement &element) override;
    void serializePayload(QXmlStreamWriter *writer) const override;

private:
    QList<XmppOmemoListedDevice> devices_;
};

/**
 * @brief Parser/serializer for an XEP-0384 OMEMO bundle PubSub item.
 *
 * repairedFrom() is an interoperability workaround for QXmpp versions that can
 * republish an incomplete bundle after a prekey is consumed. It combines the
 * freshly published bundle with stable fields from a previously valid bundle
 * while excluding prekeys known to have been consumed.
 */
class XmppOmemoBundleItem final : public QXmppPubSubBaseItem {
public:
    static bool                        isItem(const QDomElement &element);
    const QByteArray                  &identityKey() const { return identityKey_; }
    const QHash<uint32_t, QByteArray> &publicPreKeys() const { return publicPreKeys_; }
    XmppOmemoBundleItem                repairedFrom(const XmppOmemoBundleItem &validBundle,
                                                    const QSet<uint32_t>      &consumedPreKeyIds) const;

protected:
    void parsePayload(const QDomElement &element) override;
    void serializePayload(QXmlStreamWriter *writer) const override;

private:
    QByteArray                  identityKey_;
    QByteArray                  signedPreKey_;
    uint32_t                    signedPreKeyId_ { 0 };
    QByteArray                  signedPreKeySignature_;
    QHash<uint32_t, QByteArray> publicPreKeys_;
};

} // namespace AnyKeep

#endif // XMPPOMEMOPUBSUBITEMS_H
