#ifndef DRAFTMANAGER_H
#define DRAFTMANAGER_H

#include "draftstore.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>

namespace QtNote {

class ConflictResolver;
class FileDraftStore;
class NoteSaveJob;
class NoteStorage;
class StorageJob;
struct StorageError;

class QTNOTE_EXPORT DraftManager final : public QObject {
    Q_OBJECT
public:
    /**
     * Runs after a draft has left an editor and before its first publication
     * attempt. The handler may set folder metadata or change its publication
     * target, but must not perform storage I/O itself.
     */
    using PrePublicationHandler = std::function<DraftStoreError(DraftRecord *record)>;

    static DraftManager *instance();
    explicit DraftManager(std::unique_ptr<DraftStore> store, QObject *parent = nullptr);
    ~DraftManager() override;

    bool initialize(QString *error = nullptr);
    /// Backs up an unreadable on-disk draft store and starts a new empty one.
    bool    recreateStore(QString *error = nullptr);
    bool    isReady() const { return bool(store_); }
    QString lastError() const { return lastError_; }

    DraftStoreError saveEditing(const QUuid &draftId, const Note &note, const QString &title, const QString &body,
                                Note::Format format, bool folderUserOverride = false);
    QUuid           acquireEditingSession(const Note &note, const QUuid &knownDraftId = {});
    bool            isLastEditingSession(const QUuid &draftId) const;
    bool            releaseEditingSession(const QUuid &draftId);
    DraftStoreResult<DraftRecord> editingDraft(const QUuid &draftId) const;
    DraftStoreError               markReady(const QUuid &draftId);
    DraftStoreError               discard(const QUuid &draftId);
    DraftStoreError               queueRemoval(const QString &storageId, const QString &noteId);
    /**
     * Creates a persisted cross-storage move. The source is deleted only
     * after the destination draft is acknowledged by its storage.
     */
    DraftStoreError stageTransfer(const Note &source, const QString &destinationStorageId,
                                  const QUuid &destinationFolderId, QUuid *draftId = nullptr);
    bool            hasPendingTransferFrom(const QString &storageId, const QString &noteId) const;
    void            setPrePublicationHandler(PrePublicationHandler handler);
    /** Safely converts a pending draft into a restart-safe storage transfer. */
    DraftStoreError    retargetDraftForPublication(DraftRecord *record, const QString &destinationStorageId) const;
    void               publishPending();
    QList<DraftRecord> recoverableDrafts() const;
    void               setConflictResolver(std::unique_ptr<ConflictResolver> resolver);
    /// Resolves a conflict discovered after a storage operation was acknowledged.
    void resolveConcurrentEdit(const Note &localVersion, const Note &remoteVersion, const QString &message);

signals:
    void draftsChanged();
    void draftPublished(const QUuid &draftId, const Note &note);
    void draftPublishFailed(const QUuid &draftId, const QString &message);
    void publishingIdle();
    void publicationAbandoned(const QString &message);
    void conflictResolved(const QString &message);

private:
    explicit DraftManager(QObject *parent = nullptr);
    void           process(const DraftRecord &record);
    void           publish(const DraftRecord &record);
    void           remove(const DraftRecord &record);
    void           finishPublishedDraft(const DraftRecord &record, const Note &note);
    void           retry(const DraftRecord &record, const QString &message, bool retryable = true);
    void           resolveConflict(const DraftRecord &record, const StorageError &error, const Note &remoteNote = {});
    void           storageBecameReady(NoteStorage *storage);
    void           storageAboutToBeRemoved(NoteStorage *storage);
    static QString sourceKey(const Note &note);

    std::unique_ptr<DraftStore>        store_;
    QSet<QUuid>                        publishing_;
    QHash<QUuid, QPointer<StorageJob>> publishJobs_;
    QHash<QUuid, int>                  editingSessions_;
    QHash<QString, QUuid>              sourceSessions_;
    QString                            lastError_;
    std::unique_ptr<ConflictResolver>  conflictResolver_;
    PrePublicationHandler              prePublicationHandler_;
};

} // namespace QtNote

#endif // DRAFTMANAGER_H
