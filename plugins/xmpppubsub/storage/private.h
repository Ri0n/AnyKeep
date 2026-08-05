#ifndef ANYKEEP_XMPP_STORAGE_PRIVATE_H
#define ANYKEEP_XMPP_STORAGE_PRIVATE_H

#include "notestorage.h"
#include "xmppdto.h"

namespace AnyKeep::XmppStoragePrivate {

inline constexpr int MinimumRetryDelaySeconds = 30;
inline constexpr int MaximumRetryDelaySeconds = 300;

inline const QString AnyKeepKeychainService    = QStringLiteral("org.xmpp.private-notes");
inline const QString PsiKeychainService        = QStringLiteral("xmpp");
inline const QString IndexRecordTemplateKey    = QStringLiteral("xmpp.xml.v1.index-template");
inline const QString ContentRecordTemplateKey  = QStringLiteral("xmpp.xml.v1.content-template");
inline const QString ContentRevisionBackendKey = QStringLiteral("xmpp.xml.v1.content-revision");
inline const QString FolderPathBackendKey      = QStringLiteral("xmpp.xml.v1.folder-path");

inline QString passwordKeyName(const QString &jid)
{
    return QStringLiteral("xmpp-password-v1:%1").arg(jid.trimmed().section(QLatin1Char('/'), 0, 0));
}

inline QString storageKeyName(const QString &jid)
{
    return QStringLiteral("xmpp-storage-master-key-v1:%1").arg(jid.trimmed().section(QLatin1Char('/'), 0, 0));
}

inline StorageError storageError(const XmppStatusResult &result, StorageError::Code fallback)
{
    auto code = fallback;
    if (result.errorKind == XmppErrorKind::Authentication)
        code = StorageError::Authentication;
    else if (result.errorKind == XmppErrorKind::Configuration || result.errorKind == XmppErrorKind::Security)
        code = StorageError::Unavailable;
    return { code, result.error, result.retryable() };
}

} // namespace AnyKeep::XmppStoragePrivate

#endif // ANYKEEP_XMPP_STORAGE_PRIVATE_H
