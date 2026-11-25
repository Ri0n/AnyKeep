#ifndef BACKUPEXPORTJOB_H
#define BACKUPEXPORTJOB_H

#include "backupsnapshotjob.h"

#include <memory>

class QThread;

namespace AnyKeep {

// Shared desktop/mobile orchestration for readable and password-protected directory export.
// Call on the application thread. The media store must outlive the job.
// Deleting the job cancels outstanding work and joins its file writer.
class ANYKEEP_EXPORT BackupExportJob final : public StorageJob {
    Q_OBJECT
public:
    enum Phase { Collecting, PreparingMedia, Writing, Protecting };
    Q_ENUM(Phase)

    static BackupExportJob *exportDirectory(const QString &directory, NoteManager &notes, DraftManager &drafts,
                                             FolderCatalogManager &folders, LocalMediaStore &mediaStore,
                                             QObject *owner = nullptr);
    static BackupExportJob *exportDirectory(const QString &directory, const QMap<QString, NoteStorage::Ptr> &providers,
                                             DraftManager &drafts, const FolderCatalogSnapshot &folders,
                                             LocalMediaStore &mediaStore, QObject *owner = nullptr);

    // The caller supplies an existing application-private parent for plaintext
    // staging. The final destination receives only authenticated ciphertext.
    static BackupExportJob *exportProtectedDirectory(const QString &directory, const QString &privateParent,
                                                      const QString &password, NoteManager &notes, DraftManager &drafts,
                                                      FolderCatalogManager &folders, LocalMediaStore &mediaStore,
                                                      QObject *owner = nullptr);
    static BackupExportJob *exportProtectedDirectory(const QString &directory, const QString &privateParent,
                                                      const QString &password,
                                                      const QMap<QString, NoteStorage::Ptr> &providers,
                                                      DraftManager &drafts, const FolderCatalogSnapshot &folders,
                                                      LocalMediaStore &mediaStore, QObject *owner = nullptr);

    ~BackupExportJob() override;
    Phase phase() const { return phase_; }
    const PortableBackupResult &result() const { return result_; }
    void cancel() override;

signals:
    void phaseChanged(AnyKeep::BackupExportJob::Phase phase);
    void progressChanged(int completed, int total);

private:
    struct WorkerState;
    explicit BackupExportJob(const QString &directory, LocalMediaStore &mediaStore, QObject *owner);
    bool configureProtection(const QString &privateParent, const QString &password);
    void observeSnapshot(BackupSnapshotJob *job);
    void snapshotFinished();
    void startWriter();
    void writerFinished();
    void setPhase(Phase phase);

    QString directory_;
    QString privateParent_;
    QString password_;
    LocalMediaStore &mediaStore_;
    QPointer<BackupSnapshotJob> snapshotJob_;
    BackupSnapshot snapshot_;
    Phase phase_ { Collecting };
    QThread *worker_ { nullptr };
    std::shared_ptr<WorkerState> workerState_;
    PortableBackupResult result_;
};

} // namespace AnyKeep

#endif // BACKUPEXPORTJOB_H
