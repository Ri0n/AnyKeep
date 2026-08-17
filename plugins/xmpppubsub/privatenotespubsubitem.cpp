#include "privatenotespubsubitem.h"

#include <QDomDocument>
#include <QXmlStreamWriter>

namespace AnyKeep {

const QString PrivateNotesPubSubItem::payloadNamespace       = XmppPayloadXml::payloadNamespace;
const QString PrivateNotesPubSubItem::legacyPayloadNamespace = XmppPayloadXml::legacyPayloadNamespace;

PrivateNotesPubSubItem::PrivateNotesPubSubItem(const XmppEncryptedPayload &payload) :
    QXmppPubSubBaseItem(payload.id), payload_(payload), valid_(true)
{
}

bool PrivateNotesPubSubItem::isItem(const QDomElement &element)
{
    return QXmppPubSubBaseItem::isItem(element) && XmppPayloadXml::isEncryptedPayload(element.firstChildElement());
}

void PrivateNotesPubSubItem::parsePayload(const QDomElement &element)
{
    const auto parsed = XmppPayloadXml::parse(id(), element);
    payload_          = parsed.payload;
    valid_            = parsed.valid;
    parseError_       = parsed.error;
    parseFailure_     = parsed.failure;
}

void PrivateNotesPubSubItem::serializePayload(QXmlStreamWriter *writer) const
{
    writer->writeStartElement(QStringLiteral("encrypted"));
    writer->writeDefaultNamespace(XmppPayloadXml::payloadNamespace);
    writer->writeAttribute(
        QStringLiteral("key-id"),
        QString::fromLatin1(payload_.keyId.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    const auto writeBinaryElement = [writer](const QString &name, const QByteArray &value) {
        writer->writeStartElement(name);
        writer->writeCharacters(QString::fromLatin1(value.toBase64()));
        writer->writeEndElement();
    };
    writeBinaryElement(QStringLiteral("nonce"), payload_.nonce);
    writeBinaryElement(QStringLiteral("payload"), payload_.cipherText);
    writeBinaryElement(QStringLiteral("tag"), payload_.tag);
    writer->writeEndElement();
}

} // namespace AnyKeep
