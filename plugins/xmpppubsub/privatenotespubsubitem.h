#ifndef ANYKEEPPUBSUBITEM_H
#define ANYKEEPPUBSUBITEM_H

#include "xmpppayloadxml.h"

#include <QXmppPubSubBaseItem.h>

namespace AnyKeep {

/** QXmpp adapter around the backend-neutral encrypted PubSub payload codec. */
class PrivateNotesPubSubItem final : public QXmppPubSubBaseItem {
public:
    using ParseFailure = XmppPayloadParseFailure;

    static const QString payloadNamespace;
    static const QString legacyPayloadNamespace;

    PrivateNotesPubSubItem() = default;
    explicit PrivateNotesPubSubItem(const XmppEncryptedPayload &payload);

    static bool isItem(const QDomElement &element);

    const XmppEncryptedPayload &payload() const { return payload_; }
    bool                        isValid() const { return valid_; }
    const QString              &parseError() const { return parseError_; }
    ParseFailure                parseFailure() const { return parseFailure_; }
    bool                        isObsoleteOrMalformed() const
    {
        return parseFailure_ == ParseFailure::ObsoleteFormat || parseFailure_ == ParseFailure::Malformed;
    }

protected:
    void parsePayload(const QDomElement &payloadElement) override;
    void serializePayload(QXmlStreamWriter *writer) const override;

private:
    XmppEncryptedPayload payload_;
    bool                 valid_ { false };
    QString              parseError_;
    ParseFailure         parseFailure_ { ParseFailure::None };
};

} // namespace AnyKeep

#endif // ANYKEEPPUBSUBITEM_H
