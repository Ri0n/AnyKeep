#ifndef PORTABLEBACKUP_H
#define PORTABLEBACKUP_H

#include "anykeep_export.h"
#include "foldercatalog.h"
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

struct ANYKEEP_EXPORT PortableBackupStorage {
    QString storageId;
    QString displayName;
    bool    remote { false };
    // False when the caller could only enumerate a subset (e.g. offline cache).
    bool    complete { false };
    QList<PortableBackupNote> notes;
};

struct ANYKEEP_EXPORT PortableBackupResult {
    QString error;
    int     notesWritten { 0 };
    int     unavailableMedia { 0 };
    bool    complete { false };

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
    static PortableBackupResult exportDirectory(const QString &directory,
                                                const QList<PortableBackupStorage> &storages,
                                                const FolderCatalogSnapshot &folders,
                                                LocalMediaStore &mediaStore,
                                                const Progress &progress = {});
};

} // namespace AnyKeep

#endif // PORTABLEBACKUP_H
