#ifndef BACKUPRESTOREPREVIEWJOB_H
#define BACKUPRESTOREPREVIEWJOB_H

#include "notestorage.h"
#include "portablebackuprestore.h"
#include "conflictresolver.h"

#include <QMap>
#include <QHash>
#include <QPointer>

namespace AnyKeep {

class DraftManager;

struct ANYKEEP_EXPORT BackupRestoreDecision {
    enum Status { Unrouted, ProviderMissing, Unavailable, NoteMissing, Identical, Conflict };
    qsizetype storageIndex { 0 };
    qsizetype noteIndex { 0 };
    QString originStorageId;
    QString originNoteId;
    QString destinationStorageId;
    Status status { Unrouted };
    QString message;
    ConflictResolution resolution;
    // A decision is advisory, never a publication authorization.
};

// Read-only comparison against the original provider and current local
// editing intents. The prepared snapshots are copied by value; no media is
// imported and no publication or deletion is triggered.
class ANYKEEP_EXPORT BackupRestorePreviewJob final : public StorageJob {
    Q_OBJECT
public:
    static BackupRestorePreviewJob *compare(const QList<PortableBackupStorage> &storages,
                                            const QMap<QString, NoteStorage::Ptr> &providers,
                                            DraftManager &drafts, const FolderCatalogSnapshot &folders,
                                            QObject *owner = nullptr);
    ~BackupRestorePreviewJob() override;
    const QList<BackupRestoreDecision> &result() const { return decisions_; }
    void cancel() override;

signals:
    void progressChanged(int completed, int total);

private:
    explicit BackupRestorePreviewJob(QObject *parent);
    void next();
    void finishEntry(BackupRestoreDecision::Status status, const QString &message = {});
    void conflict(const Note &current, const QString &message);
    void stopRead();

    QList<PortableBackupStorage> storages_;
    QMap<QString, NoteStorage::Ptr> providers_;
    QPointer<DraftManager> drafts_;
    FolderCatalog folders_;
    QList<BackupRestoreDecision> decisions_;
    QList<DraftRecord> currentDrafts_;
    QHash<QUuid, qsizetype> draftById_;
    QMultiHash<QPair<QString, QString>, qsizetype> draftsByOrigin_;
    qsizetype index_ { 0 };
    QPointer<NoteLoadJob> active_;
    QMetaObject::Connection providerDestroyed_;
};

} // namespace AnyKeep
#endif // BACKUPRESTOREPREVIEWJOB_H
