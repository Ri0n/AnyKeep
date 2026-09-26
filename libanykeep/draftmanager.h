#ifndef DRAFTMANAGER_H
#define DRAFTMANAGER_H

#include "draftstore.h"

#include <QHash>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>

namespace AnyKeep {

class ConflictResolver;
class FileDraftStore;
class NoteEditor;
class NoteSaveJob;
class NoteStorage;
class StorageJob;
struct StorageError;

class ANYKEEP_EXPORT DraftManager final : public QObject {
    Q_OBJECT
public:
    struct RecyclePreparation {
        QString storageId;
        QString noteId;
        QUuid   draftId; // Commit with retryDraftNow() after catalog mutation succeeds.
    };
    /**
     * Runs after a draft has left an editor and before its first publication
     * attempt. The handler may set folder metadata or change its publication
     * target, but must not perform storage I/O itself.
     */
    using PrePublicationHandler = std::function<DraftStoreError(DraftRecord *record)>;

    static DraftManager *instance();
    static QString       draftsStorageId();
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
    /** Returns the canonical in-process live model for this logical note. */
    NoteEditor      *acquireEditor(const Note &note, const QUuid &knownDraftId = {});
    NoteEditor      *liveEditorForNote(const QString &storageId, const QString &noteId) const;
    NoteEditor      *liveEditorForDraft(const QUuid &draftId) const;
    int             editingSessionCountForNote(const QString &storageId, const QString &noteId) const;
    int             editingSessionCount(const QUuid &draftId) const;
    DraftStoreError discardEditingSessionsForNote(const QString &storageId, const QString &noteId);
    DraftStoreError discardEditingSessionsForDraft(const QUuid &draftId);
    bool            isLastEditingSession(const QUuid &draftId) const;
    bool            releaseEditingSession(const QUuid &draftId);
    DraftStoreResult<DraftRecord> editingDraft(const QUuid &draftId) const;
    /** Reclaims a persisted publish draft for an explicitly restored editor session. */
    DraftStoreResult<DraftRecord> resumeEditingDraft(const QUuid &draftId);
    /**
     * Resumes a durable draft and materializes its canonical local snapshot
     * without requiring or reading the target storage.
     */
    DraftStoreResult<Note>        resumeNoteForEditingDraft(const QUuid &draftId);
    DraftStoreError               markReady(const QUuid &draftId);
    DraftStoreError               discard(const QUuid &draftId);
    /** Retarget a live Editing draft without closing its shared document. */
    DraftStoreResult<DraftRecord> retargetEditingDraft(const QUuid &draftId, const QString &destinationStorageId);
    /** Cancel an in-flight publication, retarget the same persisted draft and publish it at the new storage. */
    DraftStoreError moveDraft(const QUuid &draftId, const QString &destinationStorageId);
    /** Publish a copy of the same local draft contents to another storage. */
    DraftStoreError copyDraft(const QUuid &draftId, const QString &destinationStorageId, QUuid *copyDraftId = nullptr);
    /** Update local folder metadata without touching a storage until publication. */
    DraftStoreError setDraftFolder(const QUuid &draftId, const QUuid &folderId, bool userOverride = true);
    /** Make a failed/paused publish draft immediately eligible for another publication attempt. */
    DraftStoreError retryDraftNow(const QUuid &draftId);
    DraftStoreError queueRemoval(const QString &storageId, const QString &noteId);
    /**
     * Replaces a publish/transfer draft with durable deletion intents for
     * every remote object that may already represent that logical note.
     * The draft is discarded only after all delete records are durable.
     */
    DraftStoreError queueDraftDeletion(const QUuid &draftId);
    /**
     * Closes every live view and removes any local publish draft before a
     * caller recycles the surviving persisted object. For a post-ACK transfer,
     * unresolved source deletion is made durable first. An empty pair means
     * the logical note had no persisted object to recycle.
     */
    DraftStoreResult<RecyclePreparation> prepareForRecycle(const QString &storageId, const QString &noteId,
                                                           const QUuid &recycleFolderId,
                                                           const QUuid &knownDraftId = {});
    /**
     * A storage announced that an object disappeared while its logical note is
     * still open. Persist every matching live model before any UI reacts.
     */
    DraftStoreError preserveLiveNoteAfterExternalRemoval(const QString &storageId, const QString &noteId);
    /**
     * Creates a persisted cross-storage move. The source is deleted only
     * after the destination draft is acknowledged by its storage.
     */
    DraftStoreError stageTransfer(const Note &source, const QString &destinationStorageId,
                                  const QUuid &destinationFolderId, QUuid *draftId = nullptr,
                                  bool folderUserOverride = false);
    bool            hasPendingTransferFrom(const QString &storageId, const QString &noteId) const;
    void            setPrePublicationHandler(PrePublicationHandler handler);
    /** Safely converts a pending draft into a restart-safe storage transfer. */
    DraftStoreError retargetDraftForPublication(DraftRecord *record, const QString &destinationStorageId) const;
    void            publishPending();
    /** Preserve in-flight publication drafts for a clean retry on the next launch. */
    void                          prepareForShutdown();
    QList<DraftRecord>            pendingDrafts() const;
    DraftStoreResult<DraftRecord> pendingDraft(const QUuid &draftId) const;
    DraftStoreResult<DraftRecord> pendingDraftForNote(const QString &storageId, const QString &noteId) const;
    QList<DraftRecord>            recoverableDrafts() const;
    void                          setConflictResolver(std::unique_ptr<ConflictResolver> resolver);
    /// Resolves a conflict discovered after a storage operation was acknowledged.
    void resolveConcurrentEdit(const Note &localVersion, const Note &remoteVersion, const QString &message);

signals:
    void discardEditorsForNoteRequested(const QString &storageId, const QString &noteId);
    void discardEditorsForDraftRequested(const QUuid &draftId);
    void draftsChanged();
    void draftPublished(const QUuid &draftId, const Note &note);
    void draftPublishFailed(const QUuid &draftId, const QString &message);
    void publishingIdle();
    void publicationAbandoned(const QString &message);
    void recoveryNotice(const QString &message);
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
    void           observeStorageRemovals(NoteStorage *storage);
    void           cancelPublication(const QUuid &draftId);
    void           reconcileStaleSaveSuccess(const DraftRecord &attempt, const Note &result);
    bool           recoverMissingRemoteIdentity(const DraftRecord &record, const StorageError &error);
    void           refreshLiveEditorAliases(NoteEditor *editor);
    void           removeLiveEditor(NoteEditor *editor);
    QSet<QUuid>     liveDraftIdsForAlias(const QString &storageId, const QString &noteId) const;
    static QString sourceKey(const QString &storageId, const QString &noteId);
    static QString sourceKey(const Note &note);

    std::unique_ptr<DraftStore>        store_;
    QSet<QUuid>                        publishing_;
    QHash<QUuid, QPointer<StorageJob>> publishJobs_;
    QHash<QUuid, int>                  editingSessions_;
    QHash<QString, QUuid>              sourceSessions_;
    QHash<QUuid, QString>              editingSources_;
    QHash<QUuid, QPointer<NoteEditor>> liveEditorsByDraft_;
    QHash<QString, QPointer<NoteEditor>> liveEditorsBySource_;
    QString                            lastError_;
    std::unique_ptr<ConflictResolver>  conflictResolver_;
    PrePublicationHandler              prePublicationHandler_;
    QSet<NoteStorage *>                 observedRemovalStorages_;
    bool                               shuttingDown_ { false };
};

} // namespace AnyKeep

#endif // DRAFTMANAGER_H
