#ifndef PORTABLEBACKUPRESTORE_H
#define PORTABLEBACKUPRESTORE_H

#include "portablebackup.h"

#include <memory>

class QTemporaryDir;

namespace AnyKeep {

// A verified private working copy and its isolated encrypted media store.
// Nothing is imported into the live profile until a later restore operation
// explicitly accepts a prepared snapshot. Destroying this object removes
// the working copy and its temporary media. Run preparation on a worker.
class ANYKEEP_EXPORT PortableBackupRestore final {
public:
    static std::unique_ptr<PortableBackupRestore> prepareDirectory(
        const QString &directory, const QByteArray &previewManifestSha256, const QString &privateParent,
        QString *error, const PortableBackup::Progress &progress = {},
        const PortableBackup::Continue &continueReading = {});

    ~PortableBackupRestore();

    struct CopySelection {
        qsizetype storageIndex;
        qsizetype noteIndex;
        QString destinationStorageId;
    };

    // Run on a worker after initializing the destination media key on the
    // application thread. Copies only selected attachments into the profile
    // media store; no draft or provider is mutated. A later application-thread
    // stageRecoveredCopies commits the returned notes as editing drafts.
    // Every copy gets fresh attachment identities, including missing media,
    // to avoid aliasing an existing profile's external-file locators.
    DraftStoreResult<QList<RecoveredNoteCopy>> prepareCopies(
        const QList<CopySelection> &selection, LocalMediaStore &destination,
        const PortableBackup::Progress &progress = {},
        const PortableBackup::Continue &continueReading = {}) const;

    const PortableBackupInspection &inspection() const { return inspection_; }
    const QList<PortableBackupStorage> &storages() const { return storages_; }
    const FolderCatalogSnapshot &folders() const { return folders_; }
    LocalMediaStore &mediaStore() const { return *mediaStore_; }

private:
    PortableBackupRestore();
    std::unique_ptr<QTemporaryDir> workingDirectory_;
    std::unique_ptr<LocalMediaStore> mediaStore_;
    PortableBackupInspection inspection_;
    QList<PortableBackupStorage> storages_;
    FolderCatalogSnapshot folders_;
};

} // namespace AnyKeep

#endif // PORTABLEBACKUPRESTORE_H
