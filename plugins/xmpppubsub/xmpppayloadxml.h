#ifndef ANYKEEP_XMPPPAYLOADXML_H
#define ANYKEEP_XMPPPAYLOADXML_H

#include "xmppdto.h"

#include <QDomElement>
#include <QString>

class QDomDocument;

namespace AnyKeep {

enum class XmppPayloadParseFailure { None, ObsoleteFormat, UnsupportedFormat, Malformed };

struct XmppPayloadParseResult {
    XmppEncryptedPayload    payload;
    bool                    valid { false };
    QString                 error;
    XmppPayloadParseFailure failure { XmppPayloadParseFailure::None };

    bool isObsoleteOrMalformed() const
    {
        return failure == XmppPayloadParseFailure::ObsoleteFormat || failure == XmppPayloadParseFailure::Malformed;
    }
};

/** Backend-neutral parser/serializer for the opaque encrypted PubSub payload. */
class XmppPayloadXml {
public:
    static const QString payloadNamespace;
    static const QString legacyPayloadNamespace;

    static bool                   isEncryptedPayload(const QDomElement &element);
    static XmppPayloadParseResult parse(const QString &itemId, const QDomElement &element);
    static QDomElement            serialize(QDomDocument &document, const XmppEncryptedPayload &payload);
};

} // namespace AnyKeep

#endif // ANYKEEP_XMPPPAYLOADXML_H
