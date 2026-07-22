#ifndef XMPPSTORAGE_H
#define XMPPSTORAGE_H

#include "notestorage.h"
#include "xmppdto.h"

#include <QHash>
#include <QTimer>

#include <memory>

namespace QtNote {

class XmppBackend;
class XmppSettingsController;
class RemoteCacheStore;

/**
 * @brief QtNote storage adapter backed by encrypted XMPP Personal Eventing Protocol nodes.
 *
 * XmppStorage translates between QtNote's Note/NoteStorage API and the
 * backend-neutral XmppBackend contract. It owns the backend when no backend is
 * injected, maintains a local index cache, classifies failures, and schedules
 * reconnects for transient failures. Durable write/delete retry remains the
 * responsibility of QtNote's draft machinery.
 *
 * All methods and callbacks run in this object's thread. Asynchronous callbacks
 * capture configEpoch_ and discard their result after an account/configuration
 * change, preventing an old operation from mutating the new account's cache.
 */
class XmppStorage final : public NoteStorage {
    Q_OBJECT
    Q_DISABLE_COPY(XmppStorage)

public:
    explicit XmppStorage(QObject *parent = nullptr, XmppBackend *backend = nullptr);
    ~XmppStorage() override;
    void shutdown() override;

    bool            init() override;
    StorageInitJob *initAsync(QObject *owner = nullptr) override;
    const QString   systemName() const override;
    const QString   name() const override;
    QIcon           storageIcon() const override;
    QIcon           noteIcon() const override;
    bool            isAccessible() const override;

    QList<Note::Format> availableFormats() const override;
    QList<Note>         noteList(int limit = 0) override;
    NoteListJob        *refreshNotesAsync(int limit = 0, QObject *owner = nullptr) override;
    Note                note(const QString &id) override;
    NoteLoadJob        *loadNoteAsync(const QString &id, QObject *owner = nullptr) override;
    Note                createNote() override;
    bool                loadNote(Note &note) override;
    bool                saveNote(const Note &note) override;
    NoteSaveJob        *saveNoteAsync(const Note &note, QObject *owner = nullptr) override;
    void                removeNote(const QString &noteId) override;
    NoteRemoveJob      *removeNoteAsync(const QString &noteId, QObject *owner = nullptr) override;

    bool                isConfigurable() const override { return true; }
    QUrl                settingsComponent() const override;
    SettingsController *createSettingsController(QObject *parent = nullptr) override;
    QString             tooltip() override;

    static QString storageId;

signals:
    void encryptionKeyChanged(const QByteArray &keyId, const QString &message);

private slots:
    void onRemoteNotePublished(const QtNote::XmppRemoteNote &remote);
    void onRemoteNoteRetracted(const QString &id);
    void onRemoteNodeInvalidated();
    void onConnectionChanged(bool connected);

private:
    friend class XmppSettingsController;
    XmppConfig     readConfig() const;
    bool           configIsValid(const XmppConfig &config, QString *error) const;
    Note           fromRemote(const XmppRemoteNote &remote);
    void           applyRemote(Note &note, const XmppRemoteNote &remote);
    XmppRemoteNote toRemote(const Note &note) const;
    void           reportError(const QString &error, bool invalidate = false);
    void           enterErrorState(const QString &error, bool invalidate = false);
    void           clearErrorState();
    void           handleTransientFailure(const QString &error, bool invalidate = true);
    void           scheduleRetry();
    void           retryInitialization();
    void           resetRetryBackoff();
    void           applyConfig(const XmppConfig &config);
    void           installReceivedStorageKey(const QString &jid, const QByteArray &key);
    void           resolveStorageKeys(const QString &jid, XmppSettingsController *settings = nullptr);
    bool           openPersistentCache(const XmppConfig &config);
    void           persistCache();
    void           startBodyPrefetch(const QStringList &ids);
    void           prefetchNextBody();

    /// Stable configuration snapshot used by operations until applyConfig().
    XmppConfig config_;
    /// Protocol implementation; parented to this storage when constructed internally.
    XmppBackend *backend_ { nullptr };
    /// Most recently loaded remote note index, keyed by note ID.
    QHash<QString, Note>              cache_;
    std::unique_ptr<RemoteCacheStore> persistentCache_;
    QString                           persistentCacheInstanceId_;
    bool                              cacheAvailable_ { false };
    QStringList                       bodyPrefetchQueue_;
    bool                              bodyPrefetchRunning_ { false };
    /// Whether cache_ is a complete representation of the current remote index.
    bool cacheValid_ { false };
    /// Whether normal storage operations may currently be attempted.
    bool accessible_ { false };
    /// Set for a terminal error that automatic reconnects cannot repair.
    bool    errorState_ { false };
    QString errorStateMessage_;
    /// Suppresses repeated user notifications containing the same error text.
    QString lastReportedError_;
    /// Prevents multiple key-recovery wizards from running concurrently.
    bool keyResolutionInProgress_ { false };
    /// Single-shot timer implementing storage-level reconnect backoff.
    QTimer *retryTimer_ { nullptr };
    /// Current exponential retry delay; reset after successful initialization.
    int retryDelaySeconds_ { 30 };
    /// Guards against overlapping retryInitialization() attempts.
    bool retryInProgress_ { false };
    /// Makes shutdown idempotent and prevents new callbacks from scheduling work.
    bool shuttingDown_ { false };
    /**
     * @brief Generation of the active storage configuration.
     *
     * Incremented by applyConfig() and shutdown(). Every asynchronous storage
     * operation captures the current value and compares it before applying its
     * result. A mismatch means that the callback belongs to an obsolete account
     * or backend lifetime and must be ignored/cancelled.
     */
    quint64 configEpoch_ { 0 };
};

} // namespace QtNote

#endif // XMPPSTORAGE_H
