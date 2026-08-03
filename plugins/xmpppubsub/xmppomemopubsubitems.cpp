#include "xmppomemopubsubitems.h"

#include <QDomElement>
#include <QXmlStreamWriter>

namespace AnyKeep {
namespace {
    const QString OmemoNamespace = QStringLiteral("urn:xmpp:omemo:2");

    QString localName(const QDomElement &element)
    {
        return element.localName().isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : element.localName();
    }

    bool hasPayload(const QDomElement &item, const QString &name)
    {
        if (!QXmppPubSubBaseItem::isItem(item))
            return false;
        const auto payload = item.firstChildElement();
        return !payload.isNull() && localName(payload) == name && payload.namespaceURI() == OmemoNamespace;
    }
}

bool XmppOmemoDeviceListItem::isItem(const QDomElement &element)
{
    return hasPayload(element, QStringLiteral("devices"));
}

void XmppOmemoDeviceListItem::parsePayload(const QDomElement &element)
{
    devices_.clear();
    for (auto device = element.firstChildElement(); !device.isNull(); device = device.nextSiblingElement()) {
        if (localName(device) != QStringLiteral("device") || device.namespaceURI() != OmemoNamespace)
            continue;
        const auto id   = device.attribute(QStringLiteral("id"));
        bool       idOk = false;
        id.toUInt(&idOk);
        if (idOk)
            devices_.append(
                { device.attribute(QStringLiteral("label")), id, device.attribute(QStringLiteral("labelsig")) });
    }
}

void XmppOmemoDeviceListItem::serializePayload(QXmlStreamWriter *writer) const
{
    writer->writeStartElement(QStringLiteral("devices"));
    writer->writeDefaultNamespace(OmemoNamespace);
    for (const auto &device : devices_) {
        writer->writeStartElement(QStringLiteral("device"));
        writer->writeAttribute(QStringLiteral("id"), device.id);
        if (!device.label.isEmpty())
            writer->writeAttribute(QStringLiteral("label"), device.label);
        if (!device.labelSignature.isEmpty())
            writer->writeAttribute(QStringLiteral("labelsig"), device.labelSignature);
        writer->writeEndElement();
    }
    writer->writeEndElement();
}

bool XmppOmemoBundleItem::isItem(const QDomElement &element) { return hasPayload(element, QStringLiteral("bundle")); }

void XmppOmemoBundleItem::parsePayload(const QDomElement &element)
{
    identityKey_.clear();
    signedPreKey_.clear();
    signedPreKeyId_ = 0;
    signedPreKeySignature_.clear();
    publicPreKeys_.clear();
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.namespaceURI() != OmemoNamespace)
            continue;
        const auto name = localName(child);
        if (name == QStringLiteral("ik")) {
            identityKey_ = QByteArray::fromBase64(child.text().trimmed().toLatin1());
        } else if (name == QStringLiteral("spk")) {
            signedPreKeyId_ = child.attribute(QStringLiteral("id")).toUInt();
            signedPreKey_   = QByteArray::fromBase64(child.text().trimmed().toLatin1());
        } else if (name == QStringLiteral("spks")) {
            signedPreKeySignature_ = QByteArray::fromBase64(child.text().trimmed().toLatin1());
        } else if (name == QStringLiteral("prekeys")) {
            for (auto preKey = child.firstChildElement(); !preKey.isNull(); preKey = preKey.nextSiblingElement()) {
                if (localName(preKey) != QStringLiteral("pk") || preKey.namespaceURI() != OmemoNamespace)
                    continue;
                bool       idOk = false;
                const auto id   = preKey.attribute(QStringLiteral("id")).toUInt(&idOk);
                if (idOk)
                    publicPreKeys_.insert(id, QByteArray::fromBase64(preKey.text().trimmed().toLatin1()));
            }
        }
    }
}

void XmppOmemoBundleItem::serializePayload(QXmlStreamWriter *writer) const
{
    writer->writeStartElement(QStringLiteral("bundle"));
    writer->writeDefaultNamespace(OmemoNamespace);
    writer->writeTextElement(QStringLiteral("ik"), QString::fromLatin1(identityKey_.toBase64()));
    writer->writeStartElement(QStringLiteral("spk"));
    writer->writeAttribute(QStringLiteral("id"), QString::number(signedPreKeyId_));
    writer->writeCharacters(QString::fromLatin1(signedPreKey_.toBase64()));
    writer->writeEndElement();
    writer->writeTextElement(QStringLiteral("spks"), QString::fromLatin1(signedPreKeySignature_.toBase64()));
    writer->writeStartElement(QStringLiteral("prekeys"));
    for (auto preKey = publicPreKeys_.cbegin(); preKey != publicPreKeys_.cend(); ++preKey) {
        writer->writeStartElement(QStringLiteral("pk"));
        writer->writeAttribute(QStringLiteral("id"), QString::number(preKey.key()));
        writer->writeCharacters(QString::fromLatin1(preKey.value().toBase64()));
        writer->writeEndElement();
    }
    writer->writeEndElement();
    writer->writeEndElement();
}

XmppOmemoBundleItem XmppOmemoBundleItem::repairedFrom(const XmppOmemoBundleItem &validBundle,
                                                      const QSet<uint32_t>      &consumedPreKeyIds) const
{
    auto repaired = *this;
    if (repaired.identityKey_.isEmpty())
        repaired.identityKey_ = validBundle.identityKey_;
    if (repaired.signedPreKey_.isEmpty()) {
        repaired.signedPreKey_          = validBundle.signedPreKey_;
        repaired.signedPreKeyId_        = validBundle.signedPreKeyId_;
        repaired.signedPreKeySignature_ = validBundle.signedPreKeySignature_;
    }
    repaired.publicPreKeys_ = validBundle.publicPreKeys_;
    for (const auto id : consumedPreKeyIds)
        repaired.publicPreKeys_.remove(id);
    repaired.publicPreKeys_.insert(publicPreKeys_);
    repaired.setId(id());
    return repaired;
}

} // namespace AnyKeep
