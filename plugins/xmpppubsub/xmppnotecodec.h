#ifndef XMPPNOTECODEC_H
#define XMPPNOTECODEC_H

#include "xmppdto.h"

#include "secureenvelope.h"

namespace QtNote {

/**
 * @brief Stateless portable authenticated codec for note index and content payloads.
 *
 * The XMPP wire representation is ordinary XML. AES-GCM encrypts a UTF-8 XML
 * envelope containing the PubSub node and exactly one index or content record.
 * The decrypted node, record ID, record kind and versions are verified against
 * the outer PubSub item before data is accepted.
 */
class XmppNoteCodec {
public:
    static constexpr quint16   WireMajor      = 1;
    static constexpr quint16   WireMinor      = 0;
    static constexpr quint16   SchemaMajor    = 1;
    static constexpr quint16   SchemaMinor    = 0;
    static constexpr qsizetype MaximumXmlSize = 16 * 1024 * 1024;

    static const QString storageNamespace;
    static const QString noteNamespace;

    static CryptoResult<XmppEncryptedPayload> encodeIndex(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                          const QString &nodeName);
    static CryptoResult<XmppEncryptedPayload> encodeContent(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                            const QString &nodeName);
    static CryptoResult<XmppRemoteNote> decodeIndex(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                                    const QString &nodeName);
    static CryptoResult<XmppRemoteNote> decodeContent(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                                      const QString &nodeName, const XmppRemoteNote &index);

    /** Validates one payload without requiring the matching index/content record. */
    static CryptoError validatePayload(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                       const QString &nodeName);
};

} // namespace QtNote

#endif // XMPPNOTECODEC_H
