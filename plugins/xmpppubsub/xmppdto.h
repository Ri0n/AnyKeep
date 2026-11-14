#ifndef XMPPPUBSUBDTO_H
#define XMPPPUBSUBDTO_H

#include "mediareference.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <functional>
#include <optional>

namespace AnyKeep {

/// Human-readable recovery stages; counts refer to processed notes, or -1 when not known yet.
struct XmppSyncProgress {
    QString message;
    int     completed { -1 };
    int     total { -1 };
};
using XmppProgressCallback = std::function<void(XmppSyncProgress)>;

/// Broad failure category used to decide whether automatic retry is useful.
enum class XmppErrorKind {
    None,           ///< No error.
    Transient,      ///< Temporary network/server failure; retry is appropriate.
    Authentication, ///< Credentials were rejected; user action is required.
    Configuration,  ///< Local settings or server capabilities are unsuitable.
    Security,       ///< Encryption, trust, or key validation failed.
    Protocol,       ///< A valid response could not be understood or completed.
};

/** Backend-neutral trust state of an end-to-end-encryption identity key. */
enum class XmppTrustLevel : quint8 {
    Undecided,
    AutomaticallyTrusted,
    ManuallyTrusted,
    Authenticated,
    Distrusted,
};

/** @brief Complete backend configuration for one XMPP account. */
struct XmppConfig {
    QString    instanceId; ///< Stable local identity of this configured storage instance.
    QString    jid;        ///< Bare JID used for authentication and own PEP service.
    QString    password;   ///< Transient keychain value; never persisted in plugin settings.
    QString    host;       ///< Optional server override; empty enables normal discovery.
    int        port { 0 }; ///< Optional port override; zero selects the library default.
    QString    resource { QStringLiteral("private-notes") };            ///< Requested XMPP resource.
    QString    nodeName { QStringLiteral("urn:xmpp:private-notes:0") }; ///< Versioned base AnyKeep node.
    QString    originId;                         ///< Stable installation ID used in note revision metadata.
    int        timeoutMs { 15000 };              ///< Upper bound for an individual protocol operation.
    QByteArray masterKey;                        ///< Content-encryption key for notes (not an OMEMO key).
    bool       allowForeignKeyIndices { false }; ///< Opt-in fresh-storage mode: preserve old-key XMPP items.
    QByteArray omemoStateKey;                    ///< Key encrypting local OMEMO/trust state at rest.
    QString    omemoStatePath;                   ///< Directory containing encrypted OMEMO state files.

    // A fresh storage is an explicit user choice. Otherwise even a mixed
    // readable/foreign-key index requires recovery, rather than silently
    // hiding a second client's notes.
    bool foreignKeysRequireRecovery(int foreignKeyNotes) const
    {
        return !allowForeignKeyIndices && foreignKeyNotes > 0;
    }

    QString indexNodeName() const { return nodeName + QStringLiteral(":index"); }
    QString contentNodeName() const { return nodeName + QStringLiteral(":content"); }
    QString jinglePubNodeName() const { return nodeName + QStringLiteral(":jinglepub"); }
};

/** XEP-0447 descriptor plus the AnyKeep attachment it represents. */
struct XmppRemoteMedia {
    MediaReference reference;
    /** Complete UTF-8 <file-sharing xmlns='urn:xmpp:sfs:0'> element. */
    QByteArray fileSharingXml;
};

/** @brief Backend-neutral representation of a remotely synchronized note. */
struct XmppRemoteNote {
    QString id;
    /** Revision of the index metadata and the optimistic-concurrency token. */
    QString revision;
    /** Revision bound into the separately published content payload. */
    QString     contentRevision;
    QString     parentRevision;
    QString     originId;
    QString     title;
    QString     content;
    QDateTime   modified;
    bool        preserveModified { false }; ///< Local save hint; never serialized.
    QString     format { QStringLiteral("markdown") };
    QStringList tags;
    bool        favorite { false };
    /** Media descriptors embedded as authenticated XEP-0447 content extensions. */
    QList<XmppRemoteMedia> media;
    /** Folder names from the root to this note's folder, stored in the encrypted index. */
    QStringList folderPath;
    /** Stable identity of an application-owned system folder (e.g. Recycle Bin). */
    QUuid       systemFolderId;
    bool        contentPresent { true }; ///< False for index-only list results.
    /** UTF-8 XML template containing only extensions and unknown index fields. */
    QByteArray indexRecordTemplate;
    /** UTF-8 XML template containing only extensions and unknown content fields. */
    QByteArray contentRecordTemplate;
};

/** @brief Serialized encrypted payload stored as one PubSub item. */
struct XmppEncryptedPayload {
    /// Selects the independently encrypted index metadata or note body.
    enum Kind { Index, Content };

    QString    id;
    QByteArray keyId;
    QByteArray nonce;
    QByteArray tag;
    QByteArray cipherText;
};

/** @brief OMEMO device shown by the trust and recovery UI. */
struct XmppDeviceInfo {
    QString        label;
    quint32        deviceId { 0 };
    QByteArray     keyId;
    XmppTrustLevel trustLevel { XmppTrustLevel::Undecided };
};

/** @brief Candidate note master key discovered during recovery auditing. */
struct XmppStorageKeyCandidate {
    QString    resource;
    QByteArray key;
    QByteArray keyId;
    int        indexItemCount { 0 };
    bool       local { false };
};

/** @brief Common completion status returned by all backend operations. */
struct XmppStatusResult {
    bool                          ok { false };
    bool                          conflict { false };
    bool                          notFound { false };
    QString                       error;
    std::optional<XmppRemoteNote> remoteOnConflict;
    XmppErrorKind                 errorKind { XmppErrorKind::None };

    bool retryable() const { return errorKind == XmppErrorKind::Transient; }
};

/// Result of loading the remote note index.
struct XmppListResult : XmppStatusResult {
    QList<XmppRemoteNote> notes;
    bool                  partial { false };
    int                   foreignKeyNotes { 0 };
};

/// Result of loading or saving one complete note.
struct XmppNoteResult : XmppStatusResult {
    XmppRemoteNote note;
};

/// Result of testing candidate master keys against all index items.
struct XmppKeyAuditResult : XmppStatusResult {
    QList<XmppStorageKeyCandidate> candidates;
    int                            totalIndexItems { 0 };
    // Actual online resources advertising the AnyKeep key-sync feature.
    // OMEMO device labels/IDs are not a reliable presence mapping.
    QStringList                    onlineResources;
};

/// Result of verifying remote notes and migrating records which use another key.
struct XmppRekeyResult : XmppStatusResult {
    int         migrated { 0 };  ///< Notes with at least one successfully re-encrypted record.
    int         unchanged { 0 }; ///< Authenticated notes already using the chosen key for both records.
    int         total { 0 };
    QStringList inaccessibleNoteIds;
};

/** Result of scanning or deleting obsolete unreadable PubSub records. */
struct XmppCleanupResult : XmppStatusResult {
    QStringList obsoleteIndexItemIds;
    QStringList obsoleteContentItemIds;
    int         protectedUnreadableItems { 0 };
    int         validItems { 0 };
    int         removedItems { 0 };

    int obsoleteItemCount() const { return obsoleteIndexItemIds.size() + obsoleteContentItemIds.size(); }
};

} // namespace AnyKeep

Q_DECLARE_METATYPE(AnyKeep::XmppRemoteNote)
Q_DECLARE_METATYPE(AnyKeep::XmppEncryptedPayload)
Q_DECLARE_METATYPE(AnyKeep::XmppStorageKeyCandidate)

#endif // XMPPPUBSUBDTO_H
