#include "xmppbackend.h"
#include "xmppnotecodec.h"

#include <QDateTime>

#include <utility>

namespace AnyKeep {

CryptoError XmppBackend::appendIndexPayload(const XmppConfig &config, const XmppEncryptedPayload &payload,
                                            XmppListResult &result)
{
    if (payload.keyId != SecureEnvelope::keyId(config.masterKey, KeyDerivationProfile::PrivateNotes)) {
        ++result.foreignKeyNotes;
        result.partial = true;
        return {};
    }
    auto note = XmppNoteCodec::decodeIndex(payload, config.masterKey, config.indexNodeName());
    if (!note) {
        result.partial = true;
        return note.error;
    }
    result.notes.append(std::move(note.value));
    return {};
}

void XmppBackend::completeIndexList(const XmppConfig &config, XmppListResult &result)
{
    if (config.foreignKeysRequireRecovery(result.foreignKeyNotes)) {
        result.ok        = false;
        result.errorKind = XmppErrorKind::Security;
        result.error     = QStringLiteral("XMPP storage key mismatch: %1 note index record(s) use another key")
                               .arg(result.foreignKeyNotes);
        return;
    }
    result.ok = true;
}

XmppRemoteNote XmppBackend::makeIndexUpdate(XmppRemoteNote current, const XmppRemoteNote &requested,
                                            QString newRevision, QString originId)
{
    current.folderPath     = requested.folderPath;
    current.parentRevision = current.revision;
    current.revision       = std::move(newRevision);
    current.originId       = std::move(originId);
    current.modified = requested.preserveModified && requested.modified.isValid() ? requested.modified.toUTC()
                                                                                  : QDateTime::currentDateTimeUtc();
    current.format   = QStringLiteral("markdown");
    current.contentPresent = false;
    return current;
}

} // namespace AnyKeep
