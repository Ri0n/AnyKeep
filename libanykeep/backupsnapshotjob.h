#ifndef BACKUPSNAPSHOTJOB_H
#define BACKUPSNAPSHOTJOB_H

#include "notestorage.h"
#include "portablebackup.h"

#include <QMap>
#include <QPointer>

namespace AnyKeep {

class DraftManager;
class FolderCatalogManager;
class NoteManager;

struct ANYKEEP_EXPORT BackupSnapshot {
    QList<PortableBackupStorage> storages;
    FolderCatalogSnapshot folders;
};

// Collection stays on the application thread: providers own their asynchronous
// I/O and DraftManager owns live editors. The result contains detached Note
// values suitable for the directory writer, with no provider pointers.
class ANYKEEP_EXPORT BackupSnapshotJob final : public StorageJob {
    Q_OBJECT
public:
    static BackupSnapshotJob *collect(NoteManager &notes, DraftManager &drafts,
                                      FolderCatalogManager &folders, QObject *owner = nullptr);
    static BackupSnapshotJob *collect(const QMap<QString, NoteStorage::Ptr> &providers, DraftManager &drafts,
                                      const FolderCatalogSnapshot &folders, QObject *owner = nullptr);

    const BackupSnapshot &result() const { return result_; }
    void cancel() override;

signals:
    void progressChanged(int completedProviders, int totalProviders);

private:
    explicit BackupSnapshotJob(QObject *parent);
    void begin(const QMap<QString, NoteStorage::Ptr> &providers, DraftManager &drafts);
    void nextProvider();
    void nextNote();
    void observe(StorageJob *job, std::function<void()> completion);
    void captureNote(const Note &note, const QString &id);
    void finishCollection();

    BackupSnapshot result_;
    QPointer<FolderCatalogManager> folderManager_;
    bool useLiveFolders_ { false };
    QList<NoteStorage::Ptr> providers_;
    QPointer<StorageJob> activeJob_;
    QList<Note> summaries_;
    qsizetype providerIndex_ { -1 };
    qsizetype noteIndex_ { 0 };
};

} // namespace AnyKeep

#endif // BACKUPSNAPSHOTJOB_H
