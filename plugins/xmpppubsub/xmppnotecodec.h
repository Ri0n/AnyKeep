#ifndef XMPPNOTECODEC_H
#define XMPPNOTECODEC_H

#include "xmppdto.h"

#include "secureenvelope.h"

namespace AnyKeep {

/** Encrypted records which still need publication during canonical-key recovery. */
struct XmppNoteKeyMigration {
    std::optional<XmppEncryptedPayload> index;
    std::optional<XmppEncryptedPayload> content;
};

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
    /** Optional encrypted-index extension carrying one canonical folder path. */
    static const QString folderNamespace;
    /** Optional encrypted system-folder identity, independent of display names. */
    static const QString systemFolderNamespace;
    static const QString favoriteNamespace;
    /** Required extension which binds an index-only update to an unchanged body. */
    static const QString contentRevisionNamespace;
    /** Required feature declaring XEP-0447 media descriptors in the content record. */
    static const QString mediaFeature;
    /** Required feature declaring AnyKeep chunk-authenticated XEP-0447 sources. */
    static const QString chunkedMediaFeature;

    static CryptoResult<XmppEncryptedPayload> encodeIndex(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                          const QString &nodeName);
    static CryptoResult<XmppEncryptedPayload> encodeContent(const XmppRemoteNote &note, const QByteArray &masterKey,
                                                            const QString &nodeName);
    static CryptoResult<XmppRemoteNote> decodeIndex(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                                    const QString &nodeName);
    static CryptoResult<XmppRemoteNote> decodeContent(const XmppEncryptedPayload &payload, const QByteArray &masterKey,
                                                      const QString &nodeName, const XmppRemoteNote &index);

    /**
     * Prepare only records encrypted with a different key. The caller must first
     * authenticate/decode both records and validate their ID/revision binding.
     * Empty optionals mean that the original ciphertext must remain untouched.
     */
    static CryptoResult<XmppNoteKeyMigration>
    encodeKeyMigration(const XmppRemoteNote &note, const QByteArray &indexKeyId, const QByteArray &contentKeyId,
                       const QByteArray &canonicalKey, const QString &indexNode, const QString &contentNode);

    /** Validates one payload for the expected PubSub node kind. */
    static CryptoError validatePayload(const XmppEncryptedPayload &payload, XmppEncryptedPayload::Kind expectedKind,
                                       const QByteArray &masterKey, const QString &nodeName);
};

} // namespace AnyKeep

#endif // XMPPNOTECODEC_H
