#ifndef FOLDERCATALOG_H
#define FOLDERCATALOG_H

#include "qtnote_export.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <functional>

namespace QtNote {

struct QTNOTE_EXPORT FolderRecord {
    QUuid     id;
    QUuid     parentId;
    QString   name;
    qint64    sortOrder { 0 };
    bool      collapsed { false };
    bool      favorite { false };
    bool      archived { false };
    quint64   revision { 0 };
    QDateTime modifiedAt;
    bool      tombstone { false };
};

struct QTNOTE_EXPORT NoteFolderAssignment {
    QString   storageId;
    QString   noteId;
    QUuid     folderId;
    // Meaningful only while folderId is recycleBinId().  The original folder
    // is kept with the assignment so Restore is independent of a provider's
    // native note payload.
    QUuid     previousFolderId;
    QDateTime recycledAt;
    quint64   revision { 0 };
    QDateTime modifiedAt;
    bool      tombstone { false };
};

/**
 * Maps a provider-owned folder path to a stable global folder UUID.
 *
 * Path-only providers such as Nextcloud cannot carry the UUID tree itself.
 * The local encrypted catalog retains this association so a remote path keeps
 * resolving to the same folder after a local rename or reparent operation.
 */
struct QTNOTE_EXPORT ProviderPathHint {
    QString     storageId;
    QStringList path;
    QUuid       folderId;
    quint64     revision { 0 };
    QDateTime   modifiedAt;
};

/** One remote note/category observation supplied by a path-only provider. */
struct QTNOTE_EXPORT ProviderFolderPathAssignment {
    QString     noteId;
    QStringList path;
    QDateTime   modifiedAt;
};

struct QTNOTE_EXPORT FolderCatalogSnapshot {
    QList<FolderRecord>         folders;
    QList<NoteFolderAssignment> assignments;
    QList<ProviderPathHint>     pathHints;
};

struct QTNOTE_EXPORT FolderCatalogError {
    enum Code {
        None,
        InvalidArgument,
        NotFound,
        AlreadyExists,
        Cycle,
        Conflict,
        Locked,
        CryptoUnavailable,
        Corrupt,
        Io
    };

    Code    code { None };
    QString message;

    explicit operator bool() const { return code != None; }
};

template <typename T> struct FolderCatalogResult {
    T                  value;
    FolderCatalogError error;

    explicit operator bool() const { return !error; }
};

/**
 * Validated in-memory representation of QtNote's global folder catalog.
 *
 * Folder records use stable UUIDs. A null parent denotes the root. Note
 * assignments are intentionally separate from Note so storages which do not
 * support folder metadata, such as Tomboy, can use a local overlay without
 * changing their native data format.
 */
class QTNOTE_EXPORT FolderCatalog {
public:
    FolderCatalog() = default;

    FolderCatalogError replaceSnapshot(FolderCatalogSnapshot snapshot);
    FolderCatalogError merge(const FolderCatalogSnapshot &snapshot);

    const FolderCatalogSnapshot       &snapshot() const { return snapshot_; }
    const QList<FolderRecord>         &folders() const { return snapshot_.folders; }
    const QList<NoteFolderAssignment> &assignments() const { return snapshot_.assignments; }
    const QList<ProviderPathHint>     &pathHints() const { return snapshot_.pathHints; }

    const FolderRecord         *folder(const QUuid &id) const;
    QList<FolderRecord>         children(const QUuid &parentId = {}) const;
    QStringList                 pathForFolder(const QUuid &id) const;
    const NoteFolderAssignment *assignment(const QString &storageId, const QString &noteId) const;
    QUuid                       folderForNote(const QString &storageId, const QString &noteId) const;
    const ProviderPathHint     *pathHint(const QString &storageId, const QStringList &path) const;

    static QUuid recycleBinId();
    static bool  isRecycleBinId(const QUuid &id);
    bool         isRecycled(const QString &storageId, const QString &noteId) const;

    FolderCatalogResult<QUuid> addFolder(FolderRecord record);
    FolderCatalogError         updateFolder(FolderRecord record);
    FolderCatalogError         renameFolder(const QUuid &id, const QString &name);
    FolderCatalogError         moveFolder(const QUuid &id, const QUuid &parentId, qint64 sortOrder);
    /** Move to a parent and insert before an optional sibling in one transaction. */
    FolderCatalogError moveFolderRelative(const QUuid &id, const QUuid &parentId, const QUuid &beforeId = {});
    FolderCatalogError setFolderCollapsed(const QUuid &id, bool collapsed);
    FolderCatalogError setAllFoldersCollapsed(bool collapsed);
    FolderCatalogError setFolderFlags(const QUuid &id, bool favorite, bool archived);

    FolderCatalogError assignNote(const QString &storageId, const QString &noteId, const QUuid &folderId);
    FolderCatalogError clearNoteAssignment(const QString &storageId, const QString &noteId);
    FolderCatalogError recycleNote(const QString &storageId, const QString &noteId, const QUuid &previousFolderId);
    FolderCatalogResult<QUuid> restoreRecycledNote(const QString &storageId, const QString &noteId);

    /**
     * Reconcile category paths observed from one provider in a single
     * transaction. Missing branches are added to the global tree and matching
     * paths reuse the existing folder. A stale remote observation never
     * overwrites a newer local assignment.
     */
    FolderCatalogError reconcileProviderFolderPaths(const QString                             &storageId,
                                                    const QList<ProviderFolderPathAssignment> &assignments);

private:
    FolderCatalogSnapshot snapshot_;

    static FolderCatalogError validate(const FolderCatalogSnapshot &snapshot);
    static QString            normalizedName(const QString &name);
    static QString            pathKey(const QString &storageId, const QStringList &path);
    static bool incomingWins(quint64 incomingRevision, const QDateTime &incomingModifiedAt, quint64 currentRevision,
                             const QDateTime &currentModifiedAt);
    static QDateTime currentTime();

    int                indexOfFolder(const QUuid &id) const;
    int                indexOfAssignment(const QString &storageId, const QString &noteId) const;
    int                indexOfPathHint(const QString &storageId, const QStringList &path) const;
    FolderCatalogError mutateFolder(const QUuid &id, const std::function<void(FolderRecord &)> &mutation);
};

} // namespace QtNote

#endif // FOLDERCATALOG_H
