#ifndef ANYKEEPPUBSUBITEM_H
#define ANYKEEPPUBSUBITEM_H

#include "xmppdto.h"

#include <QXmppPubSubBaseItem.h>

namespace AnyKeep {

/**
 * @brief QXmpp PubSub item adapter for AnyKeep encrypted payloads.
 *
 * The class validates the application namespace and transports the opaque
 * encrypted envelope. Plain note metadata is never exposed in the PubSub XML.
 */
class PrivateNotesPubSubItem final : public QXmppPubSubBaseItem {
public:
    enum class ParseFailure { None, ObsoleteFormat, UnsupportedFormat, Malformed };

    static const QString payloadNamespace;
    static const QString legacyPayloadNamespace; ///< Accepted only for explicit maintenance cleanup.

    PrivateNotesPubSubItem() = default;
    explicit PrivateNotesPubSubItem(const XmppEncryptedPayload &payload);

    /// Returns whether @p element is a AnyKeep encrypted PubSub item.
    static bool isItem(const QDomElement &element);

    const XmppEncryptedPayload &payload() const { return payload_; }
    bool                        isValid() const { return valid_; }
    const QString              &parseError() const { return parseError_; }
    ParseFailure                parseFailure() const { return parseFailure_; }
    /// True only for malformed/current-development payloads safe to remove explicitly.
    bool isObsoleteOrMalformed() const
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
