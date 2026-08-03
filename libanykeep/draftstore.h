#ifndef DRAFTSTORE_H
#define DRAFTSTORE_H

#include "note.h"
#include "anykeep_export.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariantMap>

namespace AnyKeep {

struct ANYKEEP_EXPORT DraftRecord {
    enum Operation { Publish, Delete };

    // Persistent publication state. A draft remains Editing while its note is
    // open; only an explicit close may make it eligible for publication.
    enum State {
        Editing,     // Locally autosaved, still being edited; never publish.
        Ready,       // Finished by the user and assigned to a storage.
        Publishing,  // A save operation is currently in progress.
        Retry,       // Publication failed temporarily and will be retried.
        NeedsRouting // Finished, but no longer assigned to a storage; publish as a new note after routing.
    };

    QUuid     id;
    Operation operation { Publish };
    State     state { Editing };
    // Origin of the edit. They are empty for a note which has not yet been
    // assigned by routing; they are not a user-selected publication target.
    QString      storageId;
    QString      remoteNoteId;
    QString      title;
    QString      body;
    Note::Format format { Note::PlainText };
    QStringList  tags;
    // Intended AnyKeep folder. It is local draft metadata until the target
    // storage acknowledges the corresponding note update.
    QUuid folderId;
    // A direct folder choice in the active editor is stronger than an
    // automatic publication rule for this draft. It is local-only metadata
    // and is discarded once publication succeeds.
    bool folderUserOverride { false };
    // A cross-storage move publishes this draft to its target first, then
    // durably queues deletion of this original.  Keeping the source in the
    // encrypted draft makes the two-phase operation restart-safe.
    QString removeSourceStorageId;
    QString removeSourceNoteId;
    // Opaque storage-specific state (XMPP revision, Nextcloud ETag, Keep base
    // version, ...), captured when editing starts and restored before save.
    QVariantMap           backendData;
    QList<MediaReference> media;
    quint64               revision { 0 }; // Monotonic checkpoint sequence within this draft session.
    QDateTime             updatedAt;
    QString               lastError;
    QDateTime             retryAt;
};

struct ANYKEEP_EXPORT DraftStoreError {
    enum Code { None, InvalidArgument, NotFound, Locked, CryptoUnavailable, Corrupt, Io };

    Code    code { None };
    QString message;

    explicit operator bool() const { return code != None; }
};

template <typename T> struct DraftStoreResult {
    T               value;
    DraftStoreError error;

    explicit operator bool() const { return !error; }
};

class ANYKEEP_EXPORT DraftStore {
public:
    virtual ~DraftStore() = default;

    virtual DraftStoreError                      write(const DraftRecord &record)                      = 0;
    virtual DraftStoreResult<DraftRecord>        load(const QUuid &id) const                           = 0;
    virtual DraftStoreResult<QList<DraftRecord>> records() const                                       = 0;
    virtual DraftStoreError                      transition(const QUuid &id, DraftRecord::State state) = 0;
    virtual DraftStoreError                      remove(const QUuid &id)                               = 0;
};

} // namespace AnyKeep

#endif // DRAFTSTORE_H
