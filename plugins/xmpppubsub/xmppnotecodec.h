#ifndef XMPPNOTECODEC_H
#define XMPPNOTECODEC_H

#include "xmppdto.h"

#include "secureenvelope.h"

namespace QtNote {

/**
 * @brief Stateless portable authenticated codec for note index and content payloads.
 *
 * The XMPP wire representation is ordinary XML in one versioned protocol
 * namespace. AES-GCM encrypts a UTF-8 XML envelope containing the complete
 * PubSub node and exactly one index or content record. The decrypted node and
 * record ID are verified against the outer PubSub container before acceptance.
 */
class XmppNoteCodec {
public:
    static constexpr qsizetype MaximumXmlSize = 16 * 1024 * 1024;

    static const QString protocolNamespace;

    static CryptoResult<XmppEncryptedPayload> encodeIndex(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                          const QString &nodeName);
    static CryptoResult<XmppEncryptedPayload> encodeContent(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                            const QString &nodeName);
    static CryptoResult<XmppRemoteNote> decodeIndex(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                                    const QString &nodeName);
    static CryptoResult<XmppRemoteNote> decodeContent(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                                      const QString &nodeName, const XmppRemoteNote &index);

    /** Validates one payload for the expected PubSub node kind. */
    static CryptoError validatePayload(const XmppEncryptedPayload &payload, XmppEncryptedPayload::Kind expectedKind,
                                       const QByteArray &masterKey, const QString &nodeName);
};

} // namespace QtNote

#endif // XMPPNOTECODEC_H
