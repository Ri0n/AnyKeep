#ifndef PORTABLEBACKUP_H
#define PORTABLEBACKUP_H

#include "anykeep_export.h"
#include "foldercatalog.h"
#include "draftstore.h"
#include "note.h"

#include <functional>

namespace AnyKeep {

class LocalMediaStore;

// The caller supplies immutable, loaded snapshots, including checkpoints of
// active editors. Provider discovery and asynchronous loading belong to the
// controller, not the portable file writer.
struct ANYKEEP_EXPORT PortableBackupNote {
    Note    snapshot;
    QString sourceNoteId;
    QUuid   draftId;
};

// Lifecycle metadata is descriptive only. Restore must never replay deletion
// or publication simply because an old backup contained an intent.
struct ANYKEEP_EXPORT PortableBackupDraftInfo {
    QUuid                  id;
    QString                storageId;
    QString                noteId;
    DraftRecord::Operation operation { DraftRecord::Publish };
    DraftRecord::State     state { DraftRecord::Editing };
    QString                transferSourceStorageId;
    QString                transferSourceNoteId;
    quint64                revision { 0 };
    QDateTime              updatedAt;
};

struct ANYKEEP_EXPORT PortableBackupStorage {
    QString storageId;
    QString displayName;
    bool    remote { false };
    // False when the caller could only enumerate a subset (e.g. offline cache).
    bool    complete { false };
    QList<PortableBackupNote> notes;
    QList<PortableBackupDraftInfo> drafts;
    QStringList omittedNoteIds;
};

struct ANYKEEP_EXPORT PortableBackupResult {
    QString error;
    int     notesWritten { 0 };
    int     unavailableMedia { 0 };
    bool    complete { false };

    explicit operator bool() const { return error.isEmpty(); }
};

// Read-only metadata for a backup preview. No NoteStorage or LocalMediaStore
// is opened while inspecting untrusted input.
struct ANYKEEP_EXPORT PortableBackupFileInfo {
    QString path;
    qint64 size { 0 };
    QByteArray sha256;
};

struct ANYKEEP_EXPORT PortableBackupMissingMedia {
    QUuid id;
    QString uri;
    QString originalName;
    QString mimeType;
    qint64 size { 0 };
    QByteArray sha256;
};

struct ANYKEEP_EXPORT PortableBackupNoteInfo {
    QString ptfId;
    QString sourceNoteId;
    QUuid draftId;
    Note::Format originalFormat { Note::Markdown };
    QDateTime modifiedUtc;
    QStringList tags;
    QList<PortableBackupMissingMedia> unavailableMedia;
};

struct ANYKEEP_EXPORT PortableBackupStorageInfo {
    QString directory;
    QString storageId;
    QString displayName;
    bool remote { false };
    bool complete { false };
    QList<PortableBackupNoteInfo> notes;
    QList<PortableBackupDraftInfo> drafts;
    QStringList omittedNoteIds;
};

struct ANYKEEP_EXPORT PortableBackupInspection {
    QString error;
    bool complete { false };
    QDateTime createdUtc;
    QList<PortableBackupStorageInfo> storages;
    QList<PortableBackupFileInfo> files;
    QByteArray manifestSha256;
    int notesWritten { 0 };
    int unavailableMedia { 0 };

    explicit operator bool() const { return error.isEmpty(); }
};

class ANYKEEP_EXPORT PortableBackup {
public:
    // Write normal PTF trees and a versioned origin manifest into a new local
    // directory. The directory must not exist; staging occurs beside it and
    // only a finished export is renamed into place. No provider is registered
    // or modified, and no draft is published.
    //
    // Run off the UI thread after resolving the local media key and obtaining
    // the snapshots. The progress callback may return false to cancel between
    // notes. A successfully written but incomplete snapshot has complete=false.
    using Progress = std::function<bool(int completed, int total)>;
    using Continue = std::function<bool()>;
    // continueWriting is checked within media copies. beforePublish is the
    // final cancellation boundary, called immediately before the directory rename.
    static PortableBackupResult exportDirectory(const QString &directory,
                                                const QList<PortableBackupStorage> &storages,
                                                const FolderCatalogSnapshot &folders,
                                                LocalMediaStore &mediaStore,
                                                const Progress &progress = {},
                                                const Continue &continueWriting = {},
                                                const Continue &beforePublish = {});

    // Validate a directory without importing media or touching application
    // state. A valid but incomplete backup is a successful inspection with
    // complete=false. This is integrity checking, not authentication.
    // Revalidate a private staged copy before using it for restoration.
    static PortableBackupInspection inspectDirectory(const QString &directory,
                                                      const Progress &progress = {},
                                                      const Continue &continueReading = {});

    // Parse and validate the manifest independently of its on-disk files.
    // Used by authenticated transport adapters before creating any paths.
    // The returned inventory is structurally validated but not yet verified.
    static PortableBackupInspection inspectManifest(const QByteArray &bytes,
                                                     const Continue &continueReading = {});

    static constexpr qint64 MaximumManifestBytes = 32 * 1024 * 1024;
};

} // namespace AnyKeep

#endif // PORTABLEBACKUP_H
