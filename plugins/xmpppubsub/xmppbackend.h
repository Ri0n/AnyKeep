#ifndef XMPPBACKEND_H
#define XMPPBACKEND_H

#include "xmppdto.h"

#include <QObject>
#include <functional>

namespace AnyKeep {

/**
 * @brief Backend-neutral asynchronous XMPP/PubSub service used by XmppStorage.
 *
 * The interface deliberately contains no QXmpp types so that another protocol
 * implementation (for example, Iris) can be introduced without changing the
 * note-storage layer. Implementations live in the QObject's thread and must not
 * block its event loop.
 *
 * Values passed to asynchronous methods are owned copies. This is important
 * because an implementation may keep them in a coroutine frame after the
 * caller has returned. A callback is expected to be invoked once while the
 * backend remains alive; destroying the backend cancels outstanding work.
 *
 * @note shutdown() is terminal until start() is called. setConfig() updates
 * configuration but does not implicitly revive a stopped backend.
 */
class XmppBackend : public QObject {
    Q_OBJECT

public:
    using StatusCallback  = std::function<void(XmppStatusResult)>;
    using ListCallback    = std::function<void(XmppListResult)>;
    using NoteCallback    = std::function<void(XmppNoteResult)>;
    using DevicesCallback = std::function<void(QList<XmppDeviceInfo>, QString)>;
    using AuditCallback   = std::function<void(XmppKeyAuditResult)>;
    using RekeyCallback   = std::function<void(XmppRekeyResult)>;
    using CleanupCallback = std::function<void(XmppCleanupResult)>;

    using QObject::QObject;
    ~XmppBackend() override = default;

    virtual void start()                                                   = 0;
    virtual void setConfig(const XmppConfig &config)                       = 0;
    virtual void shutdown()                                                = 0;
    virtual void probeAsync(StatusCallback callback)                       = 0;
    virtual void listNotesAsync(ListCallback callback)                     = 0;
    virtual void getNoteAsync(QString id, NoteCallback callback)           = 0;
    virtual void saveNoteAsync(XmppRemoteNote note, NoteCallback callback) = 0;
    /** Republishes metadata in the encrypted index without uploading note content. */
    virtual void           updateNoteIndexAsync(XmppRemoteNote note, NoteCallback callback)                 = 0;
    virtual void           deleteNoteAsync(QString id, StatusCallback callback)                             = 0;
    virtual XmppDeviceInfo ownOmemoDevice() const                                                           = 0;
    virtual void           ownOmemoDevicesAsync(DevicesCallback callback)                                   = 0;
    virtual void           ownOmemoBundleValidAsync(StatusCallback callback)                                = 0;
    virtual void           repairOwnOmemoDeviceAsync(StatusCallback callback)                               = 0;
    virtual void           trustOwnOmemoDeviceAsync(QByteArray keyId, StatusCallback callback)              = 0;
    virtual void           trustOwnOmemoDevicesAsync(QList<QByteArray> keyIds, StatusCallback callback)     = 0;
    virtual void           auditStorageKeysAsync(AuditCallback callback)                                    = 0;
    virtual void rekeyStorageAsync(QList<QByteArray> keys, QByteArray canonicalKey, RekeyCallback callback) = 0;
    virtual void scanObsoleteItemsAsync(CleanupCallback callback)                                           = 0;
    virtual void deleteObsoleteItemsAsync(QStringList indexItemIds, QStringList contentItemIds,
                                          CleanupCallback callback)
        = 0;
    virtual void approveKeySyncRequest(QString requestId) = 0;
    virtual void rejectKeySyncRequest(QString requestId)  = 0;

signals:
    /// A complete remote note was published or changed.
    void remoteNotePublished(const AnyKeep::XmppRemoteNote &note);
    /// A note item was retracted from the remote index.
    void remoteNoteRetracted(const QString &id);
    /// The remote index changed in a way that requires a full refresh.
    void remoteNodeInvalidated();
    /// Reports changes of the underlying XMPP connection state.
    void connectionChanged(bool connected);
    /// Reports a backend error that is not tied to a single callback.
    void backendError(const QString &error);
    /// Requests user approval for bootstrapping trust in an OMEMO key.
    void keySyncTrustRequested(const QString &requestId, const QByteArray &keyId);
};

} // namespace AnyKeep

#endif // XMPPBACKEND_H
