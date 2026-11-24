#ifndef BACKUPRESTOREJOB_H
#define BACKUPRESTOREJOB_H

#include "backuprestorepreviewjob.h"

#include <memory>

class QThread;

namespace AnyKeep {

class FolderCatalogManager;
class NoteManager;

// One application-thread job spanning private preparation, comparison,
// user selection and durable local recovery. The media store must outlive
// the job. No remote publication is authorized by this job.
class ANYKEEP_EXPORT BackupRestoreJob final : public StorageJob {
    Q_OBJECT
public:
    enum Phase { Preparing, Comparing, AwaitingSelection, PreparingMedia, CopyingMedia, Committing };
    Q_ENUM(Phase)

    struct Choice {
        enum Action { Skip, KeepDraft, CreateCopy };
        qsizetype decisionIndex { -1 };
        Action action { Skip };
        QString destinationStorageId;
        // Explicit user-supplied title; CreateCopy uses the resolver's suggested
        // title if this is empty. KeepDraft keeps the original title.
        QString title;
    };

    struct Result {
        QList<QUuid> draftIds;
        int skipped { 0 };
        // True if the catalog merge succeeded, even if draft persistence failed.
        bool foldersMerged { false };
        // True once media copying started: failures can leave unused blobs.
        bool mediaMayRemain { false };
    };

    static BackupRestoreJob *restoreDirectory(const QString &directory, const QByteArray &previewManifestSha256,
                                              const QString &privateParent, NoteManager &notes, DraftManager &drafts,
                                              FolderCatalogManager &folders, LocalMediaStore &mediaStore,
                                              QObject *owner = nullptr);
    ~BackupRestoreJob() override;

    Phase phase() const { return phase_; }
    const QList<BackupRestoreDecision> &decisions() const { return decisions_; }
    const PortableBackupRestore *prepared() const { return prepared_.get(); }
    const Result &result() const { return result_; }

    // Every preview row needs an explicit decision, including skipped rows.
    // Empty destination means an unrouted local draft. Returns validation
    // errors without leaving AwaitingSelection, so the caller can correct it.
    StorageError apply(const QList<Choice> &choices, bool importFolders, bool acceptIncomplete);
    void cancel() override;

signals:
    void phaseChanged(AnyKeep::BackupRestoreJob::Phase phase);
    void progressChanged(int completed, int total);

private:
    struct WorkerState;
    BackupRestoreJob(NoteManager &notes, DraftManager &drafts, FolderCatalogManager &folders,
                     LocalMediaStore &mediaStore, QObject *owner);
    void startPreparation(const QString &directory, const QByteArray &digest, const QString &privateParent);
    void preparationFinished();
    void comparisonFinished();
    void startMediaCopy();
    void mediaCopyFinished();
    void commit(const QList<RecoveredNoteCopy> &copies);
    void setPhase(Phase phase);
    void stopWorker();
    void stopComparison();
    StorageError validateChoices(const QList<Choice> &choices, bool importFolders) const;

    QPointer<NoteManager> notes_;
    QPointer<DraftManager> drafts_;
    QPointer<FolderCatalogManager> folders_;
    LocalMediaStore &mediaStore_;
    Phase phase_ { Preparing };
    QThread *worker_ { nullptr };
    std::shared_ptr<WorkerState> workerState_;
    std::unique_ptr<PortableBackupRestore> prepared_;
    QPointer<BackupRestorePreviewJob> comparison_;
    QList<BackupRestoreDecision> decisions_;
    QList<Choice> choices_;
    bool importFolders_ { false };
    Result result_;
};

} // namespace AnyKeep
#endif // BACKUPRESTOREJOB_H
