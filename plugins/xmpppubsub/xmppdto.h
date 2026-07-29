#ifndef XMPPPUBSUBDTO_H
#define XMPPPUBSUBDTO_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>

namespace QtNote {

/// Broad failure category used to decide whether automatic retry is useful.
enum class XmppErrorKind {
    None,           ///< No error.
    Transient,      ///< Temporary network/server failure; retry is appropriate.
    Authentication, ///< Credentials were rejected; user action is required.
    Configuration,  ///< Local settings or server capabilities are unsuitable.
    Security,       ///< Encryption, trust, or key validation failed.
    Protocol,       ///< A valid response could not be understood or completed.
};

/** @brief Complete backend configuration for one XMPP account. */
struct XmppConfig {
    QString    instanceId;                            ///< Stable local identity of this configured storage instance.
    QString    jid;                                   ///< Bare JID used for authentication and own PEP service.
    QString    password;                              ///< Transient keychain value; never persisted in plugin settings.
    QString    host;                                  ///< Optional server override; empty enables normal discovery.
    int        port { 0 };                            ///< Optional port override; zero selects the library default.
    QString    resource { QStringLiteral("QtNote") }; ///< Requested XMPP resource.
    QString    nodeName { QStringLiteral("urn:xmpp:qtnote:notes:1") }; ///< Versioned base QtNote node.
    QString    originId;            ///< Stable installation ID used in note revision metadata.
    int        timeoutMs { 15000 }; ///< Upper bound for an individual protocol operation.
    QByteArray masterKey;           ///< Content-encryption key for notes (not an OMEMO key).
    QByteArray omemoStateKey;       ///< Key encrypting local OMEMO/trust state at rest.
    QString    omemoStatePath;      ///< Directory containing encrypted OMEMO state files.

    QString indexNodeName() const { return nodeName + QStringLiteral(":index"); }
    QString contentNodeName() const { return nodeName + QStringLiteral(":content"); }
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
    /** Folder names from the root to this note's folder, stored in the encrypted index. */
    QStringList folderPath;
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
    QString    label;
    quint32    deviceId { 0 };
    QByteArray keyId;
    int        trustLevel { 0 };
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
};

/// Result of loading or saving one complete note.
struct XmppNoteResult : XmppStatusResult {
    XmppRemoteNote note;
};

/// Result of testing candidate master keys against all index items.
struct XmppKeyAuditResult : XmppStatusResult {
    QList<XmppStorageKeyCandidate> candidates;
    int                            totalIndexItems { 0 };
};

/// Result of re-encrypting remote notes under a chosen canonical key.
struct XmppRekeyResult : XmppStatusResult {
    int         migrated { 0 };
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

} // namespace QtNote

Q_DECLARE_METATYPE(QtNote::XmppRemoteNote)
Q_DECLARE_METATYPE(QtNote::XmppEncryptedPayload)
Q_DECLARE_METATYPE(QtNote::XmppStorageKeyCandidate)

#endif // XMPPPUBSUBDTO_H
